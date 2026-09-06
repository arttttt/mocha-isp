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

#include "viisp.h"

/* The CSI pads sit in deep power down until something asks the PMC to let
 * them out, and reading the register confirms it: CSIE comes up as
 * 0x80001000, the "sleep on" code with its own bit set. The kernel puts it
 * back when the sensor is powered down, so releasing it from a separate
 * program does not survive to the capture -- it has to happen here, after
 * the sensor is up. Offsets from the stock kernel's pmc.c; CSIE is bit 12
 * of the second request register per the driver's own table. */

/* The physical layer has never been calibrated for this lane: CILE's entry
 * in the calibration block reads zero, meaning it was never even selected.
 *
 * Our first attempt at the sequence started the calibration and then watched
 * it stay ACTIVE for all five hundred polls. The reason was two writes we had
 * left out entirely: the pads run off a bias that has to be switched on
 * first -- the clamp reference in CFG0 raised, the regulator power-down in
 * CFG2 lowered. A calibration launched over unbiased pads has nothing to
 * converge onto, so it never reports done.
 *
 * What follows is the driver's sequence in full, in its order: override the
 * clock gate, clear status, drop DSI, raise the bias, deselect every lane,
 * then select ours and trigger. The steps are numbered as they are there. */

/* The front sensor has its own shape entirely: a wider mode struct, and no
 * power call at all -- opening the node powers it up and closing it powers
 * it down. Modes: 2592x1944, 1920x1080, 1296x972, 1280x720. */

/* ---- VI / CSI registers (24.1 registers.h, t124_registers.h) ---- */
#define VI_CSI_BASE(n)              (0x100 + (n) * 0x100)
#define VI_CSI_SW_RESET             0x000
#define VI_CSI_SINGLE_SHOT          0x004
#define VI_CSI_SINGLE_SHOT_STATE_UPDATE 0x008   /* the stock writes 1 once per session */
#define TEGRA_VI_CFG_DVFS           0x0F0   /* the stock writes 0x10100010; reset value 0x4040007f */
/* VI's bandwidth ioctl (include/linux/nvhost_vi_ioctl.h): a KB/s figure
 * the driver turns into the VI write client's latency allowance. */
#ifndef NVHOST_VI_IOCTL_SET_EMC_INFO
#define NVHOST_VI_IOCTL_SET_EMC_INFO _IOW('V', 2, unsigned int)
#endif
/* The ISP's counterpart (include/linux/nvhost_isp_ioctl.h). The driver
 * derives a bandwidth, isp_clk[kHz]/1000 * bpp_output / 8 MB/s, and sets
 * the latency allowance and PTSA of the ISP write client with it -- memory
 * controller state that outlives the VE power-gating and is cleared only
 * by a reboot. The stock sends clk 81600, bpp_out 16 (162 MB/s, HARD) at
 * every opening; the trace shows it at 2592 and at 1280 alike. */
struct isp_emc_info {
    unsigned int isp_bw;
    unsigned int isp_clk;
    unsigned int bpp_input;
    unsigned int bpp_output;
};
#ifndef NVHOST_ISP_IOCTL_SET_EMC
#define NVHOST_ISP_IOCTL_SET_EMC _IOW('I', 1, struct isp_emc_info)
#endif
#define VI_CSI_IMAGE_DEF            0x00c
/* The two between IMAGE_DEF and IMAGE_SIZE, which we had never written.
 * The stock camera sets them in one run of six with the rest of the group,
 * and its RGB2Y control is not zero -- while our IMAGE_DEF leaves the pixel
 * transform switched on, so the transform has been running all along with
 * whatever those registers happened to hold. */
#define VI_CSI_RGB2Y_CTRL           0x010
#define VI_CSI_MEM_TILING           0x014
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
#define IMAGE_DEF_DEST_MEM          0x1
#define IMAGE_DEF_DEST_ISP_A        0x2
#define IMAGE_DEF_DEST_ISP_B        0x4
#define VI_CSI_ISPINTF_CONFIG       0x064
#define ISPINTF_CONFIG_ENABLE       0x3

/* The ISP side. Class ids as host1x knows them, and the registers the
 * reprocess tool already drives -- geometry, output format, the plane
 * address triplets, the enable and the trigger. What differs here is where
 * the pixels come from: 0xE30 arms the memory input port and 0x00C takes
 * 0x0B for a memory pass, while a frame arriving from VI is 0x05 and no
 * input descriptor at all. */
#define ISP_CLASS_A                 0x32
#define ISP_CLASS_B                 0x34
#define ISP_TRIGGER_SENSOR          0x05
#define ISP_TRIGGER_MEMORY          0x0B

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
#define T124_CSI_CIL_B_INT_MASK              0x96C
#define T124_CSI_CIL_B_STATUS                0x970
#define T124_CSI_CILB_STATUS                 0x974
#define T124_CILC_PAD_CONFIG0                0x994
#define T124_PHY_CILC_CONTROL0               0x99C
#define T124_CSI_CIL_C_INT_MASK              0x9A0
#define T124_CSI_CIL_C_STATUS                0x9A4
#define T124_CSI_CILC_STATUS                 0x9A8
#define T124_CILD_PAD_CONFIG0                0x9C8
#define T124_PHY_CILD_CONTROL0               0x9D0
#define T124_CSI_CIL_D_INT_MASK              0x9D4
#define T124_CSI_CIL_D_STATUS                0x9D8
#define T124_CSI_CILD_STATUS                 0x9DC
#define T124_CSI_CIL_E_STATUS                0xA18
#define T124_CSI_CILE_STATUS                 0xA1C
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

/* The pattern generator, at the parser base plus 0x18C, the same way the
 * lane interface sits at plus 0xF4. */
#define T124_PHY_CILB_CONTROL0               0x968
#define T124_CSI_CIL_B_INT_MASK              0x96C
/* The CSI block sits at 0x838 in the VI aperture: 0x838+0xF4 is CILA and
 * 0x838+0xD0 is the PHY command, both matching the absolute table, so the
 * two registers that only exist as relative offsets resolve from there. */
#define T124_CSI_CLKEN_OVERRIDE              (0x838 + 0x218)
#define T124_CSI_DEBUG_CONTROL               (0x838 + 0x21C)

#define TEGRA_VI_CFG_VI_INCR_SYNCPT     0x000
#define TEGRA_VI_CFG_VI_INCR_SYNCPT_ERROR 0x008
#define TEGRA_VI_CFG_CG_CTRL            0x0B8
#define VI_CSI_SW_RESET                 0x000
/* The 24.1 values. Sweeping the conditions here showed 10 moving -- port B's
 * frame start, which we rely on -- and 12, while 6 and 7 stayed still; but
 * that sweep only says an event did not arrive during one shot, which is
 * equally what a capture that never finishes looks like. The driver these
 * came from does complete captures, so they stand. Completion is watched in
 * the buffer instead, which does not depend on getting this right. */
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

/* The calibration gather, taken verbatim from the 24.1 ISP driver, which in
 * turn captured it off the stock camera on this device. Fifteen hundred
 * words of it, and there is no reconstructing that from a register list --
 * the three or four registers we had been sending in its place were never
 * going to stand in for it.
 *
 * The driver patches the last two words before sending: 0x053 takes 1 and
 * 0x054 takes 0, and there is deliberately no trigger at the end. */
#include "isp_real.h"
#include "stock_opening_720p.h"

/* Keep the output buffer mapped while the ISP writes it.
 *
 * The mapping belongs to the job that carried the relocation, and at 720p
 * the frame lands inside that job's life. Larger frames do not: the memory
 * controller faults on the output buffer and the capture never completes.
 * Parking a job to hold the mapping is not an option -- it outlives the
 * timeout the kernel allows and takes the channel with it.
 *
 * So instead: the frame job parks itself on the frame's own completion and
 * on statistics, so the relocation pins last exactly as long as the writes
 * they guard. No second job, no flood, nothing to strand. */

/* Put the block back to sleep. Without this it stays armed and writes a
 * later frame into a buffer we have already let go -- the memory controller
 * faults on it after the sensor has powered down, which is exactly where
 * the fault lands in the log. */

/* The configuration a real frame actually runs under.
 *
 * The opening rounds carry placeholders: the same blocks, stand-in values.
 * The working numbers arrive later, after the coefficients and after the
 * warm-up frames -- and they are different. 0x800 is {85001000,00100010,
 * 07780a00} here against {85001000,0,0} in the opening, 0xc00 is
 * {00007901,0,01030a20} against {00000101,0,00100000}, and the tail of
 * 0x400 changes entirely. We had been copying the placeholders.
 *
 * These carry the stock camera's geometry, so they belong with its
 * resolution and not with a smaller one.
 */
int isp_real_pass(int isp_fd, uint32_t sp, uint32_t work_iova, uint32_t stats_iova,
                  unsigned W, unsigned H)
{
    uint32_t cmd_h = nvmap_create(4096 * 2);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    /* Three of these blocks carry the session's geometry, and the values
     * here were read out of a 2592-wide stock session: 0x800/0x820 word 2
     * is ((H-32)<<16)|(W-32), 0xc00 word 2 has the width in its low half,
     * and 0x400 differs throughout. At 1280 wide the stock sends its own
     * set (impl-2, pp-status-bits.md: stock_front_720p_full.txt:8823);
     * until 2026-09-06 every 720p run of ours carried the 2592 words. */
    int w720 = (W == 1280);
    const struct { uint16_t m; uint16_t n; uint8_t noninc;
                   const uint32_t *d; } blk[] = {
        { 0x400, 12, 0, w720 ? isp_real_400_720 : isp_real_400 },
        { 0x800, 3, 0, w720 ? isp_real_800_720 : isp_real_800 },
        { 0x820, 3, 0, w720 ? isp_real_800_720 : isp_real_820 },
        { 0xc00, 3, 0, w720 ? isp_real_c00_720 : isp_real_c00 },
        { 0x700, 16, 0, isp_real_700 }, { 0x750, 16, 0, isp_real_750 },
        { 0xd00, 10, 0, w720 ? isp_real_d00_720 : isp_real_d00 },
        { 0xd0a, 1, 0, isp_real_d0a },
        { 0xd0b, 480, 1, w720 ? isp_real_d0b_720 : isp_real_d0b },
        /* Only the 720p configuration has this block; count 0 skips it. */
        { 0xd0c, (uint16_t)(w720 ? 2 : 0), 0, isp_real_d0c_720 },
        { 0x600, 16, 0, isp_real_600 },
    };

    uint32_t *g = malloc(4096 * 2);
    unsigned n = 0;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    for (unsigned b = 0; b < sizeof blk / sizeof blk[0]; b++) {
        if (!blk[b].n) continue;
        g[n++] = blk[b].noninc ? OP_NONINCR(blk[b].m, blk[b].n)
                               : OP_INCR(blk[b].m, blk[b].n);
        unsigned first = n;
        for (unsigned i = 0; i < blk[b].n; i++) g[n++] = blk[b].d[i];

        /* The first word of 0x800 and 0x820 is a buffer address: the
         * stock's is 0x85001000, an address in ITS mapping. Ours sit at
         * 0x81xxxxxx (inside what /proc/iomem calls kernel data, so these
         * are SMMU addresses, not physical ones) and 0x85001000 maps to
         * nothing in our context. Sent literally since 2026-09-02; no
         * memory-controller fault ever followed, so nothing has used it
         * -- but a foreign address in our gather is not the stock's
         * configuration either. Words 2 and 3 stay the stock's.
         * --stock-stats-addr sends the literal, for the comparison. */
        if ((blk[b].m == 0x800 || blk[b].m == 0x820) && stats_iova)
            g[first] = stats_iova;
        /* Geometry by formula, not by table: the stock's word 2 here is
         * ((H-32)<<16)|(W-32) at 720p and at 2592 alike (impl-2,
         * pp-status-bits.md §3), so it holds for any size. */
        if (blk[b].m == 0x800 || blk[b].m == 0x820)
            g[first + 2] = ((H - 32) << 16) | (W - 32);
        /* The shading grid's reciprocals and pitch, 0xd00 words 1..7,
         * follow the width exactly in both stock sets: 1, 3 = 2^35/W;
         * 2 = 2^34/W; 4, 6 = 2^37/(3W); 5 = 2^36/(3W), each with the low
         * four bits clear; 7 = (W/2)<<16 | W/4. Words 0, 8 and 9 do not
         * reduce to the width and stay with the nearest table. */
        if (blk[b].m == 0xd00) {
            uint64_t k = 1ull << 35;
            g[first + 1] = g[first + 3] = (uint32_t)(k / W) & ~0xfu;
            g[first + 2] = (uint32_t)((k >> 1) / W) & ~0xfu;
            g[first + 4] = g[first + 6] = (uint32_t)((k << 2) / (3ull * W)) & ~0xfu;
            g[first + 5] = (uint32_t)((k << 1) / (3ull * W)) & ~0xfu;
            g[first + 7] = ((W / 2) << 16) | (W / 4);
        }

        /* White balance. In 0x700 the stock camera moves exactly two words
         * from frame to frame, 5 and 11, with 7 and 10 fixed: per-channel
         * gains in 4.12 (0x1000 = 1.0), green staying at 1.0. The words
         * follow the mosaic, BGGR: 5 is blue, 11 is red -- measured, not
         * assumed: raising word 11 from 1.71 to 2.38 raised red in the
         * picture by 1.4, not blue. The capture's pair belongs to the
         * stock's room; --wb=R,B puts this room's in. */
        if (blk[b].m == 0x700) {
            if (wb_b) g[first + 5] = wb_b;
            if (wb_r) g[first + 11] = wb_r;
        }
    }
    g[n++] = OP_INCR(0x053, 2); g[n++] = 1; g[n++] = work_iova;
    g[n++] = OP_IMM(0, sp);

    nvmap_rw(cmd_h, 0, g, n * 4, 1);
    gather_log("working-config", g, (unsigned)n);
    free(g);

    struct nvhost_cmdbuf cb = { cmd_h, 0, n };
    struct nvhost_syncpt_incr si = { sp, 1 };
    uint32_t cls = ISP_CLASS_B;
    struct nvhost_fence fence = { 0, 0 };
    struct nvhost32_submit_args sa;
    memset(&sa, 0, sizeof sa);
    sa.num_syncpt_incrs = 1;
    sa.num_cmdbufs = 1;
    sa.timeout = 3000;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.class_ids = (uint32_t)(uintptr_t)&cls;
    sa.fences = (uint32_t)(uintptr_t)&fence;
    errno = 0;
    int rc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
    printf("stock's working configuration (%s geometry words, stats at %s): %u words, rc=%d (%s)\n",
           w720 ? "720p" : "2592", stats_iova ? "ours" : "the stock's 0x85001000",
           n, rc, rc == 0 ? "ok" : strerror(errno));
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    return rc;
}

