/*
 * NvRm shim: sits between libnvrm and the kernel, in front of its ioctl.
 *
 * Two jobs, both needed only while the MIUI ISP library is doing its own
 * bring-up inside our process:
 *
 * 1. NVMAP_IOC_MMAP translation -- the 24.1 kernel removed that ioctl;
 *    libnvrm ignores the error and later dereferences a null push buffer.
 *    Translated to GET_FD + mmap on the dmabuf. The stock kernel still has
 *    the ioctl, and there libnvrm does not even call it, so this part is
 *    idle on the stock kernel. Kept as it was in April.
 *
 * 2. Stripping the streaming trigger from HwSettingsApply's gather. The
 *    library ends its calibration submit with ISP_CONTROL (0x00C) = 0x05,
 *    which starts the block waiting for sensor pixels. Nothing feeds it,
 *    so it waits forever, and when the channel is closed the kernel waits
 *    for the block and the device hangs. That is what happened twice on
 *    2026-09-02: the April version of this strip only looked inside
 *    buffers it had itself mapped through job 1, found none on the stock
 *    kernel, and passed the trigger through while the caller believed it
 *    stripped.
 *
 *    Now the strip does not depend on how the library mapped anything. On
 *    every channel SUBMIT while NVRM_SHIM_STRIP is set, each command
 *    buffer is read back through NVMAP_IOC_READ by the handle and offset
 *    the submit itself names, walked as host1x opcodes, and any
 *    ISP_CONTROL=0x05 is replaced with a NOP through NVMAP_IOC_WRITE. Each
 *    gather is also saved to /data/local/tmp/libgather_<submit>_<n>.bin --
 *    the record of what the library actually sends. If a buffer cannot be
 *    read or written back, the submit is REFUSED (returns -1) rather than
 *    passed through unchecked, so the library's Apply fails and nothing
 *    reaches the hardware. The running tally goes to
 *    /data/local/tmp/nvrm_shim_status.txt for the caller to check.
 *
 * Build:
 *   $CC --sysroot=$SYSROOT -std=gnu99 -shared -fPIC -o nvrm_shim.so nvrm_shim.c -ldl
 *
 * Usage: LD_PRELOAD=/data/local/tmp/nvrm_shim.so <program>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/ioctl.h>

#define STATUS_PATH "/data/local/tmp/nvrm_shim_status.txt"
#define GATHER_PATH_FMT "/data/local/tmp/libgather_%02d_%u.bin"

/* nvmap ioctl magic */
#define NVMAP_IOC_MAGIC 'N'

/* Old NVMAP_IOC_MMAP — removed in 24.1, returns ENOTTY */
struct nvmap_map_caller {
    uint32_t handle;
    uint32_t offset;
    uint32_t length;
    uint32_t flags;
    unsigned long addr;
};
#define NVMAP_IOC_MMAP _IOWR(NVMAP_IOC_MAGIC, 5, struct nvmap_map_caller)

/* New NVMAP_IOC_GET_FD — exports handle as dmabuf fd */
struct nvmap_create_handle {
    union {
        uint32_t id;
        uint32_t size;
        int32_t fd;
    };
    uint32_t handle;
};
#define NVMAP_IOC_GET_FD _IOWR(NVMAP_IOC_MAGIC, 15, struct nvmap_create_handle)

/* Read/write a handle's contents through the driver, whoever mapped it. */
struct nvmap_rw_handle {
    unsigned long addr; uint32_t handle; uint32_t offset;
    uint32_t elem_size; uint32_t hmem_stride; uint32_t user_stride; uint32_t count;
};
#define NVMAP_IOC_WRITE _IOW(NVMAP_IOC_MAGIC, 6, struct nvmap_rw_handle)
#define NVMAP_IOC_READ  _IOW(NVMAP_IOC_MAGIC, 7, struct nvmap_rw_handle)

/* Track dmabuf fds so we can close them later */
#define MAX_MAPPED 64
static struct {
    unsigned long addr;
    uint32_t length;
    int dmabuf_fd;
} mapped[MAX_MAPPED];
static int num_mapped;

static int (*real_ioctl)(int fd, int request, ...) = NULL;

static void init_real_ioctl(void) {
    if (!real_ioctl) {
        real_ioctl = (int (*)(int, int, ...))dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl) {
            /* fallback: use syscall */
            real_ioctl = (int (*)(int, int, ...))dlsym(RTLD_DEFAULT, "ioctl");
        }
    }
}

/*
 * Translate NVMAP_IOC_MMAP to GET_FD + mmap.
 *
 * The old flow was:
 *   1. userspace calls mmap(NULL, size, ..., nvmap_fd, 0) → gets a VMA
 *   2. userspace calls ioctl(nvmap_fd, NVMAP_IOC_MMAP, {handle, off, len, flags, addr})
 *   3. kernel remaps the VMA to point to the nvmap handle's pages
 *
 * We can't do step 3 without kernel support. Instead:
 *   1. Get a dmabuf fd for the handle via NVMAP_IOC_GET_FD
 *   2. munmap the old VMA (from step 1 of old flow)
 *   3. mmap with the dmabuf fd → new addr
 *   4. Write new addr back to caller's struct
 */
