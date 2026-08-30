/*
 * nvrmlog -- LD_PRELOAD interceptor for NvRmMemHandleAllocAttr.
 *
 * Purpose: the stock's own working allocations happen in OUR process
 * during NvIspOpen (ten CREATE/ALLOC/MMAP triples, rc=0, seen in
 * strace) -- but we do not know the arguments. This library catches
 * them: it exports NvRmMemHandleAllocAttr, prints everything, and
 * forwards the call to the real libnvrm.so unchanged.
 *
 * Usage:
 *   LD_PRELOAD=/data/local/tmp/nvrmlog.so ispinit 4194303 rggb
 *
 * Discipline: log-and-forward, nothing substituted. The only behavior
 * change is the degenerate one -- if the real function cannot be
 * resolved at all we return 0xb without calling, which is announced in
 * the log; there is no way to "proceed" there anyway.
 *
 * Note on coverage: calls made through the dynamic linker's symbol
 * resolution (libnvisp_v3 -> libnvrm) go through this interceptor. A call
 * made through dlsym(libnvrm_handle, ...) -- as ispinit itself does --
 * resolves to the REAL symbol and bypasses us; ispinit prints its own
 * line for that call, so the comparison still lands in one log.
 *
 * Built like ispinit (crt0+libc, no -nostdlib games). This runs only in
 * our own test process; it never goes near mediaserver.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

/* nvhost_submit_args, include/linux/nvhost_ioctl.h of OUR kernel tree:
   u32 header block, then u64 pointers. On arm32 the pointers are read
   as the low words of those u64 slots (offsets 72/80/88 per header). */
struct nvhost_submit_view {
    unsigned num_cmdbufs;
    unsigned num_relocs;
    unsigned num_syncpt_incrs;
    unsigned cmdbufs;   /* ptr to nvhost_cmdbuf array {mem,off,words} */
    unsigned relocs;    /* ptr to nvhost_reloc array */
};

struct nvhost_cmdbuf_view { unsigned mem, offset, words; };
struct nvhost_reloc_view { unsigned cmdbuf_mem, cmdbuf_offset, target, target_offset; };

static int nvmap_fd = -1;
static int isp_fd = -1;
#define MAX_MAPPED 64
static unsigned map_handle[MAX_MAPPED], map_va[MAX_MAPPED], map_len[MAX_MAPPED];
static int map_n;

/*
 * dlopen flags, bionic 4.4: RTLD_NOW | RTLD_LOCAL == 0.
 * (Do NOT copy NDK header constants; see shim/src/nvisp_shim.c.)
 */
#define NVRMLOG_DLOPEN_FLAGS 0

typedef int (*AllocAttr_fn)(unsigned a1, void *attrs, void **out);

static void *real_handle;        /* libnvrm.so */
static void *real_graphics;      /* libnvrm_graphics.so */

/*
 * Lazy resolver against BOTH libraries: the stream-path functions live
 * in libnvrm_graphics.so, the memory/API core in libnvrm.so. Cached
 * per name; handles opened once process-wide.
 */
static void *resolve_real(const char *name, void **cache)
{
    if (*cache == 0) {
        if (real_graphics == 0)
            real_graphics = dlopen("libnvrm_graphics.so",
                                   NVRMLOG_DLOPEN_FLAGS);
        if (real_graphics != 0)
            *cache = dlsym(real_graphics, name);
        if (*cache == 0) {
            if (real_handle == 0)
                real_handle = dlopen("libnvrm.so", NVRMLOG_DLOPEN_FLAGS);
            if (real_handle != 0)
                *cache = dlsym(real_handle, name);
        }
    }
    return *cache;
}

