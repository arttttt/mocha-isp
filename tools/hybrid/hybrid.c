/*
 * hybrid — let libnvisp_v3 bring the ISP up, then send it a frame ourselves.
 *
 * The reprocess tool does its own initialisation and its own calibration
 * submit before every frame, so putting a library bring-up in front of it
 * changes nothing: our init writes over whatever the library configured.
 * This program is the other arrangement -- the library performs the whole
 * bring-up (channel open, the fifteen-function init chain, the pipeline
 * selection through NvIspSetConfiguration), and we then submit ONLY a frame
 * gather on our own channel. No init submit, no cal submit, nothing that
 * could undo what it set.
 *
 * That is the question this answers: whether a memory frame demosaics when
 * the block has been brought up the way the camera stack brings it up.
 *
 * Second arrangement, the April one (tools/camera/isp_reprocess.c in the
 * 24.1 tree): the library also runs HwSettingsCreate twice and
 * HwSettingsApply once, which pushes its whole calibration gather. That
 * gather ends in the streaming trigger, and without a sensor feeding the
 * block the trigger waits forever -- so it is run under the preloaded
 * nvrm_shim.so, which NOPs that one word while NVRM_SHIM_STRIP is set.
 * --hw-apply selects this; it needs LD_PRELOAD=nvrm_shim.so.
 *
 * And on top of either: the blocks the stock camera sends that neither the
 * April tool nor the first hybrid did -- the demosaic (--dm, --dm-all) and
 * the colour matrix (--ccm), word for word from the captured stock session.
 *
 * Build: tools/hybrid/build-hybrid.sh (on the build server)
 * Usage: ./hybrid <raw_bayer> [--width=N] [--height=N] [--w0=N] [--yuv-cfg]
 *                 [--no-lib] [--hw-apply] [--isp=1|2] [--dm] [--dm-all]
 *                 [--ccm] [--commit] [--out-fmt=0xN] [--in-fmt=0xN]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>

/* ---- nvmap ---- */
#define NVMAP_IOC_MAGIC 'N'
struct nvmap_create_handle {
    union { uint32_t id; uint32_t size; int32_t fd; };
    uint32_t handle;
};
struct nvmap_alloc_handle {
    uint32_t handle; uint32_t heap_mask; uint32_t flags; uint32_t align;
};
struct nvmap_rw_handle {
    unsigned long addr; uint32_t handle; uint32_t offset;
    uint32_t elem_size; uint32_t hmem_stride; uint32_t user_stride; uint32_t count;
};
struct nvmap_pin_handle { uint32_t handles; unsigned long addr; uint32_t count; };

#define NVMAP_IOC_CREATE     _IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC      _IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_FREE       _IO(NVMAP_IOC_MAGIC, 4)
#define NVMAP_IOC_WRITE      _IOW(NVMAP_IOC_MAGIC, 6, struct nvmap_rw_handle)
#define NVMAP_IOC_READ       _IOW(NVMAP_IOC_MAGIC, 7, struct nvmap_rw_handle)
#define NVMAP_IOC_PIN_MULT   _IOWR(NVMAP_IOC_MAGIC, 10, struct nvmap_pin_handle)
#define NVMAP_IOC_UNPIN_MULT _IOW(NVMAP_IOC_MAGIC, 11, struct nvmap_pin_handle)
#define NVMAP_HEAP_IOVMM     (1 << 30)
#define NVMAP_HANDLE_WRITE_COMBINE 2

/* ---- nvhost ---- */
#define NVHOST_IOCTL_MAGIC 'H'
struct nvhost_get_param_arg { uint32_t param; uint32_t value; };
struct nvhost_syncpt_incr { uint32_t syncpt_id; uint32_t syncpt_incrs; };
struct nvhost_cmdbuf { uint32_t mem; uint32_t offset; uint32_t words; };
struct nvhost_reloc { uint32_t cmdbuf_mem; uint32_t cmdbuf_offset;
                      uint32_t target; uint32_t target_offset; };