static int shim_nvmap_mmap(int fd, struct nvmap_map_caller *mc) {
    if (!mc || !mc->handle || !mc->length) {
        errno = EINVAL;
        return -1;
    }

    fprintf(stderr, "nvrm_shim: MMAP handle=%u offset=%u length=%u addr=0x%lx\n",
            mc->handle, mc->offset, mc->length, mc->addr);

    /* Step 1: Get dmabuf fd for this handle */
    struct nvmap_create_handle gf;
    memset(&gf, 0, sizeof(gf));
    gf.handle = mc->handle;
    if (real_ioctl(fd, NVMAP_IOC_GET_FD, &gf) < 0) {
        fprintf(stderr, "nvrm_shim: GET_FD failed for handle %u: %s\n",
                mc->handle, strerror(errno));
        return -1;
    }
    int dmabuf_fd = gf.fd;
    fprintf(stderr, "nvrm_shim: GET_FD handle=%u → dmabuf_fd=%d\n",
            mc->handle, dmabuf_fd);

    /* Step 2: Unmap old VMA if addr was pre-mapped (not MAP_FAILED) */
    if (mc->addr && mc->addr != (unsigned long)MAP_FAILED) {
        munmap((void *)mc->addr, mc->length);
    }

    /* Step 3: mmap the dmabuf fd */
    void *new_addr = mmap(NULL, mc->length,
                          PROT_READ | PROT_WRITE,
                          MAP_SHARED,
                          dmabuf_fd, mc->offset);
    if (new_addr == MAP_FAILED) {
        fprintf(stderr, "nvrm_shim: mmap dmabuf_fd=%d failed: %s\n",
                dmabuf_fd, strerror(errno));
        close(dmabuf_fd);
        return -1;
    }

    fprintf(stderr, "nvrm_shim: mapped handle=%u → addr=%p (len=%u)\n",
            mc->handle, new_addr, mc->length);

    /* Track for cleanup */
    if (num_mapped < MAX_MAPPED) {
        mapped[num_mapped].addr = (unsigned long)new_addr;
        mapped[num_mapped].length = mc->length;
        mapped[num_mapped].dmabuf_fd = dmabuf_fd;
        num_mapped++;
    }
    /* Otherwise keep the fd open -- closing it would unmap the buffer. */

    /* Step 4: Return new address to caller */
    mc->addr = (unsigned long)new_addr;

    return 0;
}

/*
 * Intercept mmap on /dev/nvmap.
 *
 * Old kernel allowed mmap on /dev/nvmap fd to create a VMA, then
 * NVMAP_IOC_MMAP bound that VMA to a handle's pages. New kernel rejects
 * mmap on /dev/nvmap entirely. If mmap fails on the nvmap fd, hand back
 * anonymous memory as a placeholder; the MMAP ioctl shim replaces it.
 */