int NvRmMemHandleAllocAttr(unsigned a1, void *attrs, void **out)
{
    static AllocAttr_fn real_fn;
    int rc;

    real_fn = (AllocAttr_fn)resolve_real("NvRmMemHandleAllocAttr",
                                         (void **)&real_fn);

    printf("[nvrmlog] AllocAttr arg1=0x%x attrs=%p out=%p\n",
           a1, attrs, (void *)out);

    if (attrs != 0) {
        unsigned *a = (unsigned *)attrs;
        int k;

        printf("[nvrmlog]   attrs:");
        for (k = 0; k < 8; k++)
            printf(" [%d]=0x%x", k, a[k]);
        printf("\n");

        if (a[0] != 0) {
            /* the tag list: the FIRST word decides -- outside 1..6 ends
               the loop immediately (tagless). Print eight words, mark
               the verdict. */
            unsigned *t = (unsigned *)a[0];
            printf("[nvrmlog]   tags@0x%x:", a[0]);
            for (k = 0; k < 8; k++)
                printf(" %08x", t[k]);
            printf("  (w0 %s 1..6 -> %s)\n",
                   (t[0] >= 1 && t[0] <= 6) ? "in" : "outside",
                   (t[0] >= 1 && t[0] <= 6) ? "tagged" : "tagless");
        } else {
            printf("[nvrmlog]   tags: attrs[0]==0\n");
        }
    }

    if (real_fn == 0) {
        printf("[nvrmlog]   UNRESOLVED in libnvrm_graphics.so and "
               "libnvrm.so -- refusing to fake a return; exiting 70\n");
        exit(70);
    }

    rc = real_fn(a1, attrs, out);
    printf("[nvrmlog]   -> rc=0x%x memh=%p\n", (unsigned)rc,
           out != 0 ? *out : 0);
    return rc;
}

/*
 * NvRmMemRead: libnvisp_v3 imports it, so live calls occur during normal
 * ISP operation. Arity unestablished -- four words printed and forwarded
 * as-is (ARM AAPCS ignores extra registers harmlessly; we print the
 * first four, the shape will be visible from the values).
 */
int NvRmMemRead(unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    static int (*real_fn)(unsigned, unsigned, unsigned, unsigned);
    int rc;

    real_fn = (int (*)(unsigned, unsigned, unsigned, unsigned))
        resolve_real("NvRmMemRead", (void **)&real_fn);

    printf("[nvrmlog] Read a1=0x%x a2=0x%x a3=0x%x a4=0x%x\n",
           a1, a2, a3, a4);

    if (real_fn == 0) {
        printf("[nvrmlog]   UNRESOLVED -- exiting 70\n");
        exit(70);
    }
    rc = real_fn(a1, a2, a3, a4);
    printf("[nvrmlog]   -> rc=0x%x\n", (unsigned)rc);
    return rc;
}

/*
 * NvRmMemHandleFree: the paired teardown -- captured so we see WHEN the
 * stock frees surfaces and with which handle (our own teardown waits
 * for a clean submission first).
 */
int NvRmMemHandleFree(unsigned a1)
{
    static int (*real_fn)(unsigned);
    int rc;

    real_fn = (int (*)(unsigned))resolve_real("NvRmMemHandleFree",
                                              (void **)&real_fn);

    printf("[nvrmlog] HandleFree a1=0x%x\n", a1);

    if (real_fn == 0) {
        printf("[nvrmlog]   UNRESOLVED -- exiting 70\n");
        exit(70);
    }
    rc = real_fn(a1);
    printf("[nvrmlog]   -> rc=0x%x\n", (unsigned)rc);
    return rc;
}

/*
 * Stream-path functions: if Begin/End/Flush fire during OUR submission,
 * the pushbuffer was formed and handed to hardware -- meaning format
 * and geometry passed and a nonzero rc from ProcessFrame is NOT a
 * rejection. Generic four-word print, forwarded as-is; arity
 * unestablished, extra registers are harmless on AAPCS.
 */
typedef int (*Any4_fn)(unsigned, unsigned, unsigned, unsigned);