struct nvhost_reloc_shift { uint32_t shift; };
struct nvhost_fence { uint32_t syncpt_id; uint32_t value; };
struct nvhost_ctrl_syncpt_waitex_args { uint32_t id; uint32_t thresh;
                                        int32_t timeout; uint32_t value; };
struct nvhost32_submit_args {
    uint32_t submit_version; uint32_t num_syncpt_incrs; uint32_t num_cmdbufs;
    uint32_t num_relocs; uint32_t num_waitchks; uint32_t timeout;
    uint32_t syncpt_incrs; uint32_t cmdbufs; uint32_t relocs;
    uint32_t reloc_shifts; uint32_t waitchks; uint32_t waitbases;
    uint32_t class_ids; uint32_t pad[2]; uint32_t fences; uint32_t fence;
} __attribute__((packed));

#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT _IOWR(NVHOST_IOCTL_MAGIC, 16, struct nvhost_get_param_arg)
#define NVHOST32_IOCTL_CHANNEL_SUBMIT      _IOWR(NVHOST_IOCTL_MAGIC, 15, struct nvhost32_submit_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_WAITEX    _IOWR(NVHOST_IOCTL_MAGIC, 6, struct nvhost_ctrl_syncpt_waitex_args)

/* ---- host1x opcodes ---- */
#define OP_SETCLASS(c,o,m) ((0<<28)|((o)<<16)|((c)<<6)|(m))
#define OP_INCR(o,n)       ((1<<28)|((o)<<16)|(n))
#define OP_NONINCR(o,n)    ((2<<28)|((o)<<16)|(n))
#define ISP_CLASS_A 0x32
#define ISP_CLASS_B 0x34

/* The demosaic coefficients, word for word from the stock capture. */
#include "isp_demosaic.h"

/* The rest of the demosaic family, from the same capture (tools/viisp/
 * isp_stock.h carries the full set; only these are needed here). */
static const uint32_t dm_909[7] = {
    0x00000001, 0xfc000f00, 0xf680f320, 0x0d80fde0, 0x00000030, 0x1400002a,
    0x3c00002b,
};
static const uint32_t dm_910[9] = {
    0x00000003, 0x00000028, 0x01480029, 0x0003030b, 0x00990030, 0x00000800,
    0x007b0666, 0x00000036, 0x00001f1f,
};
static const uint32_t dm_919[10] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00000200,
};
static const uint32_t dm_91b[10] = {
    0, 0, 0, 0, 0, 0x00000001, 0x00000025, 0, 0x00000026, 0x00000361,
};
static const uint32_t dm_91d[10] = {
    0, 0, 0, 0, 0, 0, 0x00000780, 0, 0x00000780, 0x00000200,
};
static const uint32_t dm_91f[1] = { 0x00000032 };
static const uint32_t dm_920[10] = {
    0x00000002, 0x10001660, 0x00000000, 0x1000f4a0, 0x0000fa80, 0x10000000,
    0x00001c50, 0x30001000, 0x30001000, 0x30001000,
};

/* The colour matrix the stock camera sends at the end of its real pass.
 * Zeroing its middle pair on the live camera turns the picture black, so
 * this is the block that builds the luminance. */
static const uint32_t ccm_600[16] = {
    0x00000005, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xf9500800,
    0x0000fec0, 0x096004c0, 0x000001d0, 0xfac0fd50, 0x00000800, 0x00000000,
    0x3fff0000, 0x3fff0000, 0x3fff0000, 0x10001000,
};

static int nvmap_fd = -1;
static unsigned W = 3264, H = 2448;
static uint32_t isp_class = ISP_CLASS_A;

/* What the library opened, so it can close it itself on every way out.
 * Leaving the process without NvIspClose leaves the channel to the kernel's
 * teardown, and a block left waiting takes the device down with it. */
static void *g_isp_lib, *g_hIsp, *g_hw0, *g_hw1;