/* The warm-up the stock camera runs before its first real frame.
 *
 * Two frames of eight pixels by eight, one component, with the flags word
 * of the processing block set to three -- and the block enable written
 * inside the first of them. It looks like a formality and is not: the tile
 * engine that builds the luma has to be brought up, and this is what does
 * it. Going straight from the opening configuration to a real frame leaves
 * that engine cold, which is why the luma surface came back as zeros while
 * the third surface, which needs no tile, still received something.
 *
 * The buffers are tiny -- eight rows of two hundred and fifty six bytes,
 * and two scratch pages the processing block wants pointers to.
 */
int isp_warmup(int isp_fd, uint32_t sp, uint32_t warm_h,
                      uint32_t stats_h, int write_enable,
                      unsigned W, unsigned H)
{
    uint32_t cmd_h = nvmap_create(4096);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    uint32_t g[48];
    int n = 0, y_word, stats_word;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    g[n++] = OP_INCR(0xE00, 1); g[n++] = 0x00070000;   /* eight across */
    g[n++] = OP_INCR(0xE01, 1); g[n++] = 0x00070000;   /* eight down */
    g[n++] = OP_INCR(0xE02, 1); g[n++] = 0x010000C9;   /* one component */
    g[n++] = OP_INCR(0xE03, 1); g[n++] = 0x00000000;
    g[n++] = OP_INCR(0xE04, 3);
    y_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0x00000100;

    /* Three of these are how far the incoming frame is decimated to reach
     * eight by eight, so they are computed from the frame that is actually
     * arriving. We had the numbers for a 2592 by 1944 frame hard-coded
     * while the receiver delivered 1280 by 720, which set the decimator to
     * a width that was not there -- and that is why the warm-up sometimes
     * never finished. Both captures agree on the three forms. */
    g[n++] = OP_INCR(0x500, 6);
    g[n++] = 0x00000003;                                /* the flags word */
    g[n++] = (1u << 23) / W;
    /* W/128 in 8.8 fixed point in the upper half: the capture has
     * 0x14400000 at 2592 wide -- 20.25, not 20 -- and 0x0a000000 at 1280,
     * where the division is exact and hid the format. Truncating it left
     * the decimator with the wrong width at 2592, and no warm-up frame
     * ever completed there. */
    g[n++] = (W << 17);
    g[n++] = (H / 8) << 20;
    g[n++] = 0x00000000;
    g[n++] = 0x00080008;                                /* eight by eight */

    /* Once per session, and inside this frame rather than out in the
     * opening configuration, which is where we had been putting it. */
    if (write_enable) { g[n++] = OP_INCR(0x015, 1); g[n++] = ISP_ENABLE_WORD; }

    g[n++] = OP_INCR(0x100, 4);
    stats_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;

    g[n++] = OP_NONINCR(0x000, 1); g[n++] = 0x00000424;
    g[n++] = OP_NONINCR(0x000, 1); g[n++] = 0x00000525;
    g[n++] = OP_NONINCR(0x000, 1); g[n++] = 0x00000627;
    g[n++] = OP_NONINCR(0x00C, 1); g[n++] = 0x00000005;
    g[n++] = OP_IMM(0, sp);

    nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);
    gather_log("warm-up", g, (unsigned)n);

    /* Two, not four: the pair in the middle of the processing block are
     * decimation ratios, not addresses, and relocating them was writing
     * buffer pointers into arithmetic. */
    struct nvhost_reloc rel[2] = {
        { cmd_h, (uint32_t)y_word * 4, warm_h, 0 },
        { cmd_h, (uint32_t)stats_word * 4, stats_h, 0 },
    };
    struct nvhost_reloc_shift sh[2] = { { 0 }, { 0 } };
    struct nvhost_cmdbuf cb = { cmd_h, 0, (uint32_t)n };
    /* Declared: only the immediate increment on the sequencing counter.
     * Declaring the three armed conditions as well (36, 37, 39) turned the
     * output into deterministic garbage on 2026-09-02 -- identical to the
     * decimal across six runs -- and the reason is not understood yet. */
    struct nvhost_syncpt_incr si = { sp, 1 };
    uint32_t cls = ISP_CLASS_B;
    struct nvhost_fence fence = { 0, 0 };
    struct nvhost32_submit_args sa;
    memset(&sa, 0, sizeof sa);
    sa.num_syncpt_incrs = 1;
    sa.num_cmdbufs = 1;
    sa.num_relocs = 2;
    sa.timeout = 3000;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.relocs = (uint32_t)(uintptr_t)rel;
    sa.reloc_shifts = (uint32_t)(uintptr_t)sh;
    sa.class_ids = (uint32_t)(uintptr_t)&cls;
    sa.fences = (uint32_t)(uintptr_t)&fence;
    errno = 0;
    int rc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
    printf("warm-up frame (%s the enable): %d words, rc=%d (%s)\n",
           write_enable ? "with" : "without", n, rc,
           rc == 0 ? "ok" : strerror(errno));
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    return rc;
}

/* Run-level verdict state: rounds in which the block never wrote, whether
 * the stop was taken, and where the sequencing counter stood when the run
 * began. */
static int isp_nowrite = 0, isp_fences_met = -1;
static uint32_t isp_seq_base = 0;
/* Off by default: with both halves in, the ISP wrote nothing at 720p
 * (2026-09-06, twice: 15:10 and 15:13, parser 0x30/0xb4, first warm-up
 * never written), while either half alone changed nothing visible. The
 * stock writes these zeros before its sensor streams; we write them with
 * the wire already live, which is not the same experiment. */
static int ping_only = 0, syncpts_only = 0, mipi_dump_only = 0;

/* One job that does nothing but raise the sequencing counter. If the
 * channel retires it, the ISP-B path -- host1x, the channel, the class --
 * is taking work. If it does not, nothing else this tool could send would
 * fare better. */
int isp_ping(int isp_fd, uint32_t sp, int wait_ms)
{
    uint32_t cmd_h = nvmap_create(4096);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -2;
    uint32_t g[2] = { OP_SETCLASS(ISP_CLASS_B), OP_IMM(0, sp) };
    nvmap_rw(cmd_h, 0, g, sizeof g, 1);
    gather_log("ping", g, 2);
    struct nvhost_cmdbuf cb = { cmd_h, 0, 2 };
    struct nvhost_syncpt_incr si = { sp, 1 };
    uint32_t cls = ISP_CLASS_B;
    struct nvhost_fence fence = { 0, 0 };
    struct nvhost32_submit_args sa;
    memset(&sa, 0, sizeof sa);
    sa.num_syncpt_incrs = 1;
    sa.num_cmdbufs = 1;
    sa.timeout = 3000;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.class_ids = (uint32_t)(uintptr_t)&cls;
    sa.fences = (uint32_t)(uintptr_t)&fence;
    uint32_t was = syncpt_read(sp);
    errno = 0;
    int rc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
    int waited = 0;
    if (rc == 0)
        while (syncpt_read(sp) == was && waited < wait_ms) { usleep(2000); waited += 2; }
    uint32_t now = syncpt_read(sp);
    printf("ISP ping: submit rc=%d (%s), counter %u %u -> %u after %d ms: %s\n",
           rc, rc == 0 ? "ok" : strerror(errno), sp, was, now, waited,
           rc ? "NOT SUBMITTED" : now != was ? "retired" : "NEVER RETIRED");
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    return rc ? -2 : now != was ? 0 : -1;
}

/* Dead or alive, before a run submits anything: a counter with increments
 * still promised is a channel that already has a stuck job, and a ping
 * that never retires is one that will not take another. Prints the
 * verdict line; returns non-zero when dead. */
int isp_alive_check(int isp_fd, uint32_t sp, const char *when)
{
    uint32_t mn = syncpt_read(sp), mx = syncpt_read_max(sp);
    if (mx != mn) {
        printf("ISP VERDICT: DEAD %s -- counter %u value %u promised %u:"
               " %u job(s) owed by an earlier run; nothing submitted; reboot\n",
               when, sp, mn, mx, mx - mn);
        return 1;
    }
    if (isp_ping(isp_fd, sp, 500) != 0) {
        printf("ISP VERDICT: DEAD %s -- the ping job never retired; reboot\n", when);
        return 1;
    }
    printf("ISP alive %s: counter %u at %u, nothing owed, ping retired\n", when, sp, mn);
    return 0;
}

/* --stock-opening: the stock camera's 720p opening, submit for submit
 * (stock_opening_720p.h, from impl-1's stock-opening-720p.txt), in place
 * of this tool's own 5507-word table and its warm-up gathers. Plain jobs
 * go verbatim, with the stock's fixed statistics address 0x85001000 --
 * an address in its mapping, not ours -- swapped for our buffer. The two
 * warm-up captures relocate their plane and statistics pointers onto our
 * buffers and declare the three armed conditions as the stock does. The
 * ticks (host1x waits on 36 and 37 for the capture just sent) are not
 * submitted: a wait that never clears takes the channel down, so the
 * caller's own polling stands in for them. */
static uint32_t stock_open_thr36, stock_open_thr37;
static unsigned stock_open_W = 1280, stock_open_H = 720;
static int stock_open_submit(int isp_fd, uint32_t sp, uint32_t sp_mem, unsigned idx,
                             uint32_t warm_h, uint32_t stats_h, uint32_t stats_iova)
{
    if (idx >= sizeof stock_opening_720p / sizeof stock_opening_720p[0]) return -1;
    const struct stock_open_submit *so = &stock_opening_720p[idx];
    if (so->kind == 2) {
        int w = 0;
        while ((syncpt_read(sp_mem) < stock_open_thr36 || syncpt_read(sp_mem + 1) < stock_open_thr37)
               && w < 2500) { usleep(2000); w += 2; }
        printf("stock opening %u: tick -- 36 at %u (wanted %u), 37 at %u (wanted %u) after %d ms\n",
               idx, syncpt_read(sp_mem), stock_open_thr36, syncpt_read(sp_mem + 1), stock_open_thr37, w);
        return 0;
    }
    uint32_t bytes = ((so->n + 8) * 4 + 4095) & ~4095u;
    uint32_t cmd_h = nvmap_create(bytes);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;
    uint32_t *g = malloc(bytes);
    memcpy(g, so->w, so->n * 4);
    unsigned n = so->n, nrel = 0, swapped = 0;
    struct nvhost_reloc rel[2];
    struct nvhost_reloc_shift sh[2] = { { 0 }, { 0 } };
    for (unsigned i = 0; i < so->n; i++) {
        if (so->kind == 1 && g[i] == OP_INCR(0xE04, 3) && i + 1 < so->n) {
            rel[nrel].cmdbuf_mem = cmd_h; rel[nrel].cmdbuf_offset = (i + 1) * 4;
            rel[nrel].target = warm_h; rel[nrel].target_offset = 0; nrel++;
            g[i + 1] = 0;
        } else if (so->kind == 1 && g[i] == OP_INCR(0x500, 6) && i + 6 < so->n) {
            /* The warm-up capture's decimation trio is the one place the
             * 720p and 2592 openings differ (impl-1): (1<<23)/W, W<<17 and
             * (H/8)<<20, the same forms the warm-up builder computes. */
            g[i + 2] = (1u << 23) / stock_open_W;
            g[i + 3] = stock_open_W << 17;
            g[i + 4] = (stock_open_H / 8) << 20;
        } else if (so->kind == 1 && g[i] == OP_INCR(0x100, 4) && i + 1 < so->n) {
            rel[nrel].cmdbuf_mem = cmd_h; rel[nrel].cmdbuf_offset = (i + 1) * 4;
            rel[nrel].target = stats_h; rel[nrel].target_offset = 0; nrel++;
            g[i + 1] = 0;
        } else if (so->kind == 0 && g[i] == 0x85001000 && stats_iova) {
            g[i] = stats_iova; swapped++;
        }
    }
    g[n++] = OP_IMM(0, sp);
    nvmap_rw(cmd_h, 0, g, n * 4, 1);
    char what[32]; snprintf(what, sizeof what, "stock-opening-%u", idx);
    gather_log(what, g, n);

    struct nvhost_cmdbuf cb = { cmd_h, 0, n };
    struct nvhost_syncpt_incr si[4] = { { sp, 1 }, { sp_mem, 1 }, { sp_mem + 1, 1 }, { sp_mem + 3, 1 } };
    unsigned nsi = so->kind == 1 ? 4 : 1;
    uint32_t cls = ISP_CLASS_B;
    struct nvhost_fence fence = { 0, 0 };
    struct nvhost32_submit_args sa;
    memset(&sa, 0, sizeof sa);
    sa.num_syncpt_incrs = nsi;
    sa.num_cmdbufs = 1;
    sa.num_relocs = nrel;
    sa.timeout = 3000;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.relocs = (uint32_t)(uintptr_t)rel;
    sa.reloc_shifts = (uint32_t)(uintptr_t)sh;
    sa.class_ids = (uint32_t)(uintptr_t)&cls;
    sa.fences = (uint32_t)(uintptr_t)&fence;
    if (so->kind == 1) {
        stock_open_thr36 = syncpt_read(sp_mem) + 1;
        stock_open_thr37 = syncpt_read(sp_mem + 1) + 1;
    }
    uint32_t was = syncpt_read(sp);
    errno = 0;
    int rc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
    int w = 0;
    /* Plain jobs retire at once; serialise on them so the log's order is
     * the hardware's. A capture retires only with a frame, which the
     * caller triggers after this returns. */
    if (rc == 0 && so->kind == 0)
        while (syncpt_read(sp) == was && w < 500) { usleep(1000); w++; }
    printf("stock opening %u: %u words%s%s, %s, rc=%d (%s)%s\n", idx, n,
           nrel ? ", 2 relocs" : "", swapped ? ", stats address ours" : "",
           so->kind == 1 ? (nsi == 4 ? "conditions declared" : "38 only") : "plain",
           rc, rc == 0 ? "ok" : strerror(errno),
           so->kind == 0 ? (syncpt_read(sp) != was ? " retired" : " NOT RETIRED") : "");
    free(g);
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    return rc;
}