static int nvmap_fd_cached = -1;

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    static void *(*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;
    if (!real_mmap)
        real_mmap = dlsym(RTLD_NEXT, "mmap");

    void *result = real_mmap(addr, length, prot, flags, fd, offset);

    if (result == MAP_FAILED && fd >= 0 && (flags & MAP_SHARED)) {
        if (fd == nvmap_fd_cached) {
            result = real_mmap(NULL, length, prot,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (result != MAP_FAILED) {
                fprintf(stderr, "nvrm_shim: mmap fallback for nvmap fd=%d → %p (len=%zu)\n",
                        fd, result, length);
            }
        }
    }

    return result;
}

/* ---- the strip ---- */

static int submits, scanned, stripped, failed;

static void status_write(void) {
    FILE *f = fopen(STATUS_PATH, "w");
    if (!f) return;
    fprintf(f, "submits=%d scanned=%d stripped=%d failed=%d\n",
            submits, scanned, stripped, failed);
    fclose(f);
}

static int nvmap_rw(int fd, uint32_t handle, uint32_t off, void *buf,
                    uint32_t len, int write) {
    struct nvmap_rw_handle rw = {
        .addr = (unsigned long)buf, .handle = handle, .offset = off,
        .elem_size = len, .hmem_stride = len, .user_stride = len, .count = 1,
    };
    return real_ioctl(fd, write ? NVMAP_IOC_WRITE : NVMAP_IOC_READ, &rw);
}

/* Walk one gather as host1x opcodes and NOP every ISP_CONTROL = 0x05.
 * Returns the number replaced. Walking by opcode, not by word: a data
 * word that happens to look like the trigger must not be touched. */
static int strip_gather(uint32_t *w, uint32_t words, unsigned g) {
    int hits = 0;
    uint32_t i = 0;
    while (i < words) {
        uint32_t op = w[i], opc = op >> 28, m = (op >> 16) & 0xfff, cnt = op & 0xffff;
        if (opc == 1 || opc == 2) {                 /* INCR / NONINCR */
            if (m == 0x00c && cnt == 1 && i + 1 < words && w[i + 1] == 0x05) {
                fprintf(stderr, "nvrm_shim: NOP streaming trigger, gather %u word %u\n", g, i);
                w[i] = 0x20000000; w[i + 1] = 0x20000000;
                hits++;
            }
            i += 1 + cnt;
        } else if (opc == 3) {                      /* MASK */
            i += 1 + (uint32_t)__builtin_popcount(cnt);
        } else if (opc == 4) {                      /* IMM */
            if (m == 0x00c && cnt == 0x05) {
                fprintf(stderr, "nvrm_shim: NOP streaming trigger (IMM), gather %u word %u\n", g, i);
                w[i] = 0x20000000;
                hits++;
            }
            i += 1;
        } else {                                    /* SETCLASS and the rest */
            i += 1;
        }
    }
    return hits;
}

static int refuse(const char *why) {
    fprintf(stderr, "nvrm_shim: REFUSING submit: %s\n", why);
    failed++;
    status_write();
    errno = ECANCELED;
    return -1;
}

/* Intercepted ioctl */
int ioctl(int fd, int request, ...) {
    void *arg;
    __builtin_va_list ap;
    __builtin_va_start(ap, request);
    arg = __builtin_va_arg(ap, void *);
    __builtin_va_end(ap);

    init_real_ioctl();

    int strip_streaming = getenv("NVRM_SHIM_STRIP") ? 1 : 0;

    unsigned int nr = _IOC_NR(request);
    unsigned int type = _IOC_TYPE(request);

    /* Track nvmap fd: for the mmap fallback, and for reading gathers back. */
    if (type == NVMAP_IOC_MAGIC && nvmap_fd_cached < 0) {
        nvmap_fd_cached = fd;
        fprintf(stderr, "nvrm_shim: detected nvmap fd=%d\n", fd);
    }

    /* Intercept NVMAP_IOC_MMAP */
    if (type == NVMAP_IOC_MAGIC && nr == 5) {
        return shim_nvmap_mmap(fd, (struct nvmap_map_caller *)arg);
    }

    /* Channel SUBMIT (NR=15, 32-bit form) while stripping is on. */
    #define NVHOST_MAGIC 'H'
    if (type == NVHOST_MAGIC && nr == 15 && strip_streaming) {
        struct {
            uint32_t submit_version, num_syncpt_incrs, num_cmdbufs;
            uint32_t num_relocs, num_waitchks, timeout;
            uint32_t syncpt_incrs, cmdbufs;
        } *sa = arg;
        struct { uint32_t mem; uint32_t offset; uint32_t words; } *cbs =
            (void *)(uintptr_t)sa->cmdbufs;

        submits++;
        if (nvmap_fd_cached < 0)
            return refuse("no nvmap fd seen yet, cannot read the gather back");
        if (sa->num_cmdbufs == 0 || sa->num_cmdbufs > 16)
            return refuse("implausible cmdbuf count");

        for (uint32_t g = 0; g < sa->num_cmdbufs; g++) {
            uint32_t words = cbs[g].words;
            if (words == 0 || words > (1u << 20))
                return refuse("implausible gather length");
            uint32_t *buf = malloc((size_t)words * 4);
            if (!buf)
                return refuse("out of memory");
            if (nvmap_rw(nvmap_fd_cached, cbs[g].mem, cbs[g].offset, buf, words * 4, 0) < 0) {
                free(buf);
                return refuse("NVMAP_IOC_READ of the gather failed");
            }
            scanned++;

            char fname[128];
            snprintf(fname, sizeof fname, GATHER_PATH_FMT, submits, g);
            FILE *gf = fopen(fname, "wb");
            if (gf) {
                fwrite(buf, 4, words, gf);
                fclose(gf);
            }
            fprintf(stderr, "nvrm_shim: submit %d gather %u: %u words (handle %u off %u) -> %s\n",
                    submits, g, words, cbs[g].mem, cbs[g].offset, fname);

            int hits = strip_gather(buf, words, g);
            if (hits) {
                if (nvmap_rw(nvmap_fd_cached, cbs[g].mem, cbs[g].offset, buf, words * 4, 1) < 0) {
                    free(buf);
                    return refuse("NVMAP_IOC_WRITE of the stripped gather failed");
                }
                stripped += hits;
            }
            free(buf);
        }
        fprintf(stderr, "nvrm_shim: submit %d passes: %d triggers stripped so far\n",
                submits, stripped);
        status_write();
    }

    return real_ioctl(fd, request, arg);
}