static void lib_tear_down(void)
{
    if (!g_isp_lib || !g_hIsp) return;
    void (*HwDestroy)(void *) = dlsym(g_isp_lib, "NvIspHwSettingsDestroy");
    void (*Close)(void *) = dlsym(g_isp_lib, "NvIspClose");
    if (HwDestroy) {
        if (g_hw1) HwDestroy(g_hw1);
        if (g_hw0) HwDestroy(g_hw0);
    }
    g_hw0 = g_hw1 = 0;
    if (Close) Close(g_hIsp);
    printf("library closed its channel (NvIspClose)\n");
    g_hIsp = 0;
}

#define SHIM_STATUS "/data/local/tmp/nvrm_shim_status.txt"

static uint32_t nvmap_create(uint32_t size) {
    struct nvmap_create_handle ch = { .size = size };
    if (ioctl(nvmap_fd, NVMAP_IOC_CREATE, &ch) < 0) { perror("nvmap create"); return 0; }
    return ch.handle;
}
static int nvmap_alloc(uint32_t h) {
    struct nvmap_alloc_handle ah = { .handle = h, .heap_mask = NVMAP_HEAP_IOVMM,
        .flags = NVMAP_HANDLE_WRITE_COMBINE, .align = 4096 };
    if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah) < 0) { perror("nvmap alloc"); return -1; }
    return 0;
}
static int nvmap_write(uint32_t h, uint32_t off, const void *d, uint32_t n) {
    struct nvmap_rw_handle rw = { .addr = (unsigned long)d, .handle = h, .offset = off,
        .elem_size = n, .hmem_stride = n, .user_stride = n, .count = 1 };
    return ioctl(nvmap_fd, NVMAP_IOC_WRITE, &rw);
}
static int nvmap_read(uint32_t h, uint32_t off, void *d, uint32_t n) {
    struct nvmap_rw_handle rw = { .addr = (unsigned long)d, .handle = h, .offset = off,
        .elem_size = n, .hmem_stride = n, .user_stride = n, .count = 1 };
    return ioctl(nvmap_fd, NVMAP_IOC_READ, &rw);
}
static uint32_t nvmap_pin(uint32_t h) {
    struct nvmap_pin_handle ph = { .handles = h, .addr = 0, .count = 1 };
    if (ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &ph) < 0) { perror("nvmap pin"); return 0; }
    return (uint32_t)ph.addr;
}
static void nvmap_unpin(uint32_t h) {
    struct nvmap_pin_handle ph = { .handles = h, .addr = 0, .count = 1 };
    ioctl(nvmap_fd, NVMAP_IOC_UNPIN_MULT, &ph);
}
static void nvmap_free(uint32_t h) { ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)h); }

/* The selector sets the camera stack sends, read out of libnvmm. The sensor
 * pipeline fills twelve of them; the YUV one fills four and skips mode 2. */
static const uint32_t CFG_PRIMARY[16] = {
    1, 7, 9, 0xa, 3, 0, 6, 8, 0x11, 0xf, 0xc, 0xe, 0xb, 0, 0x10, 0xd
};
static const uint32_t CFG_YUV[16] = {
    1, 0, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0xd
};

/* Whether the trigger-stripping shim is in this process. Without it,
 * HwSettingsApply's gather ends in a streaming trigger that waits for
 * sensor pixels which never come, and the channel times out. */
static int shim_loaded(void)
{
    FILE *m = fopen("/proc/self/maps", "r");
    char line[512];
    int found = 0;
    if (!m) return 0;
    while (fgets(line, sizeof line, m))
        if (strstr(line, "nvrm_shim.so")) { found = 1; break; }
    fclose(m);
    return found;
}

