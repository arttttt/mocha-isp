/*
 * vicap — pull one frame out of the sensor through VI, into memory.
 *
 * No ISP yet and no NVIDIA userspace at all. The sensor already streams
 * from our own code (/dev/imx179 takes a power call and a mode, the tables
 * live in the kernel driver), and the VI registers answer a read through
 * nvhost's module register ioctl. This puts the two together: configure the
 * CSI channel, point it at an nvmap buffer, fire a single shot, and see
 * what lands.
 *
 * Everything here comes from the 24.1 tree's camera driver -- registers.h
 * for the offsets, core.c for the format codes and the word count, and
 * channel.c for the order the writes go in. Nothing is guessed; if a frame
 * does not arrive, the missing piece is the CSI PHY bring-up, which the
 * stock VI driver does and we do not.
 *
 * Build: tools/vicap/build-vicap.sh (on the build server)
 * Usage: ./vicap [--width=N] [--height=N] [--sensor=imx179|ov5693]
 *                [--port=N] [--no-sensor] [--dump]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* The CSI pads sit in deep power down until something asks the PMC to let
 * them out, and reading the register confirms it: CSIE comes up as
 * 0x80001000, the "sleep on" code with its own bit set. The kernel puts it
 * back when the sensor is powered down, so releasing it from a separate
 * program does not survive to the capture -- it has to happen here, after
 * the sensor is up. Offsets from the stock kernel's pmc.c; CSIE is bit 12
 * of the second request register per the driver's own table. */
#define PMC_BASE            0x7000E400UL
#define PMC_IO_DPD2_REQ     0x1C0
#define PMC_DPD_CODE_OFF    0x40000000u
#define PMC_DPD_BIT_CSIE    (1u << 12)