/* --stock-frame: the stock's steady-state cycle at 720p is a 25-word
 * [cal] round -- the 0x930 window block and 0x053 = {1, 0} -- followed a
 * couple of milliseconds later by a 45-word frame gather that carries
 * only geometry, planes, the processing block, the three arms and the
 * trigger (impl-2, stock-steady-cycle-720p.md §1, §8). This is the cal
 * round, verbatim; the frame gather is isp_frame with everything else
 * left out. */
static int isp_cal_round(int isp_fd, uint32_t sp)
{
    static const uint32_t cal[25] = {
        0x00000d00, 0x19300012, 0x0000001d,
        0x88888888, 0x78787800, 0x00000078, 0x88888888, 0x78787800, 0x00000078,
        0x88888888, 0x78787800, 0x00000078, 0x88888888, 0x78787800, 0x00000078,
        0x3fc00000, 0x00000000, 0x00070000, 0x00000000, 0x00070000,
        0x00000d00, 0x00000d00, 0x10530002, 0x00000001, 0x00000000,
    };
    uint32_t cmd_h = nvmap_create(4096);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;
    uint32_t g[28];
    memcpy(g, cal, sizeof cal);
    unsigned n = 25;
    g[n++] = OP_IMM(0, sp);
    nvmap_rw(cmd_h, 0, g, n * 4, 1);
    gather_log("cal-round", g, n);
    struct nvhost_cmdbuf cb = { cmd_h, 0, n };
    struct nvhost_syncpt_incr si = { sp, 1 };
    uint32_t cls = ISP_CLASS_B;
    struct nvhost_fence fence = { 0, 0 };
    struct nvhost32_submit_args sa;
    memset(&sa, 0, sizeof sa);
    sa.num_syncpt_incrs = 1;
    sa.num_cmdbufs = 1;
    sa.timeout = 3000;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.class_ids = (uint32_t)(uintptr_t)&cls;
    sa.fences = (uint32_t)(uintptr_t)&fence;
    uint32_t was = syncpt_read(sp);
    errno = 0;
    int rc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
    int w = 0;
    if (rc == 0) while (syncpt_read(sp) == was && w < 500) { usleep(1000); w++; }
    printf("stock cal round: %u words, rc=%d (%s)%s\n", n, rc, rc == 0 ? "ok" : strerror(errno),
           syncpt_read(sp) != was ? " retired" : " NOT RETIRED");
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    return rc;
}


/* The per-frame gather, word for word as the stock camera sends it: output
 * geometry, three plane triplets for planar YUV, the processing block, the
 * stats buffer, three conditional syncpoint increments and the streaming
 * trigger. There are no input registers -- in streaming mode the pixels
 * arrive from VI and there is nothing to describe. */
int isp_frame(int isp_fd, uint32_t out_h, uint32_t stats_h,
                     unsigned W, unsigned H, uint32_t fmt,
                     uint32_t u_off, uint32_t v_off,
                     uint32_t sp_mem, uint32_t sp_stats, uint32_t sp_loadv,
                     uint32_t sp)
{
    uint32_t cmd_h = nvmap_create(4096);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    /* Planar YUV takes a byte per luma sample and half-width chroma; the
     * packed forms take four bytes a pixel in one plane. */
    int planar = (fmt & 0xFF) == 0xE6;
    /* Strides to 64, as the stock's own 2592 frame has them: 0xE04 stride
     * 0xa40 (2624) and 0xE07/0xE0A stride 0x540 (1344). The plane offsets
     * in the stock's buffers (0x540000, 0x6a0000) come from its allocator,
     * not from the stride -- a 128 rounding read off them was wrong. */
    uint32_t stride_y = planar ? ((W + 63) & ~63u) : W * 4;
    uint32_t stride_uv = ((W / 2) + 63) & ~63u;

    uint32_t *g = malloc(256 * 4);
    int n = 0, y_word, u_word, v_word;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);



    g[n++] = OP_INCR(0xE00, 1); g[n++] = ((W - 1) & 0x3FFF) << 16;
    g[n++] = OP_INCR(0xE01, 1); g[n++] = ((H - 1) & 0x3FFF) << 16;
    g[n++] = OP_INCR(0xE02, 1); g[n++] = fmt;
    g[n++] = OP_INCR(0xE03, 1); g[n++] = 0;

    {
        g[n++] = OP_INCR(0xE04, 3);
        y_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_y;
        g[n++] = OP_INCR(0xE07, 3);
        u_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_uv;
        g[n++] = OP_INCR(0xE0A, 3);
        v_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_uv;
    }

    /* The first word of this block is the streaming path's only flags
     * field; anything but zero is fatal. */
    g[n++] = OP_INCR(0x500, 6);
    g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = (H << 16) | W;


    /* Only the conditions that have a counter behind them. Arming one
     * against an id this channel does not own leaves the job waiting on
     * something nothing will raise, and host1x eventually kills the
     * channel -- which is exactly what happened. */
    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    g[n++] = OP_NONINCR(0x000, 1); g[n++] = (4u << 8) | sp_mem;
    if (sp_stats) { g[n++] = OP_NONINCR(0x000, 1); g[n++] = (5u << 8) | sp_stats; }
    if (sp_loadv) { g[n++] = OP_NONINCR(0x000, 1); g[n++] = (6u << 8) | sp_loadv; }

    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    g[n++] = OP_NONINCR(0x00C, 1); g[n++] = ISP_TRIGGER_SENSOR;

    /* Park the job on the frame's own completion and on statistics, the way
     * stock's post-frame submit does. The relocation pins then last exactly
     * as long as the writes they guard, and the keepalive flood that papered
     * over the gap is gone. The threshold is read at submit time: nothing
     * else on this exclusive channel moves 36 or 37 between here and the
     * frame, so +1 is unambiguous. If the sensor never delivers, the job
     * sits until its timeout and the channel dies -- the same price every
     * wedged run paid. */
    /* The channel's own counter goes up HERE, right after the trigger and
     * before the parking waits: the stock's frame job carries no waits at
     * all and its increment means "config loaded, trigger given", while a
     * separate two-word gather behind it parks the channel on 36/37. With
     * the increment after the waits it meant "frame finished", and the VI
     * side had nothing to wait on before its single-shot. */
    g[n++] = OP_IMM(0, sp);
    {
        /* hold_at is the queue depth: 0 when this job is the only one in
         * flight, 1 when it is queued behind a frame still running and
         * must park on the frame after that one. */
        /* park_mem/park_stats, when given, are absolute: the stream keeps
         * its own count of frames, because a threshold taken from a read of
         * the counter at submit time races the previous frame's completion
         * -- the output runs a job behind at 2592 -- and one off-by-one per
         * frame drifts the queue until a job parks on a value that never
         * comes. */
        uint32_t want_mem = syncpt_read(sp_mem) + 1;
        g[n++] = OP_SETCLASS(HOST1X_CLASS_ID);
        g[n++] = OP_INCR(HOST1X_WAIT_SYNCPT, 1);
        g[n++] = (sp_mem << 24) | (want_mem & 0xFFFFFF);
    }

    nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);
    gather_log("frame", g, (unsigned)n);
    free(g);

    struct nvhost_reloc rel[3] = {
        { cmd_h, (uint32_t)y_word * 4, out_h, 0 },
        { cmd_h, (uint32_t)u_word * 4, out_h, u_off },
        { cmd_h, (uint32_t)v_word * 4, out_h, v_off },
    };
    struct nvhost_reloc_shift sh[3] = { { 0 }, { 0 }, { 0 } };
    (void)stats_h;
    struct nvhost_cmdbuf cb = { cmd_h, 0, (uint32_t)n };
    /* Declared: only the immediate increment on the sequencing counter --
     * see the warm-up for why the armed conditions are not declared too. */
    struct nvhost_syncpt_incr si = { sp, 1 };
    uint32_t cls = ISP_CLASS_B;
    struct nvhost_fence fence = { 0, 0 };
    struct nvhost32_submit_args sa;
    memset(&sa, 0, sizeof sa);
    sa.num_syncpt_incrs = 1;
    sa.num_cmdbufs = 1;
    sa.num_relocs = 3;
    /* The single capture job is parked on a counter that moves only when
     * the whole capture is done, so it outlives the default three seconds;
     * a stream job parks on the next frame, at most two periods away, and
     * a wedged stream should die in seconds rather than hold the channel
     * for a minute after the tool has exited. */
    sa.timeout = (uint32_t)isp_job_timeout_ms;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.relocs = (uint32_t)(uintptr_t)rel;
    sa.reloc_shifts = (uint32_t)(uintptr_t)sh;
    sa.class_ids = (uint32_t)(uintptr_t)&cls;
    sa.fences = (uint32_t)(uintptr_t)&fence;

    errno = 0;
    int rc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
    printf("ISP frame: %d words, %ux%u fmt 0x%08x, strides %u/%u,"
           " rc=%d (%s)\n", n, W, H, fmt, stride_y,
           stride_uv, rc, rc == 0 ? "ok" : strerror(errno));
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    return rc;
}

/* The mode table ends with the streaming bit, so this is the moment the
 * sensor's clock lane leaves LP-11 for good. The receiver has to be awake
 * and configured before it: a CIL brought up under a clock lane already in
 * HS never sees the transition it locks on, and no frame ever starts. That
 * is exactly what the first run after every boot looked like -- receiver
 * cold, sensor started first, CIL E status 0x100, twelve single-shots and
 * not one frame -- while the second run rode on the receiver the first one
 * had left powered and locked. The stock and the R21.5 driver both bring
 * the CSI up first and start the sensor after. */
/* The sensor's own frame length per mode, from the stock driver's mode
 * tables (ov5693.c, registers 0x380e/0x380f): 2592x1944, 1296x972 and
 * 1920x1080 all run 2688 x 1984 (30 fps), 1280x720 runs 1752 x 760 (60
 * fps). The default frame length is twice this -- half the native rate,
 * which is where 2592 (4128 lines) and 720p (2064) were run by hand. */
static uint32_t native_vts(unsigned W, unsigned H)
{
    if (W == 1280 && H == 720) return 760;
    return 1984;
}

static void sensor_start_front(int sfd, unsigned W, unsigned H,
                               uint32_t frame_length, uint32_t coarse_time,
                               uint32_t gain)
{
    /* The driver writes the mode table and then writes exposure from these
     * fields unconditionally -- passing zeros programs the sensor with no
     * frame length and no integration time, which is a part that streams
     * nothing. */
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
}

