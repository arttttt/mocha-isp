/*
 * ISP reprocess test — pure kernel path, NO blobs
 *
 * Opens ISP channel via NvRmChannelOpen (from libnvrm.so) for proper
 * HW init, but builds and submits gathers ourselves — no libnvisp_v3.so.
 *
 * This isolates the channel init (which we can't reproduce yet) from
 * the gather building (which we understand fully).
 *
 * Input: 2592x1944 BG10 (10-bit Bayer BGGR, 16-bit LE containers)
 * Output: RGBA 2592x1944 (32bpp)
 *
 * Build:
 *   $CC --sysroot=$SYSROOT -std=gnu99 -pie -o isp_reprocess_pure isp_reprocess_pure.c -ldl
 *
 * Usage:
 *   LD_LIBRARY_PATH=/system/vendor/lib ./isp_reprocess_pure /data/local/tmp/front_raw.raw
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/ioctl.h>

/* ---- nvmap ioctls ---- */
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
struct nvmap_pin_handle {
    uint32_t handles; unsigned long addr; uint32_t count;
};

#define NVMAP_IOC_CREATE   _IOWR(NVMAP_IOC_MAGIC, 0, struct nvmap_create_handle)
#define NVMAP_IOC_ALLOC    _IOW(NVMAP_IOC_MAGIC, 3, struct nvmap_alloc_handle)
#define NVMAP_IOC_FREE     _IO(NVMAP_IOC_MAGIC, 4)
#define NVMAP_IOC_WRITE    _IOW(NVMAP_IOC_MAGIC, 6, struct nvmap_rw_handle)
#define NVMAP_IOC_READ     _IOW(NVMAP_IOC_MAGIC, 7, struct nvmap_rw_handle)
#define NVMAP_IOC_PIN_MULT _IOWR(NVMAP_IOC_MAGIC, 10, struct nvmap_pin_handle)
#define NVMAP_IOC_UNPIN_MULT _IOW(NVMAP_IOC_MAGIC, 11, struct nvmap_pin_handle)
#define NVMAP_HEAP_IOVMM   (1 << 30)
#define NVMAP_HANDLE_WRITE_COMBINE 2

/* ---- nvhost ioctls ---- */
#define NVHOST_IOCTL_MAGIC 'H'

struct nvhost_set_nvmap_fd_args { uint32_t fd; };
struct nvhost_get_param_arg { uint32_t param; uint32_t value; };
struct nvhost_syncpt_incr { uint32_t syncpt_id; uint32_t syncpt_incrs; };
struct nvhost_cmdbuf { uint32_t mem; uint32_t offset; uint32_t words; };
struct nvhost_reloc { uint32_t cmdbuf_mem; uint32_t cmdbuf_offset; uint32_t target; uint32_t target_offset; };
struct nvhost_reloc_shift { uint32_t shift; };
struct nvhost_fence { uint32_t syncpt_id; uint32_t value; };
struct nvhost_ctrl_syncpt_waitex_args { uint32_t id; uint32_t thresh; int32_t timeout; uint32_t value; };

struct nvhost32_submit_args {
    uint32_t submit_version; uint32_t num_syncpt_incrs; uint32_t num_cmdbufs;
    uint32_t num_relocs; uint32_t num_waitchks; uint32_t timeout;
    uint32_t syncpt_incrs; uint32_t cmdbufs; uint32_t relocs;
    uint32_t reloc_shifts; uint32_t waitchks; uint32_t waitbases;
    uint32_t class_ids; uint32_t pad[2]; uint32_t fences; uint32_t fence;
} __attribute__((packed));

#define NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD _IOW(NVHOST_IOCTL_MAGIC, 5, struct nvhost_set_nvmap_fd_args)
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT _IOWR(NVHOST_IOCTL_MAGIC, 16, struct nvhost_get_param_arg)
#define NVHOST32_IOCTL_CHANNEL_SUBMIT _IOWR(NVHOST_IOCTL_MAGIC, 15, struct nvhost32_submit_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_WAITEX _IOWR(NVHOST_IOCTL_MAGIC, 6, struct nvhost_ctrl_syncpt_waitex_args)

/* Clock/power control */
struct nvhost_clk_rate_args { uint32_t rate; uint32_t moduleid; };
#define NVHOST_IOCTL_CHANNEL_SET_CLK_RATE _IOW(NVHOST_IOCTL_MAGIC, 10, struct nvhost_clk_rate_args)

/* PIO register read/write (via ctrl node) */
struct nvhost32_ctrl_module_regrdwr_args {
    uint32_t id;
    uint32_t num_offsets;
    uint32_t block_size;
    uint32_t offsets;
    uint32_t values;
    uint32_t write;
};
/* NR=14 for channel fd, NR=5 for ctrl fd */
#define NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR _IOWR(NVHOST_IOCTL_MAGIC, 14, struct nvhost32_ctrl_module_regrdwr_args)

/* Host1X opcodes */
#define OP_SETCLASS(c,o,m) ((0<<28)|((o)<<16)|((c)<<6)|(m))
#define OP_INCR(o,n)       ((1<<28)|((o)<<16)|(n))
#define OP_NONINCR(o,n)    ((2<<28)|((o)<<16)|(n))
#define ISP_CLASS_A 0x32
#define ISP_CLASS_B 0x34
static int isp_class = ISP_CLASS_B;  /* default ISP-B */
#define ISP_CLASS isp_class

/* Frame params: W/H are runtime (--width/--height), defaults 2592x1944.
   No compile-time geometry: every derived value is computed in main. */
#define BPP 2
static unsigned W = 2592, H = 1944;

static int nvmap_fd = -1;

static uint32_t nvmap_create(uint32_t size) {
    struct nvmap_create_handle ch = { .size = size };
    if (ioctl(nvmap_fd, NVMAP_IOC_CREATE, &ch) < 0) { perror("nvmap create"); return 0; }
    return ch.handle;
}
static int nvmap_alloc(uint32_t handle) {
    struct nvmap_alloc_handle ah = { .handle = handle, .heap_mask = NVMAP_HEAP_IOVMM,
        .flags = NVMAP_HANDLE_WRITE_COMBINE, .align = 4096 };
    if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah) < 0) { perror("nvmap alloc"); return -1; }
    return 0;
}

/* Alloc with blocklinear kind (0xFE) for ISP output */
struct nvmap_alloc_kind_handle {
    uint32_t handle; uint32_t heap_mask; uint32_t flags; uint32_t align;
    uint8_t kind; uint8_t comp_tags;
};
#define NVMAP_IOC_ALLOC_KIND _IOW('N', 100, struct nvmap_alloc_kind_handle)

static int nvmap_alloc_kind(uint32_t handle, uint8_t kind) {
    struct nvmap_alloc_kind_handle ah = { .handle = handle, .heap_mask = NVMAP_HEAP_IOVMM,
        .flags = NVMAP_HANDLE_WRITE_COMBINE, .align = 4096, .kind = kind, .comp_tags = 0 };
    if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC_KIND, &ah) < 0) { perror("nvmap alloc_kind"); return -1; }
    return 0;
}
static int nvmap_write(uint32_t handle, uint32_t offset, const void *data, uint32_t size) {
    struct nvmap_rw_handle rw = { .addr = (unsigned long)data, .handle = handle,
        .offset = offset, .elem_size = size, .hmem_stride = size, .user_stride = size, .count = 1 };
    if (ioctl(nvmap_fd, NVMAP_IOC_WRITE, &rw) < 0) { perror("nvmap write"); return -1; }
    return 0;
}
static int nvmap_read(uint32_t handle, uint32_t offset, void *data, uint32_t size) {
    struct nvmap_rw_handle rw = { .addr = (unsigned long)data, .handle = handle,
        .offset = offset, .elem_size = size, .hmem_stride = size, .user_stride = size, .count = 1 };
    if (ioctl(nvmap_fd, NVMAP_IOC_READ, &rw) < 0) { perror("nvmap read"); return -1; }
    return 0;
}
/* Every run pinned tens of megabytes of IOVMM and released none of it,
 * which is why the device only survived a couple of runs before the whole
 * system went sluggish. Unpin and free what we pinned. */
static void nvmap_unpin(uint32_t handle) {
    struct nvmap_pin_handle ph = { .handles = handle, .addr = 0, .count = 1 };
    ioctl(nvmap_fd, NVMAP_IOC_UNPIN_MULT, &ph);
}
static void nvmap_free(uint32_t handle) {
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)handle);
}

static uint32_t nvmap_pin(uint32_t handle) {
    struct nvmap_pin_handle ph = { .handles = handle, .addr = 0, .count = 1 };
    if (ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &ph) < 0) { perror("nvmap pin"); return 0; }
    return (uint32_t)ph.addr;
}