static int pmc_dpd_release(uint32_t bit)
{
    long page = sysconf(_SC_PAGESIZE);
    unsigned long addr = PMC_BASE + PMC_IO_DPD2_REQ;
    unsigned long base = addr & ~(unsigned long)(page - 1);
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { printf("dpd: /dev/mem %s\n", strerror(errno)); return -1; }
    void *m = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)base);
    if (m == MAP_FAILED) {
        printf("dpd: mmap %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    volatile uint32_t *r = (volatile uint32_t *)((char *)m + (addr - base));
    uint32_t before = *r;
    *r = PMC_DPD_CODE_OFF | bit;
    __sync_synchronize();
    uint32_t after = *r;
    printf("  CSI pads out of deep power down: 0x%08x -> 0x%08x\n",
           before, after);
    munmap(m, (size_t)page * 2);
    close(fd);
    return 0;
}

/* ---- sensor node (stock kernel include/media/imx179.h) ---- */
struct sensor_mode {
    int xres, yres;
    uint32_t frame_length, coarse_time;
    uint16_t gain;
};
#define SENSOR_IOCTL_SET_MODE   _IOW('o', 1, struct sensor_mode)
#define SENSOR_IOCTL_SET_POWER  _IOW('o', 20, uint32_t)

/* The front sensor has its own shape entirely: a wider mode struct, and no
 * power call at all -- opening the node powers it up and closing it powers
 * it down. Modes: 2592x1944, 1920x1080, 1296x972, 1280x720. */
struct ov5693_mode {
    int res_x, res_y, fps;
    uint32_t frame_length, coarse_time, coarse_time_short;
    uint16_t gain;
    uint8_t hdr_en;
};
#define OV5693_IOCTL_SET_MODE   _IOW('o', 1, struct ov5693_mode)

/* ---- nvmap ---- */
#define NVMAP_IOC_MAGIC 'N'
struct nvmap_create_handle {
    union { uint32_t id; uint32_t size; int32_t fd; };
    uint32_t handle;
};
struct nvmap_alloc_handle { uint32_t handle, heap_mask, flags, align; };
struct nvmap_rw_handle {
    unsigned long addr; uint32_t handle, offset, elem_size,
                  hmem_stride, user_stride, count;
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

/* ---- nvhost register access ---- */
#define NVHOST_IOCTL_MAGIC 'H'
struct regrdwr_args {
    uint32_t id, num_offsets, block_size, offsets, values, write;
};
struct nvhost_get_param_arg { uint32_t param, value; };
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT \
    _IOWR(NVHOST_IOCTL_MAGIC, 16, struct nvhost_get_param_arg)
#define NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR \
    _IOWR(NVHOST_IOCTL_MAGIC, 14, struct regrdwr_args)

/* ---- VI / CSI registers (24.1 registers.h, t124_registers.h) ---- */
#define VI_CSI_BASE(n)              (0x100 + (n) * 0x100)
#define VI_CSI_SINGLE_SHOT          0x004
#define VI_CSI_IMAGE_DEF            0x00c
#define VI_CSI_IMAGE_SIZE           0x018
#define VI_CSI_IMAGE_SIZE_WC        0x01c
#define VI_CSI_IMAGE_DT             0x020
#define VI_CSI_SURFACE0_OFFSET_MSB  0x024
#define VI_CSI_SURFACE0_OFFSET_LSB  0x028
#define VI_CSI_SURFACE0_STRIDE      0x054
#define VI_CSI_ERROR_STATUS         0x084

#define BYPASS_PXL_TRANSFORM_OFFSET 24
#define IMAGE_DEF_FORMAT_OFFSET     16
#define IMAGE_SIZE_HEIGHT_OFFSET    16
#define SINGLE_SHOT_CAPTURE         0x1

/* Port B's command register is 0x87C -- 0x86C is its INPUT_STREAM_CONTROL,
 * and having the wrong one here meant the final write landed on top of the
 * stock value the bring-up had just put there. */
#define PP_A_PIXEL_STREAM_PP_COMMAND 0x848
#define PP_B_PIXEL_STREAM_PP_COMMAND 0x87C
#define CSI_PP_ENABLE                0x1
#define CSI_PP_SINGLE_SHOT_ENABLE    (0x1 << 2)
#define CSI_PP_START_MARKER_FRAME_MAX_OFFSET 12

#define IMAGE_FORMAT_T_R16_I        32      /* raw pass-through */
#define IMAGE_DT_RAW10              43

/* CSI receiver, absolute offsets in the VI aperture (t124_registers.h) */
#define T124_PP_A_PIXEL_STREAM_PP_COMMAND    PP_A_PIXEL_STREAM_PP_COMMAND
#define T124_PP_A_INPUT_STREAM_CONTROL       0x838
#define T124_PP_A_PIXEL_STREAM_CONTROL0      0x83C
#define T124_PP_A_PIXEL_STREAM_CONTROL1      0x840
#define T124_PP_A_PIXEL_STREAM_GAP           0x844
#define T124_PP_A_PIXEL_STREAM_EXPECTED_FRAME 0x84C
#define T124_PP_A_PIXEL_STREAM_PP_INT_MASK   0x850
#define T124_PP_A_PIXEL_PARSER_STATUS        0x854
#define T124_CSI_PHY_CIL_COMMAND             0x908
#define T124_CILA_PAD_CONFIG0                0x92C
#define T124_PHY_CILA_CONTROL0               0x934
#define T124_CSI_CIL_A_INT_MASK              0x938
#define T124_CSI_CIL_A_STATUS                0x93C
#define T124_CSI_CILA_STATUS                 0x940
#define T124_CILB_PAD_CONFIG0                0x960
#define T124_PP_B_INPUT_STREAM_CONTROL       0x86C
#define T124_PP_B_PIXEL_STREAM_CONTROL0      0x870
#define T124_PP_B_PIXEL_STREAM_CONTROL1      0x874
#define T124_PP_B_PIXEL_STREAM_GAP           0x878
#define T124_PP_B_PIXEL_STREAM_PP_COMMAND    0x87C
#define T124_PP_B_PIXEL_STREAM_EXPECTED_FRAME 0x880
#define T124_PP_B_PIXEL_STREAM_PP_INT_MASK   0x884
#define T124_PP_B_PIXEL_PARSER_STATUS        0x888
#define T124_CILE_PAD_CONFIG0                0xA08
#define T124_PHY_CILE_CONTROL0               0xA10
#define T124_CSI_CIL_E_INT_MASK              0xA14
#define T124_PHY_CILB_CONTROL0               0x968
#define T124_CSI_CIL_B_INT_MASK              0x96C
/* The CSI block sits at 0x838 in the VI aperture: 0x838+0xF4 is CILA and
 * 0x838+0xD0 is the PHY command, both matching the absolute table, so the
 * two registers that only exist as relative offsets resolve from there. */
#define T124_CSI_CLKEN_OVERRIDE              (0x838 + 0x218)
#define T124_CSI_DEBUG_CONTROL               (0x838 + 0x21C)

#define TEGRA_VI_CFG_VI_INCR_SYNCPT     0x000
#define T124_PPA_FRAME_START            9
#define T124_PPB_FRAME_START            10
#define T124_MWA_ACK_DONE               6
#define T124_MWB_ACK_DONE               7

#define BRICK_CLOCK_A_4X                (0x1 << 16)
#define T124_CIL_PHY_CONTROL_DEFAULT    0x09
#define T124_CIL_A_ENABLE               0x0001
#define T124_CIL_B_ENABLE               0x0100
#define T124_CIL_AB_4LANE               (T124_CIL_A_ENABLE | T124_CIL_B_ENABLE)
#define T124_CIL_CMD_HI_MASK            0xFFFF0000
#define T124_PP_FRAME_MIN_GAP           0x14
#define T124_CSI_DEBUG_COUNTER_CFG      0x454340E1
#define PP_FRAME_MIN_GAP_OFFSET         16
#define CSI_SKIP_PACKET_THRESHOLD_OFFSET 16
#define CSI_PP_RST                      0x3
#define CSI_PP_PACKET_HEADER_SENT       (0x1 << 4)
#define CSI_PP_DATA_IDENTIFIER_ENABLE   (0x1 << 5)
#define CSI_PP_WORD_COUNT_SELECT_HEADER (0x1 << 6)
#define CSI_PP_CRC_CHECK_ENABLE         (0x1 << 7)
#define CSI_PP_WC_CHECK                 (0x1 << 8)
#define CSI_PP_OUTPUT_FORMAT_STORE      (0x3 << 16)
#define CSI_PPA_PAD_LINE_NOPAD          (0x2 << 24)
#define CSI_PP_HEADER_EC_DISABLE        (0x1 << 27)
#define CSI_PPA_PAD_FRAME_NOPAD         (0x2 << 28)
#define CSI_PP_TOP_FIELD_FRAME_OFFSET   0
#define CSI_PP_TOP_FIELD_FRAME_MASK_OFFSET 4

static int nvmap_fd = -1, vi_fd = -1;

static int vi_reg(uint32_t off, uint32_t *val, int write)
{
    uint32_t offsets[1] = { off }, values[1] = { *val };
    struct regrdwr_args a;
    memset(&a, 0, sizeof a);
    a.id = 0;
    a.num_offsets = 1;
    a.block_size = 4;
    a.offsets = (uint32_t)(uintptr_t)offsets;
    a.values = (uint32_t)(uintptr_t)values;
    a.write = (uint32_t)write;
    int rc = ioctl(vi_fd, NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR, &a);
    if (rc == 0 && !write) *val = values[0];
    return rc;
}
/* Writes go into a batch and leave together. nvhost powers the module for
 * the duration of one ioctl and lets it go again afterwards, so a setup
 * spread over forty separate calls is forty separate power-ups: the
 * receiver never stays on long enough to lock, which is what a correct
 * configuration that captures nothing looks like. One call keeps the whole
 * sequence inside a single powered window. */
#define VI_BATCH_MAX 64
static uint32_t batch_off[VI_BATCH_MAX], batch_val[VI_BATCH_MAX];
static int batch_n;

static void vi_wr(uint32_t off, uint32_t val)
{
    if (batch_n >= VI_BATCH_MAX) { printf("  batch full, dropping 0x%03x\n", off); return; }
    batch_off[batch_n] = off;
    batch_val[batch_n] = val;
    batch_n++;
}

static int vi_flush(const char *what)
{
    if (!batch_n) return 0;
    struct regrdwr_args a;
    memset(&a, 0, sizeof a);
    a.id = 0;
    a.num_offsets = (uint32_t)batch_n;
    a.block_size = 4;
    a.offsets = (uint32_t)(uintptr_t)batch_off;
    a.values = (uint32_t)(uintptr_t)batch_val;
    a.write = 1;
    errno = 0;
    int rc = ioctl(vi_fd, NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR, &a);
    printf("  %s: %d registers in one call, rc=%d (%s)\n",
           what, batch_n, rc, rc == 0 ? "ok" : strerror(errno));
    batch_n = 0;
    return rc;
}
static uint32_t vi_rd(uint32_t off)
{
    uint32_t v = 0;
    if (vi_reg(off, &v, 0)) return 0xdeadbeef;
    return v;
}

static uint32_t nvmap_create(uint32_t size) {
    struct nvmap_create_handle ch = { .size = size };
    if (ioctl(nvmap_fd, NVMAP_IOC_CREATE, &ch) < 0) { perror("nvmap create"); return 0; }
    return ch.handle;
}
static int nvmap_alloc(uint32_t h) {
    struct nvmap_alloc_handle ah = { h, NVMAP_HEAP_IOVMM,
                                     NVMAP_HANDLE_WRITE_COMBINE, 4096 };
    if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah) < 0) { perror("nvmap alloc"); return -1; }
    return 0;
}
static uint32_t nvmap_pin(uint32_t h) {
    struct nvmap_pin_handle ph = { h, 0, 1 };
    if (ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &ph) < 0) { perror("nvmap pin"); return 0; }
    return (uint32_t)ph.addr;
}
static void nvmap_unpin(uint32_t h) {
    struct nvmap_pin_handle ph = { h, 0, 1 };
    ioctl(nvmap_fd, NVMAP_IOC_UNPIN_MULT, &ph);
}
static int nvmap_rw(uint32_t h, uint32_t off, void *d, uint32_t n, int wr) {
    struct nvmap_rw_handle rw = { (unsigned long)d, h, off, n, n, n, 1 };
    return ioctl(nvmap_fd, wr ? NVMAP_IOC_WRITE : NVMAP_IOC_READ, &rw);
}