int main(int argc, char **argv)
{
    /* Through adb the output is a pipe and fully buffered, so a crash takes
     * the last screen of it along; the log then ends several steps before
     * the place that died. Unbuffered, every line is out before the next
     * thing happens. */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Default to the front camera: it is the one that still works through
     * the stock app, so its live register values are on record and a
     * mismatch means our configuration, not the hardware. The rear does not
     * stream through the camera stack at all, which leaves any negative
     * result there impossible to attribute. */
    /* The front sensor's stock session runs at its full 2592x1944, and the
     * image definition it uses on this channel was read off that session. */
    unsigned W = 2592, H = 1944;
    int dump = 0;
    /* Destination is in the low bits: 1 memory, 2 ISP-A, 4 ISP-B. Stock
     * reads 0x00200004 because it sends pixels to the ISP, not to memory
     * -- copying its value told our VI to do the same, which is why the
     * buffer stayed untouched and nothing ever reported an error. For a
     * memory write it is the format, the transform bypass, and DEST_MEM. */
    /* Sent to the ISP rather than to memory, and the pixel transform is NOT
     * bypassed -- the driver clears that bit whenever the ISP is in the
     * path, because the ISP wants pixels rather than raw wire words. */
    /* The receiver can be told to deliver to memory and to the ISP at the
     * same time, and that settles by comparison what has been guesswork:
     * the raw frame and what the ISP made of THAT SAME frame, side by
     * side. Add the memory bit with --both. */
    uint32_t image_def = (0u << BYPASS_PXL_TRANSFORM_OFFSET) |
                         (IMAGE_FORMAT_T_R16_I << IMAGE_DEF_FORMAT_OFFSET) |
                         IMAGE_DEF_DEST_ISP_B;
    /* Gain of sixteen is unity on this sensor, and with the tablet lying in
     * an ordinary room that puts the whole picture in the bottom five per
     * cent of the range -- far too dark to tell a colour from a cast. Both
     * are adjustable now, because judging the pipeline on a black frame
     * tells us nothing about it. */
    /* frame_length 0 = derived from the mode: twice the sensor's native
     * frame length (half its native rate), see native_vts(). */
    uint32_t frame_length = 0, coarse_time = 2000, gain = 16;
    /* One frame. Every extra one queues another job behind a block that may
     * already be stuck, and when it is, the channel dies and only a reboot
     * brings the camera back -- so the cost of asking for more is paid by
     * hand, every time. Two is the ceiling; anything larger is clamped. */
    int shots = 1;
    const int settle = 200;
    const uint32_t isp_clk = 384000000;
    /* The ISP output: one packed RGB plane by default, no colour config,
     * the enable the reprocess tool settled on, and the sensor trigger. All
     * four are worth varying, since none of them has been exercised on a
     * frame that came from VI rather than from memory. */
    /* Packed, one plane, four bytes a pixel. This is the form that comes
     * back whole: the ISP writes every byte of the surface and the picture
     * is the scene.
     *
     * The planar codes are still selectable and still wrong somewhere. The
     * format word carries the plane count in its third byte -- 0xFE for
     * three, zero for one -- and the layout in its top: the stock trace's
     * 0x04FE00E6 is three planes block-linear, which wants buffers we do
     * not allocate; 0x010000E6 is pitch-linear but a single plane, which
     * is why one quarter-size plane came back written and the luma surface
     * stayed empty; and with 0x01FE00E6 the block lays the chroma out from
     * the luma base itself rather than from the addresses we give it. */
    uint32_t isp_fmt = 0x04FE00E6;   /* YUV420 planar, block-linear: what the stock writes */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--width=", 8) == 0)       W = (unsigned)strtoul(a + 8, 0, 0);
        else if (strncmp(a, "--height=", 9) == 0) H = (unsigned)strtoul(a + 9, 0, 0);
        else if (strcmp(a, "--dump") == 0)        dump = 1;
        else if (strncmp(a, "--frame-length=", 15) == 0)
            frame_length = (uint32_t)strtoul(a + 15, 0, 0);
        else if (strncmp(a, "--coarse=", 9) == 0)
            coarse_time = (uint32_t)strtoul(a + 9, 0, 0);
        else if (strncmp(a, "--gain=", 7) == 0)
            gain = (uint32_t)strtoul(a + 7, 0, 0);
        else if (strncmp(a, "--wb=", 5) == 0) {     /* --wb=R,B in hex 4.12 */
            wb_r = (uint32_t)strtoul(a + 5, 0, 16);
            const char *comma = strchr(a + 5, ',');
            if (comma) wb_b = (uint32_t)strtoul(comma + 1, 0, 16);
        }
        else if (strncmp(a, "--shots=", 8) == 0) {
            shots = atoi(a + 8);
            if (shots > 32) {
                printf("shots: asked for %d, taking 32 -- more than that"
                       " queues jobs behind a block that may already be"
                       " stuck, and that costs a reboot\n", shots);
                shots = 32;
            }
            if (shots < 1) shots = 1;
        }
        else if (strcmp(a, "--no-isp") == 0)      no_isp = 1;
        else if (strcmp(a, "--ping") == 0)        ping_only = 1;
        else if (strcmp(a, "--syncpts") == 0)     syncpts_only = 1;
        else if (strcmp(a, "--mipi-dump") == 0)   mipi_dump_only = 1;
        else { printf("unknown option %s\n", a); return 1; }
    }

    /* --no-isp: the frame goes to memory instead of the ISP, so the receiver
     * and VI can be judged on their own. */
    if (no_isp)
        image_def = (image_def & ~(IMAGE_DEF_DEST_MEM | IMAGE_DEF_DEST_ISP_A |
                                   IMAGE_DEF_DEST_ISP_B)) | IMAGE_DEF_DEST_MEM;
    if (W != 2592 && W != 1280)
        printf("WARNING: no measured geometry for %ux%u -- using the"
               " nearest captured set\n", W, H);

    uint32_t base = VI_CSI_BASE(1);          /* port B: the front sensor */
    uint32_t stride = W * 2;                 /* RAW10 lands in 16-bit words */
    uint32_t frame = stride * H;
    uint32_t wc = W * 10 / 8;                /* core.c: width * bpp / 8 */

    if (syncpts_only) { syncpt_table(); return 0; }
    if (mipi_dump_only) {
        /* The MIPI calibration block's registers, read with its clocks
         * held through /dev/mipi-cal (the block is unclocked otherwise
         * and a read then hangs the bus). Touches nothing else: safe
         * before, after or during a stock session -- the values persist,
         * so what the stock's calibration left can be read after it. */
        int cal_fd = open("/dev/mipi-cal", O_RDWR);
        if (cal_fd < 0) { printf("/dev/mipi-cal: %s\n", strerror(errno)); return 1; }
        printf("MIPI_CAL 0x700E3000:");
        for (unsigned off = 0; off < 0x80; off += 4) {
            uint32_t v = 0;
            if (off % 32 == 0) printf("\n  +0x%02x:", off);
            mem_rd(MIPI_CAL_BASE + off, &v);
            printf(" %08x", v);
        }
        printf("\n");
        close(cal_fd);
        return 0;
    }
    stock_open_W = W; stock_open_H = H;
    if (!frame_length) frame_length = 2 * native_vts(W, H);
    if (coarse_time >= frame_length) coarse_time = frame_length - 8;
    printf("sensor timing: frame length %u lines (native %u), coarse %u, gain %u\n",
           frame_length, native_vts(W, H), coarse_time, gain);
    if (ping_only) printf("=== viisp --ping: is the ISP-B channel taking work? (no sensor, no VI) ===\n");
    else printf("=== viisp: ov5693 %ux%u, CSI port B ===\n", W, H);
    printf("stride %u, frame %u bytes, word count %u\n", stride, frame, wc);

    nvmap_fd = open("/dev/nvmap", O_RDWR | O_SYNC);
    /* The kernel keeps two VI modules on the one aperture: vi (port A:
     * clocks vi, csi, cilab) and vi.1 (port B: vi, csi, cilcd, cile), both
     * in the VENC power partition, gated 500 ms after the last use. The
     * stock opens vi.1 for this sensor, and the kernel then owns every
     * clock the port needs. Opening vi instead is what made this tool
     * switch cilcd/cile on through the clock controller by hand -- and
     * leave them on for the kernel to gate the partition underneath. */
    vi_fd = open("/dev/nvhost-vi.1", O_RDWR);
    if (nvmap_fd < 0 || vi_fd < 0) {
        printf("open failed: nvmap=%d vi=%d\n", nvmap_fd, vi_fd);
        return 1;
    }

    /* Tell the channel which memory context our buffers live in. Without
     * this the address we program is not one VI can resolve -- it has its
     * own translation context, which is what the kernel's dual-mapping fix
     * was about -- and the write has nowhere to land however correct the
     * rest of the configuration is. */
    {
        struct nvhost_set_nvmap_fd_args nf = { (uint32_t)nvmap_fd };
        int rc = ioctl(vi_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &nf);
        printf("channel memory context: rc=%d (%s)\n", rc,
               rc == 0 ? "ok" : strerror(errno));
    }

    /* The channel's own syncpoint, asked for rather than assumed. */
    struct nvhost_get_param_arg gp = { .param = 0, .value = 0 };
    uint32_t sp_id = 0;
    if (ioctl(vi_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gp) == 0)
        sp_id = gp.value;
    uint32_t sp_mw = sp_id, mw_base = 0, fe_base = 0;
    gp.param = 1; gp.value = 0;
    if (ioctl(vi_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gp) == 0 && gp.value)
        sp_mw = gp.value;
    /* A third, for the command buffer to retire on, so that neither of the
     * hardware conditions shares a counter with our own submits. */
    uint32_t sp_cmd = sp_mw;
    gp.param = 2; gp.value = 0;
    if (ioctl(vi_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &gp) == 0 && gp.value)
        sp_cmd = gp.value;
    printf("VI syncpoints: frame %u, memory write %u, command %u\n",
           sp_id, sp_mw, sp_cmd);
    uint32_t hold_thresh = 0, isp_hold_thresh = 0;

    uint32_t buf_h = nvmap_create(frame);
    if (!buf_h || nvmap_alloc(buf_h)) return 1;
    uint32_t iova = nvmap_pin(buf_h);
    printf("frame buffer: handle %u, iova 0x%08x\n", buf_h, iova);
    if (!iova) return 1;

    /* The ISP's own channel and its output buffer. ISP-B is the one port B
     * feeds; the node is nvhost-isp.1, nvhost-isp being ISP-A. */
    /* Planar YUV, laid out the way stock lays it out: the luma plane first,
     * then the two chroma planes on 64K boundaries. Strides are the width
     * rounded up to 64, and the chroma planes are half of everything. */
    unsigned OH = H;
    int isp_planar = (isp_fmt & 0xFF) == 0xE6;
    /* 64-aligned, as the stock's own frame registers have it (see the
     * frame builder). */
    uint32_t stride_y = isp_planar ? ((W + 63) & ~63u) : W * 4;
    uint32_t stride_uv = ((W / 2) + 63) & ~63u;
    /* Stock's own plane offsets for this sensor are 0x540000 and 0x6a0000 --
     * a wider gap than the planes need, and not what rounding the sizes up
     * produces. Overridable for that reason. */
    /* Block-linear does not write row by row: it fills tiles sixty-four
     * bytes wide and eight rows tall, gathered into blocks, and a plane
     * therefore occupies whole blocks whether or not the picture fills
     * them. Sizing a plane as stride times height is right for a linear
     * surface and short for this one -- at 720 rows the last chroma plane
     * needs 384 rows of room, not 360, and the fifteen kilobytes of
     * difference is exactly where the memory controller caught the ISP
     * writing outside our surface. */
    int isp_blocklinear = ((isp_fmt >> 24) & 0xFF) == 0x04;
    unsigned rows_y = isp_blocklinear ? ((OH + 127) & ~127u) : OH;
    unsigned rows_uv = isp_blocklinear ? (((OH / 2) + 127) & ~127u) : OH / 2;

    uint32_t u_off = (stride_y * rows_y + 0xFFFF) & ~0xFFFFu;
    uint32_t v_off = (u_off + stride_uv * rows_uv + 0xFFFF) & ~0xFFFFu;
    uint32_t out_bytes = isp_planar ? v_off + stride_uv * rows_uv
                                    : stride_y * rows_y;
    /* And a block over, because the block height is inferred rather than
     * read from anywhere: running past the end costs a reboot, and sixty
     * four kilobytes costs nothing. */
    if (isp_blocklinear) out_bytes += 0x10000;

    /* The planes must tile without overlap: the ISP writes each surface to
     * its full block-rounded extent, and two surfaces sharing bytes is what
     * a silently dead channel looks like. Refuse the run instead. */
    {
        uint32_t yext = stride_y * rows_y;
        uint32_t uext = stride_uv * rows_uv;
        uint32_t vend = v_off + uext;
        int bad = 0;
        if (isp_planar) {
            if (u_off < yext) bad = 1;
            if (v_off < u_off + uext) bad = 1;
            if (vend > out_bytes) bad = 1;
            if ((u_off | v_off) & 0xFFF) bad = 1;
        }
        if (bad) {
            printf("plane layout invalid: Y[0..%u) U@%u+%u V@%u+%u,"
                   " buffer %u, 4K alignment required\n",
                   yext, u_off, uext, v_off, uext, out_bytes);
            return 1;
        }
    }
    /* --no-isp: never open the ISP channel. With --image-def=00200001 the
     * frame goes to memory alone, which is how the receiver and VI are
     * judged on their own -- a parser status with the ISP out of the
     * picture cannot be back-pressure from an ISP that stopped taking
     * lines. */
    int isp_fd = no_isp ? -1 : open("/dev/nvhost-isp.1", O_RDWR);
    uint32_t out_h = 0, out_iova = 0, isp_sp = 0, work_h = 0, stats_h = 0;
    uint32_t work_iova = 0, stats_iova = 0;
    uint32_t sp_mem = 0, sp_stats = 0, sp_loadv = 0;
    uint32_t isp_base_mem = 0, isp_base_stats = 0, isp_base_loadv = 0;
    if (isp_fd < 0) {
        printf("open /dev/nvhost-isp.1: %s\n", strerror(errno));
        if (ping_only) { printf("ISP VERDICT: DEAD BEFORE THE RUN -- the channel node did not open\n"); return 3; }
    } else {
        struct nvhost_set_nvmap_fd_args snf = { (uint32_t)nvmap_fd };
        ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &snf);
        /* This channel has exactly two, and the kernel prints their names
         * when it dumps a stuck job: 36 is ispb_memory and 38 is
         * ispb_stream. Handing the first one to our own sequencing
         * increment and arming the output condition on 37 -- which belongs
         * to nothing -- is what wedged the channel: the job sat waiting on
         * a counter nobody would ever raise, and host1x timed it out.
         *
         * So the first is the output condition and the second sequences the
         * submits. The stats and read conditions have no counter of their
         * own here; they are left unarmed rather than aimed at someone
         * else's. */
        /* And the second one is not the one that comment assumed. The
         * kernel's own dump settles it: 36 is ispb_memory, 37 is ispb_stats
         * and 38 is ispb_stream, and after a wedged run 37 stood at 3754
         * against 4010 asked for -- two hundred and fifty six increments
         * owed -- while 38 had moved ten times in the whole session. So
         * asking for parameter one was handing our sequencing to the
         * statistics counter, which the hardware raises on its own
         * schedule and, when the statistics stage is not producing, never
         * raises at all. The job waits, host1x times it out, and the
         * channel goes with it.
         *
         * So take them by what they are: the first for the output
         * condition, and for sequencing the first one after it that is
         * neither the output nor the statistics counter. */
        uint32_t sps[4] = { 0, 0, 0, 0 };
        for (unsigned p = 0; p < 4; p++) {
            struct nvhost_get_param_arg ip = { .param = p, .value = 0 };
            if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &ip) == 0)
                sps[p] = ip.value;
        }
        printf("channel syncpoints: %u %u %u %u\n",
               sps[0], sps[1], sps[2], sps[3]);
        sp_mem = sps[0];
        isp_sp = 0;
        for (unsigned p = 1; p < 4; p++)
            if (sps[p] && sps[p] != sp_mem && sps[p] != sp_mem + 1) {
                isp_sp = sps[p];
                break;
            }
        if (!isp_sp) isp_sp = sp_mem;
        /* 38 is also the stock camera's own sequencing counter: it raises
         * it immediately after every trigger (0x000 = 0x26) and waits on
         * it in the next gather. With the pipeline streaming it looked as
         * if the hardware moved 38 on its own (a run died with 38 one
         * ahead of the kernel, thresh 3220 against 3221) and the sequencing
         * wandered to 36, 39, 49 and 32 in turn -- 36 released the frame's
         * park early, 39 hung the device at the first warm-up, 49 belongs
         * to the VI1 receiver. The one ahead was ours all along: the
         * warm-up and frame gathers armed conditions 4, 5 and 6 without
         * declaring them. Declared, 38 holds. --seq-sp still overrides,
         * for experiments. */

        /* --ping: is the ISP-B channel alive? Counters first -- a job
         * still owed from an earlier run is a dead channel and gets no
         * more jobs stacked on it -- then one job that does nothing but
         * raise the sequencing counter. The sensor is never touched. */
        if (ping_only) {
            syncpt_table();
            int dead = isp_alive_check(isp_fd, isp_sp, "AT PING");
            nvmap_unpin(buf_h);
            ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)buf_h);
            close(isp_fd);
            close(vi_fd);
            close(nvmap_fd);
            return dead ? 3 : 0;
        }

        /* And arm the other two conditions, which stock arms on every real
         * frame: 0x424 is condition four on 36, 0x525 is five on 37, 0x627
         * is six on 39, and it then waits on both 36 and 37. We had these
         * off because arming them once wedged the channel -- but that was
         * while our own submits were also sequenced on 37, which is the
         * counter the hardware raises for the statistics stage. With the
         * sequencing moved to 38 the conflict is gone, and a pipeline whose
         * statistics stage is never asked to finish is a fair suspect for
         * one that never finishes the write either. */
        sp_stats = sps[1];
        sp_loadv = sps[3];

        /* The ISP's own clocks. 384 MHz carried 1280 wide (40 Mpix/s at
         * the sensor's line rate); at 2592 the pipeline has to take 81
         * Mpix/s and no frame completed, so the rate is a knob now. */
        struct nvhost_clk_rate_args ic;
        ic.moduleid = 0; ic.rate = isp_clk;
        errno = 0;
        int crc0 = ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &ic);
        int cerr0 = errno;
        /* Memory bandwidth, the way the stock asks for it: moduleid is the
         * clock's id from the module's pdata in bits 15..0 -- 0x4b is the
         * EMC entry -- and the attribute in bits 31..24, where 1 means the
         * rate is a bandwidth in bytes per second, which the kernel turns
         * into an EMC floor. The "moduleid = 1" this used to send matches
         * no pdata entry, and the kernel then falls back to clock zero:
         * the ISP clock was being set twice and the memory never asked
         * for. The stock's figure, 163.2 MB/s, at 2592 and at 1280. */
        ic.moduleid = (1u << 24) | 0x4b; ic.rate = (uint32_t)emc_bw;
        errno = 0;
        /* --no-emc-bw leaves this lever out, for attributing the EMC rise
         * between it and the SET_EMC reservation below: by the kernel's
         * arithmetic neither asks for more than ~10 MHz, yet with both sent
         * EMC sits on PLLM while the ISP is busy (impl-1, §2.2). */
        int crc1 = ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &ic);
        /* This kernel has no GET_CLK_RATE (it logs "unrecognized ioctl"), so
         * the rate actually granted is only visible in
         * /sys/kernel/debug/clock/ispb/rate while the channel is busy. */
        printf("ISP clock request: %u Hz -> rc=%d (%s); EMC bandwidth %lu B/s -> rc=%d\n",
               isp_clk, crc0, crc0 ? strerror(cerr0) : "ok", emc_bw, crc1);
        /* The rate actually granted. GET_CLK_RATE exists in this kernel in
         * its read-only form (0x80084809, which is what the stock issues);
         * the read-write form we used to send was "unrecognized". */
        {
            struct nvhost_clk_rate_args gr = { 0, 0 };
            errno = 0;
            int grc = ioctl(isp_fd, _IOR('H', 9, struct nvhost_clk_rate_args), &gr);
            printf("ISP clock granted: %u Hz (rc=%d%s%s)\n", gr.rate, grc,
                   grc ? " " : "", grc ? strerror(errno) : "");
        }
        /* The ISP write client's latency allowance, as the stock sets it
         * at every opening. Without it, on a fresh boot, the ISP never
         * finished even an 8x8 warm-up; after one run of the stock camera
         * -- whose setting outlives everything but a reboot -- it did. */
        /* Not on the channel: the channel's ioctl handler refuses every
         * magic but its own with EFAULT. The ISP driver's ioctls live on
         * its control node, /dev/nvhost-ctrl-isp.1 for ISP-B. */
        struct isp_emc_info ei = { 0, isp_emc_clk, 0, 16 };
        int lfd = open("/dev/nvhost-ctrl-isp.1", O_RDWR);
        errno = 0;
        int lrc = lfd < 0 ? -1 : ioctl(lfd, NVHOST_ISP_IOCTL_SET_EMC, &ei);
        printf("ISP latency allowance (clk %u kHz, 16 bpp out, %u MB/s HARD) -> rc=%d%s%s\n",
               isp_emc_clk, isp_emc_clk / 1000 * 16 / 8, lrc,
               lrc ? " " : "", lrc ? strerror(errno) : "");
        if (lfd >= 0) close(lfd);

        /* Two register writes before anything else, both of which stock
         * makes when it opens the ISP. 0xFC is the block's own enable. 0x54
         * is the pipeline mode -- the same thing method 0x015 addresses,
         * four times the method number -- and the driver's own commit for
         * it describes our symptom exactly: without it the ISP accepts
         * gathers and moves syncpoints but processes no pixels, and the
         * output stays black. A gather cannot substitute; it has to be a
         * register write. */
        {
            /* Stock writes ONE register directly, and it is 0xFC. Its whole
             * PIO trace for a camera session is seven lines and every one of
             * them is 0xFC=0x20 -- 0x54 never appears; the enable goes
             * inside the first warm-up frame instead. */
            uint32_t off[1] = { 0xFC };
            uint32_t val[1] = { 0x20 };
            struct regrdwr_args ra;
            memset(&ra, 0, sizeof ra);
            ra.id = 0;
            ra.num_offsets = 1;
            ra.block_size = 4;
            ra.offsets = (uint32_t)(uintptr_t)off;
            ra.values = (uint32_t)(uintptr_t)val;
            ra.write = 1;
            errno = 0;
            int rrc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR, &ra);
            printf("ISP register 0xFC=0x20: rc=%d (%s)\n",
                   rrc, rrc == 0 ? "ok" : strerror(errno));
        }

        /* A scratch buffer the ISP wants for its own working state. The
         * reprocess tool calls it required for a cold start. */
        /* Big enough for the offsets the runtime configuration hands out --
         * it reaches 0x3f4a0 into this buffer. */
        work_h = nvmap_create(work_kb * 1024);
        if (work_h && nvmap_alloc(work_h) == 0) {
            work_iova = nvmap_pin(work_h);
            /* Zeroed. The carveout hands out the same region run after
             * run with whatever the previous run's ISP left in it, and the
             * block reads this buffer (0x054 at init and in the working
             * configuration, the tile engine's scratch). Stale contents
             * here are state that survives a warm reboot and only a cold
             * start clears -- which is what a "bad boot" looked like. */
            uint32_t chunk = 65536, total = work_kb * 1024;
            void *z = calloc(1, chunk);
            for (uint32_t o = 0; o < total; o += chunk)
                nvmap_rw(work_h, o, z, total - o < chunk ? total - o : chunk, 1);
            free(z);
        }

        /* The stock camera's statistics buffers sit half a megabyte apart,
         * eight of them in rotation, so half a megabyte is what it gives
         * them. But growing ours to that stopped the frame completing --
         * measured, twice -- so the size is a knob and the default is the
         * one that works. */
        stats_h = nvmap_create(stats_kb * 1024);
        if (stats_h && nvmap_alloc(stats_h) == 0) {
            stats_iova = nvmap_pin(stats_h);
            /* Filled with a pattern of its own, so what the ISP puts there
             * is distinguishable from what was never touched. */
            void *p = malloc(64 * 1024);
            memset(p, 0x3C, 64 * 1024);
            nvmap_rw(stats_h, 0, p, 64 * 1024, 1);
            free(p);
        }

        out_h = nvmap_create(out_bytes);
        if (out_h) {
            if (nvmap_alloc(out_h) == 0) out_iova = nvmap_pin(out_h);
        }
        printf("ISP-B channel fd=%d, syncpoints %u/%u/%u/%u, output %u bytes"
               " at 0x%08x (U at +0x%x, V at +0x%x)\n", isp_fd, isp_sp,
               sp_mem, sp_stats, sp_loadv, out_bytes, out_iova, u_off, v_off);
        /* A run on a channel that is already dead measures nothing, and
         * one was scored as a result on 2026-09-06 (14:30): both warm-ups
         * timed out, the stop was never acknowledged, the output held its
         * fill -- and the frame was still discussed. So: no job goes in
         * until the channel has shown it retires one. */
        if (isp_alive_check(isp_fd, isp_sp, "BEFORE THE RUN")) return 3;
        isp_seq_base = syncpt_read(isp_sp);
        /* Submits 0..10 of the stock's opening: two zero passes, the shading
         * tables and value rounds, the ticks and the 0x053 job. */
        for (unsigned i = 0; i <= 10; i++)
            stock_open_submit(isp_fd, isp_sp, sp_mem, i, 0, stats_h, stats_iova);
        if (out_h) {
            uint32_t chunk = 65536;
            void *p = malloc(chunk);
            memset(p, 0x5A, chunk);
            for (uint32_t o = 0; o < out_bytes; o += chunk) {
                uint32_t len = out_bytes - o < chunk ? out_bytes - o : chunk;
                nvmap_rw(out_h, o, p, len, 1);
            }
            free(p);
        }
    }

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

    /* Routing before the sensor streams. The driver is explicit about the
     * order -- the pixel path has to be configured before the sensor starts
     * -- and sending it afterwards is what made the frame start vanish:
     * writing the image definition through host1x while the wire was
     * already live killed the capture, though the same value written to the
     * same register by hand did not.
     *
     * A job of its own that carries nothing and finishes at once: no
     * relocation to keep alive, nothing to park on. */
    /* Route to the ISP whenever the ISP is among the destinations -- the
     * old test asked whether memory was NOT one of them, which meant
     * delivering to both silently dropped the routing. */
    if (image_def & (IMAGE_DEF_DEST_ISP_A | IMAGE_DEF_DEST_ISP_B)) {
        uint32_t cmd_h = nvmap_create(4096);
        nvmap_alloc(cmd_h);
        uint32_t g[10];
        int n = 0;
        g[n++] = OP_SETCLASS(VI_CLASS_ID);
        g[n++] = OP_INCR(VI_METHOD(0x264), 1); g[n++] = ISPINTF_CONFIG_ENABLE;
        /* IMAGE_DEF by its method, 0x083, as the stock's per-frame VI
         * gather writes it. This used to be 0x282, which is the CIL E pad
         * register (the stock writes {0, 0, 9} there): the image word went
         * into the pad and the bring-up's zero later covered it. */
        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_IMAGE_DEF), 1); g[n++] = image_def;
        g[n++] = OP_IMM(0, sp_cmd);
        nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);
        gather_log("vi-routing", g, (unsigned)n);

        struct nvhost_cmdbuf cb = { cmd_h, 0, (uint32_t)n };
        struct nvhost_syncpt_incr si = { sp_cmd, 1 };
        uint32_t cls = VI_CLASS_ID;
        struct nvhost_fence fence = { 0, 0 };
        struct nvhost32_submit_args sa;
        memset(&sa, 0, sizeof sa);
        sa.num_syncpt_incrs = 1;
        sa.num_cmdbufs = 1;
        sa.timeout = 3000;
        sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
        sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
        sa.class_ids = (uint32_t)(uintptr_t)&cls;
        sa.fences = (uint32_t)(uintptr_t)&fence;
        errno = 0;
        int rc = ioctl(vi_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
        printf("VI routing via host1x: %d words, rc=%d (%s)\n",
               n, rc, rc == 0 ? "ok" : strerror(errno));
        ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    }

    int sfd = -1;
    {
        char sn[64];
        snprintf(sn, sizeof sn, "/dev/ov5693");
        sfd = open(sn, O_RDWR);
        if (sfd < 0) { printf("open %s: %s\n", sn, strerror(errno)); return 1; }
        /* Opening the node already powered it -- but only just. The log
         * puts the mode ioctl twenty-five microseconds after the power
         * sequence returns, and the part answers neither of the two
         * writes that follow: "no acknowledge from address 0x36". The
         * driver's power-on does not wait for the sensor to come out of
         * reset, so the wait has to be here. */
        usleep(50000);
        /* The stream itself starts here, before the receiver comes up: the
         * MIPI calibration needs the clock lane live. */
        sensor_start_front(sfd, W, H, frame_length, coarse_time, gain);
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
    /* --piggyback: touch nothing that is already running. The stock camera
     * brings this receiver up correctly, so let it, and change only where
     * the pixels go -- the destination field and the surface. If a frame
     * lands, the capture path is ours; if it does not, the fault is in the
     * write itself and not in any of the bring-up we have been repeating.
     *
     * It also corrects a wrong reading: the frame-start syncpoint counts
     * our own shots, not arriving frames -- it advances by the same amount
     * with no sensor at all. */
    /* Deep power down of the pads. The stock kernel's ardbeg_ov5693_power_on
     * -- which the sensor open above already went through -- takes CSIE
     * (DPD2 bit 12) out and leaves CSIA/CSIB in; its power_off puts CSIE
     * back, which is why a fingerprint taken after a run shows bit 12 set
     * again. So the request here is a repeat of the kernel's and the
     * status read back is the check that it held; releasing CSIB as well
     * is a departure from the stock kernel and stays behind a flag. */
    /* No clock-controller or PMC writes from here: the sensor open above
     * took the CSI-E pad out of deep power down in the kernel's own
     * power-on, and the port's clocks belong to the vi.1 module. */

    /* The driver writes this the moment VI comes up and we never wrote it at
     * all. It governs VI's internal clock gating, so with it unset the
     * registers still answer -- nvhost powers the module for an ioctl -- and
     * the write engine has no clock to run on. */
    vi_wr(TEGRA_VI_CFG_CG_CTRL, 1);

    /* Port B, one lane, on CIL E. This whole block is the 24.1 driver's
     * own port-1 path, value for value and in its order -- that code
     * captured from this sensor, which is a stronger claim than anything
     * we have inferred. Everything we had here before came from a dump
     * taken while the REAR camera streamed, so it described the other
     * brick and disagreed with this in almost every register. */
    printf("bringing up the CSI receiver (port B / CIL E, 1 lane)\n");
    vi_wr(T124_CSI_CLKEN_OVERRIDE, 0);


    /* Clear every status, both bricks, as the driver does -- stale bits
     * on a lane we are not using still gate the parser. */
    vi_wr(T124_CSI_CIL_A_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CIL_B_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CIL_C_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CIL_D_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CIL_E_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CILA_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CILB_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CILC_STATUS, 0xFFFFFFFF);
    vi_wr(T124_CSI_CILD_STATUS, 0xFFFFFFFF);
    vi_wr(T124_PP_A_PIXEL_PARSER_STATUS, 0xFFFFFFFF);
    vi_wr(T124_PP_B_PIXEL_PARSER_STATUS, 0xFFFFFFFF);
    vi_wr(VI_CSI_BASE(0) + VI_CSI_ERROR_STATUS, 0xFFFFFFFF);
    vi_wr(VI_CSI_BASE(1) + VI_CSI_ERROR_STATUS, 0xFFFFFFFF);

    /* The pads: C carries the clock-and-data back mode, D and E are
     * left at zero. E being zero is not an omission -- the lane pad
     * itself takes no configuration on this port. */
    vi_wr(T124_CILD_PAD_CONFIG0, 0x00000000);
    vi_wr(T124_CILE_PAD_CONFIG0, 0x00000000);

    vi_wr(T124_CSI_CIL_C_INT_MASK, 0x0);
    vi_wr(T124_CSI_CIL_D_INT_MASK, 0x0);
    vi_wr(T124_CSI_CIL_E_INT_MASK, 0x0);
    vi_wr(T124_PHY_CILE_CONTROL0, 0x00000009);

    /* Reset the parser, configure it, then enable -- the command word
     * is written twice on purpose, and the single-shot bit rides along
     * both times. */
    vi_wr(T124_PP_B_PIXEL_STREAM_PP_COMMAND, 0x0000f007);
    vi_wr(T124_PP_B_PIXEL_STREAM_PP_INT_MASK, 0x0);
    /* --stock-vi: the words the stock camera (an R19-era stack) writes
     * through its VI channel, read out of its gathers -- PAD_FRAME 0
     * where the R21.5 driver has NOPAD, CONTROL1 clear, the packet-skip
     * threshold 0x7f with two extra low bits in INPUT_STREAM_CONTROL.
     * The same words at 2592 and 1280 wide. */
    vi_wr(T124_PP_B_PIXEL_STREAM_CONTROL0, 0x080301f1);
    vi_wr(T124_PP_B_PIXEL_STREAM_PP_COMMAND, 0x0000f005);
    vi_wr(T124_PP_B_PIXEL_STREAM_CONTROL1, 0);
    vi_wr(T124_PP_B_PIXEL_STREAM_GAP, 0x00140000);
    vi_wr(T124_PP_B_PIXEL_STREAM_EXPECTED_FRAME, 0x0);
    vi_wr(T124_PP_B_INPUT_STREAM_CONTROL, 0x007f0014);

    /* Only the upper half of the brick command is ours; the lower half
     * belongs to the rear path and has to survive our write. The stock
     * enables brick E alone (0x10000000) and leaves C and D untouched. */
    vi_wr(T124_CSI_PHY_CIL_COMMAND,
          (vi_rd(T124_CSI_PHY_CIL_COMMAND) & 0x0000FFFF) | 0x10000000);
    vi_wr(T124_CSI_DEBUG_CONTROL, T124_CSI_DEBUG_COUNTER_CFG);
    /* --tpg: let the receiver make its own picture. This splits the
     * problem in half -- if the pattern lands in the buffer then VI,
     * the parser and the write path are all sound and the fault is on
     * the wire; if it does not, the fault is in our channel setup and
     * the sensor was never the question. */
    vi_flush("CSI bring-up");
    mipi_calibrate_csie();
    printf("  CILE pad0 after bring-up and calibration: 0x%08x\n",
           vi_rd(T124_CILE_PAD_CONFIG0));

    /* Channel setup, in the order channel.c writes it. bypass_pixel_transform
     * is 1 here: we want the raw bayer in memory, not a converted image. */
    printf("configuring CSI channel at 0x%03x\n", base);
    /* The one register outside the channel that still differs from a live
     * stock session: VI's dynamic clocking control. Ours reads 0x4040007f
     * against stock's 0x10100010, and clock management is exactly the kind
     * of thing that can hold the memory client off without reporting
     * anything. */
    /* 0x0F0 is 0x4040007f where capture works: leave it alone. */

    /* Frames start but nothing is ever written, with no fault reported --
     * which is what a memory client with no bandwidth reserved looks like.
     * Ask for a rate on the module and on memory, the way the ISP path
     * does before it moves a frame. */
    {
        struct nvhost_clk_rate_args c;
        /* The driver asks for the maximum, 600MHz, whenever a channel starts
         * streaming. We had been asking for 408. */
        c.moduleid = 0; c.rate = 600000000;
        int a1 = ioctl(vi_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &c);
        /* No VI bandwidth ioctl: the stock's trace has SET_EMC for the ISP
         * only, and on this kernel the VI one fails inside the isomgr
         * ("bad handle" for vi.1) after having set the latency allowance,
         * leaving an error in dmesg on every run. The old "moduleid = 1"
         * memory request that used to sit here matched no pdata entry and
         * set the VI clock a second time. */
        printf("clock request: VI module rc=%d\n", a1);
    }

    /* The syncpoint control register, which we had never written at all --
     * the driver puts 0x100 here before any capture. */
    vi_wr(0x004, 0x00000100);

    /* Reset the channel first. Its single-shot bit has been left armed by
     * every attempt that never completed, and nothing clears it -- the
     * driver's own recovery path resets the channel for exactly this. */
    vi_wr(base + VI_CSI_SW_RESET, 0xF);
    vi_wr(base + VI_CSI_SW_RESET, 0x0);

    vi_wr(base + VI_CSI_ERROR_STATUS, 0xFFFFFFFF);
    {
        /* Two VI registers the stock writes and no V4L2 driver of ours ever
         * has: the DVFS word, and SINGLE_SHOT_STATE_UPDATE = 1 once per
         * session. Both from the stock's VI gathers, the same at every
         * resolution. */
        vi_wr(TEGRA_VI_CFG_DVFS, 0x10100010);
        vi_wr(base + VI_CSI_SINGLE_SHOT_STATE_UPDATE, 1);
    }
    /* Measured off a live stock session on this channel: 0x00200004. The
     * transform-bypass bit is CLEAR there, where we had been setting it,
     * and the low nibble carries a 4 we had left at zero. Bypass off is
     * what the driver does whenever the ISP is in the path. */
    vi_wr(base + VI_CSI_IMAGE_DEF, image_def);
    /* The interface between the channel and the ISP. Nothing reaches the
     * ISP with this at zero, whatever the destination bits say. */
    vi_wr(base + VI_CSI_ISPINTF_CONFIG, ISPINTF_CONFIG_ENABLE);
    /* What the capture has for this group: 0x001c984c, and zero here kills
     * the path. */
    vi_wr(base + VI_CSI_RGB2Y_CTRL, 0x001c984c);
    vi_wr(base + VI_CSI_MEM_TILING, 0);
    vi_wr(base + VI_CSI_IMAGE_DT, IMAGE_DT_RAW10);
    vi_wr(base + VI_CSI_IMAGE_SIZE_WC, wc);
    vi_wr(base + VI_CSI_IMAGE_SIZE, (H << IMAGE_SIZE_HEIGHT_OFFSET) | W);
    /* The address goes in here as well as through the submit. The
     * relocation's pin lasts only as long as the job, and the write
     * happens afterwards, when the frame completes -- by which time that
     * mapping may be gone, which would look exactly like this: frames
     * arriving, nothing written, nothing complaining. Our own pin lives
     * for the whole run. */
    vi_wr(base + VI_CSI_SURFACE0_OFFSET_MSB, 0);
    vi_wr(base + VI_CSI_SURFACE0_OFFSET_LSB, iova);
    vi_wr(base + VI_CSI_SURFACE0_STRIDE, stride);

    /* The rest of the channel, as it stands when a frame lands. None of
     * these were in our setup at all -- copied from that state rather than
     * derived, and the one whose meaning is plain, the second stride at
     * +0x58, agrees with our geometry. */
    vi_wr(base + 0x08, 0x00000001);
    vi_wr(base + 0x10, 0x001c984c);
    vi_wr(base + 0x30, 0x0054c004);
    vi_wr(base + 0x38, 0x0c232102);
    vi_wr(base + 0x40, 0x08080808);
    vi_wr(base + 0x48, 0x30210015);
    vi_wr(base + 0x50, 0x202021ba);
    vi_wr(base + 0x58, 0x00001110);
    vi_wr(base + 0x5c, 0x00000484);
    vi_wr(base + 0x60, 0x00000001);
    vi_wr(base + 0x64, 0x00000003);

    printf("  CILE pad0 after the sensor start: 0x%08x\n",
           vi_rd(T124_CILE_PAD_CONFIG0));

    /* Pixel parser: single shot, armed for one frame. */
    uint32_t pp = PP_B_PIXEL_STREAM_PP_COMMAND;
    vi_wr(pp, (0xFu << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
              CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_ENABLE);

    /* Arm the syncpoint conditions before the shot. VI_INCR_SYNCPT is a
     * command register -- it takes a condition and a syncpoint id and arms
     * one increment -- so it reads back as zero and never showed up in the
     * comparison against stock. The memory-write acknowledge is the one
     * that has to be armed before the DMA starts. */
    /* Two different syncpoints, as the driver uses: one for the frame
     * start and a separate one for the memory-write acknowledge. We had
     * been arming both conditions against the same id. */
    /* Both acknowledge conditions. The header says outright that these
     * event numbers were found by trial rather than derived, so which of
     * the two belongs to this port is worth not assuming. */
    /* Only the frame start is armed here. The driver arms the memory-write
     * acknowledge only after the frame start has fired -- once the DMA is
     * already running -- so arming it up front was our invention. */
    vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, T124_PPB_FRAME_START << 8 | sp_id);

    /* And the trigger goes in the same batch: the receiver has to still be
     * powered and configured when the shot is fired, which is the whole
     * reason for doing this in one call. */
    vi_flush("setup");


    /* When the frame goes to the ISP there is no surface for VI to write and
     * so nothing to keep mapped -- which is just as well, because the job we
     * used to park for that purpose is what killed the VI channel twice: the
     * kernel caps its timeout at ten seconds and a run with retries in it
     * goes past that. Nothing here goes through host1x on the VI side at
     * all; the routing and the trigger are plain register writes. */
    if (image_def & IMAGE_DEF_DEST_MEM) {
        uint32_t cmd_h = nvmap_create(4096);
        nvmap_alloc(cmd_h);
        uint32_t g[24];
        int n = 0, addr_word;
        g[n++] = OP_SETCLASS(VI_CLASS_ID);

        /* Routing to the ISP, through host1x methods rather than register
         * writes. The note left in the 24.1 driver is explicit about the
         * difference: writing these registers sets the bits but does not
         * activate the pixel path, while the methods do. The numbering is
         * the driver's own -- 0x099 for port B's ISP interface and 0x282
         * for its image definition -- and it is not the +0x1FF form the
         * rest of the channel uses. */
        g[n++] = OP_INCR(VI_METHOD(0x264), 1);
        g[n++] = ISPINTF_CONFIG_ENABLE;
        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_IMAGE_DEF), 1);
        g[n++] = image_def;

        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_SURFACE0_OFFSET_MSB), 1);
        g[n++] = 0;
        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_SURFACE0_OFFSET_LSB), 1);
        addr_word = n;
        g[n++] = 0;                       /* the kernel fills this in */
        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_SINGLE_SHOT), 1);
        g[n++] = SINGLE_SHOT_CAPTURE;

        /* No parking. This job used to wait on the memory-write condition
         * (41), which never fires on this path -- the kernel refused our
         * CPU increment of it ("beyond max") -- so the job sat until its
         * timeout, ten seconds after every --no-isp run: cdma_timeout 42,
         * the VI channel dead, and the device unstable in idle afterwards.
         * Our own pin keeps the surface mapped for the whole run, and the
         * address is written to the registers as well, so the job may
         * retire at once. */
        /* Retire the command buffer on a syncpoint of its own. It used to
         * share one with the frame-start condition, so that counter moved
         * once per submit whether or not a frame ever started -- which is
         * exactly the reading we were treating as evidence. */
        g[n++] = OP_IMM(0, sp_cmd);
        nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);
        gather_log("vi-capture", g, (unsigned)n);

        struct nvhost_reloc rel = { cmd_h, (uint32_t)addr_word * 4, buf_h, 0 };
        struct nvhost_reloc_shift sh = { 0 };
        struct nvhost_cmdbuf cb = { cmd_h, 0, (uint32_t)n };
        struct nvhost_syncpt_incr si = { sp_cmd, 1 };
        uint32_t cls = VI_CLASS_ID;
        struct nvhost_fence fence = { 0, 0 };
        struct nvhost32_submit_args sa;
        memset(&sa, 0, sizeof sa);
        sa.num_syncpt_incrs = 1;
        sa.num_cmdbufs = 1;
        sa.num_relocs = 1;
        /* This job is parked on purpose and must outlive the capture. Three
         * seconds was the default and the capture grew past it once the ISP
         * joined in, so host1x killed the VI channel -- vi0_stream timing
         * out in the log. Long enough that only a genuine hang reaches it. */
        sa.timeout = 60000;
        sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
        sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
        sa.relocs = (uint32_t)(uintptr_t)&rel;
        sa.reloc_shifts = (uint32_t)(uintptr_t)&sh;
        sa.class_ids = (uint32_t)(uintptr_t)&cls;
        sa.fences = (uint32_t)(uintptr_t)&fence;

        /* Submit once, only to get the surface address written through a
         * relocation -- that is the sole reason this goes through host1x.
         * The trigger is a different matter: the driver fires it with a
         * plain register write, and doing it any other way is a difference
         * we introduced and never justified. */
        errno = 0;
        int rc = ioctl(vi_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
        printf("surface programmed via host1x: %d words, rc=%d (%s)\n",
               n, rc, rc == 0 ? "ok" : strerror(errno));
        usleep(50000);
        ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    }

    {
        /* Baselines, because a counter's value says nothing on its own --
         * reading 10 there once had us believe the stats stage was running
         * when the number was simply where it already stood. */
        if (isp_fd >= 0 && out_iova && stats_h) {
            isp_base_mem = syncpt_read(sp_mem);
            /* The channel's other two counters by position, armed or not:
             * what moves on them over the run is what a job may declare. */
            isp_base_stats = syncpt_read(sp_mem + 1);
            isp_base_loadv = syncpt_read(sp_mem + 3);
        }

        /* A trigger written while the sensor is part way through a frame
         * captures only what is left of it: the first shot wrote rows 0 to
         * 1704 and stopped, because that is how many lines remained. There
         * is no register that says "wait for the boundary", so the shot is
         * simply repeated until one lands in the blanking and the frame
         * arrives whole. In practice the retry is the one right after the
         * short frame ends, which is exactly where the boundary is.
         *
         * The fill pattern is what tells us: the last line losing it means
         * the frame reached the bottom. A counter cannot tell us the same --
         * the acknowledge condition fires a millisecond after it is armed,
         * wherever the frame happens to be. */
        uint8_t fill[64];
        memset(fill, 0xA5, sizeof fill);

        /* Arm the frame-start condition and block until it moves. Every use
         * of this sits between frames, never during one. */
        #define WAIT_FRAME_START(limit_ms) ({                                 \
            uint32_t _b = syncpt_read(sp_id);                                 \
            vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT,                                \
                  T124_PPB_FRAME_START << 8 | sp_id);                         \
            vi_flush(0);                                                      \
            int _w = 0;                                                       \
            while (syncpt_read(sp_id) == _b && _w < (limit_ms)) {             \
                usleep(500); _w++;                                            \
            }                                                                 \
            syncpt_read(sp_id) != _b ? _w : -1;                               \
        })

        /* A trigger written part way through a frame captures only what is
         * left of it -- the first one wrote rows 0 to 1704, exactly the
         * lines that remained. The whole frame comes from triggering in the
         * blanking, and the blanking is narrow: the sensor's own registers
         * put it at forty lines out of 1984, a millisecond and a half.
         *
         * So the frame period is measured first, from one start to the next,
         * and the trigger is aimed just short of the following one. */
        vi_wr(base + VI_CSI_SW_RESET, 0xF);
        vi_wr(base + VI_CSI_SW_RESET, 0x0);
        vi_wr(pp, (0xFu << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
                  CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_RST);
        vi_wr(pp, (0xFu << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
                  CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_ENABLE);
        vi_flush(0);

        int period = 0;
        WAIT_FRAME_START(400);
        for (int i = 0; i < 3; i++) {
            int p = WAIT_FRAME_START(400);
            if (p > 0 && (period == 0 || p < period)) period = p;
        }
        period = period > 0 ? period : 132;      /* half-millisecond ticks */
        printf("  frame period: %d.%d ms\n", period / 2, (period % 2) * 5);

        /* Bring the tile engine up the way stock does, before asking for a
         * real frame: a warm-up frame with the enable inside it, then the
         * coefficients, then a second warm-up frame, then the working
         * configuration. Skipping all of this and going straight to a real
         * frame is what left the luma path cold. */
        uint32_t warm_h = 0;
        int warm_left = 0;
        if (isp_fd >= 0) {
            warm_h = nvmap_create(64 * 1024);
            if (warm_h && nvmap_alloc(warm_h) == 0) {
                nvmap_pin(warm_h);
                warm_left = 2;
            } else {
                warm_h = 0;
            }
        }

        mw_base = syncpt_read(sp_mw);
        fe_base = syncpt_read(VI1_ISPB_SYNCPT);
        for (int shot = 0; shot < shots; shot++) {
            int started = 0, waited = 0;

            uint32_t fs0 = syncpt_read(sp_id);
            /* The ISP channel's own counter (38): every job of ours ends
             * with an immediate increment on it, so it tells whether the
             * job has actually executed -- config loaded, trigger 0x05
             * given -- as opposed to merely being queued. */
            uint32_t isp_fence0 = isp_fd >= 0 ? syncpt_read(isp_sp) : 0;

            /* Wiping is a diagnostic, not part of a capture: it is ten
             * megabytes through a hundred and fifty ioctls, it runs
             * while the hardware is writing, and the holes it leaves in
             * the picture are its own. Off by default for that reason --
             * and the row-to-row jumps that looked like tearing were
             * those holes, since the fill pattern reads as 42405 against
             * a picture whose values sit near 25. */
            nvmap_rw(buf_h, frame - sizeof fill, fill, sizeof fill, 1);

            /* Reset only on the way in. A reset resynchronises the
             * parser to wherever the sensor is right now, so resetting
             * before every retry guaranteed every retry started mid
             * frame -- eight attempts, eight short frames. Left alone,
             * the parser finishes one capture at the end of a frame and
             * the next trigger is already on the boundary. */
            /* The surface goes in again before every frame, exactly as
             * the driver's start-thread does it. We had written it once
             * through host1x and assumed it stayed -- it reads back
             * unchanged, but the driver reprogramming it per frame is
             * not decoration, and one capture per programming is what
             * the short frames look like. */
            /* The ISP's per-frame gather goes out just before the
             * trigger, the way the driver's start-thread does it: the
             * block has to be armed and waiting when the pixels cross,
             * because nothing buffers them on the way. */
            /* And the working configuration after the first frame, in
             * the place the capture puts it. */
            if (isp_fd >= 0 && out_iova && shot > 0 && !real_sent) {
                isp_real_pass(isp_fd, isp_sp, work_iova, stats_iova, W, OH);
                real_sent = 1;
            }

            /* The warm-up needs pixels, which is what running it before
             * the capture ever started was missing. So it goes here
             * instead, in place of a real frame, while the receiver is
             * armed and the sensor is delivering -- the way stock runs
             * it, between frames of a live stream. */
            if (warm_left > 0 && isp_fd >= 0 && warm_h) {
                /* The stock's two warm-up captures, with its curves job
                 * between them (submits 11, 13, 14); the ticks are the
                 * caller's polling below. No demosaic or colour jobs of
                 * our own: the stock carries their content in the
                 * working configuration. */
                isp_base_mem = syncpt_read(sp_mem);
                if (warm_left == 1) {
                    stock_open_submit(isp_fd, isp_sp, sp_mem, 12, warm_h, stats_h, stats_iova);
                    stock_open_submit(isp_fd, isp_sp, sp_mem, 13, warm_h, stats_h, stats_iova);
                }
                stock_open_submit(isp_fd, isp_sp, sp_mem, warm_left == 2 ? 11 : 14,
                                  warm_h, stats_h, stats_iova);
            }
            else if (isp_fd >= 0 && out_iova && stats_h) {
                isp_base_mem = syncpt_read(sp_mem);
                isp_base_stats = syncpt_read(sp_stats);
                isp_cal_round(isp_fd, isp_sp);
                isp_frame(isp_fd, out_h, stats_h, W, OH, isp_fmt, u_off, v_off,
                          sp_mem, sp_stats, sp_loadv, isp_sp);
                /* And the same flush job the stream queues behind its last
                 * frame: the frame's output lands only with a job behind it,
                 * and the stop job must not be that job -- it disabled the
                 * pipeline while chroma and statistics were still being
                 * written. An 8x8 job without parking, into the warm-up
                 * buffer, which stays mapped until the run ends. */
                if (warm_h)
                    isp_warmup(isp_fd, isp_sp, warm_h, stats_h, 0, W, OH);
            }

            /* The stock's order, in its VI gathers: WAIT on the ISP
             * channel's counter for the job just submitted, and only
             * then the single-shot. Arming the VI before the ISP job has
             * executed lets the frame arrive at an ISP that is not yet
             * taking lines; at 2592 wide that showed as parser overflow
             * (0x34/0xb4) and bands of the picture shifted sideways by
             * the pixels lost. */
            if (isp_fd >= 0 && (warm_h || (out_iova && stats_h))) {
                int wj = 0;
                while (syncpt_read(isp_sp) == isp_fence0 && wj < 500) {
                    usleep(1000);
                    wj++;
                }
                if (wj >= 500)
                    printf("  ISP job not executed within 500 ms (38 unchanged)\n");
            }
            vi_wr(base + VI_CSI_SURFACE0_OFFSET_MSB, 0);
            vi_wr(base + VI_CSI_SURFACE0_OFFSET_LSB, iova);
            vi_wr(base + VI_CSI_SURFACE0_STRIDE, stride);
            if (!cile_rewritten) {
                /* The stock re-writes the CILE pads right before its
                 * FIRST single-shot (VI gather 0x282/3: pad0 0, pad1 0,
                 * THS 9) and never again -- the later per-frame VI
                 * gathers carry only the wait and the shot. After the
                 * failed first run of every boot this pad register read
                 * 0x00200001 where the bring-up had written 0; after a
                 * working run, 0. Something between the sensor start
                 * and the shot rewrites it, and this writes it back.
                 * Once only: doing it before every shot, on a lane
                 * already in HS, broke every frame after the first
                 * (parser 0x1b4). */
                printf("  CILE pad0 before the first shot: 0x%08x\n",
                       vi_rd(T124_CILE_PAD_CONFIG0));
                vi_wr(T124_CILE_PAD_CONFIG0, 0x00000000);
                vi_wr(T124_CILE_PAD_CONFIG0 + 4, 0x00000000);
                vi_wr(T124_PHY_CILE_CONTROL0, 0x00000009);
                cile_rewritten = 1;
            }
            vi_wr(pp, (0xFu << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
                      CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_ENABLE);
            vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, T124_PPB_FRAME_START << 8 | sp_id);
            /* --no-isp: the frame goes to memory, and the write's completion
             * is the memory-write-ack condition of port B on the channel's
             * second syncpoint -- armed here, one arm per shot, the way the
             * stock's done-thread arms it. It used to be waited on without
             * ever being armed. */
            if (no_isp)
                vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, T124_MWB_ACK_DONE << 8 | sp_mw);
            /* Frame end of port B onto 46, every shot: whether the parser
             * reaches the end of a frame at all is the receiver-side fact
             * the run's summary reports. */
            vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, (0x0fu << 8) | VI1_ISPB_SYNCPT);
            vi_wr(base + VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);
            vi_flush(0);

            waited = 0;
            while (syncpt_read(sp_id) == fs0 && waited < 400) {
                usleep(1000);
                waited++;
            }
            started = syncpt_read(sp_id) != fs0;

            /* Release the ISP's parked job as soon as the block says it
             * has written, and give up after a bounded wait either way.
             * Holding it until the end of the whole capture is what kept
             * running past the job timeout the kernel allows -- and a
             * job that overruns takes the ISP channel with it, which
             * costs a reboot. Short and self-limiting instead. */
            /* The parked job now holds the mapping until the frame and
             * the statistics are actually done, so the loop here only
             * watches -- no more keepalive submits every two
             * milliseconds, which was what buried the channel in
             * three-second timeouts whenever anything stalled. */
            if (isp_fd >= 0 && out_iova) {
                int w2 = 0;
                /* A full-resolution frame writes at roughly three rows a
                 * millisecond here, so it needs the better part of a
                 * second -- six hundred milliseconds cut it off at
                 * seventeen hundred rows of nineteen hundred. */
                while (syncpt_read(sp_mem) == isp_base_mem && w2 < isp_wait_ms) {
                    usleep(2000);
                    w2 += 2;
                }
                /* A moment beyond the condition, because the last
                 * transfer may still be draining when it fires. */
                if (syncpt_read(sp_mem) == isp_base_mem) isp_nowrite++;
                printf("  ISP wrote after %dms%s\n", w2,
                       syncpt_read(sp_mem) != isp_base_mem ? "" : " (NO)");
            }

            /* One whole frame from the start, plus what the caller asks
             * for on top -- unless --fast-arm. The sensor's active part
             * is about a third of the 66 ms period; a trigger that
             * arrives a quarter of a second after the last frame lands
             * at a random phase, and inside the active part it catches
             * a frame already under way: the receiver flags it (parser
             * 0x34, 0x1b4) and the block never completes. The stock
             * camera arms the next frame the moment the previous one is
             * done, inside the blanking. */
            printf("  frame %d: %s (start %dms, settle %dms), parser %08x\n",
                   shot, started ? "started" : "NEVER STARTED", waited, settle,
                   vi_rd(T124_PP_B_PIXEL_PARSER_STATUS));

            /* A warm-up round is not one of the frames that were asked
             * for, so it does not spend one. */
            if (warm_left > 0) {
                printf("  (that was a warm-up round, output condition %+d)\n",
                       (int)(syncpt_read(sp_mem) - isp_base_mem));
                if (--warm_left == 0) {
                    /* Where the capture puts it: after the warm-up and the
                     * coefficients, once, and never in the opening round. */
                    stock_open_submit(isp_fd, isp_sp, sp_mem, 15, warm_h, stats_h, stats_iova);
                    if (!real_sent) {
                        isp_real_pass(isp_fd, isp_sp, work_iova, stats_iova, W, OH);
                        real_sent = 1;
                    }
                }
                shot--;
            }
        }
        if (warm_h) {
            /* The warm-up wrote an eight-by-eight decimation of the whole
             * frame into this buffer. Dump it before letting it go. */
            uint8_t w[512];
            if (nvmap_rw(warm_h, 0, w, sizeof w, 0) == 0) {
                FILE *wf = fopen("/data/local/tmp/viisp_warm.bin", "wb");
                if (wf) { fwrite(w, 1, sizeof w, wf); fclose(wf); }
                printf("warm buffer (8x8 decimated frame), 64 bytes:");
                for (int i = 0; i < 64; i++)
                    printf(" %02x", w[i * 4]);   /* stride 0x100, one row of 8 every 256 */
                printf("\n");
                printf("warm rows:");
                for (int r = 0; r < 8; r++) {
                    printf("\n  ");
                    for (int c = 0; c < 8; c++)
                        printf(" %02x", w[r * 256 + c]);
                }
                printf("\n");
            }
            nvmap_unpin(warm_h);
            ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)warm_h);
        }

        /* The session's end, the stock's way (impl-1, stock-front-timeline
         * §5): not one write reaches the ISP -- no 0x015 = 0, no stop job
         * -- the client waits for the frame's fences, releases its
         * buffers, and on the VI side clears the routing and switches
         * brick E off; the ISP keeps its last configuration and loses
         * power when it idles. Ours used to disable the ISP first, and
         * the device hung in idle after two of the three runs that
         * followed the stock's opening. So: fences first, bounded. */
        if (isp_fd >= 0 && out_iova) {
            int w = 0;
            while ((syncpt_read_max(isp_sp) != syncpt_read(isp_sp)
                    || syncpt_read(sp_mem) == isp_base_mem
                    || syncpt_read(sp_stats) == isp_base_stats) && w < 1000) {
                usleep(2000);
                w += 2;
            }
            isp_fences_met = syncpt_read_max(isp_sp) == syncpt_read(isp_sp)
                             && syncpt_read(sp_mem) != isp_base_mem
                             && syncpt_read(sp_stats) != isp_base_stats;
            printf("session end: fences %s after %d ms (38 %u/%u, 36 %+d, 37 %+d)\n",
                   isp_fences_met ? "met" : "NOT MET", w,
                   syncpt_read(isp_sp), syncpt_read_max(isp_sp),
                   (int)(syncpt_read(sp_mem) - isp_base_mem),
                   (int)(syncpt_read(sp_stats) - isp_base_stats));
        }
        /* The stock's VI-side teardown: the routing cleared and brick E
         * switched off. The low half of the brick command belongs to the
         * rear path and survives. */
        vi_wr(base + VI_CSI_IMAGE_DEF, 0);
        vi_wr(T124_CSI_PHY_CIL_COMMAND,
              (vi_rd(T124_CSI_PHY_CIL_COMMAND) & 0x0000FFFF) | 0x20000000);
        vi_flush("session end (VI unrouted, brick E off)");

        /* Release whichever job is parked -- VI's on the memory path, the
         * ISP's on this one. Both wait on the same counter and neither can
         * strand a channel, because raising it is ours to do. */
        if (hold_thresh || isp_hold_thresh) {
            int cfd = open("/dev/nvhost-ctrl", O_RDWR);
            if (cfd >= 0) {
                struct nvhost_ctrl_syncpt_incr_args ia = { sp_mw };
                int irc = ioctl(cfd, NVHOST_IOCTL_CTRL_SYNCPT_INCR, &ia);
                close(cfd);
                printf("held mapping released: syncpoint %u, rc=%d\n",
                       sp_mw, irc);
            }
        }
    }
    usleep(200000);

    printf("readback: IMAGE_DEF=0x%08x DT=0x%08x SIZE=0x%08x WC=0x%08x\n",
           vi_rd(base + VI_CSI_IMAGE_DEF), vi_rd(base + VI_CSI_IMAGE_DT),
           vi_rd(base + VI_CSI_IMAGE_SIZE), vi_rd(base + VI_CSI_IMAGE_SIZE_WC));
    printf("readback: SURFACE0=0x%08x STRIDE=0x%08x PP=0x%08x\n",
           vi_rd(base + VI_CSI_SURFACE0_OFFSET_LSB),
           vi_rd(base + VI_CSI_SURFACE0_STRIDE), vi_rd(pp));

    /* Ask the hardware whether it saw anything at all. The frame-start and
     * memory-write conditions each increment a syncpoint, so a value that
     * has not moved says plainly that no frame started and nothing was
     * written -- which is a different failure from a frame that arrived
     * and went astray. */
    {
        int ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR);
        struct nvhost_ctrl_syncpt_read_args r1 = { sp_id, 0 }, r2 = { sp_mw, 0 };
        ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &r1);
        ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &r2);
        close(ctrl_fd);
        printf("syncpoints after: frame %u = %u, memory write %u = %u\n",
               sp_id, r1.value, sp_mw, r2.value);
    }

    /* Read the lane interface while the sensor is still powered. A stock
     * session shows 0x110 here; zero means nothing ever arrived on the
     * wire, which points at the sensor rather than the receiver. */
    printf("CIL status: E=0x%08x CILE=0x%08x parser=0x%08x; frame ends (46) this run: %u\n",
           vi_rd(0xA18), vi_rd(0xA1C), vi_rd(T124_PP_B_PIXEL_PARSER_STATUS),
           syncpt_read(VI1_ISPB_SYNCPT) - fe_base);

    /* The channel is exclusive, so nothing outside this process can read
     * the aperture while we hold it -- and picking ranges by hand is how a
     * difference gets missed. Dump the lot and diff it against a stock
     * session offline. */

    uint32_t err = vi_rd(base + VI_CSI_ERROR_STATUS);
    printf("ERROR_STATUS = 0x%08x, SINGLE_SHOT = 0x%08x\n",
           err, vi_rd(base + VI_CSI_SINGLE_SHOT));

    nvmap_invalidate(buf_h, frame);

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

    /* The ISP's output is the point of this tool. Report whether anything
     * landed in it before saving, because "the file exists" has misled us
     * before. */
    if (out_h && out_iova) {
        uint32_t chunk = 65536, changed = 0, nz = 0;
        uint8_t *p = malloc(chunk);
        for (uint32_t o = 0; o < out_bytes; o += chunk) {
            uint32_t part = out_bytes - o < chunk ? out_bytes - o : chunk;
            if (nvmap_rw(out_h, o, p, part, 0) < 0) break;
            for (uint32_t i = 0; i < part; i++) {
                if (p[i] != 0x5A) changed++;
                if (p[i]) nz++;
            }
        }
        free(p);
        printf("ISP output: %u of %u bytes differ from the fill, %u"
               " non-zero\n", changed, out_bytes, nz);
        /* Whether the ISP believes it did anything: the three conditions the
         * per-frame gather arms. All still means it never ran. */
        printf("ISP output condition (syncpoint %u) moved by %+d\n",
               sp_mem, (int)(syncpt_read(sp_mem) - isp_base_mem));
        /* And the other two, so a declaration can match what the hardware
         * actually raises. Declaring an increment that never comes leaves
         * the kernel's count ahead of the hardware for the rest of the
         * boot, for every user of that counter -- the stock camera went
         * black after one such run. */
        printf("ISP statistics condition (syncpoint %u) moved by %+d, "
               "loadv (syncpoint %u) moved by %+d\n",
               sp_mem + 1, (int)(syncpt_read(sp_mem + 1) - isp_base_stats),
               sp_mem + 3, (int)(syncpt_read(sp_mem + 3) - isp_base_loadv));

        /* The stats condition is the one that fires, so this is where to
         * look for proof that pixels reached the ISP at all. Numbers here
         * that track the scene mean the frame crossed from VI; a buffer
         * still holding its fill means the condition fires on something
         * other than work done. */
        if (stats_h) {
            uint32_t sbytes = 64 * 1024, sch = 0, snz = 0;
            uint8_t *s = malloc(sbytes);
            if (nvmap_rw(stats_h, 0, s, sbytes, 0) == 0) {
                for (uint32_t i = 0; i < sbytes; i++) {
                    if (s[i] != 0x3C) sch++;
                    if (s[i]) snz++;
                }
                printf("ISP stats: %u of %u bytes differ from the fill, %u"
                       " non-zero\n", sch, sbytes, snz);
                printf("  first words:");
                for (int i = 0; i < 8; i++)
                    printf(" %08x", ((uint32_t *)s)[i]);
                printf("\n");
                if (dump) {
                    FILE *sf = fopen("/data/local/tmp/viisp_stats.bin", "wb");
                    if (sf) { fwrite(s, 1, sbytes, sf); fclose(sf);
                        printf("saved /data/local/tmp/viisp_stats.bin\n"); }
                }
            }
            free(s);
        }

        if (dump) {
            FILE *f = fopen("/data/local/tmp/viisp_out.raw", "wb");
            if (f) {
                uint8_t *q = malloc(chunk);
                for (uint32_t o = 0; o < out_bytes; o += chunk) {
                    uint32_t part = out_bytes - o < chunk ? out_bytes - o : chunk;
                    nvmap_rw(out_h, o, q, part, 0);
                    fwrite(q, 1, part, f);
                }
                free(q);
                fclose(f);
                printf("saved /data/local/tmp/viisp_out.raw (%u bytes)\n",
                       out_bytes);
            }
        }
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
        /* The front sensor powers down when its node is closed; it has no
         * power call of its own. */
        close(sfd);
        printf("sensor powered down\n");
    }
    goto shutdown;