static int nvrmlog_4(const char *name, const char *tag,
                     unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    static void *cache[8];
    static const char *names[8];
    int (*fn)(unsigned, unsigned, unsigned, unsigned);
    int i, rc;

    for (i = 0; i < 8; i++) {
        if (names[i] == 0) {
            names[i] = name;
            cache[i] = 0;
        }
        if (names[i] == name)
            break;
    }
    fn = (Any4_fn)resolve_real(name, (void **)&cache[i < 8 ? i : 0]);

    printf("[nvrmlog] %s a1=0x%x a2=0x%x a3=0x%x a4=0x%x\n",
           tag, a1, a2, a3, a4);
    if (fn == 0) {
        printf("[nvrmlog]   UNRESOLVED -- exiting 70\n");
        exit(70);
    }
    rc = fn(a1, a2, a3, a4);
    printf("[nvrmlog]   -> rc=0x%x\n", (unsigned)rc);
    return rc;
}

int NvRmStreamBegin(unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    return nvrmlog_4("NvRmStreamBegin", "StreamBegin", a1, a2, a3, a4);
}

int NvRmStreamEnd(unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    return nvrmlog_4("NvRmStreamEnd", "StreamEnd", a1, a2, a3, a4);
}

int NvRmStreamFlush(unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    return nvrmlog_4("NvRmStreamFlush", "StreamFlush", a1, a2, a3, a4);
}

/* the accumulated stream-error query: stage 3 surfaces ITS result as
   the ProcessFrame code (0xa). Seeing GetError's return directly shows
   the stream state at the moment it is queried, not at the end */
int NvRmStreamGetError(unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    return nvrmlog_4("NvRmStreamGetError", "StreamGetError",
                     a1, a2, a3, a4);
}

int NvRmChannelSyncPointWaitTimeout(unsigned a1, unsigned a2,
                                    unsigned a3, unsigned a4)
{
    return nvrmlog_4("NvRmChannelSyncPointWaitTimeout",
                     "SyncPointWaitTimeout", a1, a2, a3, a4);
}

int NvRmFenceWait(unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    return nvrmlog_4("NvRmFenceWait", "FenceWait", a1, a2, a3, a4);
}

/*
 * Command-buffer path: what the library actually pushes per frame.
 * PushReloc is the interesting one -- relocations carry (command
 * buffer handle, offset, TARGET memory handle, target offset, shift);
 * counting them per frame tells us whether the output surface address
 * ever reaches the hardware.
 */
typedef int (*PushReloc_fn)(unsigned cmdbuf, unsigned off, unsigned memh,
                            unsigned toff, unsigned shift);

int NvRmStreamPushReloc(unsigned cmdbuf, unsigned off, unsigned memh,
                        unsigned toff, unsigned shift)
{
    static PushReloc_fn real_fn;
    int rc;

    real_fn = (PushReloc_fn)resolve_real("NvRmStreamPushReloc",
                                         (void **)&real_fn);
    printf("[nvrmlog] PushReloc cmdbuf=0x%x off=%u target-hmem=0x%x "
           "target-off=%u shift=%u\n",
           cmdbuf, off, memh, toff, shift);
    if (real_fn == 0) {
        printf("[nvrmlog]   UNRESOLVED -- exiting 70\n");
        exit(70);
    }
    rc = real_fn(cmdbuf, off, memh, toff, shift);
    printf("[nvrmlog]   -> rc=0x%x\n", (unsigned)rc);
    return rc;
}

int NvRmStreamPush(unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    static Any4_fn real_fn;
    int rc;

    real_fn = (Any4_fn)resolve_real("NvRmStreamPush", (void **)&real_fn);
    printf("[nvrmlog] Push a1=0x%x a2=0x%x a3=0x%x a4=0x%x\n",
           a1, a2, a3, a4);
    if (real_fn == 0) {
        printf("[nvrmlog]   UNRESOLVED -- exiting 70\n");
        exit(70);
    }
    rc = real_fn(a1, a2, a3, a4);
    printf("[nvrmlog]   -> rc=0x%x\n", (unsigned)rc);
    return rc;
}