/* ---- Output surface layout -------------------------------------------
 * The number of planes is a property of the FORMAT, not a flag: the ISP
 * takes one address triplet per plane (E04/E07/E0A = plane 1/2/3), so we
 * push exactly as many as the format has.
 *   0x43 RGBA    -- 1 plane,  interleaved, stride W*4
 *   0xE7 NV12    -- 2 planes, Y + interleaved UV (UV row is W bytes)
 *   0xE6 YUV420P -- 3 planes, Y + U + V (chroma row is W/2 bytes)
 * Planes are packed in that order, each one page-aligned.
 */
#define ALIGN_UP(v,a) ((((uint32_t)(v)) + (a) - 1u) & ~((a) - 1u))

struct out_layout {
    uint32_t fmt;            /* full E02 word */
    int      planes;         /* triplets we push */
    uint32_t stride[3];
    uint32_t offset[3];
    uint32_t size[3];
    uint32_t total;          /* bytes the output buffer must hold */
    int      blocklinear;    /* nvmap kind 0xFE */
};

static uint32_t plane_align = 1;         /* --plane-align=N, 1 = packed */

static void layout_build(struct out_layout *L, uint32_t fmt)
{
    uint32_t y_stride = ALIGN_UP(W, 64u);
    uint32_t c_stride = ALIGN_UP(W / 2, 64u);

    memset(L, 0, sizeof(*L));
    L->fmt = fmt;
    switch (fmt & 0xFF) {
    case 0xE6:                                  /* YUV420 planar */
        L->planes = 3;
        L->stride[0] = y_stride;
        L->stride[1] = c_stride;
        L->stride[2] = c_stride;
        L->size[0] = y_stride * H;
        L->size[1] = c_stride * (H / 2);
        L->size[2] = L->size[1];
        L->blocklinear = 1;
        break;
    case 0xE7:                                  /* NV12: UV interleaved */
        L->planes = 2;
        L->stride[0] = y_stride;
        L->stride[1] = y_stride;                /* one UV row = W bytes */
        L->size[0] = y_stride * H;
        L->size[1] = y_stride * (H / 2);
        L->blocklinear = 1;
        break;
    default:                                    /* 0x43 and friends: packed */
        L->planes = 1;
        L->stride[0] = W * 4;
        L->size[0] = W * 4 * H;
        L->blocklinear = 0;
        break;
    }
    /* Planes are packed, not page-aligned. Measured: the hardware takes the
     * addresses we give it for planes 1 and 2, but places plane 3 itself,
     * immediately after plane 2 ends -- with a page-aligned layout we were
     * reading the third plane 3 KB past where it was written. */
    uint32_t off = 0;
    for (int i = 0; i < L->planes; i++) {
        L->offset[i] = off;
        off += L->size[i];
        if (plane_align > 1) off = ALIGN_UP(off, plane_align);
    }
    L->total = ALIGN_UP(off, 4096u);
}

/* Named after the FORMAT, not after how many triplets we happen to push:
 * with --out-planes=3 on RGBA the extra triplets are still packed pixels. */