shutdown:
    /* --no-isp: the last frame's write must have landed before the buffer
     * is unpinned -- it had not, once ("mc-err: (vi) csw_viw" at the
     * frame buffer's address). The memory-write-ack condition armed with
     * each shot moves the channel's second syncpoint when the write is
     * done; wait for that, with the stock's bound, then unpin. */
    if (no_isp) {
        int wmw = 0;
        while (syncpt_read(sp_mw) == mw_base && wmw < 200) { usleep(1000); wmw++; }
        printf("memory-write ack %s after %d ms\n",
               syncpt_read(sp_mw) != mw_base ? "seen" : "NOT seen", wmw);
    }
    nvmap_unpin(buf_h);
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)buf_h);
    close(vi_fd);
    close(nvmap_fd);
    /* The line the wrapper reads. A job still owed on the sequencing
     * counter, or a stop the block never took, is a dead channel: the
     * kernel will print its timeout within isp_job_timeout_ms, and nothing
     * this run produced is evidence of anything. */
    int rc_final = 0;
    if (isp_fd >= 0 && isp_sp) {
        uint32_t mn = syncpt_read(isp_sp), mx = syncpt_read_max(isp_sp);
        unsigned owed = mx - mn, done = mn - isp_seq_base;
        if (owed == 0 && isp_fences_met != 0) {
            printf("ISP VERDICT: ALIVE -- counter %u: %u job(s) this run, all retired%s%s\n",
                   isp_sp, done,
                   isp_fences_met > 0 ? ", fences met" : "",
                   isp_nowrite ? " -- BUT the block produced no write in some round(s), see above" : "");
        } else {
            printf("ISP VERDICT: DEAD -- counter %u value %u promised %u: %u job(s) still owed%s;"
                   " the kernel times the channel out within %d s;"
                   " THIS RUN IS NOT EVIDENCE -- reboot before the next\n",
                   isp_sp, mn, mx, owed,
                   isp_fences_met == 0 ? ", fences never met" : "",
                   isp_job_timeout_ms / 1000 + 1);
            rc_final = 3;
        }
        if (isp_nowrite)
            printf("ISP NOTE: %d round(s) ended without the ISP's output write\n", isp_nowrite);
    }
    printf("=== done ===\n");
    return rc_final;
}