int main(int argc, char **argv)
{
    /* Default to the front camera: it is the one that still works through
     * the stock app, so its live register values are on record and a
     * mismatch means our configuration, not the hardware. The rear does not
     * stream through the camera stack at all, which leaves any negative
     * result there impossible to attribute. */
    /* The front sensor's stock session runs at its full 2592x1944, and the
     * image definition it uses on this channel was read off that session. */
    unsigned W = 2592, H = 1944, port = 1;
    const char *sensor = "ov5693";
    int use_sensor = 1, dump = 0, front = 1;
    uint32_t image_def = 0x00200004;
    uint32_t frame_length = 2064, coarse_time = 2000, gain = 16;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--width=", 8) == 0)       W = (unsigned)strtoul(a + 8, 0, 0);
        else if (strncmp(a, "--height=", 9) == 0) H = (unsigned)strtoul(a + 9, 0, 0);
        else if (strncmp(a, "--port=", 7) == 0)   port = (unsigned)strtoul(a + 7, 0, 0);
        else if (strncmp(a, "--sensor=", 9) == 0) {
            sensor = a + 9;
            front = (strcmp(sensor, "ov5693") == 0);
            if (!front) port = 0;
        }
        else if (strcmp(a, "--no-sensor") == 0)   use_sensor = 0;
        else if (strcmp(a, "--dump") == 0)        dump = 1;
        else if (strncmp(a, "--image-def=", 12) == 0)
            image_def = (uint32_t)strtoul(a + 12, 0, 16);
        else if (strncmp(a, "--frame-length=", 15) == 0)
            frame_length = (uint32_t)strtoul(a + 15, 0, 0);
        else if (strncmp(a, "--coarse=", 9) == 0)
            coarse_time = (uint32_t)strtoul(a + 9, 0, 0);
        else if (strncmp(a, "--gain=", 7) == 0)
            gain = (uint32_t)strtoul(a + 7, 0, 0);
        else { printf("unknown option %s\n", a); return 1; }
    }

    uint32_t base = VI_CSI_BASE(port);
    uint32_t stride = W * 2;                 /* RAW10 lands in 16-bit words */
    uint32_t frame = stride * H;
    uint32_t wc = W * 10 / 8;                /* core.c: width * bpp / 8 */

    printf("=== vicap: %s %ux%u, CSI port %u ===\n", sensor, W, H, port);
    printf("stride %u, frame %u bytes, word count %u\n", stride, frame, wc);

    nvmap_fd = open("/dev/nvmap", O_RDWR | O_SYNC);
    vi_fd = open("/dev/nvhost-vi", O_RDWR);
    if (nvmap_fd < 0 || vi_fd < 0) {
        printf("open failed: nvmap=%d vi=%d\n", nvmap_fd, vi_fd);
        return 1;
    }

    /* The channel's own syncpoint, asked for rather than assumed. */
    struct nvhost_get_param_arg gp = { .param = 0, .value = 0 };
    uint32_t sp_id = 0;
    if (ioctl(vi_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gp) == 0)
        sp_id = gp.value;
    printf("VI syncpoint: %u\n", sp_id);

    uint32_t buf_h = nvmap_create(frame);
    if (!buf_h || nvmap_alloc(buf_h)) return 1;
    uint32_t iova = nvmap_pin(buf_h);
    printf("frame buffer: handle %u, iova 0x%08x\n", buf_h, iova);
    if (!iova) return 1;

    /* Fill with a pattern, so anything the hardware writes is visible as
     * a change rather than being confused with a buffer that was empty. */
    {
        uint32_t chunk = 65536;
        uint8_t *p = malloc(chunk);
        memset(p, 0xA5, chunk);
        for (uint32_t o = 0; o < frame; o += chunk)
            nvmap_rw(buf_h, o, p, frame - o < chunk ? frame - o : chunk, 1);
        free(p);
    }

    int sfd = -1;
    if (use_sensor) {
        char sn[64];
        snprintf(sn, sizeof sn, "/dev/%s", sensor);
        sfd = open(sn, O_RDWR);
        if (sfd < 0) { printf("open %s: %s\n", sn, strerror(errno)); return 1; }
        if (front) {
            /* Opening the node already powered it; just pick a mode. */
            /* The driver writes the mode table and then writes exposure
             * from these fields unconditionally -- passing zeros programs
             * the sensor with no frame length and no integration time,
             * which is a part that streams nothing. */
            struct ov5693_mode m;
            memset(&m, 0, sizeof m);
            m.res_x = (int)W;
            m.res_y = (int)H;
            m.fps = 30;
            m.frame_length = frame_length;
            m.coarse_time = coarse_time;
            m.gain = (uint16_t)gain;
            if (ioctl(sfd, OV5693_IOCTL_SET_MODE, &m) < 0)
                printf("sensor mode: %s\n", strerror(errno));
            else
                printf("sensor streaming at %ux%u\n", W, H);
        } else {
            uint32_t on = 1;
            if (ioctl(sfd, SENSOR_IOCTL_SET_POWER, &on) < 0)
                printf("sensor power: %s\n", strerror(errno));
            struct sensor_mode m = { (int)W, (int)H, 0, 0, 0 };
            if (ioctl(sfd, SENSOR_IOCTL_SET_MODE, &m) < 0)
                printf("sensor mode: %s\n", strerror(errno));
            else
                printf("sensor streaming at %ux%u\n", W, H);
        }
    }

    /* CSI receiver bring-up, in the order csi.c does it for a T124 with the
     * sensor on port A: four lanes across brick 0, CILA and CILB. Without
     * this the channel takes its configuration and then waits forever,
     * which is exactly what the first run did -- SINGLE_SHOT stayed armed
     * and the buffer kept its fill pattern. Offsets are the absolute ones
     * from t124_registers.h, the same space the PP command already
     * answered in. */
    /* For the front camera these are not derived values -- they are what a
     * live stock session actually had in these registers, read back while
     * its preview was running. Copying them verbatim is the point: if the
     * capture still does not happen with the working configuration in
     * place, the missing piece is somewhere other than the CSI setup. */
    pmc_dpd_release(front ? PMC_DPD_BIT_CSIE : (1u << 0) /* CSIA */);

    if (front) {
        printf("bringing up the CSI receiver (port B / CIL E, from stock)\n");
        vi_wr(T124_CSI_CLKEN_OVERRIDE, 0);

        vi_wr(T124_PP_B_PIXEL_PARSER_STATUS, 0xFFFFFFFF);
        vi_wr(T124_CSI_CIL_E_INT_MASK, 0x0);
        vi_wr(T124_CILE_PAD_CONFIG0, 0x0);
        vi_wr(T124_PHY_CILE_CONTROL0, T124_CIL_PHY_CONTROL_DEFAULT);

        vi_wr(T124_CSI_PHY_CIL_COMMAND, 0x00000202);

        vi_wr(T124_PP_B_PIXEL_STREAM_PP_COMMAND,
              (0xFu << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
              CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_RST);
        vi_wr(T124_PP_B_PIXEL_STREAM_PP_INT_MASK, 0x0);
        vi_wr(T124_PP_B_PIXEL_STREAM_CONTROL0, 0x080301f1);
        vi_wr(T124_PP_B_PIXEL_STREAM_CONTROL1,
              (0x1u << CSI_PP_TOP_FIELD_FRAME_OFFSET) |
              (0x1u << CSI_PP_TOP_FIELD_FRAME_MASK_OFFSET));
        vi_wr(T124_PP_B_PIXEL_STREAM_GAP, 0x00140000);
        vi_wr(T124_PP_B_PIXEL_STREAM_EXPECTED_FRAME, 0x0);
        vi_wr(T124_PP_B_INPUT_STREAM_CONTROL, 0x007f0014);
        vi_wr(T124_CSI_DEBUG_CONTROL, T124_CSI_DEBUG_COUNTER_CFG);
        vi_wr(T124_PP_B_PIXEL_STREAM_PP_COMMAND, 0x0000f005);
    } else {
    printf("bringing up the CSI receiver (port A, 4 lanes)\n");
    vi_wr(T124_CSI_CLKEN_OVERRIDE, 0);

    vi_wr(T124_PP_A_PIXEL_PARSER_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CIL_A_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CILA_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CIL_A_INT_MASK, 0x0);
    vi_wr(T124_CSI_CIL_B_INT_MASK, 0x0);

    vi_wr(T124_CILA_PAD_CONFIG0, BRICK_CLOCK_A_4X);
    vi_wr(T124_CILB_PAD_CONFIG0, 0x0);
    vi_wr(T124_PHY_CILA_CONTROL0, T124_CIL_PHY_CONTROL_DEFAULT);
    vi_wr(T124_PHY_CILB_CONTROL0, T124_CIL_PHY_CONTROL_DEFAULT);

    /* Enable both halves of the brick, preserving the other brick's bits. */
    {
        uint32_t cil = vi_rd(T124_CSI_PHY_CIL_COMMAND);
        uint32_t val = (cil & T124_CIL_CMD_HI_MASK) | T124_CIL_AB_4LANE;
        vi_wr(T124_CSI_PHY_CIL_COMMAND, val);
        printf("  PHY_CIL_COMMAND 0x%08x -> 0x%08x\n", cil, val);
    }

    /* Pixel parser: reset, then configure, then enable. */
    vi_wr(T124_PP_A_PIXEL_STREAM_PP_COMMAND,
          (0xFu << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
          CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_RST);
    vi_wr(T124_PP_A_PIXEL_STREAM_PP_INT_MASK, 0x0);
    vi_wr(T124_PP_A_PIXEL_STREAM_CONTROL0,
          CSI_PP_PACKET_HEADER_SENT | CSI_PP_DATA_IDENTIFIER_ENABLE |
          CSI_PP_WORD_COUNT_SELECT_HEADER | CSI_PP_CRC_CHECK_ENABLE |
          CSI_PP_WC_CHECK | CSI_PP_OUTPUT_FORMAT_STORE |
          CSI_PPA_PAD_LINE_NOPAD | CSI_PP_HEADER_EC_DISABLE |
          CSI_PPA_PAD_FRAME_NOPAD | 0 /* port A */);
    vi_wr(T124_PP_A_PIXEL_STREAM_CONTROL1,
          (0x1u << CSI_PP_TOP_FIELD_FRAME_OFFSET) |
          (0x1u << CSI_PP_TOP_FIELD_FRAME_MASK_OFFSET));
    vi_wr(T124_PP_A_PIXEL_STREAM_GAP,
          T124_PP_FRAME_MIN_GAP << PP_FRAME_MIN_GAP_OFFSET);
    vi_wr(T124_PP_A_PIXEL_STREAM_EXPECTED_FRAME, 0x0);
    vi_wr(T124_PP_A_INPUT_STREAM_CONTROL,
          (0x3fu << CSI_SKIP_PACKET_THRESHOLD_OFFSET) | (4 - 1));
    vi_wr(T124_CSI_DEBUG_CONTROL, T124_CSI_DEBUG_COUNTER_CFG);
    }

    /* Channel setup, in the order channel.c writes it. bypass_pixel_transform
     * is 1 here: we want the raw bayer in memory, not a converted image. */
    printf("configuring CSI channel at 0x%03x\n", base);
    vi_wr(base + VI_CSI_ERROR_STATUS, 0xFFFFFFFF);
    /* Measured off a live stock session on this channel: 0x00200004. The
     * transform-bypass bit is CLEAR there, where we had been setting it,
     * and the low nibble carries a 4 we had left at zero. Bypass off is
     * what the driver does whenever the ISP is in the path. */
    vi_wr(base + VI_CSI_IMAGE_DEF, image_def);
    vi_wr(base + VI_CSI_IMAGE_DT, IMAGE_DT_RAW10);
    vi_wr(base + VI_CSI_IMAGE_SIZE_WC, wc);
    vi_wr(base + VI_CSI_IMAGE_SIZE, (H << IMAGE_SIZE_HEIGHT_OFFSET) | W);
    vi_wr(base + VI_CSI_SURFACE0_OFFSET_MSB, 0);
    vi_wr(base + VI_CSI_SURFACE0_OFFSET_LSB, iova);
    vi_wr(base + VI_CSI_SURFACE0_STRIDE, stride);

    /* Pixel parser: single shot, armed for one frame. */
    uint32_t pp = (port == 0) ? PP_A_PIXEL_STREAM_PP_COMMAND
                             : PP_B_PIXEL_STREAM_PP_COMMAND;
    vi_wr(pp, (0xFu << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
              CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_ENABLE);

    /* Arm the syncpoint conditions before the shot. VI_INCR_SYNCPT is a
     * command register -- it takes a condition and a syncpoint id and arms
     * one increment -- so it reads back as zero and never showed up in the
     * comparison against stock. The memory-write acknowledge is the one
     * that has to be armed before the DMA starts. */
    vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT,
          (front ? T124_MWB_ACK_DONE : T124_MWA_ACK_DONE) << 8 | sp_id);
    vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT,
          (front ? T124_PPB_FRAME_START : T124_PPA_FRAME_START) << 8 | sp_id);

    /* And the trigger goes in the same batch: the receiver has to still be
     * powered and configured when the shot is fired, which is the whole
     * reason for doing this in one call. */
    vi_wr(base + VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);
    vi_flush("setup + single shot");
    usleep(200000);

    printf("readback: IMAGE_DEF=0x%08x DT=0x%08x SIZE=0x%08x WC=0x%08x\n",
           vi_rd(base + VI_CSI_IMAGE_DEF), vi_rd(base + VI_CSI_IMAGE_DT),
           vi_rd(base + VI_CSI_IMAGE_SIZE), vi_rd(base + VI_CSI_IMAGE_SIZE_WC));
    printf("readback: SURFACE0=0x%08x STRIDE=0x%08x PP=0x%08x\n",
           vi_rd(base + VI_CSI_SURFACE0_OFFSET_LSB),
           vi_rd(base + VI_CSI_SURFACE0_STRIDE), vi_rd(pp));

    uint32_t err = vi_rd(base + VI_CSI_ERROR_STATUS);
    printf("ERROR_STATUS = 0x%08x, SINGLE_SHOT = 0x%08x\n",
           err, vi_rd(base + VI_CSI_SINGLE_SHOT));

    /* Did anything land? The buffer was filled with 0xA5. */
    {
        uint32_t chunk = 65536, scan = frame < (2u << 20) ? frame : (2u << 20);
        uint8_t *p = malloc(chunk);
        uint32_t changed = 0, nz = 0;
        uint8_t head[16];
        nvmap_rw(buf_h, 0, head, 16, 0);
        printf("first bytes:");
        for (int i = 0; i < 16; i++) printf(" %02x", head[i]);
        printf("\n");
        for (uint32_t o = 0; o < scan; o += chunk) {
            uint32_t part = scan - o < chunk ? scan - o : chunk;
            if (nvmap_rw(buf_h, o, p, part, 0) < 0) break;
            for (uint32_t i = 0; i < part; i++) {
                if (p[i] != 0xA5) changed++;
                if (p[i]) nz++;
            }
        }
        printf("scanned %u bytes: %u differ from the fill, %u non-zero\n",
               scan, changed, nz);
        free(p);
    }

    if (dump) {
        FILE *f = fopen("/data/local/tmp/vicap.raw", "wb");
        if (f) {
            uint32_t chunk = 65536;
            uint8_t *p = malloc(chunk);
            for (uint32_t o = 0; o < frame; o += chunk) {
                uint32_t part = frame - o < chunk ? frame - o : chunk;
                nvmap_rw(buf_h, o, p, part, 0);
                fwrite(p, 1, part, f);
            }
            free(p);
            fclose(f);
            printf("saved /data/local/tmp/vicap.raw (%u bytes)\n", frame);
        }
    }

    if (sfd >= 0) {
        /* Only the rear sensor has a power call. The front one powers down
         * when its node is closed, and asking it for a power write hits a
         * different command entirely on that driver -- one that writes back
         * through the pointer we hand it. */
        if (!front) {
            uint32_t off = 0;
            ioctl(sfd, SENSOR_IOCTL_SET_POWER, &off);
        }
        close(sfd);
        printf("sensor powered down\n");
    }
    nvmap_unpin(buf_h);
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)buf_h);
    close(vi_fd);
    close(nvmap_fd);
    printf("=== done ===\n");
    return 0;
}