/* "1,4cc,0" -> words. Returns how many were read, -1 on a bad list. */
static int parse_words(const char *s, uint32_t *out, int max)
{
    int k = 0;
    while (*s && k < max) {
        char *end;
        out[k++] = (uint32_t)strtoul(s, &end, 16);
        if (end == s) return -1;
        s = end;
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return *s ? -1 : k;
}

static const char *plane_name(const struct out_layout *L, int i)
{
    switch (L->fmt & 0xFF) {
    case 0xE6: return i == 0 ? "Y" : (i == 1 ? "U" : "V");
    case 0xE7: return i == 0 ? "Y" : "UV";
    default:   return "packed";
    }
}

/* 4 for a packed RGBA surface, 1 otherwise -- again a property of the
 * format alone, so an --out-planes override cannot switch the stats off. */
static int plane_channels(const struct out_layout *L)
{
    return (L->fmt & 0xFF) == 0x43 ? 4 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf(
"Usage: %s <raw_bayer_file> [format_hex] [isp_enable_hex] [options]\n"
"\n"
"  Geometry:  --width=N --height=N            (default 2592x1944)\n"
"  Output:    --out-fmt=0x43|0xe6|0xe7|<word> (default 0x43 RGBA)\n"
"             --out-e02-hi=0xNNNN             high halfword of E02\n"
"             --out-planes=N                  override triplet count (A/B)\n"
"             --out-alloc=pitch|blocklinear   override nvmap kind\n"
"             --uv-stride=N --uv-offset=N --v-offset=N\n"
"             --out-e03=0xN                   output colour config\n"
"  Input:     --in-fmt=0xN                    E33 code (default 0x10200024)\n"
"             --in-swaprb                     swap R/B bayer sites in RAM\n"
"             --rgba-in                       input is RGBA, not bayer\n"
"  ISP:       --isp-enable=0xN                reg 0x015 (default 0x7)\n"
"             --stat-bytes=N                  readback scan budget, 0=all\n"
"             --param-fill=0xWORD             fill the 0x100 DMA block (def 0)\n"
"  Pipeline:  --pipeline                      stock 0x200/0x202/0x205\n"
"             --r200=a,b --r202=a,b,c --r205=a,b,c,d   hex words, imply\n"
"                                             --pipeline; default per class\n"
"  Colour:    --curve=identity|scurve         tone curves (default identity)\n"
"             --gpp-gain=0xWORD               0x600 words 12..14 (def 0x3fff0000)\n"
"             --ccm=w0,w1,..,w7               push CCM 0x300/0x304 (default: off)\n"
"             --no-ls                         disable lens shading (0xD0A=0)\n"
"  Legacy:    --yuv --nv12 --nv12-layout\n",
            argv[0]);
        return 1;
    }

    int rgba_input = 0;
    int opt_planes = 0;                 /* 0 = from format */
    int opt_alloc = -1;                 /* -1 = from format, 0 pitch, 1 blocklinear */
    int opt_swaprb = 0, opt_no_ls = 0;
    int opt_scurve = 0;                 /* default: identity curves */
    uint32_t opt_uv_stride = 0, opt_uv_offset = 0, opt_v_offset = 0;
    uint32_t opt_gpp_gain = 0x3fff0000;
    uint32_t opt_e03 = 0;
    uint32_t opt_in_fmt = 0;            /* 0 = from --rgba-in */
    uint32_t ccm[8];
    int have_ccm = 0;
    uint32_t out_fmt = 0x43;            /* RGBA unless asked otherwise */
    int have_e02_hi = 0;
    uint32_t opt_e02_hi = 0;
    uint32_t opt_isp_enable = 0x00000007;   /* from blob gather RE */
    uint32_t opt_stat_bytes = 4u << 20;     /* readback scan budget per plane */
    uint32_t opt_param_fill = 0;            /* word written across the 0x100 block */
    int opt_commit = 0;                     /* 1 = 0x00C=0x0F, 2 = with 0x01F/0x05F */
    int opt_cal_colour = 0;                 /* real coefficients in the cal submit */
    int opt_teardown = 1;                   /* quiesce the ISP on the way out */
    int opt_dump = 1;                       /* write the surface to a file */
    int opt_in_shift = 0;                   /* restack samples in the container */
    int opt_blk400 = 0;                     /* 0x400 from the host settings */
    int opt_blk400_live = 0;                /* 0x400 as the hardware gets it */
    int opt_pipeline = 0;                   /* stock 0x200/0x202/0x205 */
    uint32_t r200[2], r202[3], r205[4];
    int n200 = 0, n202 = 0, n205 = 0;       /* 0 = use the class default */
    uint32_t r400[12];
    int n400 = 0;                           /* leading words of 0x400 to replace */
    int opt_blk500 = 0;                     /* stock 0x500 words instead of ours */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--width=", 8) == 0)       W = (unsigned)strtoul(a + 8, 0, 0);
        else if (strncmp(a, "--height=", 9) == 0) H = (unsigned)strtoul(a + 9, 0, 0);
        else if (strncmp(a, "--out-fmt=", 10) == 0)   out_fmt = strtoul(a + 10, 0, 16);
        else if (strncmp(a, "--out-e02-hi=", 13) == 0) {
            opt_e02_hi = strtoul(a + 13, 0, 16); have_e02_hi = 1;
        }
        else if (strncmp(a, "--out-planes=", 13) == 0) opt_planes = atoi(a + 13);
        else if (strncmp(a, "--out-alloc=", 12) == 0)
            opt_alloc = (strcmp(a + 12, "blocklinear") == 0) ? 1 : 0;
        else if (strncmp(a, "--uv-stride=", 12) == 0) opt_uv_stride = strtoul(a + 12, 0, 0);
        else if (strncmp(a, "--uv-offset=", 12) == 0) opt_uv_offset = strtoul(a + 12, 0, 0);
        else if (strncmp(a, "--v-offset=", 11) == 0)  opt_v_offset = strtoul(a + 11, 0, 0);
        else if (strncmp(a, "--out-e03=", 10) == 0)   opt_e03 = strtoul(a + 10, 0, 16);
        else if (strncmp(a, "--in-fmt=", 9) == 0)     opt_in_fmt = strtoul(a + 9, 0, 16);
        else if (strncmp(a, "--isp-enable=", 13) == 0) opt_isp_enable = strtoul(a + 13, 0, 16);
        else if (strncmp(a, "--stat-bytes=", 13) == 0) opt_stat_bytes = strtoul(a + 13, 0, 0);
        else if (strncmp(a, "--param-fill=", 13) == 0) opt_param_fill = strtoul(a + 13, 0, 16);
        else if (strcmp(a, "--cal-colour") == 0)      opt_cal_colour = 1;
        else if (strcmp(a, "--no-teardown") == 0)     opt_teardown = 0;
        else if (strcmp(a, "--no-dump") == 0)         opt_dump = 0;
        else if (strncmp(a, "--plane-align=", 14) == 0)
            plane_align = (uint32_t)strtoul(a + 14, 0, 0);
        else if (strncmp(a, "--in-shift=", 11) == 0)  opt_in_shift = atoi(a + 11);
        else if (strcmp(a, "--blk400") == 0)          opt_blk400 = 1;
        else if (strcmp(a, "--blk400-live") == 0)     opt_blk400_live = 1;
        else if (strncmp(a, "--r400=", 7) == 0) {
            n400 = parse_words(a + 7, r400, 12); opt_blk400 = 1;
            if (n400 < 0) { printf("bad --r400 list\n"); return 1; }
        }
        else if (strcmp(a, "--pipeline") == 0)        opt_pipeline = 1;
        else if (strncmp(a, "--r200=", 7) == 0) {
            n200 = parse_words(a + 7, r200, 2); opt_pipeline = 1;
            if (n200 < 0) { printf("bad --r200 list\n"); return 1; }
        }
        else if (strncmp(a, "--r202=", 7) == 0) {
            n202 = parse_words(a + 7, r202, 3); opt_pipeline = 1;
            if (n202 < 0) { printf("bad --r202 list\n"); return 1; }
        }
        else if (strncmp(a, "--r205=", 7) == 0) {
            n205 = parse_words(a + 7, r205, 4); opt_pipeline = 1;
            if (n205 < 0) { printf("bad --r205 list\n"); return 1; }
        }
        else if (strcmp(a, "--blk500") == 0)          opt_blk500 = 1;
        else if (strcmp(a, "--commit") == 0)          opt_commit = 1;
        else if (strcmp(a, "--commit-full") == 0)     opt_commit = 2;
        else if (strcmp(a, "--in-swaprb") == 0)       opt_swaprb = 1;
        else if (strcmp(a, "--rgba-in") == 0)         rgba_input = 1;
        else if (strncmp(a, "--curve=", 8) == 0)      opt_scurve = (strcmp(a + 8, "scurve") == 0);
        else if (strncmp(a, "--gpp-gain=", 11) == 0)  opt_gpp_gain = strtoul(a + 11, 0, 16);
        else if (strcmp(a, "--no-ls") == 0)           opt_no_ls = 1;
        else if (strncmp(a, "--ccm=", 6) == 0) {
            const char *q = a + 6;
            int k = 0;
            while (k < 8 && *q) {
                ccm[k++] = strtoul(q, (char **)&q, 16);
                if (*q == ',') q++;
            }
            if (k != 8) { printf("--ccm needs 8 hex words\n"); return 1; }
            have_ccm = 1;
        }
        /* legacy flags, kept so old command lines still reproduce */
        else if (strcmp(a, "--yuv") == 0)  out_fmt = 0x010000E6;
        else if (strcmp(a, "--nv12") == 0) out_fmt = 0x010000E7;
        else if (strcmp(a, "--nv12-layout") == 0) {
            out_fmt = 0x010000E7;
            opt_uv_stride = 0xffffffff;   /* marker: UV stride = Y stride */
        }
        else if (a[0] == '-') { printf("unknown option: %s\n", a); return 1; }
    }
    /* Legacy positional format / isp-enable. Both are only positional when
     * they are not options: an unguarded argv[3] silently turned --height=
     * into ISP_ENABLE=0 and quietly changed what the run measured. */
    if (argc > 2 && argv[2][0] != '-') out_fmt = strtoul(argv[2], NULL, 16);
    if (argc > 3 && argv[3][0] != '-' && argv[2][0] != '-')
        opt_isp_enable = strtoul(argv[3], NULL, 16);
    /* a bare low byte gets the default high halfword for that format */
    if (out_fmt <= 0xFF && (out_fmt == 0xE6 || out_fmt == 0xE7)) out_fmt |= 0x01000000;
    if (have_e02_hi) out_fmt = (out_fmt & 0x0000FFFF) | (opt_e02_hi << 16);

    if (W == 0 || H == 0 || (W & 1) || (H & 1)) {
        printf("bad geometry: --width/--height must be nonzero even numbers\n");
        return 1;
    }

    struct out_layout L;
    layout_build(&L, out_fmt);
    if (opt_uv_stride == 0xffffffff) opt_uv_stride = ALIGN_UP(W, 64u);
    if (opt_planes > L.planes) {
        /* asking for more triplets than the format has: the extra ones
         * repeat plane 0, which is what the tool used to do for RGBA */
        for (int i = L.planes; i < opt_planes && i < 3; i++) {
            L.stride[i] = L.stride[0];
            L.offset[i] = L.offset[0];
            L.size[i]   = 0;
        }
        L.planes = opt_planes > 3 ? 3 : opt_planes;
    } else if (opt_planes > 0) {
        L.planes = opt_planes;
    }
    if (opt_uv_stride) L.stride[1] = opt_uv_stride;
    if (opt_uv_offset) L.offset[1] = opt_uv_offset;
    if (opt_v_offset)  L.offset[2] = opt_v_offset;
    if (opt_alloc >= 0) L.blocklinear = opt_alloc;
    {   /* the buffer must cover every plane we actually programmed */
        uint32_t need = W * 4 * H;
        for (int i = 0; i < L.planes; i++) {
            uint32_t end = L.offset[i] + L.size[i];
            if (end > need) need = end;
        }
        if (L.total < need) L.total = need;
    }
    printf("=== ISP Pure Reprocess (no blobs) ===\n");
    printf("Geometry: W=%u H=%u BPP=%u in=%u\n", W, H, BPP, W * H * BPP);
    printf("Output: fmt=0x%08x planes=%d alloc=%s total=%u\n",
           L.fmt, L.planes, L.blocklinear ? "blocklinear" : "pitch", L.total);
    for (int i = 0; i < L.planes; i++)
        printf("  plane %d (%-6s) stride=%-6u offset=0x%06x size=%u\n",
               i, plane_name(&L, i), L.stride[i], L.offset[i], L.size[i]);
    printf("Colour: curve=%s gpp-gain=0x%08x ls=%s ccm=%s swaprb=%d\n",
           opt_scurve ? "scurve" : "identity", opt_gpp_gain,
           opt_no_ls ? "off" : "on", have_ccm ? "set" : "off", opt_swaprb);
    printf("ISP_ENABLE=0x%08x e03=0x%08x\n", opt_isp_enable, opt_e03);

    /* Open devices */
    nvmap_fd = open("/dev/nvmap", O_RDWR | O_SYNC);
    if (nvmap_fd < 0) { perror("open nvmap"); return 1; }

    /* Open ISP ctrl node first (like NvIspOpen) */
    int isp_ctrl_fd = open("/dev/nvhost-ctrl-isp", O_RDWR);
    if (isp_ctrl_fd < 0) perror("open isp ctrl (non-fatal)");

    /* Open ISP-A first for testing, fallback to ISP-B */
    int isp_fd = open("/dev/nvhost-isp", O_RDWR);
    if (isp_fd >= 0) {
        printf("ISP-A fd=%d\n", isp_fd);
        isp_class = ISP_CLASS_A;
    } else {
        isp_fd = open("/dev/nvhost-isp.1", O_RDWR);
        if (isp_fd < 0) { perror("open isp"); return 1; }
        printf("ISP-B fd=%d (fallback)\n", isp_fd);
        isp_class = ISP_CLASS_B;
    }

    /* NOTE: CHANNEL_OPEN ioctl (NR=112) causes kernel panic on 24.1.
     * On 24.1, open() already creates hwctx via nvhost_channelopen().
     * Skip CHANNEL_OPEN and rely on open() + init gather. */

    int ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);
    if (ctrl_fd < 0) { perror("open ctrl"); return 1; }

    /* Set nvmap fd for ISP channel */
    struct nvhost_set_nvmap_fd_args snf = { .fd = nvmap_fd };
    if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &snf) < 0)
        perror("set nvmap fd (non-fatal)");

    /* Set ISP clock rate — required for ISP to process frames.
     * moduleid encoding: [31:24]=clock_attr, [15:0]=module_id
     * clock_attr: 0=NVHOST_CLOCK, 1=NVHOST_BW
     * module_id: from enum nvhost_module_id (ISP=3) */
    struct nvhost_clk_rate_args clk;

    /* ISP core clock = 384 MHz */
    clk.rate = 384000000;
    clk.moduleid = 0;  /* clock index 0 = ISP core */
    if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &clk) < 0)
        perror("set ISP clk (non-fatal)");
    else
        printf("ISP clk set to %u Hz\n", clk.rate);

    /* EMC clock = 768 MHz */
    clk.rate = 768000000;
    clk.moduleid = 1;  /* clock index 1 = EMC */
    if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &clk) < 0)
        perror("set EMC clk (non-fatal)");
    else
        printf("EMC clk set to %u Hz\n", clk.rate);

    /* PIO write: ISP register 0xFC = 0x20 (enable/reset)
     * From RE of NvIspOpen — NvRmHostModuleRegWr(hRm, module_id, 1, {0xFC, 0x20})
     * This is the ISP top-level enable that must happen before any submit. */
    {
        uint32_t offset = 0xFC;
        uint32_t value = 0x20;
        struct nvhost32_ctrl_module_regrdwr_args rw;
        memset(&rw, 0, sizeof(rw));
        rw.id = 0x0B;           /* ISP module id */
        rw.num_offsets = 1;
        rw.block_size = 4;
        rw.offsets = (uint32_t)(uintptr_t)&offset;
        rw.values = (uint32_t)(uintptr_t)&value;
        rw.write = 1;
        /* NR=14 on channel fd — same as working pio_test */
        if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR, &rw) == 0)
            printf("PIO write: ISP reg 0xFC = 0x20 OK\n");
        else
            perror("PIO write 0xFC FAILED");
    }

    /* Get waitbase (NR=17) — stock camera does this during init */
    struct nvhost_get_param_arg gwb;
    gwb.param = 0;
    if (ioctl(isp_fd, _IOWR(NVHOST_IOCTL_MAGIC, 17, struct nvhost_get_param_arg), &gwb) < 0)
        perror("get waitbase (non-fatal)");
    else
        printf("Waitbase param=0 → %u\n", gwb.value);

    /* Get waitbases (NR=3) */
    struct { uint32_t value; } gwbs;
    if (ioctl(isp_fd, _IOR(NVHOST_IOCTL_MAGIC, 3, gwbs), &gwbs) < 0)
        perror("get waitbases (non-fatal)");
    else
        printf("Waitbases → %u\n", gwbs.value);

    /* Get syncpoints */
    struct nvhost_get_param_arg gsp;
    gsp.param = 0; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_memory = gsp.value;
    gsp.param = 1; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_stats = gsp.value;
    gsp.param = 2; ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp);
    uint32_t sp_loadv = gsp.value;
    printf("ISP fd=%d, syncpts: memory=%u stats=%u loadv=%u\n",
           isp_fd, sp_memory, sp_stats, sp_loadv);

    /* Allocate buffers */
    uint32_t out_size = L.total;
    uint32_t in_size = rgba_input ? (W * 4 * H) : (W * H * BPP);
    uint32_t in_h = nvmap_create(in_size);
    uint32_t out_h = nvmap_create(out_size);
    uint32_t work_h = nvmap_create(512 * 1024);  /* ISP work buffer */
    uint32_t cmd_h = nvmap_create(32768);  /* larger for lens shading + tone curves */
    uint32_t param_h = nvmap_create(4096); /* ISP demosaic parameter block */
    if (!in_h || !out_h || !work_h || !cmd_h || !param_h) { printf("alloc failed\n"); return 1; }
    nvmap_alloc(in_h); nvmap_alloc(work_h); nvmap_alloc(cmd_h); nvmap_alloc(param_h);
    /* Output buffer kind comes from the layout (--out-alloc overrides it) */
    if (L.blocklinear) {
        if (nvmap_alloc_kind(out_h, 0xFE) < 0) {
            printf("blocklinear alloc failed, fallback to pitch\n");
            nvmap_alloc(out_h);
        } else {
            printf("Output buffer: blocklinear kind=0xFE\n");
        }
    } else {
        nvmap_alloc(out_h);
    }

    uint32_t in_iova = nvmap_pin(in_h);
    uint32_t out_iova = nvmap_pin(out_h);
    uint32_t work_iova = nvmap_pin(work_h);
    uint32_t param_iova = nvmap_pin(param_h);
    printf("in_iova=0x%08x out_iova=0x%08x work_iova=0x%08x param_iova=0x%08x\n",
           in_iova, out_iova, work_iova, param_iova);

    /* Fill ISP parameter block with identity/zero coefficients.
     * Stock uses 592-byte slots: 104-byte + 260-byte + 4x36-byte.
     * ISP reads demosaic/color-correction data from this address via reg 0x100.
     * For initial test: zero-fill (identity). */
    {
        uint32_t fill[1024];
        for (int i = 0; i < 1024; i++) fill[i] = opt_param_fill;
        nvmap_write(param_h, 0, fill, 4096);
        printf("Param block: 4096 bytes of 0x%08x at IOVA 0x%08x\n",
               opt_param_fill, param_iova);
    }

    /* Load raw frame */
    printf("Loading %s...\n", argv[1]);
    {
        FILE *sz = fopen(argv[1], "rb");
        if (sz != 0) {
            fseek(sz, 0, SEEK_END);
            long fsz = ftell(sz);
            fclose(sz);
            unsigned expect = W * H * BPP;
            if ((unsigned)fsz != expect)
                printf("WARNING: input file %ld bytes, expected %u for "
                       "%ux%u -- continuing (tool reads partial files)\n",
                       fsz, expect, W, H);
        }
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open raw"); return 1; }
    uint8_t *raw_buf = calloc(1, in_size);
    fread(raw_buf, 1, in_size, f);
    fclose(f);

    /* --in-swaprb: exchange the two diagonal bayer sites in the 2x2 cell,
     * i.e. BGGR <-> RGGB. The green sites are untouched. Lets us test the
     * bayer phase without guessing an E33 code. */
    /* --in-shift: restack the 10 bits inside their 16-bit container. Our
     * raw is right-justified; if the ISP unpacks from the top of the word
     * we are handing it the wrong bits, which would explain the output
     * surviving as exactly the top 5 bits of 10 -- and a bad unpack breaks
     * the channel weights and the third bayer site along with the depth. */
    if (opt_in_shift && !rgba_input) {
        uint16_t *px = (uint16_t *)raw_buf;
        size_t npx = (size_t)W * H;
        if (opt_in_shift > 0)
            for (size_t i = 0; i < npx; i++) px[i] = (uint16_t)(px[i] << opt_in_shift);
        else
            for (size_t i = 0; i < npx; i++) px[i] = (uint16_t)(px[i] >> -opt_in_shift);
        printf("Input: samples shifted %d bits inside the container\n", opt_in_shift);
    }

    if (opt_swaprb && !rgba_input) {
        uint16_t *px = (uint16_t *)raw_buf;
        for (unsigned y = 0; y + 1 < H; y += 2) {
            uint16_t *r0 = px + (size_t)y * W;
            uint16_t *r1 = r0 + W;
            for (unsigned x = 0; x + 1 < W; x += 2) {
                uint16_t t = r0[x];
                r0[x] = r1[x + 1];
                r1[x + 1] = t;
            }
        }
        printf("Input: swapped R/B bayer sites (BGGR <-> RGGB)\n");
    }

    int chunk = 65536;
    for (int off = 0; off < (int)in_size; off += chunk) {
        int sz = ((int)in_size - off < chunk) ? (int)in_size - off : chunk;
        nvmap_write(in_h, off, raw_buf + off, sz);
    }
    free(raw_buf);

    /* Zero output */
    uint8_t *zeros = calloc(1, chunk);
    for (uint32_t off = 0; off < out_size; off += chunk) {
        uint32_t sz = (out_size - off < (uint32_t)chunk) ? out_size - off : (uint32_t)chunk;
        nvmap_write(out_h, off, zeros, sz);
    }
    free(zeros);

    /* Get stream syncpt (used by init and cal submits) */
    struct nvhost_get_param_arg gsp_s;
    gsp_s.param = 2;
    ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gsp_s);
    uint32_t sp_stream = gsp_s.value;
    printf("Stream syncpt=%u\n", sp_stream);

    /* Init gather: configure ISP DMA pipeline (required for pixel output) */
    {
        uint32_t init_cmd[16];
        int ini = 0;
        init_cmd[ini++] = OP_SETCLASS(ISP_CLASS, 0, 0);
        /* Stock DMA values from SetConfig RE + 0x01C=2 for reprocess */
        /* Original working DMA values (give luma output) */
        init_cmd[ini++] = OP_INCR(0x019, 1);
        init_cmd[ini++] = 0x00000400;  /* 0x019 */
        init_cmd[ini++] = OP_INCR(0x01B, 2);
        init_cmd[ini++] = 0x00000200;  /* 0x01B */
        init_cmd[ini++] = 0x00000002;  /* 0x01C */

        uint32_t init_h = nvmap_create(4096);
        nvmap_alloc(init_h);
        nvmap_write(init_h, 0, init_cmd, ini * 4);

        /* G[1]: immediate syncpt incr */

        uint32_t g1_data[2];
        g1_data[0] = (4 << 28) | sp_stream;  /* IMM incr */
        g1_data[1] = 0;
        nvmap_write(init_h, 256, g1_data, 8);

        struct nvhost_cmdbuf icbs[2];
        icbs[0] = (struct nvhost_cmdbuf){ .mem = init_h, .offset = 0, .words = ini };
        icbs[1] = (struct nvhost_cmdbuf){ .mem = init_h, .offset = 256, .words = 2 };
        struct nvhost_syncpt_incr isi = { .syncpt_id = sp_stream, .syncpt_incrs = 1 };
        uint32_t iclasses[2] = { ISP_CLASS, ISP_CLASS };
        struct nvhost_fence ifence = {0,0};

        struct nvhost32_submit_args isa;
        memset(&isa, 0, sizeof(isa));
        isa.submit_version = 0;
        isa.num_syncpt_incrs = 1;
        isa.num_cmdbufs = 2;
        isa.timeout = 5000;
        isa.syncpt_incrs = (uint32_t)(uintptr_t)&isi;
        isa.cmdbufs = (uint32_t)(uintptr_t)icbs;
        isa.class_ids = (uint32_t)(uintptr_t)iclasses;
        isa.fences = (uint32_t)(uintptr_t)&ifence;

        if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &isa) < 0)
            perror("init submit FAILED");
        else
            printf("Init submit OK (0x019/0x01B/0x01C), fence=%u\n", isa.fence);
        nvmap_free(init_h);
    }

    /* Cal gather: initialize all shadow registers + post-apply trigger 0x0F
     * Stock sends this as separate submit before per-frame */
    {
        uint32_t cal[2048];
        int cn = 0;
        cal[cn++] = OP_SETCLASS(ISP_CLASS, 0, 0);

        /* 0x200-0x208: pipeline mode (zeros) */
        cal[cn++] = OP_INCR(0x202, 3); cal[cn++]=0; cal[cn++]=0; cal[cn++]=0;
        cal[cn++] = OP_INCR(0x200, 2); cal[cn++]=0; cal[cn++]=0;
        cal[cn++] = OP_INCR(0x205, 4); cal[cn++]=0; cal[cn++]=0; cal[cn++]=0; cal[cn++]=0;

        /* 0x700-0x75F: NR (zeros) */
        cal[cn++] = OP_INCR(0x700, 16);
        for (int i=0;i<16;i++) cal[cn++]=0;
        cal[cn++] = OP_INCR(0x750, 16);
        for (int i=0;i<16;i++) cal[cn++]=0;

        /* 0xD00-0xD0B: lens shading (zeros) */
        cal[cn++] = OP_INCR(0xD00, 10);
        for (int i=0;i<10;i++) cal[cn++]=0;
        cal[cn++] = OP_INCR(0xD0A, 1); cal[cn++]=0;
        cal[cn++] = OP_NONINCR(0xD0B, 480);
        for (int i=0;i<480;i++) cal[cn++]=0;
        cal[cn++] = OP_INCR(0xD0C, 2); cal[cn++]=0; cal[cn++]=0;
        cal[cn++] = OP_INCR(0xD20, 6);
        for (int i=0;i<6;i++) cal[cn++]=0;

        /* Colour blocks. This gather is the one that ends in the 0x0F
         * apply, so whatever it writes is what the hardware actually runs
         * on; the same registers written later in the frame gather are
         * never committed. --cal-colour puts the real coefficients here
         * instead of the zeros the tool has always sent. */
        static const uint32_t demosaic[9] = {
            0x3f3fcff3, 0x00000000, 0x04c1304c, 0x08220882, 0x00000000,
            0x03d0f43d, 0x08621886, 0x01204812, 0x06e1b86e
        };

        /* 0x506-0x50E: demosaic */
        cal[cn++] = OP_INCR(0x506, 9);
        for (int i=0;i<9;i++) cal[cn++] = opt_cal_colour ? demosaic[i] : 0;

        /* 0x600-0x60F: GPP */
        cal[cn++] = OP_INCR(0x600, 16);
        for (int i=0;i<16;i++) {
            uint32_t v = 0;
            if (opt_cal_colour) {
                if (i == 0) v = 0x00000005;
                else if (i >= 12 && i <= 14) v = opt_gpp_gain;
                else if (i == 15) v = work_iova + 0x31000;
            }
            cal[cn++] = v;
        }
        cal[cn++] = OP_INCR(0x650, 1);
        cal[cn++] = opt_cal_colour ? 0x00000003 : 0;

        /* Tone curves */
        for (int ch=0; ch<4; ch++) {
            cal[cn++] = OP_INCR(0x651+ch*2, 1); cal[cn++]=0;
            cal[cn++] = OP_NONINCR(0x652+ch*2, 257);
            for (int i=0;i<257;i++) {
                uint32_t v = 0;
                if (opt_cal_colour) {
                    v = 0x1000;
                    if (opt_scurve) {
                        if (i < 64)       v = 0x1000;
                        else if (i < 192) v = 0x1000 + (i - 64) * 0x2000 / 128;
                        else              v = 0x3000;
                    }
                }
                cal[cn++] = v;
            }
        }

        /* 0x300-0x307: CCM */
        cal[cn++] = OP_INCR(0x300, 4);
        for (int i=0;i<4;i++) cal[cn++] = (opt_cal_colour && have_ccm) ? ccm[i] : 0;
        cal[cn++] = OP_INCR(0x304, 4);
        for (int i=4;i<8;i++) cal[cn++] = (opt_cal_colour && have_ccm) ? ccm[i] : 0;

        /* The two blocks that only ever appear in the stock init gather,
         * alongside the colour coefficients: 0x400 (luma weights and the
         * per-channel words) and 0xC00. Neither has been in our cal. */
        if (opt_cal_colour) {
            static const uint32_t c400[12] = {
                0x00000001, 0x004b0000, 0x00930000, 0x00220000,
                0x2ff01000, 0x2ff01000, 0x2ff01000, 0x2ff01000,
                0x00030000, 0x00000000, 0x00020000, 0x00000000
            };
            cal[cn++] = OP_INCR(0x400, 12);
            for (int i = 0; i < 12; i++) cal[cn++] = c400[i];
            cal[cn++] = OP_INCR(0xC00, 3);
            cal[cn++] = 0x00000101; cal[cn++] = 0x00000000;
            cal[cn++] = 0x00100000;
        }

        if (opt_cal_colour)
            printf("Cal: colour blocks carry real coefficients "
                   "(demosaic, gpp 0x%08x, curve=%s, ccm=%s)\n",
                   opt_gpp_gain, opt_scurve ? "scurve" : "identity",
                   have_ccm ? "set" : "zero");

        /* 0x053: work buffer */
        cal[cn++] = OP_INCR(0x053, 2); cal[cn++]=0; cal[cn++]=0;

        /* Post-apply trigger 0x0F */
        cal[cn++] = OP_NONINCR(0x00C, 1); cal[cn++]=0x0F;

        /* 0x01F, 0x05F */
        cal[cn++] = OP_INCR(0x01F, 1); cal[cn++]=1;
        cal[cn++] = OP_INCR(0x05F, 1); cal[cn++]=0x10;

        printf("Cal gather: %d words\n", cn);

        uint32_t cal_h = nvmap_create(cn*4+256);
        nvmap_alloc(cal_h);
        nvmap_write(cal_h, 0, cal, cn*4);

        /* G[1]: syncpt incr */
        uint32_t cg1[2] = { (4<<28)|sp_stream, 0 };
        nvmap_write(cal_h, cn*4+128, cg1, 8);

        struct nvhost_cmdbuf ccbs[2] = {
            { .mem=cal_h, .offset=0, .words=cn },
            { .mem=cal_h, .offset=cn*4+128, .words=2 }
        };
        struct nvhost_syncpt_incr csi = { .syncpt_id=sp_stream, .syncpt_incrs=1 };
        uint32_t cclasses[2] = { ISP_CLASS, ISP_CLASS };
        struct nvhost_fence cfence = {0,0};

        struct nvhost32_submit_args csa;
        memset(&csa, 0, sizeof(csa));
        csa.submit_version = 0;
        csa.num_syncpt_incrs = 1;
        csa.num_cmdbufs = 2;
        csa.timeout = 5000;
        csa.syncpt_incrs = (uint32_t)(uintptr_t)&csi;
        csa.cmdbufs = (uint32_t)(uintptr_t)ccbs;
        csa.class_ids = (uint32_t)(uintptr_t)cclasses;
        csa.fences = (uint32_t)(uintptr_t)&cfence;

        if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &csa) < 0)
            perror("cal submit FAILED");
        else
            printf("Cal submit OK, fence=%u\n", csa.fence);
        nvmap_free(cal_h);
    }

    /* Build reprocess gather with ISP pipeline init */
    #include "isp_lens_shading.h"
    uint32_t cmd[2048];
    int n = 0;
    int plane_reloc[3] = { -1, -1, -1 };
    int in_reloc = -1;
    int work_reloc = -1;

    /* SETCLASS must be first — tells host1x which engine gets the commands */
    cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);

    /* ISP work buffer (0x053) — needed for cold start */
    cmd[n++] = OP_INCR(0x053, 2);
    work_reloc = n;
    cmd[n++] = 1;                         /* enable */
    cmd[n++] = 0;                         /* IOVA patched by reloc */

    /* Zero 0x200 (reset from previous) */
    cmd[n++] = OP_INCR(0x200, 9);
    cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
    cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0; cmd[n++] = 0;
    cmd[n++] = 0;

    /* ---- S5 register blocks ---- */

    /* 0x700: processing channel A (16 words) */
    cmd[n++] = OP_INCR(0x700, 16);
    cmd[n++] = 0x00000001; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00001a40;  /* ISP-B stride */
    cmd[n++] = 0x00000000; cmd[n++] = work_iova + 0x30000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00001000;
    cmd[n++] = 0x00001a00;  /* ISP-B */
    cmd[n++] = work_iova + 0x20000; cmd[n++] = work_iova + 0x20000;
    cmd[n++] = work_iova + 0x20000; cmd[n++] = work_iova + 0x20000;

    /* 0x750: processing channel B (16 words) */
    cmd[n++] = OP_INCR(0x750, 16);
    cmd[n++] = 0x00000003; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = work_iova + 0x20000; cmd[n++] = work_iova + 0x20000;
    cmd[n++] = work_iova + 0x20000; cmd[n++] = work_iova + 0x20000;

    /* 0xC00: extra config */
    cmd[n++] = OP_INCR(0xC00, 3);
    cmd[n++] = 0x00000101;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x00100000;

    /* 0x506: demosaic (9 words) */
    cmd[n++] = OP_INCR(0x506, 9);
    cmd[n++] = 0x3f3fcff3;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x04c1304c;
    cmd[n++] = 0x08220882;
    cmd[n++] = 0x00000000;
    cmd[n++] = 0x03d0f43d;
    cmd[n++] = 0x08621886;
    cmd[n++] = 0x01204812;
    cmd[n++] = 0x06e1b86e;

    /* 0x600: GPP config (16 words) */
    cmd[n++] = OP_INCR(0x600, 16);
    cmd[n++] = 0x00000005; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = 0x00000000; cmd[n++] = 0x00000000;
    cmd[n++] = opt_gpp_gain; cmd[n++] = opt_gpp_gain;
    cmd[n++] = opt_gpp_gain; cmd[n++] = work_iova + 0x31000;

    /* 0x650: tone curve enable */
    cmd[n++] = OP_INCR(0x650, 1);
    cmd[n++] = 0x00000003;

    /* ---- End S5 blocks ---- */

    /* Lens shading control (0xD00, 10 words) — from stock color filter trace */
    cmd[n++] = OP_INCR(0xD00, 10);
    cmd[n++] = 0x00000001;  /* enable */
    cmd[n++] = 0x00ca4580;
    cmd[n++] = 0x006522c0;
    cmd[n++] = 0x00ca4580;
    cmd[n++] = 0x010db200;
    cmd[n++] = 0x0086d900;
    cmd[n++] = 0x010db200;
    cmd[n++] = 0x05100288;  /* grid: 1296x648 */
    cmd[n++] = 0x03cc01e6;  /* grid: 972x486 */
    cmd[n++] = 0x00000021;  /* mode */

    /* Lens shading enable (--no-ls turns the OV5693 table off) */
    cmd[n++] = OP_INCR(0xD0A, 1);
    cmd[n++] = opt_no_ls ? 0 : 1;

    /* Lens shading table — 480 words from stock OV5693 */
    cmd[n++] = OP_NONINCR(0xD0B, LS_DATA_WORDS);
    for (int i = 0; i < LS_DATA_WORDS; i++)
        cmd[n++] = ls_data[i];

    /* Tone curves. Default is identity (0x1000 = 1.0 everywhere): the old
     * S-curve multiplied highlights by 3.0, which clips a channel to 255
     * before we ever see its real value. --curve=scurve restores it. */
    for (int ch = 0; ch < 4; ch++) {
        cmd[n++] = OP_INCR(0x651 + ch * 2, 1);
        cmd[n++] = 0;
        cmd[n++] = OP_NONINCR(0x652 + ch * 2, 257);
        for (int i = 0; i < 257; i++) {
            uint32_t val = 0x1000;                       /* 1.0 */
            if (opt_scurve) {
                if (i < 64)       val = 0x1000;
                else if (i < 192) val = 0x1000 + (i - 64) * 0x2000 / 128;
                else              val = 0x3000;
            }
            cmd[n++] = val;
        }
    }

    /* Colour correction matrix — the tool never programmed 0x300/0x304, so
     * the ISP ran with a zero matrix. Off by default (old behaviour). */
    if (have_ccm) {
        cmd[n++] = OP_INCR(0x300, 4);
        for (int i = 0; i < 4; i++) cmd[n++] = ccm[i];
        cmd[n++] = OP_INCR(0x304, 4);
        for (int i = 4; i < 8; i++) cmd[n++] = ccm[i];
        printf("CCM 0x300/0x304: %08x %08x %08x %08x %08x %08x %08x %08x\n",
               ccm[0], ccm[1], ccm[2], ccm[3], ccm[4], ccm[5], ccm[6], ccm[7]);
    }

    /* Commit the settings just written. 0x00C is the trigger register, and
     * 0x0F means "apply the configuration": the cal submit issues one, but
     * every colour block above is written after that, so without a second
     * commit here they sit in a shadow bank and the frame trigger never
     * sees them -- which is exactly why curves, CCM, the 0x100 page and
     * E03 all produced byte-identical output. */
    if (opt_commit) {
        cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);
        if (opt_commit > 1) {
            cmd[n++] = OP_INCR(0x01F, 1);
            cmd[n++] = 0x00000001;
            cmd[n++] = OP_INCR(0x05F, 1);
            cmd[n++] = 0x00000010;
        }
        cmd[n++] = OP_NONINCR(0x00C, 1);
        cmd[n++] = 0x0000000F;
        printf("Commit: 0x00C=0x0F after the settings blocks%s\n",
               opt_commit > 1 ? " (with 0x01F/0x05F)" : "");
    }

    /* The live form of 0x400. What the tool sent before came out of the
     * library's host-side settings structure; what the hardware actually
     * receives every frame is a six-word NONINCR describing the ring of
     * 0x250-byte records -- slot index, and the stride packed with it.
     * We have one slot, so the index stays 0. */
    if (opt_blk400_live) {
        cmd[n++] = OP_NONINCR(0x400, 6);
        cmd[n++] = 0x00000000;          /* slot index */
        cmd[n++] = 0x00000d00;
        cmd[n++] = 0x00000040;
        cmd[n++] = 0x20080001;
        cmd[n++] = (0x250 << 16) | 0;   /* record stride | slot */
        cmd[n++] = 0x00000d00;
        printf("0x400: live ring form (6 words)\n");
    }

    /* Pipeline mode. 0x200 alone was tried in April and turned the frame
     * solid red; stock never sends it alone, it sends three blocks
     * together, and the companions carry real values (0x780078 pairs,
     * 0x600c8, 0xf000f). This is that combination. */
    if (opt_pipeline) {
        /* Defaults follow the class we are actually submitting on. The
         * trace carries a different set per ISP -- 0x202 pairs are
         * 0x02000200 on class 0x32 and 0x00780078 on 0x34, and 0x205's
         * last word is 0x3333 against 0 -- so taking one ISP's numbers
         * for the other feeds it someone else's geometry. */
        int a_class = (ISP_CLASS == ISP_CLASS_A);
        uint32_t d200[2] = { 0x00000001, 0x00000000 };
        uint32_t d202[3] = { 0x00000001,
                             a_class ? 0x02000200 : 0x00780078,
                             a_class ? 0x02000200 : 0x00780078 };
        uint32_t d205[4] = { 0x00000000, 0x000600c8, 0x000f000f,
                             a_class ? 0x00003333 : 0x00000000 };
        if (!n200) { memcpy(r200, d200, sizeof d200); n200 = 2; }
        if (!n202) { memcpy(r202, d202, sizeof d202); n202 = 3; }
        if (!n205) { memcpy(r205, d205, sizeof d205); n205 = 4; }

        cmd[n++] = OP_INCR(0x200, n200);
        for (int i = 0; i < n200; i++) cmd[n++] = r200[i];
        cmd[n++] = OP_INCR(0x202, n202);
        for (int i = 0; i < n202; i++) cmd[n++] = r202[i];
        cmd[n++] = OP_INCR(0x205, n205);
        for (int i = 0; i < n205; i++) cmd[n++] = r205[i];

        printf("pipeline (class 0x%02x): 0x200=", ISP_CLASS);
        for (int i = 0; i < n200; i++) printf("%08x ", r200[i]);
        printf(" 0x202=");
        for (int i = 0; i < n202; i++) printf("%08x ", r202[i]);
        printf(" 0x205=");
        for (int i = 0; i < n205; i++) printf("%08x ", r205[i]);
        printf("\n");
    }

    /* 0x400, 12 words -- the block the tool has never programmed. Words
     * 1..3 are 75/147/34, which over 256 are 0.293/0.574/0.133: the BT.601
     * luma weights, the same numbers that sit as floats in the library's
     * settings object. Words 4..7 are one value per bayer channel. Taken
     * verbatim from a stock session that produces colour. */
    if (opt_blk400) {
        uint32_t blk400[12] = {
            0x00000001, 0x004b0000, 0x00930000, 0x00220000,
            0x2ff01000, 0x2ff01000, 0x2ff01000, 0x2ff01000,
            0x00030000, 0x00000000, 0x00020000, 0x00000000
        };
        for (int i = 0; i < n400 && i < 12; i++) blk400[i] = r400[i];
        cmd[n++] = OP_INCR(0x400, 12);
        for (int i = 0; i < 12; i++) cmd[n++] = blk400[i];
        printf("0x400:");
        for (int i = 0; i < 12; i++) printf(" %08x", blk400[i]);
        printf("\n");
    }

    /* === MIUI-only register blocks (from stock camera gather #8) === */
    /* 0x500: processing block. Ours is five zeros and (H<<16)|W in the
     * last word; stock sends {3, 0xca4, 0x14400000, 0x0f300000, 0, 0x80008}
     * on every frame -- so the last word is not dimensions, and word 0 is
     * a flags field we leave clear. --blk500 sends the stock words. */
    cmd[n++] = OP_INCR(0x500, 6);
    if (opt_blk500) {
        static const uint32_t blk500[6] = {
            0x00000003, 0x00000ca4, 0x14400000,
            0x0f300000, 0x00000000, 0x00080008
        };
        for (int i = 0; i < 6; i++) cmd[n++] = blk500[i];
        printf("0x500: stock words\n");
    } else {
        cmd[n++] = 0x00000000;            /* flags = 0 */
        cmd[n++] = 0x00000000;
        cmd[n++] = 0x00000000;
        cmd[n++] = 0x00000000;
        cmd[n++] = 0x00000000;
        cmd[n++] = (H << 16) | W;        /* 0x505 */
    }

    /* Output: dims + format */
    cmd[n++] = OP_INCR(0xE00, 1);
    cmd[n++] = ((W - 1) & 0x3FFF) << 16;
    cmd[n++] = OP_INCR(0xE01, 1);
    cmd[n++] = ((H - 1) & 0x3FFF) << 16;
    cmd[n++] = OP_INCR(0xE02, 1);
    cmd[n++] = L.fmt;
    cmd[n++] = OP_INCR(0xE03, 1);
    cmd[n++] = opt_e03;               /* output colour config (stock=0) */

    /* One address triplet per plane the format has: E04/E07/E0A. */
    static const uint32_t plane_reg[3] = { 0xE04, 0xE07, 0xE0A };
    for (int pl = 0; pl < L.planes; pl++) {
        cmd[n++] = OP_INCR(plane_reg[pl], 3);
        plane_reloc[pl] = n;
        cmd[n++] = 0;                 /* IOVA patched by reloc */
        cmd[n++] = 0x00000000;
        cmd[n++] = L.stride[pl];
    }

    /* Input: dims + format + surface + strip + trigger */
    cmd[n++] = OP_INCR(0xE31, 1);
    cmd[n++] = (H << 16) | W;
    cmd[n++] = OP_INCR(0xE33, 1);
    uint32_t in_fmt = opt_in_fmt ? opt_in_fmt
                                 : (rgba_input ? 0x43 : 0x10200024);
    cmd[n++] = in_fmt;
    printf("Input format: 0x%08x%s\n", in_fmt, rgba_input ? " (RGBA)" : " (BG10)");
    cmd[n++] = OP_INCR(0xE34, 3);
    in_reloc = n;
    cmd[n++] = 0;                         /* IOVA patched by reloc */
    cmd[n++] = 0x00000000;
    cmd[n++] = rgba_input ? W * 4 : W * BPP;  /* input stride */
    cmd[n++] = OP_INCR(0xE32, 1);
    cmd[n++] = W & 0x3FFF;                /* strip width */
    cmd[n++] = OP_INCR(0xE30, 1);
    cmd[n++] = 1;                         /* input trigger */

    /* ISP_ENABLE — try different values */
    cmd[n++] = OP_INCR(0x015, 1);
    uint32_t isp_enable = opt_isp_enable;
    cmd[n++] = isp_enable;
    printf("ISP_ENABLE: 0x%08x\n", isp_enable);

    /* Syncpt conditional incrs */
    cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (4 << 8) | sp_memory;
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (5 << 8) | sp_stats;
    cmd[n++] = OP_NONINCR(0x000, 1);
    cmd[n++] = (6 << 8) | sp_loadv;

    /* 0x100: ISP parameter block POINTER (not stats buffer!)
     * ISP reads demosaic/color-correction coefficients from this DMA address.
     * Stock uses RELOC to a ring-buffer slot. We use param_h. */
    cmd[n++] = OP_INCR(0x100, 4);
    int param_reloc = n;
    cmd[n++] = 0;  /* IOVA patched by reloc → param_h */
    cmd[n++] = 0;
    cmd[n++] = 0;
    cmd[n++] = 0;

    /* Reprocess trigger: single 0x0B (from blob gather RE) */
    cmd[n++] = OP_SETCLASS(ISP_CLASS, 0, 0);
    cmd[n++] = OP_NONINCR(0x00C, 1);
    cmd[n++] = 0x0B;

    printf("Gather: %d words\n", n);
    nvmap_write(cmd_h, 0, cmd, n * 4);

    /* Relocs */
    struct nvhost_reloc relocs[8];
    struct nvhost_reloc_shift shifts[8];
    int nr = 0;
    relocs[nr] = (struct nvhost_reloc){ cmd_h, (work_reloc+1)*4, work_h, 0 };
    shifts[nr++].shift = 0;
    for (int pl = 0; pl < L.planes; pl++) {
        relocs[nr] = (struct nvhost_reloc){ cmd_h, plane_reloc[pl]*4, out_h, L.offset[pl] };
        shifts[nr++].shift = 0;
    }
    relocs[nr] = (struct nvhost_reloc){ cmd_h, in_reloc*4, in_h, 0 };
    shifts[nr++].shift = 0;
    /* 0x100 → param block (ISP reads demosaic coefficients from here) */
    relocs[nr] = (struct nvhost_reloc){ cmd_h, param_reloc*4, param_h, 0 };
    shifts[nr++].shift = 0;

    /* Submit */
    struct nvhost_ctrl_syncpt_waitex_args rd = { .id = sp_memory, .thresh = 0, .timeout = 0 };
    ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &rd);
    printf("Submit (syncpt %u cur=%u)...\n", sp_memory, rd.value);

    struct nvhost_cmdbuf cb = { .mem = cmd_h, .offset = 0, .words = n };
    struct nvhost_syncpt_incr si = { .syncpt_id = sp_memory, .syncpt_incrs = 1 };
    uint32_t class_id = ISP_CLASS;
    struct nvhost_fence fence = { 0, 0 };

    struct nvhost32_submit_args sa;
    memset(&sa, 0, sizeof(sa));
    sa.submit_version = 0;
    sa.num_syncpt_incrs = 1;
    sa.num_cmdbufs = 1;
    sa.num_relocs = nr;
    sa.timeout = 5000;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.relocs = (uint32_t)(uintptr_t)relocs;
    sa.reloc_shifts = (uint32_t)(uintptr_t)shifts;
    sa.class_ids = (uint32_t)(uintptr_t)&class_id;
    sa.fences = (uint32_t)(uintptr_t)&fence;

    if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa) < 0) {
        perror("submit");
        printf("SUBMIT FAILED\n");
        return 1;
    }

    uint32_t thresh = sa.fence;
    printf("Fence=%u, waiting...\n", thresh);

    struct nvhost_ctrl_syncpt_waitex_args wa = {
        .id = sp_memory, .thresh = thresh, .timeout = 5000
    };
    if (ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &wa) < 0) {
        printf("TIMEOUT (syncpt %u thresh %u)\n", sp_memory, thresh);
    } else {
        printf("Done (syncpt=%u val=%u)\n", sp_memory, wa.value);
    }

    /* The 0x100 buffer is the ISP's, not ours: the hardware writes a small
     * per-frame record into it every frame and the library reads it back in
     * GetStats. We have been handing it a page and never once looking at
     * what came back -- the only channel the block has to talk to us. */
    {
        uint32_t st[16];
        memset(st, 0, sizeof(st));
        if (nvmap_read(param_h, 0, st, sizeof(st)) == 0) {
            int any = 0;
            for (int i = 0; i < 16; i++) if (st[i] != opt_param_fill) any = 1;
            printf("Stats block at 0x100 (%s):\n",
                   any ? "ISP WROTE SOMETHING" : "unchanged");
            for (int i = 0; i < 16; i += 4)
                printf("  +0x%02x: %08x %08x %08x %08x\n",
                       i * 4, st[i], st[i+1], st[i+2], st[i+3]);
        }
    }

    /* Read back per plane: first bytes, a non-zero count and the byte range.
     * For a packed RGBA plane report each channel separately -- "R is always
     * 255, B never leaves 0..115" is the whole diagnosis in one line. */
    {
        uint8_t *buf = malloc(chunk);
        for (int pl = 0; pl < L.planes; pl++) {
            /* Scanning a 30 MB plane byte by byte costs hundreds of nvmap
             * reads for numbers a few megabytes already settle. Bounded by
             * default; --stat-bytes=0 scans the whole plane. */
            uint32_t sz = L.size[pl] ? L.size[pl] : (uint32_t)chunk;
            if (opt_stat_bytes && sz > opt_stat_bytes) sz = opt_stat_bytes;
            uint8_t head[16];
            nvmap_read(out_h, L.offset[pl], head, 16);
            printf("plane %d (%s) @0x%06x:", pl, plane_name(&L, pl), L.offset[pl]);
            for (int i = 0; i < 16; i++) printf(" %02x", head[i]);
            printf("\n");

            uint32_t nz = 0;
            uint32_t lo[4] = { 255, 255, 255, 255 }, hi[4] = { 0, 0, 0, 0 };
            uint64_t sum[4] = { 0, 0, 0, 0 };
            uint32_t cnt[4] = { 0, 0, 0, 0 };
            int chans = plane_channels(&L);
            for (uint32_t off = 0; off < sz; off += chunk) {
                uint32_t part = (sz - off < (uint32_t)chunk) ? sz - off : (uint32_t)chunk;
                if (nvmap_read(out_h, L.offset[pl] + off, buf, part) < 0) break;
                for (uint32_t i = 0; i < part; i++) {
                    uint8_t v = buf[i];
                    if (v) nz++;
                    int c = chans == 4 ? (int)((off + i) & 3) : 0;
                    if (v < lo[c]) lo[c] = v;
                    if (v > hi[c]) hi[c] = v;
                    sum[c] += v; cnt[c]++;
                }
            }
            printf("  scanned=%u of %u nonzero=%u (%.1f%%)\n",
                   sz, L.size[pl], nz, sz ? 100.0 * nz / sz : 0.0);
            static const char *cn[4] = { "R", "G", "B", "A" };
            for (int c = 0; c < chans; c++)
                printf("  %s min=%u max=%u mean=%.1f\n",
                       chans == 4 ? cn[c] : plane_name(&L, pl),
                       lo[c], hi[c], cnt[c] ? (double)sum[c] / cnt[c] : 0.0);

            /* Say the fill colour out loud. Configurations that all read
             * as "solid" can still differ in the value they fill with,
             * and that difference is the only signal such a run carries. */
            int flat = 1;
            for (int c = 0; c < chans; c++)
                if (lo[c] != hi[c]) flat = 0;
            if (flat) {
                printf("  SOLID FILL:");
                for (int c = 0; c < chans; c++) printf(" %02x", lo[c]);
                if (chans == 4)
                    printf("   (R=%u G=%u B=%u A=%u)",
                           lo[0], lo[1], lo[2], lo[3]);
                printf("\n");
            }
        }
        free(buf);
    }

    /* Dump: the whole surface, plus one file per plane so a multi-plane
     * output can be looked at without slicing it by hand. Skipped by
     * --no-dump: a sweep only needs the numbers above, and the dump is
     * some thirty megabytes of nvmap reads and a file write per run. */
    if (!opt_dump) printf("Dump: skipped (--no-dump)\n");
    if (opt_dump)
    {
        uint8_t *buf = malloc(chunk);
        char outpath[160];
        snprintf(outpath, sizeof(outpath),
                 "/data/local/tmp/isp_%08x_%ux%u.bin", L.fmt, W, H);
        FILE *fp = fopen(outpath, "wb");
        if (fp) {
            for (uint32_t off = 0; off < L.total; off += chunk) {
                uint32_t sz = (L.total - off < (uint32_t)chunk) ? L.total - off : (uint32_t)chunk;
                nvmap_read(out_h, off, buf, sz);
                fwrite(buf, 1, sz, fp);
            }
            fclose(fp);
            printf("Saved %s (%u bytes)\n", outpath, L.total);
        }
        /* Per-plane files only for a genuinely multi-plane format: on a
         * packed surface with --out-planes=3 they would be copies. */
        if (L.planes > 1 && plane_channels(&L) == 1) {
            for (int pl = 0; pl < L.planes; pl++) {
                if (!L.size[pl]) continue;
                snprintf(outpath, sizeof(outpath),
                         "/data/local/tmp/isp_%08x_%ux%u_%s.bin",
                         L.fmt, W, H, plane_name(&L, pl));
                fp = fopen(outpath, "wb");
                if (!fp) continue;
                for (uint32_t off = 0; off < L.size[pl]; off += chunk) {
                    uint32_t sz = (L.size[pl] - off < (uint32_t)chunk)
                                ? L.size[pl] - off : (uint32_t)chunk;
                    nvmap_read(out_h, L.offset[pl] + off, buf, sz);
                    fwrite(buf, 1, sz, fp);
                }
                fclose(fp);
                printf("Saved %s (%u bytes, stride %u)\n",
                       outpath, L.size[pl], L.stride[pl]);
            }
        }
        free(buf);
    }

    /* Put the ISP back down before we exit. The tool has always walked
     * away with 0x015 still enabled and the input trigger still armed,
     * leaving the block live with no one driving it -- which is the shape
     * of a device that runs a couple of frames and then goes sluggish.
     * Clearing the enable is the least we can do; --no-teardown skips it
     * so the old behaviour stays measurable. */
    if (opt_teardown) {
        uint32_t td[8];
        int t = 0;
        td[t++] = OP_SETCLASS(ISP_CLASS, 0, 0);
        td[t++] = OP_INCR(0xE30, 1);
        td[t++] = 0x00000000;          /* disarm the input trigger */
        td[t++] = OP_INCR(0x015, 1);
        td[t++] = 0x00000000;          /* ISP_ENABLE off */

        uint32_t td_h = nvmap_create(4096);
        if (td_h) {
            nvmap_alloc(td_h);
            nvmap_write(td_h, 0, td, t * 4);
            uint32_t g1[2] = { (4 << 28) | sp_memory, 0 };
            nvmap_write(td_h, 256, g1, 8);

            struct nvhost_cmdbuf tcb[2] = {
                { td_h, 0, (uint32_t)t }, { td_h, 256, 2 }
            };
            struct nvhost_syncpt_incr tsi = { sp_memory, 1 };
            uint32_t tcls[2] = { (uint32_t)ISP_CLASS, (uint32_t)ISP_CLASS };
            struct nvhost_fence tf = { 0, 0 };
            struct nvhost32_submit_args tsa;
            memset(&tsa, 0, sizeof(tsa));
            tsa.num_syncpt_incrs = 1;
            tsa.num_cmdbufs = 2;
            tsa.timeout = 5000;
            tsa.syncpt_incrs = (uint32_t)(uintptr_t)&tsi;
            tsa.cmdbufs = (uint32_t)(uintptr_t)tcb;
            tsa.class_ids = (uint32_t)(uintptr_t)tcls;
            tsa.fences = (uint32_t)(uintptr_t)&tf;
            if (ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &tsa) < 0) {
                perror("teardown submit");
            } else {
                struct nvhost_ctrl_syncpt_waitex_args tw = {
                    .id = sp_memory, .thresh = tsa.fence, .timeout = 2000
                };
                ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_WAITEX, &tw);
                printf("Teardown: 0xE30=0, 0x015=0\n");
            }
            nvmap_free(td_h);
        }
    }

    /* Release the IOVMM before we go, in pin order's reverse. */
    nvmap_unpin(param_h); nvmap_unpin(work_h);
    nvmap_unpin(out_h);   nvmap_unpin(in_h);
    nvmap_free(cmd_h);   nvmap_free(param_h); nvmap_free(work_h);
    nvmap_free(out_h);   nvmap_free(in_h);
    printf("Released: unpinned and freed the frame buffers\n");

    close(ctrl_fd);
    close(isp_fd);
    close(nvmap_fd);
    printf("=== Done ===\n");
    return 0;
}