static void *lib_bring_up(uint32_t w0, int yuv_cfg, int inst, int hw_apply)
{
    void *rm = dlopen("libnvrm.so", RTLD_NOW);
    void *isp = dlopen("libnvisp_v3.so", RTLD_NOW);
    if (!rm || !isp) { printf("dlopen: %s\n", dlerror()); return 0; }
    g_isp_lib = isp;

    int (*NvRmOpen)(void **, uint32_t) = dlsym(rm, "NvRmOpen");
    int (*NvIspOpen)(void *, int, void **) = dlsym(isp, "NvIspOpen");
    int (*NvIspSetConfiguration)(void *, int, void *, unsigned *) =
        dlsym(isp, "NvIspSetConfiguration");
    if (!NvRmOpen || !NvIspOpen || !NvIspSetConfiguration) {
        printf("missing symbols\n"); return 0;
    }

    void *hRm = 0;
    int rc = NvRmOpen(&hRm, 0);
    printf("NvRmOpen: rc=%d handle=%p\n", rc, hRm);
    if (!hRm) return 0;

    void *hIsp = 0;
    rc = NvIspOpen(hRm, inst, &hIsp);         /* 1 = ISP-A, 2 = ISP-B */
    printf("NvIspOpen(%d): rc=%d handle=%p\n", inst, rc, hIsp);
    if (!hIsp) return 0;
    g_hIsp = hIsp;

    uint32_t cfg1[16];
    memcpy(cfg1, yuv_cfg ? CFG_YUV : CFG_PRIMARY, sizeof cfg1);
    cfg1[0] = w0;
    unsigned sz = sizeof cfg1;
    rc = NvIspSetConfiguration(hIsp, 1, cfg1, &sz);
    printf("SetConfiguration mode=1 (%s, w0=%u): rc=%d\n",
           yuv_cfg ? "YUV" : "primary", w0, rc);

    if (!yuv_cfg) {
        uint32_t cfg2 = 2;
        sz = sizeof cfg2;
        rc = NvIspSetConfiguration(hIsp, 2, &cfg2, &sz);
        printf("SetConfiguration mode=2: rc=%d\n", rc);
    }

    /* The April arrangement: two settings objects, as the stock stack
     * creates them, and one Apply -- the library's own calibration gather
     * goes down its own channel here. */
    if (hw_apply) {
        int (*HwCreate)(void *, void **) = dlsym(isp, "NvIspHwSettingsCreate");
        int (*HwApply)(void *) = dlsym(isp, "NvIspHwSettingsApply");
        if (!HwCreate || !HwApply) { printf("missing HwSettings symbols\n"); return 0; }
        if (!shim_loaded()) {
            printf("nvrm_shim.so is not preloaded -- HwSettingsApply would "
                   "leave a streaming trigger waiting forever; stopping\n");
            lib_tear_down();
            return 0;
        }
        rc = HwCreate(hIsp, &g_hw0);
        printf("HwSettingsCreate[0]: rc=%d settings=%p\n", rc, g_hw0);
        rc = HwCreate(hIsp, &g_hw1);
        printf("HwSettingsCreate[1]: rc=%d settings=%p\n", rc, g_hw1);
        if (!g_hw0) { lib_tear_down(); return 0; }

        /* The shim reports what it did through a file; a stale one from an
         * earlier run must not be mistaken for this one. */
        unlink(SHIM_STATUS);
        setenv("NVRM_SHIM_STRIP", "1", 1);
        rc = HwApply(g_hw0);
        unsetenv("NVRM_SHIM_STRIP");

        int submits = 0, scanned = 0, stripped = 0, failed = 0;
        char trig[256] = "?";
        FILE *st = fopen(SHIM_STATUS, "r");
        if (st) {
            if (fscanf(st, "submits=%d scanned=%d stripped=%d failed=%d triggers=%255s",
                       &submits, &scanned, &stripped, &failed, trig) < 4)
                submits = -1;
            fclose(st);
        } else {
            submits = -1;
        }
        printf("HwSettingsApply: rc=%d; shim: %d submits, %d gathers scanned, "
               "ISP_CONTROL values %s, %d streaming triggers stripped, %d refused\n",
               rc, submits, scanned, trig, stripped, failed);

        /* Not proven safe means not continued. Proven means: the shim saw
         * every submit, read every gather, and refused nothing -- so any
         * streaming trigger there is gone. (On this kernel the library
         * ends its gathers with 0x0F, the apply, and there is nothing to
         * strip; what took the device down before was leaving without
         * NvIspClose.) Close it while the library still can. */
        if (rc != 0 || submits <= 0 || scanned <= 0 || failed) {
            printf("Apply was not seen through by the shim -- closing, not continuing\n");
            lib_tear_down();
            return 0;
        }
    }
    return hIsp;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: %s <raw_bayer> [--width=N] [--height=N] [--w0=N] "
               "[--yuv-cfg] [--no-lib] [--out-fmt=0xN] [--in-fmt=0xN]\n",
               argv[0]);
        return 1;
    }
    uint32_t w0 = 1, out_fmt = 0x43, in_fmt = 0x10200024;
    int yuv_cfg = 0, use_lib = 1, dma_init = 1, hw_apply = 0, inst = 1;
    int dm = 0, dm_all = 0, ccm = 0, commit = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--width=", 8) == 0)       W = (unsigned)strtoul(a + 8, 0, 0);
        else if (strncmp(a, "--height=", 9) == 0) H = (unsigned)strtoul(a + 9, 0, 0);
        else if (strncmp(a, "--w0=", 5) == 0)     w0 = (uint32_t)strtoul(a + 5, 0, 0);
        else if (strcmp(a, "--yuv-cfg") == 0)     yuv_cfg = 1;
        else if (strcmp(a, "--no-lib") == 0)      use_lib = 0;
        else if (strcmp(a, "--no-dma") == 0)      dma_init = 0;
        else if (strcmp(a, "--hw-apply") == 0)    hw_apply = 1;
        else if (strncmp(a, "--isp=", 6) == 0)    inst = atoi(a + 6);
        else if (strcmp(a, "--dm") == 0)          dm = 1;
        else if (strcmp(a, "--dm-all") == 0)      dm = dm_all = 1;
        else if (strcmp(a, "--ccm") == 0)         ccm = 1;
        else if (strcmp(a, "--commit") == 0)      commit = 1;
        else if (strncmp(a, "--out-fmt=", 10) == 0) out_fmt = strtoul(a + 10, 0, 16);
        else if (strncmp(a, "--in-fmt=", 9) == 0)   in_fmt = strtoul(a + 9, 0, 16);
    }
    if (inst != 1 && inst != 2) { printf("--isp must be 1 or 2\n"); return 1; }
    isp_class = inst == 2 ? ISP_CLASS_B : ISP_CLASS_A;
    const char *isp_node = inst == 2 ? "/dev/nvhost-isp.1" : "/dev/nvhost-isp";

    printf("=== hybrid: %s bring-up%s, our frame only ===\n",
           use_lib ? "library" : "no", hw_apply ? " + HwSettingsApply" : "");
    printf("ISP-%c class 0x%02x, geometry %ux%u, in 0x%08x, out 0x%08x, "
           "demosaic %s, matrix %s\n",
           inst == 2 ? 'B' : 'A', isp_class, W, H, in_fmt, out_fmt,
           dm_all ? "all blocks" : dm ? "coefficients" : "off",
           ccm ? "on" : "off");

    if (use_lib && !lib_bring_up(w0, yuv_cfg, inst, hw_apply)) {
        printf("library bring-up failed -- stopping\n");
        return 1;
    }

    nvmap_fd = open("/dev/nvmap", O_RDWR | O_SYNC);
    int ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);

    /* The channel node is exclusive: once the library has opened it our own
     * open fails. But it opened it inside THIS process, so the descriptor is
     * in our own table -- find it and submit through that. The library's
     * context and its fence machinery stay live, which is the point. */
    int isp_fd = open(isp_node, O_RDWR);
    if (isp_fd < 0) {
        for (int fd = 0; fd < 256 && isp_fd < 0; fd++) {
            char p[64], t[128];
            snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
            ssize_t k = readlink(p, t, sizeof t - 1);
            if (k <= 0) continue;
            t[k] = 0;
            if (strcmp(t, isp_node) == 0) {
                isp_fd = fd;
                printf("channel is the library's: reusing fd %d\n", fd);
            }
        }
    }
    if (nvmap_fd < 0 || isp_fd < 0 || ctrl_fd < 0) {
        printf("open failed: nvmap=%d isp=%d ctrl=%d\n", nvmap_fd, isp_fd, ctrl_fd);
        lib_tear_down();
        return 1;
    }

    struct nvhost_get_param_arg g;
    g.param = 0; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &g);
    uint32_t sp_memory = g.value;
    g.param = 1; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &g);
    uint32_t sp_stats = g.value;
    g.param = 2; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &g);
    uint32_t sp_stream = g.value;
    printf("syncpts: memory=%u (ours), %u and %u belong to the hardware and the library\n",
           sp_memory, sp_stats, sp_stream);

    uint32_t in_size = W * H * 2, out_size = W * 4 * H;
    uint32_t in_h = nvmap_create(in_size), out_h = nvmap_create(out_size);
    uint32_t cmd_h = nvmap_create(4096), work_h = nvmap_create(512 * 1024);
    if (!in_h || !out_h || !cmd_h || !work_h) {
        printf("alloc failed\n");
        lib_tear_down();
        return 1;
    }
    nvmap_alloc(in_h); nvmap_alloc(out_h); nvmap_alloc(cmd_h); nvmap_alloc(work_h);
    nvmap_pin(in_h); nvmap_pin(out_h); nvmap_pin(work_h);

    /* Load the frame, zero the output so anything present afterwards is the
     * hardware's doing. */
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open raw"); lib_tear_down(); return 1; }
    uint8_t *buf = calloc(1, in_size);
    size_t got = fread(buf, 1, in_size, f);
    fclose(f);
    printf("loaded %zu of %u bytes\n", got, in_size);
    const uint32_t chunk = 65536;
    for (uint32_t o = 0; o < in_size; o += chunk)
        nvmap_write(in_h, o, buf + o, in_size - o < chunk ? in_size - o : chunk);
    memset(buf, 0, chunk);
    for (uint32_t o = 0; o < out_size; o += chunk)
        nvmap_write(out_h, o, buf, out_size - o < chunk ? out_size - o : chunk);
    free(buf);

    /* The frame, and nothing else -- plus, on request, the blocks the
     * stock camera sends before its frames and we never had. */
    uint32_t cmd[512];
    int n = 0, y_reloc, in_reloc, work_reloc;
    cmd[n++] = OP_SETCLASS(isp_class, 0, 0);

    /* The hybrid proper: the library selected the pipeline, but nothing in
     * its bring-up configures the memory output path, so the first attempt
     * submitted cleanly and wrote nothing. These are the DMA thresholds our
     * own init sends -- output burst, input burst, mode -- and they are the
     * part that has to come from us. --no-dma leaves them out. */
    if (dma_init) {
        cmd[n++] = OP_INCR(0x019, 1); cmd[n++] = 0x00000400;
        cmd[n++] = OP_INCR(0x01B, 2); cmd[n++] = 0x00000200;
                                      cmd[n++] = 0x00000002;
    }

    /* The demosaic: enable, then the two coefficient sets with their shift
     * words. The two-part blocks are the stock's mode-1 shape -- one INCR
     * for the head word, one NONINCR for the table behind it. */
    if (dm) {
        cmd[n++] = OP_INCR(0x900, 2); cmd[n++] = 1; cmd[n++] = 1;
        cmd[n++] = OP_INCR(0x902, 1); cmd[n++] = isp_dm_902[0];
        cmd[n++] = OP_NONINCR(0x903, 64);
        for (int i = 0; i < 64; i++) cmd[n++] = isp_dm_903[i];
        cmd[n++] = OP_INCR(0x904, 2); cmd[n++] = isp_dm_904[0]; cmd[n++] = isp_dm_904[1];
        cmd[n++] = OP_INCR(0x906, 1); cmd[n++] = isp_dm_906[0];
        cmd[n++] = OP_NONINCR(0x907, 36);
        for (int i = 0; i < 36; i++) cmd[n++] = isp_dm_907[i];
        cmd[n++] = OP_INCR(0x908, 1); cmd[n++] = isp_dm_908[0];
    }
    if (dm_all) {
        cmd[n++] = OP_INCR(0x909, 7);
        for (int i = 0; i < 7; i++) cmd[n++] = dm_909[i];
        cmd[n++] = OP_INCR(0x910, 9);
        for (int i = 0; i < 9; i++) cmd[n++] = dm_910[i];
        cmd[n++] = OP_INCR(0x919, 1); cmd[n++] = dm_919[0];
        cmd[n++] = OP_NONINCR(0x91a, 9);
        for (int i = 1; i < 10; i++) cmd[n++] = dm_919[i];
        cmd[n++] = OP_INCR(0x91b, 1); cmd[n++] = dm_91b[0];
        cmd[n++] = OP_NONINCR(0x91c, 9);
        for (int i = 1; i < 10; i++) cmd[n++] = dm_91b[i];
        cmd[n++] = OP_INCR(0x91d, 1); cmd[n++] = dm_91d[0];
        cmd[n++] = OP_NONINCR(0x91e, 9);
        for (int i = 1; i < 10; i++) cmd[n++] = dm_91d[i];
        cmd[n++] = OP_INCR(0x91f, 1); cmd[n++] = dm_91f[0];
        cmd[n++] = OP_INCR(0x920, 10);
        for (int i = 0; i < 10; i++) cmd[n++] = dm_920[i];
    }
    if (ccm) {
        cmd[n++] = OP_INCR(0x600, 16);
        for (int i = 0; i < 16; i++) cmd[n++] = ccm_600[i];
    }

    cmd[n++] = OP_INCR(0x053, 2);
    work_reloc = n;
    cmd[n++] = 1;
    cmd[n++] = 0;

    /* What the library does right after its own configuration and work
     * pointer: ISP_CONTROL = 0x0F, the apply. Every one of its eight
     * configuration rounds ends with it. If the block only takes new
     * settings at this point, then everything we wrote above sat in the
     * shadow and never reached the pipeline -- which would be why a
     * gather full of demosaic blocks changed nothing. */
    if (commit) {
        cmd[n++] = OP_NONINCR(0x00C, 1); cmd[n++] = 0x0F;
    }

    cmd[n++] = OP_INCR(0xE00, 1); cmd[n++] = ((W - 1) & 0x3FFF) << 16;
    cmd[n++] = OP_INCR(0xE01, 1); cmd[n++] = ((H - 1) & 0x3FFF) << 16;
    cmd[n++] = OP_INCR(0xE02, 1); cmd[n++] = out_fmt;
    cmd[n++] = OP_INCR(0xE03, 1); cmd[n++] = 0;
    cmd[n++] = OP_INCR(0xE04, 3);
    y_reloc = n;
    cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = W * 4;
    cmd[n++] = OP_INCR(0xE31, 1); cmd[n++] = (H << 16) | W;
    cmd[n++] = OP_INCR(0xE33, 1); cmd[n++] = in_fmt;
    cmd[n++] = OP_INCR(0xE34, 3);
    in_reloc = n;
    cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = W * 2;
    cmd[n++] = OP_INCR(0xE32, 1); cmd[n++] = W & 0x3FFF;
    cmd[n++] = OP_INCR(0xE30, 1); cmd[n++] = 1;
    cmd[n++] = OP_INCR(0x015, 1); cmd[n++] = 0x00000007;
    cmd[n++] = OP_SETCLASS(isp_class, 0, 0);
    /* One conditional increment, on the memory syncpoint, and it is the one
     * the submit declares. The other two the ISP-B channel hands out --
     * 37 (ispb_stats) and 38 (ispb_stream) -- are raised by the hardware on
     * its own events and are the library's own sequencing. Arming them here
     * as well, undeclared, left 38 one ahead of what the kernel believed
     * (min 29, max 28 in the dump of 2026-09-02), and the library's next
     * bring-up waited on it forever and took the channel down. */
    cmd[n++] = OP_NONINCR(0x000, 1); cmd[n++] = (4 << 8) | sp_memory;
    cmd[n++] = OP_NONINCR(0x00C, 1); cmd[n++] = 0x0B;
    printf("gather: %d words\n", n);
    nvmap_write(cmd_h, 0, cmd, n * 4);

    struct nvhost_reloc relocs[4];
    struct nvhost_reloc_shift shifts[4];
    int nr = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, (uint32_t)(work_reloc + 1) * 4, work_h, 0 };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, (uint32_t)y_reloc * 4, out_h, 0 };
    shifts[nr++].shift = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, (uint32_t)in_reloc * 4, in_h, 0 };
    shifts[nr++].shift = 0;

    struct nvhost_cmdbuf cb = { cmd_h, 0, (uint32_t)n };
    struct nvhost_syncpt_incr si = { sp_memory, 1 };
    uint32_t cls = isp_class;
    struct nvhost_fence fence = { 0, 0 };
    struct nvhost32_submit_args sa;
    memset(&sa, 0, sizeof sa);
    sa.num_syncpt_incrs = 1;
    sa.num_cmdbufs = 1;
    sa.num_relocs = (uint32_t)nr;
    sa.timeout = 5000;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.relocs = (uint32_t)(uintptr_t)relocs;
    sa.reloc_shifts = (uint32_t)(uintptr_t)shifts;
    sa.class_ids = (uint32_t)(uintptr_t)&cls;
    sa.fences = (uint32_t)(uintptr_t)&fence;

    if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa) < 0) {
        perror("submit");
        lib_tear_down();
        return 1;
    }
    printf("submitted, fence=%u\n", sa.fence);
    struct nvhost_ctrl_syncpt_waitex_args wa = { sp_memory, sa.fence, 5000, 0 };
    if (ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &wa) < 0)
        printf("TIMEOUT waiting for syncpt %u\n", sp_memory);
    else
        printf("done, syncpt value %u\n", wa.value);

    /* Read back: the first bytes, and each channel separately -- with a
     * synthetic frame the red-lit and blue-lit bands must differ if the
     * mosaic is being demosaiced rather than averaged. */
    {
        uint32_t scan = out_size < (4u << 20) ? out_size : (4u << 20);
        uint8_t *rb = malloc(chunk);
        uint32_t lo[4] = {255,255,255,255}, hi[4] = {0,0,0,0}, cnt[4] = {0,0,0,0};
        uint64_t sum[4] = {0,0,0,0};
        uint8_t head[16];
        nvmap_read(out_h, 0, head, 16);
        printf("first bytes:");
        for (int i = 0; i < 16; i++) printf(" %02x", head[i]);
        printf("\n");
        for (uint32_t o = 0; o < scan; o += chunk) {
            uint32_t part = scan - o < chunk ? scan - o : chunk;
            if (nvmap_read(out_h, o, rb, part) < 0) break;
            for (uint32_t i = 0; i < part; i++) {
                int c = (int)((o + i) & 3);
                uint8_t v = rb[i];
                if (v < lo[c]) lo[c] = v;
                if (v > hi[c]) hi[c] = v;
                sum[c] += v; cnt[c]++;
            }
        }
        static const char *cn[4] = { "byte0", "byte1", "byte2", "byte3" };
        for (int c = 0; c < 4; c++)
            printf("  %s min=%3u max=%3u mean=%6.1f\n", cn[c], lo[c], hi[c],
                   cnt[c] ? (double)sum[c] / cnt[c] : 0.0);
        free(rb);
    }

    /* Dump so the bands can be compared on the host. */
    {
        char path[128];
        snprintf(path, sizeof path, "/data/local/tmp/hybrid_%08x_%ux%u.bin",
                 out_fmt, W, H);
        FILE *fp = fopen(path, "wb");
        if (fp) {
            uint8_t *rb = malloc(chunk);
            for (uint32_t o = 0; o < out_size; o += chunk) {
                uint32_t part = out_size - o < chunk ? out_size - o : chunk;
                nvmap_read(out_h, o, rb, part);
                fwrite(rb, 1, part, fp);
            }
            free(rb);
            fclose(fp);
            printf("saved %s (%u bytes)\n", path, out_size);
        }
    }

    nvmap_unpin(work_h); nvmap_unpin(out_h); nvmap_unpin(in_h);
    nvmap_free(cmd_h); nvmap_free(work_h); nvmap_free(out_h); nvmap_free(in_h);
    close(ctrl_fd); close(nvmap_fd);
    /* The channel descriptor is the library's: it closes it, not us. */
    lib_tear_down();
    printf("=== done ===\n");
    return 0;
}