int NvRmStreamPushSetClass(unsigned a1, unsigned a2, unsigned a3,
                           unsigned a4)
{
    static Any4_fn real_fn;
    int rc;

    real_fn = (Any4_fn)resolve_real("NvRmStreamPushSetClass",
                                    (void **)&real_fn);
    printf("[nvrmlog] PushSetClass a1=0x%x a2=0x%x a3=0x%x a4=0x%x\n",
           a1, a2, a3, a4);
    if (real_fn == 0) {
        printf("[nvrmlog]   UNRESOLVED -- exiting 70\n");
        exit(70);
    }
    rc = real_fn(a1, a2, a3, a4);
    printf("[nvrmlog]   -> rc=0x%x\n", (unsigned)rc);
    return rc;
}

/* increments may go through Push rather than a dedicated call; if this
   export does not exist it simply never fires */
int NvRmStreamPushIncr(unsigned a1, unsigned a2, unsigned a3, unsigned a4)
{
    static Any4_fn real_fn;
    int rc;

    real_fn = (Any4_fn)resolve_real("NvRmStreamPushIncr",
                                    (void **)&real_fn);
    printf("[nvrmlog] PushIncr a1=0x%x a2=0x%x a3=0x%x a4=0x%x\n",
           a1, a2, a3, a4);
    if (real_fn == 0) {
        printf("[nvrmlog]   UNRESOLVED -- exiting 70\n");
        exit(70);
    }
    rc = real_fn(a1, a2, a3, a4);
    printf("[nvrmlog]   -> rc=0x%x\n", (unsigned)rc);
    return rc;
}

/*
 * --- channel visibility: open + ioctl interposition --------------------
 * LD_PRELOAD in the STOCK mediaserver session: record the nvhost-isp
 * and nvmap fds, then on every SUBMIT ioctl print the command-buffer
 * words and relocations. Everything is bounded; anything unparsable is
 * printed as a refusal and skipped. The stock library is NOT replaced.
 */
#define NVMAP_IOC_MMAP_RAW   0xC0144E05u  /* _IOWR('N', 5, 20) */
#define SUBMIT_RAW           0xC078481Au  /* _IOWR('H', 26, 120) */
#define MAX_WORDS_DUMP       128
#define MAX_CMDBUFS_DUMP     4
#define MAX_RELOCS_DUMP      16

static int (*real_open)(const char *, int, ...);
static int (*real_ioctl)(int, unsigned long, void *);
static int real_open_ready, real_ioctl_ready;

static int find_map_by_handle(unsigned handle, unsigned *va, unsigned *len)
{
    int i;
    for (i = 0; i < map_n; i++) {
        if (map_handle[i] == handle) {
            *va = map_va[i];
            *len = map_len[i];
            return 1;
        }
    }
    return 0;
}

int open(const char *path, int flags, ...)
{
    if (real_open == 0) {
        real_open = (int (*)(const char *, int, ...))
            dlsym(((void *)0xffffffffL), "open");
        real_open_ready = 1;
    }
    {
        int fd = real_open(path, flags);
        if (path != 0) {
            if (strstr(path, "/dev/nvmap") != 0)
                nvmap_fd = fd;
            else if (strstr(path, "/dev/nvhost-isp") != 0) {
                isp_fd = fd;
                printf("[nvrmlog] isp channel open: fd=%d\n", fd);
            }
        }
        return fd;
    }
}

int ioctl(int fd, unsigned long request, void *arg)
{
    if (real_ioctl == 0) {
        real_ioctl = (int (*)(int, unsigned long, void *))
            dlsym(((void *)0xffffffffL), "ioctl");
        real_ioctl_ready = 1;
    }

    /* NVMAP_IOC_MMAP: kernel maps the handle into our space and
       returns the user VA in the struct -- record handle -> VA */
    if (fd == nvmap_fd && nvmap_fd >= 0 &&
        (request & 0xFC00FFFFu) == 0x00004E05u && arg != 0) {
        unsigned *a = (unsigned *)arg;
        int rc = real_ioctl(fd, request, arg);
        if (rc == 0 && map_n < MAX_MAPPED) {
            map_handle[map_n] = a[0];
            map_va[map_n] = a[4];
            map_len[map_n] = a[2];
            printf("[nvrmlog] nvmap map: handle=%u va=0x%x len=%u\n",
                   a[0], a[4], a[2]);
            map_n++;
        }
        return rc;
    }

    /* SUBMIT on the ISP channel: dump command-buffer words and
       relocations */
    if (fd == isp_fd && isp_fd >= 0 && request == SUBMIT_RAW &&
        arg != 0) {
        unsigned *a = (unsigned *)arg;
        unsigned num_cmdbufs = a[2];
        unsigned num_relocs = a[3];
        unsigned num_incrs = a[1];
        unsigned cmdbufs_p = a[18];  /* offset 72 per the header */
        unsigned relocs_p = a[20];   /* offset 80 per the header */
        struct nvhost_cmdbuf_view *cb =
            (struct nvhost_cmdbuf_view *)cmdbufs_p;
        struct nvhost_reloc_view *rl =
            (struct nvhost_reloc_view *)relocs_p;
        unsigned q, w, cmdbuf_words = 0;

        printf("[nvrmlog] SUBMIT: incrs=%u cmdbufs=%u relocs=%u\n",
               num_incrs, num_cmdbufs, num_relocs);
        if (num_cmdbufs > MAX_CMDBUFS_DUMP) {
            printf("[nvrmlog]   %u cmdbufs -- showing first %u\n",
                   num_cmdbufs, (unsigned)MAX_CMDBUFS_DUMP);
            num_cmdbufs = MAX_CMDBUFS_DUMP;
        }
        if (num_relocs > MAX_RELOCS_DUMP) {
            printf("[nvrmlog]   %u relocs -- showing first %u\n",
                   num_relocs, (unsigned)MAX_RELOCS_DUMP);
            num_relocs = MAX_RELOCS_DUMP;
        }
        if (cmdbufs_p == 0)
            num_cmdbufs = 0;
        if (relocs_p == 0)
            num_relocs = 0;

        for (q = 0; q < num_cmdbufs; q++) {
            unsigned handle = cb[q].mem;
            unsigned off = cb[q].offset;
            unsigned words = cb[q].words;
            unsigned va = 0, len = 0;
            unsigned dumpw;
            if (find_map_by_handle(handle, &va, &len) == 0) {
                printf("[nvrmlog]   cmdbuf[%u] handle=%u words=%u -- "
                       "no mapping recorded, content skipped\n",
                       q, handle, words);
                cmdbuf_words += words;
                continue;
            }
            if ((unsigned long)off + words * 4 > len) {
                printf("[nvrmlog]   cmdbuf[%u] exceeds mapping "
                       "(off=%u words=%u len=%u) -- clipped\n",
                       q, off, words, len);
            }
            dumpw = words;
            if (dumpw > MAX_WORDS_DUMP)
                dumpw = MAX_WORDS_DUMP;
            if ((unsigned long)off / 4 + (unsigned long)dumpw >
                len / 4)
                dumpw = len / 4 - off / 4;
            printf("[nvrmlog]   cmdbuf[%u] handle=%u off=%u words=%u "
                   "va=0x%x\n", q, handle, off, words, va);
            cmdbuf_words += words;
            for (w = 0; w < dumpw; w++) {
                unsigned word = *(const unsigned *)
                    (va + off * 4 + w * 4);
                if (w % 8 == 0)
                    printf("[nvrmlog]   +%04x:", w * 4);
                printf(" %08x", word);
                if (w % 8 == 7)
                    printf("\n");
            }
            if (dumpw % 8 != 0)
                printf("\n");
        }

        for (q = 0; q < num_relocs; q++) {
            printf("[nvrmlog]   reloc[%u] cmdbuf_off=0x%x target=%u "
                   "target_off=%u\n",
                   q, rl[q].cmdbuf_offset, rl[q].target,
                   rl[q].target_offset);
        }
        printf("[nvrmlog]   totals: cmdbuf words=%u relocs=%u incrs=%u\n",
               cmdbuf_words, num_relocs, num_incrs);
    }

    if (real_ioctl_ready == 0 && real_ioctl == 0)
        real_ioctl = (int (*)(int, unsigned long, void *))
            dlsym(((void *)0xffffffffL), "ioctl");
    return real_ioctl(fd, request, arg);
}
