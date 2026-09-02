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
#define TPG_B_BASE                           (0x86C + 0x18C)
#define TPG_CTRL                             0x000
#define TPG_PHASE                            0x008
#define TPG_RED_FREQ                         0x00C
#define TPG_RED_FREQ_RATE                    0x010
#define TPG_GREEN_FREQ                       0x014
#define TPG_GREEN_FREQ_RATE                  0x018
#define TPG_BLUE_FREQ                        0x01C
#define TPG_BLUE_FREQ_RATE                   0x020
#define PG_MODE_OFFSET                       2
#define PG_ENABLE                            0x1
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
#include "isp_b_cal.h"
#include "isp_stock.h"
#include "isp_demosaic.h"
#include "isp_real.h"

/* Put the stock camera's own configuration into the gather.
 *
 * Every block it fills in when it opens the ISP, with the values it fills
 * them with -- read out of the running process, not reconstructed. This is
 * what was missing: the shape of these blocks we had right, and the
 * clearing pass matches stock block for block, but the coefficient arrays
 * behind the demosaic (0x903, sixty-four words, and 0x907, thirty-six) we
 * only ever zeroed. The stage was switched on and computing with nothing.
 *
 * The work buffer's address is ours, not the one the stock process had.
 */
/* Which part of the pipeline a block belongs to.
 *
 * Sending the whole table at once cost a capture and a reboot: the
 * statistics blocks went in with it, ours had been configured differently,
 * and the frame then waited on a statistics syncpoint that never moved.
 * One thing at a time, so a failure says which thing.
 */
#define STOCK_DEMOSAIC 1
#define STOCK_COLOUR   2
#define STOCK_STATS    4
#define STOCK_INPUT    8

static unsigned stock_group(unsigned method)
{
    switch (method) {
    case 0x900: case 0x902: case 0x904: case 0x906: case 0x908:
    case 0x506:
        return STOCK_DEMOSAIC;
    case 0x600: case 0x650: case 0x651: case 0x653: case 0x655:
    case 0x657: case 0xd00: case 0xd0a: case 0xd0c: case 0xd20:
        return STOCK_COLOUR;
    case 0x909: case 0x910: case 0x919: case 0x91b: case 0x91d:
    case 0x91f: case 0x920:
        return STOCK_STATS;
    default:
        return STOCK_INPUT;
    }
}

unsigned isp_stock_emit(uint32_t *g, unsigned n, uint32_t work_iova,
                               unsigned groups)
{
    unsigned count = sizeof isp_stock_blocks / sizeof isp_stock_blocks[0];
    for (unsigned b = 0; b < count; b++) {
        const struct isp_block *bl = &isp_stock_blocks[b];
        const uint32_t *d = bl->data;

        /* Skipped deliberately: its second word is where the stock process
         * kept its scratch buffer, and that address means nothing here. */
        if (bl->method == 0x053) continue;
        if (!(stock_group(bl->method) & groups)) continue;

        if (bl->mode == 0) {
            g[n++] = OP_INCR(bl->method, bl->count);
            for (unsigned i = 0; i < bl->count; i++) g[n++] = d[i];
        } else {
            /* One word to the method itself, the rest to the array port
             * that follows it. */
            g[n++] = OP_INCR(bl->method, 1);
            g[n++] = d[0];
            g[n++] = OP_NONINCR(bl->method + 1, bl->count - 1);
            for (unsigned i = 1; i < bl->count; i++) g[n++] = d[i];
        }
    }
    if (groups) { g[n++] = OP_INCR(0x053, 2); g[n++] = 1; g[n++] = work_iova; }
    return n;
}

int isp_init(int isp_fd, uint32_t work_h, uint32_t enable, uint32_t sp,
                    uint32_t work_iova, uint32_t stats_iova, int demosaic_zero,
                    uint32_t rt_luma, uint32_t ccm_word, unsigned skip,
                    uint32_t gpp_gain, int luma_lo,
                    uint32_t in_dims, uint32_t in_mode, uint32_t in_phase,
                    int zero_init, int apply, uint32_t stats_ctrl,
                    int stock_cfg, const struct geom_cfg *geo)
{
    unsigned words = sizeof isp_b_cal_data / sizeof isp_b_cal_data[0];
    /* Room for the zero-init as well as the blob: that clearing pass alone
     * is fourteen hundred words. */
    /* Two clearing passes plus the runtime block and the blob. */
    uint32_t bytes = (words + 12288) * 4;
    uint32_t cmd_h = nvmap_create((bytes + 4095) & ~4095u);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    uint32_t *g = malloc(bytes);
    unsigned n = 0;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);

    /* What the stock camera actually does when it opens this block, taken
     * from its own trace on this device: it writes ZEROS into every one of
     * these and nothing else. Not a single value we had been sending
     * appears -- no pipeline mode, no processing channels, no demosaic
     * coefficients, no gains, no curves. One word is non-zero in the whole
     * of it, 0x91F, and it takes a two.
     *
     * And stock gets colour. So the demosaic runs on the hardware's own
     * defaults, and what we were doing was overwriting them. Leaving those
     * registers alone is not the same as clearing them -- whatever an
     * earlier run left behind stays -- so this clears them the way stock
     * does. */
    if (zero_init) {
        /* The clearing pass, and it is longer than what I had: the colour
         * matrix and the work buffer are cleared with the rest. Stock runs
         * it, applies, writes the DMA block and the three registers beside
         * it, then runs the whole pass again and applies again. */
        static const struct { uint16_t m; uint16_t n; uint8_t noninc; } z[] = {
            { 0x202, 3, 0 }, { 0x200, 2, 0 }, { 0x205, 4, 0 },
            { 0x700, 16, 0 }, { 0x750, 16, 0 },
            { 0xD00, 10, 0 }, { 0xD0A, 1, 0 }, { 0xD0B, 480, 1 },
            { 0xD0C, 2, 0 }, { 0xD20, 6, 0 },
            { 0x900, 2, 0 }, { 0x902, 1, 0 }, { 0x903, 64, 1 },
            { 0x904, 2, 0 }, { 0x906, 1, 0 }, { 0x907, 36, 1 },
            { 0x908, 1, 0 }, { 0x920, 10, 0 }, { 0x909, 7, 0 },
            { 0x910, 9, 0 },
            { 0x91B, 1, 0 }, { 0x91C, 9, 1 }, { 0x91D, 1, 0 },
            { 0x91E, 9, 1 },
            { 0x506, 9, 0 }, { 0x600, 16, 0 },
            { 0x650, 1, 0 },
            { 0x651, 1, 0 }, { 0x652, 257, 1 },
            { 0x653, 1, 0 }, { 0x654, 257, 1 },
            { 0x655, 1, 0 }, { 0x656, 257, 1 },
            { 0x657, 1, 0 }, { 0x658, 257, 1 },
            { 0x300, 4, 0 }, { 0x304, 4, 0 }, { 0x053, 2, 0 },
        };
        for (int pass = 0; pass < 2; pass++) {
            for (unsigned k = 0; k < sizeof z / sizeof z[0]; k++) {
                g[n++] = z[k].noninc ? OP_NONINCR(z[k].m, z[k].n)
                                     : OP_INCR(z[k].m, z[k].n);
                for (unsigned i = 0; i < z[k].n; i++) g[n++] = 0;
            }
            /* The one block whose tail is not zeros: stock sets the eighth
             * slot of the 0x91a array port, and the header word with it. */
            g[n++] = OP_INCR(0x919, 1); g[n++] = 0x00000000;
            g[n++] = OP_NONINCR(0x91A, 9);
            for (int i = 0; i < 8; i++) g[n++] = 0;
            g[n++] = 0x00000200;
            g[n++] = OP_INCR(0x91F, 1); g[n++] = 0x00000002;

            /* Apply, then the transfer configuration: the memory-path set
             * on the first pass, the streaming set on the second. */
            g[n++] = OP_NONINCR(0x00C, 1); g[n++] = 0x0F;
            g[n++] = OP_INCR(0x018, 5);
            if (pass && stream_xfer) {
                /* The second pass carries a different block altogether, not
                 * the first one with its last word flipped: both captures
                 * (2592 and 720) send exactly this, and the live camera's
                 * registers 0x018..0x01C read back exactly this. The
                 * reconstruction had collapsed the two sets into one.
                 *
                 * Behind --stream-xfer: with it the pipeline really streams
                 * (Y carries the picture) and the hardware drives syncpoint
                 * 38 itself, which is what took the channel down twice.
                 * Without it the block runs as before, in bypass. */
                g[n++] = 0x0a00500a; g[n++] = 0x00008089;
                g[n++] = 0x013645cb; g[n++] = 0x000001e7;
                g[n++] = 0x00000001;
            } else {
                g[n++] = 0x00000000; g[n++] = 0x00000400;
                g[n++] = 0x00000000; g[n++] = 0x00000200;
                g[n++] = pass ? 0x00000001 : 0x00000002;
            }
            if (!pass) {
                g[n++] = OP_INCR(0x01E, 1); g[n++] = 0x00000000;
                g[n++] = OP_INCR(0x01F, 1); g[n++] = 0x00000001;
                g[n++] = OP_INCR(0x05F, 1); g[n++] = 0x00000010;
            }
        }
    }

    /* The transfer configuration now rides inside the clearing pass above,
     * in the order and with the values the trace shows. */

    /* The runtime configuration, ported from the driver's streaming init
     * for ISP-B. The calibration blob alone left the block completing
     * frames and writing black, and the reason is here: the input stage is
     * never switched on without 0x200, and the processing channels have
     * nowhere to work without the addresses in 0x700 and 0x750. None of
     * this appears in the per-frame trace because stock had already sent it
     * at init. */
    /* Everything from here to the calibration blob is the streaming init
     * from the 24.1 driver -- which is the owner's own reconstruction, not
     * anything from the vendor, so it is a hypothesis rather than a
     * reference. None of it appears in the stock traces either, those
     * carrying only the per-frame gather. Bit 3 drops the whole of it, so
     * the ISP can be given nothing but what the traces actually show. */
    if (!(skip & 0x08)) {

    if (!(skip & 0x01)) {
        /* The three words after the enable are 75, 147 and 34 -- the BT.601
         * luma weights, which is what folding three channels into one looks
         * like. */
        /* 75, 147 and 34 -- the BT.601 luma weights -- and the driver puts
         * them in the high half of each word. If the hardware reads them
         * from the low half, the luma comes out zero, and a zero luma is
         * exactly what the planar output shows: an empty Y plane with the
         * chroma carrying the picture. In the packed forms that same zero
         * luma leaves red alive and clamps green and blue to nothing, which
         * is what we have been calling "only red". Worth trying both. */
        g[n++] = OP_INCR(0x400, 12);
        g[n++] = rt_luma;
        g[n++] = luma_lo ? 0x0000004b : 0x004b0000;
        g[n++] = luma_lo ? 0x00000093 : 0x00930000;
        g[n++] = luma_lo ? 0x00000022 : 0x00220000;
        g[n++] = 0x2ff01000; g[n++] = 0x2ff01000;
        g[n++] = 0x2ff01000; g[n++] = 0x2ff01000;
        g[n++] = 0x00030000; g[n++] = 0x00000000;
        g[n++] = 0x00020000; g[n++] = 0x00000000;
    }

    /* The stock has 0x85001000 in the first word here from the opening
     * to the last frame -- a constant, not an address. Ours put the
     * statistics buffer's address there, which 1280 wide tolerated. */
    g[n++] = OP_INCR(0x800, 3);
    g[n++] = bare_warmup ? 0x85001000 : stats_iova; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_INCR(0x820, 3);
    g[n++] = bare_warmup ? 0x85001000 : stats_iova; g[n++] = 0; g[n++] = 0;

    /* The 720 capture's real histogram windows: word 0 is 0x1d and the
     * four window words describe the 1280x720 frame -- the zeros-and-
     * 0x70000 tail was the 8x8 warm-up's values, never the running
     * configuration. */
    g[n++] = OP_INCR(0x930, 18);
    g[n++] = 0x0000001d; g[n++] = 0x88888888;
    g[n++] = 0x78787800; g[n++] = 0x00000078;
    g[n++] = 0x88888888; g[n++] = 0x78787800;
    g[n++] = 0x00000078; g[n++] = 0x88888888;
    g[n++] = 0x78787800; g[n++] = 0x00000078;
    g[n++] = 0x88888888; g[n++] = 0x78787800;
    g[n++] = 0x00000078; g[n++] = 0x3fc00000;
    if (bare_warmup) {
        /* The warm-up form of the windows, which is what the stock has in
         * place while its 8x8 frames run; the working windows follow in
         * the colour job. */
        g[n++] = 0x00000000; g[n++] = 0x00070000;
        g[n++] = 0x00000000; g[n++] = 0x00070000;
    } else {
        g[n++] = 0x00220000; g[n++] = 0x0004003f;
        g[n++] = 0x00120000; g[n++] = 0x0003003f;
    }

    g[n++] = OP_INCR(0xC00, 3);
    g[n++] = 0x00000101; g[n++] = 0x00000000; g[n++] = 0x00100000;

    /* The input stage: dimensions, then the enable, then stride and format. */
    if (!(skip & 0x04)) {
        /* 0x203 and 0x204 carry 0x00780078 in the driver for this sensor and
         * 0x02000200 for the other -- a pair of sixteen-bit fields that have
         * nothing to do with either sensor's dimensions, so what they mean
         * is a guess. Both they and the pipeline mode in 0x200 are knobs:
         * the April notes say a non-zero mode is what takes the block out of
         * minimal processing, and minimal processing is exactly what we
         * measure -- the mosaic arrives at the output intact. */
        g[n++] = OP_INCR(0x202, 3);
        g[n++] = 0x00000001; g[n++] = in_dims; g[n++] = in_dims;
        g[n++] = OP_INCR(0x200, 2);
        g[n++] = in_mode; g[n++] = 0x00000000;
        /* The last word of this block is the one place in the driver where
         * the two sensors differ in a way that looks like Bayer order:
         * 0x3333 for the rear camera, zero for this one -- and the two
         * sensors are RGGB and BGGR. A zero there could as easily mean "no
         * mosaic", which would explain a demosaic stage that never runs. */
        g[n++] = OP_INCR(0x205, 4);
        g[n++] = 0x00000000; g[n++] = 0x000600c8;
        g[n++] = 0x000f000f; g[n++] = in_phase;
    }

    /* Straight out of the stock trace, where this block turns up inside two
     * of the eight calibration gathers. Every value differs from the
     * reconstruction we had been sending, and the shape of the difference
     * matters: stock puts constants where we were putting addresses of our
     * own scratch buffer -- 0x1e700000 in the eighth word, 0x30001000 in
     * the last four. */
    /* The live stock configuration puts a constant in the eighth word and
     * four more in the tail. The reconstruction we were following read
     * those as pointers into a scratch buffer and substituted ours -- they
     * are nothing of the kind, and every one of them was garbage to the
     * hardware. */
    g[n++] = OP_INCR(0x700, 16);
    g[n++] = 0x00000001; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = geo->x700_w5;
    g[n++] = 0x00000000; g[n++] = 0x10000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00001000; g[n++] = geo->x700_w11;
    g[n++] = 0x30001000; g[n++] = 0x30001000;
    g[n++] = 0x30001000; g[n++] = 0x30001000;

    g[n++] = OP_INCR(0x750, 16);
    g[n++] = 0x00000003; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x30001000; g[n++] = 0x30001000;
    g[n++] = 0x30001000; g[n++] = 0x30001000;

    g[n++] = OP_INCR(0xD20, 6);
    g[n++] = geo->d20_mode; g[n++] = 0x00000000;
    g[n++] = geo->d20_step; g[n++] = geo->d20_step;
    g[n++] = geo->d20_step; g[n++] = geo->d20_step;

    g[n++] = OP_INCR(0x900, 2);
    g[n++] = 0x00000001; g[n++] = 0x00000001;
    /* The statistics control word. It has a name in the headers and nobody
     * writes it -- not stock, not us, we only clear it -- and the stage it
     * belongs to is silent here alongside the demosaic. Worth one value. */
    if (stats_ctrl) { g[n++] = OP_INCR(0x902, 1); g[n++] = stats_ctrl; }
    /* The shift fields, and ours were a step too high. Injected into the
     * stock camera in place of its own they made its picture brighter --
     * 137.9 against 124.6 by measurement, and visibly so -- which is what
     * one extra step of scaling does to every demosaic coefficient. */
    g[n++] = OP_INCR(0x904, 2);
    g[n++] = bare_warmup ? 0x00005555 : 0x00004444; g[n++] = 0x00000001;
    g[n++] = OP_INCR(0x908, 1); g[n++] = bare_warmup ? 0x00005555 : 0x00004334;

    /* Three of these words are addresses, not settings: a base and two
     * windows a fixed distance into it. We had been sending the stock
     * process's base, 0x10000000, which in our address space is nothing --
     * so the stage that works through them had nowhere to work, and that
     * is the most likely reason the luma surface came back as zeros while
     * the third one still received something. Ours goes in instead, with
     * the same two offsets. */
    g[n++] = OP_INCR(0x920, 10);
    g[n++] = 0x00000002;
    g[n++] = own_scratch ? work_iova + 0x1660 : 0x10001660;
    g[n++] = 0x00000000;
    g[n++] = own_scratch ? work_iova + 0xf4a0 : 0x1000f4a0;
    g[n++] = 0x0000fa80;
    g[n++] = own_scratch ? work_iova : 0x10000000;
    g[n++] = 0x00001c50; g[n++] = 0x30001000;
    g[n++] = 0x30001000; g[n++] = 0x30001000;

    g[n++] = OP_INCR(0x909, 7);
    g[n++] = 0x00000001; g[n++] = 0xfc000f00;
    g[n++] = 0xf680f320; g[n++] = 0x0d80fde0;
    g[n++] = geo->s909_w4; g[n++] = 0x1400002a;
    g[n++] = 0x3c00002b;

    g[n++] = OP_INCR(0x910, 9);
    g[n++] = 0x00000003; g[n++] = 0x00000028;
    g[n++] = 0x01480029; g[n++] = geo->s910_w3;
    g[n++] = 0x00990030; g[n++] = 0x00000800;
    g[n++] = 0x007b0666; g[n++] = geo->s910_w7;
    g[n++] = geo->s910_w8;

    g[n++] = OP_INCR(0x91B, 1); g[n++] = 0x00000000;
    g[n++] = OP_NONINCR(0x91C, 9);
    g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = 0x00000001; g[n++] = geo->s91c_w5;
    g[n++] = 0x00000000; g[n++] = 0x00000026; g[n++] = 0x00000361;
    g[n++] = OP_INCR(0x91D, 1); g[n++] = 0x00000000;
    g[n++] = OP_NONINCR(0x91E, 9);
    g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = 0; g[n++] = 0x00000780;
    g[n++] = 0; g[n++] = 0x00000780; g[n++] = 0x00000200;
    g[n++] = OP_INCR(0x91F, 1); g[n++] = 0x00000032;

    /* Demosaic. Only red comes out of the pipeline -- green and blue are
     * exactly zero in every packed format -- and the reprocess notes say
     * stock sends nine zeros here and lets the hardware use its own
     * defaults, while these coefficients are the tool's own. Selectable
     * because that is the difference worth testing. */
    g[n++] = OP_INCR(0x506, 9);
    if (demosaic_zero) {
        for (int i = 0; i < 9; i++) g[n++] = 0;
    } else {
        g[n++] = 0x3f3fcff3; g[n++] = 0x00000000;
        g[n++] = 0x04c1304c; g[n++] = 0x08220882;
        g[n++] = 0x00000000; g[n++] = 0x03d0f43d;
        g[n++] = 0x08621886; g[n++] = 0x01204812;
        g[n++] = 0x06e1b86e;
    }

    if (!(skip & 0x02)) {
    /* The colour conversion, and we had been sending it empty.
     *
     * Every coefficient here was zero, which is why the luma plane came
     * back as a full frame of black while one chroma plane carried the
     * whole picture: with a zero matrix there is nothing to build a
     * luminance out of. These are the stock camera's own, read out of its
     * running configuration. */
    g[n++] = OP_INCR(0x600, 16);
    g[n++] = 0x00000005; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    /* A row at a time, because the whole matrix at once takes the channel
     * down. The words are signed pairs in eleven fractional bits, 0x0800
     * being one, and they come in three rows: the first builds the
     * luminance, the other two the colour differences. Sending only the
     * first tells us whether it is the arithmetic the block objects to or
     * the amount of it. */
    /* Zero here, always. The capture has the matrix empty in every opening
     * round without exception; the real one arrives later, on its own. */
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    /* Three per-channel words. The reprocess tool carries the same
     * 0x3fff0000 here and comes out monochrome too, which makes this the
     * one value both paths share and both paths fail on. The output is also
     * attenuated some sixty-fold against the raw capture, which is what a
     * gain in the wrong fixed-point format would do. */
    g[n++] = gpp_gain; g[n++] = gpp_gain;
    g[n++] = gpp_gain; g[n++] = 0x10001000;
    }

    /* One, which is what the stock camera has here. We were sending three,
     * and writing coefficients into a stage set up differently from the way
     * their owner set it up is a fair account of a block that stalls
     * part-way through the stream rather than faulting on anything. */
    g[n++] = OP_INCR(0x650, 1); g[n++] = bare_warmup ? 0x00000003 : 0x00000001;
    g[n++] = OP_INCR(0x651, 1); g[n++] = 0x00000000;

    }   /* end of the reconstructed streaming init */

    /* The colour matrix. Neither we nor that init write it, so whatever it
     * holds after reset is what the pipeline uses. This does not try to
     * guess the right coefficients; it establishes whether these registers
     * are the gate at all. */
    if (ccm_word) {
        g[n++] = OP_INCR(0x300, 4);
        for (int i = 0; i < 4; i++) g[n++] = ccm_word;
        g[n++] = OP_INCR(0x304, 4);
        for (int i = 0; i < 4; i++) g[n++] = ccm_word;
    }

    memcpy(&g[n], isp_b_cal_data, words * 4);
    n += words;
    /* The blob ends with 0x053 and 0x054 -- the work buffer's enable and its
     * address. The driver patches a zero into the address and calls that
     * what stock does, but the stock streaming trace carries a real pointer
     * there. A pipeline handed a null scratch buffer has every reason to
     * fall back to the least it can do, which is what we see. */
    g[n - 2] = 0x00000001;                /* 0x053 */
    g[n - 1] = bare_warmup ? 0 : work_iova;   /* 0x054: the stock has 0 until the real pass */

    /* And then the real thing, last, so it stands over everything above:
     * the configuration read out of the stock camera while it was running.
     * The demosaic coefficients live here and nowhere else we could reach. */
    if (stock_cfg) n = isp_stock_emit(g, n, work_iova, stock_cfg);

    /* Shading off until the real pass, as the stock has it during the
     * warm-ups; whatever the blob above loaded into 0xd00 is overridden
     * here, the table itself is harmless while the stage is off. */
    if (bare_warmup) { g[n++] = OP_INCR(0xd00, 1); g[n++] = 0x00000000; }

    /* Unless the enable belongs later: stock writes it exactly once for a
     * whole session, and not here -- it goes inside the first warm-up
     * frame, after everything else has been configured. */
    if (!enable_late) { g[n++] = OP_INCR(0x015, 1); g[n++] = enable; }

    /* Commit. Everything above this is written into shadow state and takes
     * effect only when the block is told to apply it -- the April notes
     * list this trigger among the few things the stock settings blob
     * actually carries. We had dropped it on a driver comment that is about
     * the per-frame calibration, not about init, and that would explain why
     * nothing we wrote here ever changed more than the level. */
    /* Optional, and the question is sharper than it looks. Stock's trace
     * has no 0x0F anywhere -- only the streaming trigger. If it writes its
     * clearing pass into shadow state and never applies it, then the
     * hardware defaults are what stay in force, demosaic included. Applying
     * ours would then be the very thing that destroys them. */
    if (apply) { g[n++] = OP_NONINCR(0x00C, 1); g[n++] = 0x0F; }
    g[n++] = OP_IMM(0, sp);

    nvmap_rw(cmd_h, 0, g, n * 4, 1);
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
    printf("ISP calibration: %u words, enable 0x%08x, rc=%d (%s)\n",
           n, enable, rc, rc == 0 ? "ok" : strerror(errno));
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    (void)work_h;
    return rc;
}

/* The other block's runtime configuration, as the stock camera sends it.
 *
 * Stock sets ISP-A up in full -- submits of 3654, 1817, 1817 and 1238 words
 * -- before ISP-B, which is the one the front sensor feeds, ever gets more
 * than a clearing pass. If the two share anything that has to be configured
 * once, this is where it happens, and we have never done it.
 *
 * Values decoded from a live session on this device. The sensor-specific
 * words are the rear camera's, because that is whose block this is. */
int isp_init_a(int fd, uint32_t sp)
{
    uint32_t cmd_h = nvmap_create(32768);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    uint32_t *g = malloc(8192 * 4);
    unsigned n = 0;
    g[n++] = OP_SETCLASS(ISP_CLASS_A);

    /* The clearing pass first, twice, exactly as this block gets it -- our
     * earlier version jumped straight to the runtime values and sent a
     * couple of hundred words where stock sends thousands. */
    {
        static const struct { uint16_t m; uint16_t n; uint8_t noninc; } z[] = {
            { 0x202, 3, 0 }, { 0x200, 2, 0 }, { 0x205, 4, 0 },
            { 0x700, 16, 0 }, { 0x750, 16, 0 },
            { 0xD00, 10, 0 }, { 0xD0A, 1, 0 }, { 0xD0B, 480, 1 },
            { 0xD0C, 2, 0 }, { 0xD20, 6, 0 },
            { 0x900, 2, 0 }, { 0x902, 1, 0 }, { 0x903, 64, 1 },
            { 0x904, 2, 0 }, { 0x906, 1, 0 }, { 0x907, 36, 1 },
            { 0x908, 1, 0 }, { 0x920, 10, 0 }, { 0x909, 7, 0 },
            { 0x910, 9, 0 }, { 0x919, 1, 0 },
            { 0x91B, 1, 0 }, { 0x91C, 9, 1 }, { 0x91D, 1, 0 },
            { 0x91E, 9, 1 },
            { 0x506, 9, 0 }, { 0x600, 16, 0 },
            { 0x650, 1, 0 },
            { 0x651, 1, 0 }, { 0x652, 257, 1 },
            { 0x653, 1, 0 }, { 0x654, 257, 1 },
            { 0x655, 1, 0 }, { 0x656, 257, 1 },
            { 0x657, 1, 0 }, { 0x658, 257, 1 },
            { 0x300, 4, 0 }, { 0x304, 4, 0 }, { 0x053, 2, 0 },
        };
        for (int pass = 0; pass < 2; pass++) {
            for (unsigned k = 0; k < sizeof z / sizeof z[0]; k++) {
                g[n++] = z[k].noninc ? OP_NONINCR(z[k].m, z[k].n)
                                     : OP_INCR(z[k].m, z[k].n);
                for (unsigned i = 0; i < z[k].n; i++) g[n++] = 0;
            }
            g[n++] = OP_NONINCR(0x91A, 9);
            for (int i = 0; i < 8; i++) g[n++] = 0;
            g[n++] = 0x00000200;
            g[n++] = OP_INCR(0x91F, 1); g[n++] = 0x00000002;
            g[n++] = OP_NONINCR(0x00C, 1); g[n++] = 0x0F;
            g[n++] = OP_INCR(0x018, 5);
            if (pass && stream_xfer) {
                /* The second pass carries a different block altogether, not
                 * the first one with its last word flipped: both captures
                 * (2592 and 720) send exactly this, and the live camera's
                 * registers 0x018..0x01C read back exactly this. The
                 * reconstruction had collapsed the two sets into one.
                 *
                 * Behind --stream-xfer: with it the pipeline really streams
                 * (Y carries the picture) and the hardware drives syncpoint
                 * 38 itself, which is what took the channel down twice.
                 * Without it the block runs as before, in bypass. */
                g[n++] = 0x0a00500a; g[n++] = 0x00008089;
                g[n++] = 0x013645cb; g[n++] = 0x000001e7;
                g[n++] = 0x00000001;
            } else {
                g[n++] = 0x00000000; g[n++] = 0x00000400;
                g[n++] = 0x00000000; g[n++] = 0x00000200;
                g[n++] = pass ? 0x00000001 : 0x00000002;
            }
            if (!pass) {
                g[n++] = OP_INCR(0x01E, 1); g[n++] = 0x00000000;
                g[n++] = OP_INCR(0x01F, 1); g[n++] = 0x00000001;
                g[n++] = OP_INCR(0x05F, 1); g[n++] = 0x00000010;
            }
        }
    }

    g[n++] = OP_INCR(0x400, 12);
    g[n++] = 0x00000001; g[n++] = 0x004b0000;
    g[n++] = 0x00930000; g[n++] = 0x00220000;
    g[n++] = 0x2ff01000; g[n++] = 0x2ff01000;
    g[n++] = 0x2ff01000; g[n++] = 0x2ff01000;
    g[n++] = 0x00030000; g[n++] = 0x00000000;
    g[n++] = 0x00020000; g[n++] = 0x00000000;

    g[n++] = OP_INCR(0x800, 3);
    g[n++] = 0x85001000; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_INCR(0x820, 3);
    g[n++] = 0x85001000; g[n++] = 0; g[n++] = 0;

    g[n++] = OP_INCR(0x930, 18);
    g[n++] = 0x0000001c; g[n++] = 0x88888888;
    g[n++] = 0x78787800; g[n++] = 0x00000078;
    g[n++] = 0x88888888; g[n++] = 0x78787800;
    g[n++] = 0x00000078; g[n++] = 0x88888888;
    g[n++] = 0x78787800; g[n++] = 0x00000078;
    g[n++] = 0x88888888; g[n++] = 0x78787800;
    g[n++] = 0x00000078; g[n++] = 0x3fc00000;
    g[n++] = 0x00000000; g[n++] = 0x00070000;
    g[n++] = 0x00000000; g[n++] = 0x00070000;

    g[n++] = OP_INCR(0xC00, 3);
    g[n++] = 0x00000101; g[n++] = 0; g[n++] = 0x00100000;

    g[n++] = OP_INCR(0x202, 3);
    g[n++] = 0x00000001; g[n++] = 0x02000200; g[n++] = 0x02000200;
    g[n++] = OP_INCR(0x200, 2);
    g[n++] = 0x00000001; g[n++] = 0x00000000;
    g[n++] = OP_INCR(0x205, 4);
    g[n++] = 0x00000000; g[n++] = 0x000600c8;
    g[n++] = 0x000f000f; g[n++] = 0x00003333;

    g[n++] = OP_INCR(0x700, 16);
    g[n++] = 0x00000001; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = 0; g[n++] = 0x00001dc0;
    g[n++] = 0; g[n++] = 0x10000000;
    g[n++] = 0; g[n++] = 0;
    g[n++] = 0x00001000; g[n++] = 0x00001c50;
    g[n++] = 0x30001000; g[n++] = 0x30001000;
    g[n++] = 0x30001000; g[n++] = 0x30001000;

    g[n++] = OP_INCR(0x750, 16);
    g[n++] = 0x00000003;
    for (int i = 0; i < 11; i++) g[n++] = 0;
    g[n++] = 0x30001000; g[n++] = 0x30001000;
    g[n++] = 0x30001000; g[n++] = 0x30001000;

    g[n++] = OP_INCR(0xD20, 6);
    g[n++] = 0x00003101; g[n++] = 0;
    g[n++] = 0x01ec0000; g[n++] = 0x01ec0000;
    g[n++] = 0x01ec0000; g[n++] = 0x01ec0000;

    g[n++] = OP_INCR(0x900, 2); g[n++] = 1; g[n++] = 1;
    g[n++] = OP_INCR(0x904, 2); g[n++] = 0x00005555; g[n++] = 1;
    g[n++] = OP_INCR(0x908, 1); g[n++] = 0x00005555;

    g[n++] = OP_INCR(0x920, 10);
    g[n++] = 0x00000002; g[n++] = 0x10001660;
    g[n++] = 0x00000000; g[n++] = 0x1000f4a0;
    g[n++] = 0x0000fa80; g[n++] = 0x10000000;
    g[n++] = 0x00001c50; g[n++] = 0x30001000;
    g[n++] = 0x30001000; g[n++] = 0x30001000;

    g[n++] = OP_INCR(0x909, 7);
    g[n++] = 0x00000001; g[n++] = 0xfc000f00;
    g[n++] = 0xf680f320; g[n++] = 0x0d80fde0;
    g[n++] = 0x00000000; g[n++] = 0x1400002a;
    g[n++] = 0x3c00002b;

    g[n++] = OP_INCR(0x910, 9);
    g[n++] = 0x00000003; g[n++] = 0x00000028;
    g[n++] = 0x01480029; g[n++] = 0x00177e0b;
    g[n++] = 0x00990030; g[n++] = 0x00000800;
    g[n++] = 0x007b0666; g[n++] = 0x00000039;
    g[n++] = 0x00000000;

    g[n++] = OP_NONINCR(0x91C, 9);
    g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = 0x00000001; g[n++] = 0x00000026;
    g[n++] = 0; g[n++] = 0x00000026; g[n++] = 0x00000361;
    g[n++] = OP_NONINCR(0x91E, 9);
    g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = 0; g[n++] = 0x00000780;
    g[n++] = 0; g[n++] = 0x00000780; g[n++] = 0x00000200;
    g[n++] = OP_INCR(0x91F, 1); g[n++] = 0x00000032;

    g[n++] = OP_INCR(0x506, 9);
    g[n++] = 0x3f3fcff3; g[n++] = 0x00000000;
    g[n++] = 0x04c1304c; g[n++] = 0x08220882;
    g[n++] = 0x00000000; g[n++] = 0x03d0f43d;
    g[n++] = 0x08621886; g[n++] = 0x01204812;
    g[n++] = 0x06e1b86e;

    g[n++] = OP_INCR(0x600, 16);
    g[n++] = 0x00000005;
    for (int i = 0; i < 11; i++) g[n++] = 0;
    g[n++] = 0x3fff0000; g[n++] = 0x3fff0000;
    g[n++] = 0x3fff0000; g[n++] = 0x10001000;

    g[n++] = OP_INCR(0x650, 1); g[n++] = 0x00000003;
    g[n++] = OP_INCR(0x053, 2); g[n++] = 1; g[n++] = 0;
    g[n++] = OP_IMM(0, sp);

    nvmap_rw(cmd_h, 0, g, n * 4, 1);

    struct nvhost_cmdbuf cb = { cmd_h, 0, n };
    struct nvhost_syncpt_incr si = { sp, 1 };
    uint32_t cls = ISP_CLASS_A;
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
    int rc = ioctl(fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
    printf("ISP-A configured: %u words, rc=%d (%s)\n", n, rc,
           rc == 0 ? "ok" : strerror(errno));
    free(g);
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    return rc;
}

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
/* The demosaic, sent when the stock camera sends it.
 *
 * Not during the opening configuration -- which is where we had been
 * putting it, and it never took. The captured opening sequence is plain
 * about the order: the block is enabled and the whole configuration
 * committed, the routing from the receiver is set up, one frame goes
 * through, and only then does the stack push the coefficients, in a job of
 * their own carrying nothing else. The values here are that job, word for
 * word.
 */
int isp_demosaic(int isp_fd, uint32_t sp, uint32_t out_h,
                        uint32_t stats_h, uint32_t u_off, uint32_t v_off,
                        uint32_t work_iova)
{
    uint32_t cmd_h = nvmap_create(4096);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    uint32_t g[160];
    unsigned n = 0;
    int y_word, u_word, v_word, stats_word;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);

    /* Carrying the surfaces as well, though this job has nothing to say
     * about them: the mapping belongs to whichever job carried the
     * relocation, and a job submitted between two frames without them lets
     * it lapse. The memory controller caught the ISP writing into an
     * address inside our own output that was no longer mapped. */
    g[n++] = OP_INCR(0xE04, 3);
    y_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_INCR(0xE07, 3);
    u_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_INCR(0xE0A, 3);
    v_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_INCR(0x100, 4);
    stats_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;

    g[n++] = OP_INCR(0x902, 1);
    g[n++] = isp_dm_902[0];
    g[n++] = OP_NONINCR(0x903, 64);
    for (int i = 0; i < 64; i++) g[n++] = isp_dm_903[i];
    g[n++] = OP_INCR(0x904, 2);
    g[n++] = isp_dm_904[0]; g[n++] = isp_dm_904[1];
    g[n++] = OP_INCR(0x906, 1);
    g[n++] = isp_dm_906[0];
    g[n++] = OP_NONINCR(0x907, 36);
    for (int i = 0; i < 36; i++) g[n++] = isp_dm_907[i];
    g[n++] = OP_INCR(0x908, 1);
    g[n++] = isp_dm_908[0];
    /* The capture has a null work buffer here, but stock's scratch is set
     * up elsewhere in its session and ours is not: handing the block a null
     * pointer while it is live is the one place where copying the trace
     * literally puts it in a position it was never in. Ours goes here. */
    g[n++] = OP_INCR(0x053, 2); g[n++] = 1; g[n++] = work_iova;
    g[n++] = OP_IMM(0, sp);

    nvmap_rw(cmd_h, 0, g, n * 4, 1);

    struct nvhost_reloc rel[4] = {
        { cmd_h, (uint32_t)y_word * 4, out_h, 0 },
        { cmd_h, (uint32_t)u_word * 4, out_h, u_off },
        { cmd_h, (uint32_t)v_word * 4, out_h, v_off },
        { cmd_h, (uint32_t)stats_word * 4, stats_h, 0 },
    };
    struct nvhost_reloc_shift sh[4] = { { 0 }, { 0 }, { 0 }, { 0 } };
    struct nvhost_cmdbuf cb = { cmd_h, 0, n };
    struct nvhost_syncpt_incr si = { sp, 1 };
    uint32_t cls = ISP_CLASS_B;
    struct nvhost_fence fence = { 0, 0 };
    struct nvhost32_submit_args sa;
    memset(&sa, 0, sizeof sa);
    sa.num_syncpt_incrs = 1;
    sa.num_cmdbufs = 1;
    sa.num_relocs = 4;
    sa.timeout = 3000;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.relocs = (uint32_t)(uintptr_t)rel;
    sa.reloc_shifts = (uint32_t)(uintptr_t)sh;
    sa.class_ids = (uint32_t)(uintptr_t)&cls;
    sa.fences = (uint32_t)(uintptr_t)&fence;
    errno = 0;
    int rc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
    printf("demosaic coefficients: %u words, rc=%d (%s)\n", n, rc,
           rc == 0 ? "ok" : strerror(errno));
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    return rc;
}

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
int isp_real_pass(int isp_fd, uint32_t sp, uint32_t work_iova)
{
    uint32_t cmd_h = nvmap_create(4096 * 2);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    static const struct { uint16_t m; uint16_t n; uint8_t noninc;
                          const uint32_t *d; } blk[] = {
        { 0x400, 12, 0, isp_real_400 }, { 0x800, 3, 0, isp_real_800 },
        { 0x820, 3, 0, isp_real_820 }, { 0xc00, 3, 0, isp_real_c00 },
        { 0x700, 16, 0, isp_real_700 }, { 0x750, 16, 0, isp_real_750 },
        { 0xd00, 10, 0, isp_real_d00 }, { 0xd0a, 1, 0, isp_real_d0a },
        { 0xd0b, 480, 1, isp_real_d0b }, { 0x600, 16, 0, isp_real_600 },
    };

    uint32_t *g = malloc(4096 * 2);
    unsigned n = 0;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    for (unsigned b = 0; b < sizeof blk / sizeof blk[0]; b++) {
        g[n++] = blk[b].noninc ? OP_NONINCR(blk[b].m, blk[b].n)
                               : OP_INCR(blk[b].m, blk[b].n);
        unsigned first = n;
        for (unsigned i = 0; i < blk[b].n; i++) g[n++] = blk[b].d[i];

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

        /* Some of what the capture holds is not configuration but the
         * stock process's own addresses, and those mean nothing here --
         * loading them verbatim is what pointed the block at memory it
         * could not reach. Every one of them gets our scratch instead.
         *
         * The tile engine takes its working memory through these, which is
         * why the luma came back as zeros while the third surface still
         * received something: that path needs no intermediate storage. */
        if (!own_scratch) continue;
        if (blk[b].m == 0x400)
            for (unsigned i = 4; i <= 7; i++)
                g[first + i] = work_iova + 0x100000 + (i - 4) * 0x40000;
        else if (blk[b].m == 0x800)
            g[first] = work_iova + 0x80000;
        else if (blk[b].m == 0x820)
            g[first] = work_iova + 0xC0000;
    }
    g[n++] = OP_INCR(0x053, 2); g[n++] = 1; g[n++] = work_iova;
    g[n++] = OP_IMM(0, sp);

    nvmap_rw(cmd_h, 0, g, n * 4, 1);
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
    printf("stock's working configuration: %u words, rc=%d (%s)\n", n, rc,
           rc == 0 ? "ok" : strerror(errno));
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
                      uint32_t stats_h, int write_enable, uint32_t enable,
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
    if (write_enable) { g[n++] = OP_INCR(0x015, 1); g[n++] = enable; }

    g[n++] = OP_INCR(0x100, 4);
    stats_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;

    g[n++] = OP_NONINCR(0x000, 1); g[n++] = 0x00000424;
    g[n++] = OP_NONINCR(0x000, 1); g[n++] = 0x00000525;
    g[n++] = OP_NONINCR(0x000, 1); g[n++] = 0x00000627;
    g[n++] = OP_NONINCR(0x00C, 1); g[n++] = 0x00000005;
    g[n++] = OP_IMM(0, sp);

    nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);

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

/* The colour conversion, sent where the stock camera sends it.
 *
 * In a whole session it appears with a non-zero matrix exactly once, and
 * not in the opening configuration: it comes mid-stream, after the warm-up
 * and after the coefficients, at the end of its pass just before the work
 * buffer. We had been putting it at the front of the opening round, which
 * is the one place the capture never has it.
 */
int isp_colour(int isp_fd, uint32_t sp, uint32_t work_iova,
                      unsigned W, unsigned H)
{
    uint32_t cmd_h = nvmap_create(4096);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    /* 48 was enough without the geometry group and eleven words short with
     * it: the tail of the matrix went over the end of the array and into
     * the return address, and the crash landed at 0x40000024 with the
     * matrix words in the saved registers. Every run of --geo-blocks died
     * this way, which is what "the geometry blocks take the channel down"
     * actually was. */
    uint32_t g[128];
    int n = 0;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);

    /* Geometry, derived rather than copied.
     *
     * The capture has 0x07780a00 here for a 2592 by 1944 frame, and
     * 1944-32 is 1912 while 2592-32 is 2560 -- the word is the frame less
     * thirty-two in each direction, packed high and low. The neighbouring
     * block carries the width outright. Sending stock's numbers unchanged
     * described a frame nearly twice the size of the one arriving, so the
     * stage that walks the picture was reading rows that were not there,
     * and what it computed for the luma was nothing. */
    uint32_t geo_word = ((H - 32) << 16) | (W - 32);
    /* Measured, not derived. The stock camera has now been captured twice
     * -- at 2592 by 1944, and at 1280 by 720 in its own video mode -- so
     * these are its numbers for the frame in hand rather than a formula
     * fitted through a single point. Earlier attempts sent the large
     * frame's numbers at the small size and took the channel down, which
     * looked like the blocks being unacceptable and was really them
     * describing a picture nearly twice the size of the one arriving.
     *
     * The geometry word is the frame less thirty-two each way, and both
     * captures agree on that. The others do not follow from any rule two
     * points can settle, so they are taken as they came. */
    const struct geom_cfg *geo = geom_for(W, H);
    if (geo_blocks) {
    g[n++] = OP_INCR(0x800, 3);
    g[n++] = 0x85001000; g[n++] = 0x00100010; g[n++] = geo_word;
    g[n++] = OP_INCR(0x820, 3);
    g[n++] = 0x85001000; g[n++] = 0x00100010; g[n++] = geo_word;
    g[n++] = OP_INCR(0xc00, 3);
    g[n++] = geo->c00_w0;
    g[n++] = 0x00000000;
    g[n++] = (geo->c00_w2_hi << 16) | W;

    /* And this one changes with the frame as well, which the diff between
     * the two captures turned up and I had not known: every word but the
     * first and the last is different at the smaller size. */
    g[n++] = OP_INCR(0xd00, 10);
    g[n++] = 0x00000001;
    g[n++] = geo->d00[0];
    g[n++] = geo->d00[1];
    g[n++] = geo->d00[2];
    g[n++] = geo->d00[3];
    g[n++] = geo->d00[4];
    g[n++] = geo->d00[5];
    g[n++] = geo->d00[6];
    g[n++] = geo->d00[7];
    g[n++] = 0x00000021;
    }
    /* Still behind the flag. With the large frame's numbers this group took
     * the channel down, and with the small frame's own numbers, measured
     * from a second capture at exactly our size, it takes the channel down
     * too. So it is not the values: this configuration will not accept
     * these blocks, and the run that works does not send them. */

    /* The output stage, with the four words we had been leaving as a stub.
     * Every fractional field in ours was zero, which builds no luminance at
     * all -- and that is why the luma plane came back black while the whole
     * picture collapsed into one chroma plane. The stock camera replaces
     * exactly this tail when it starts real frames; the leading weights,
     * 75/147/34, are the same in both and were never the problem.
     *
     * This has to be the last thing to touch 0x400 before a frame: the
     * opening round writes the stub, so anything that re-runs it afterwards
     * puts the stub back. */
    g[n++] = OP_INCR(0x400, 12);
    g[n++] = 0x00000001; g[n++] = 0x004b0000;
    g[n++] = 0x00930000; g[n++] = 0x00220000;
    g[n++] = 0x2ff01000; g[n++] = 0x2ff01000;
    g[n++] = 0x2ff01000; g[n++] = 0x2ff01000;
    /* And this tail is per-resolution too: the small capture has different
     * numbers here, and sending the large frame's was another way of
     * describing the wrong picture. */
    g[n++] = geo->p400_w8;
    g[n++] = geo->p400_w9;
    g[n++] = geo->p400_w10;
    g[n++] = geo->p400_w11;

    g[n++] = OP_INCR(0x600, 16);
    g[n++] = 0x00000005; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0xf9500800;
    g[n++] = 0x0000fec0; g[n++] = 0x096004c0;
    g[n++] = 0x000001d0; g[n++] = 0xfac0fd50;
    g[n++] = 0x00000800; g[n++] = 0x00000000;
    g[n++] = 0x3fff0000; g[n++] = 0x3fff0000;
    g[n++] = 0x3fff0000; g[n++] = 0x10001000;
    if (bare_warmup) {
        /* What the opening held back for the warm-ups: the working
         * statistics windows and the lookup-table enable at its running
         * value. */
        g[n++] = OP_INCR(0x930, 18);
        g[n++] = 0x0000001d; g[n++] = 0x88888888;
        g[n++] = 0x78787800; g[n++] = 0x00000078;
        g[n++] = 0x88888888; g[n++] = 0x78787800;
        g[n++] = 0x00000078; g[n++] = 0x88888888;
        g[n++] = 0x78787800; g[n++] = 0x00000078;
        g[n++] = 0x88888888; g[n++] = 0x78787800;
        g[n++] = 0x00000078; g[n++] = 0x3fc00000;
        g[n++] = 0x00220000; g[n++] = 0x0004003f;
        g[n++] = 0x00120000; g[n++] = 0x0003003f;
        g[n++] = OP_INCR(0x650, 1); g[n++] = 0x00000001;
    }
    g[n++] = OP_INCR(0x053, 2); g[n++] = 1; g[n++] = work_iova;
    g[n++] = OP_IMM(0, sp);

    nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);

    struct nvhost_cmdbuf cb = { cmd_h, 0, (uint32_t)n };
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
    printf("colour conversion: %d words, rc=%d (%s)\n", n, rc,
           rc == 0 ? "ok" : strerror(errno));
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    return rc;
}

void isp_stop(int isp_fd, uint32_t sp)
{
    uint32_t cmd_h = nvmap_create(4096);
    if (!cmd_h || nvmap_alloc(cmd_h)) return;

    /* No 0x00C=0: the stock camera never writes a zero trigger, and a write
     * the hardware has no meaning for is one more thing to differ on. */
    uint32_t g[8];
    int n = 0;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    g[n++] = OP_INCR(0x015, 1); g[n++] = 0x00000000;
    g[n++] = OP_IMM(0, sp);
    nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);

    struct nvhost_cmdbuf cb = { cmd_h, 0, (uint32_t)n };
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
    uint32_t was = syncpt_read(sp);
    int rc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);

    /* Wait for the block to actually take it. Submitting and walking away
     * left a write still in flight, and by the time it landed the buffer
     * had been unmapped: the memory controller reported a decode error
     * from the ISP's own write client on an address in what had been our
     * output. Nothing here may release memory the hardware can still
     * reach. */
    int waited = 0;
    while (syncpt_read(sp) == was && waited < 500) {
        usleep(2000);
        waited += 2;
    }
    /* And a moment beyond that, because the disable is what the job
     * carries, not proof that the last transfer has drained. */
    usleep(20000);

    printf("ISP stopped: rc=%d (%s), settled in %d ms%s\n", rc,
           rc == 0 ? "ok" : strerror(errno), waited,
           syncpt_read(sp) == was ? " -- NEVER ACKNOWLEDGED" : "");
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
}

/* The per-frame gather, word for word as the stock camera sends it: output
 * geometry, three plane triplets for planar YUV, the processing block, the
 * stats buffer, three conditional syncpoint increments and the streaming
 * trigger. There are no input registers -- in streaming mode the pixels
 * arrive from VI and there is nothing to describe. */
int isp_frame(int isp_fd, uint32_t out_h, uint32_t stats_h,
                     unsigned W, unsigned H, uint32_t fmt, uint32_t e03,
                     uint32_t trigger, uint32_t u_off, uint32_t v_off,
                     uint32_t sp_mem, uint32_t sp_stats, uint32_t sp_loadv,
                     uint32_t sp, uint32_t hold_sp, uint32_t hold_at,
                     uint32_t in_fmt, uint32_t work_iova, int per_frame_cal,
                     uint32_t proc_flags)
{
    /* Room for the calibration too, now that it rides with every frame. */
    uint32_t cmd_h = nvmap_create(16384);
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

    unsigned cal_words = sizeof isp_b_cal_data / sizeof isp_b_cal_data[0];
    uint32_t *g = malloc((cal_words + 128) * 4);
    int n = 0, y_word, u_word, v_word, stats_word;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);

    /* The calibration goes out with every frame, which is what stock does:
     * lens shading, the four curves and the work buffer, ahead of the frame
     * itself, in every one of its cycles. We had been sending it once at
     * init, so the block ran the whole session on whatever the clearing
     * pass had left. */
    if (per_frame_cal) {
        memcpy(&g[n], isp_b_cal_data, cal_words * 4);
        n += cal_words;
        g[n - 2] = 0x00000001;
        g[n - 1] = work_iova;
    }

    /* The work buffer, renewed for every frame. Stock's calibration gather
     * ends with this pair in all eight of its cycles -- enable and a real
     * pointer -- so the block is handed its scratch memory again before
     * each capture. We were setting it once at init and never again, and
     * the clearing pass before that had turned it off. A pipeline running
     * without scratch memory is a plausible reason for the stages that need
     * it -- demosaic, statistics -- to stay quiet while tone and gain,
     * which do not, keep working. */
    g[n++] = OP_INCR(0x053, 2);
    g[n++] = 0x00000001; g[n++] = work_iova;

    g[n++] = OP_INCR(0xE00, 1); g[n++] = ((W - 1) & 0x3FFF) << 16;
    g[n++] = OP_INCR(0xE01, 1); g[n++] = ((H - 1) & 0x3FFF) << 16;
    g[n++] = OP_INCR(0xE02, 1); g[n++] = fmt;
    g[n++] = OP_INCR(0xE03, 1); g[n++] = e03;

    /* With one component the block writes to the first of these, at the
     * start of the buffer, and gets it right. Ask for three and it writes
     * only to the last -- where we put a chroma-width stride -- and what
     * lands there is a full-width picture folded into half the row, which
     * is exactly the striping we see. That is what a reversed plane order
     * would look like, so it can be tried: give the last triplet the
     * luma stride and the start of the buffer, and see whether the picture
     * straightens out. */
    if (plane_rev) {
        g[n++] = OP_INCR(0xE04, 3);
        v_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_uv;
        g[n++] = OP_INCR(0xE07, 3);
        u_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_uv;
        g[n++] = OP_INCR(0xE0A, 3);
        y_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_y;
    } else {
        g[n++] = OP_INCR(0xE04, 3);
        y_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_y;
        g[n++] = OP_INCR(0xE07, 3);
        u_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_uv;
        g[n++] = OP_INCR(0xE0A, 3);
        v_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_uv;
    }

    /* The first word of this block is the only flags field the streaming
     * path has. The April notes call a non-zero value there fatal, but that
     * was the reprocess path, and this one behaves differently enough to be
     * worth its own look. */
    g[n++] = OP_INCR(0x500, 6);
    g[n++] = proc_flags; g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = (H << 16) | W;

    /* Tell the block what is arriving. The input-format register is
     * documented as belonging to the reprocess path, where it names the
     * Bayer order and depth -- but nothing in the streaming path names them
     * either, and a pipeline that does not know it is looking at a mosaic
     * has no reason to demosaic. Written without arming the memory input
     * trigger, so the source stays VI. */
    if (in_fmt) { g[n++] = OP_INCR(0xE33, 1); g[n++] = in_fmt; }

    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    g[n++] = OP_INCR(0x100, 4);
    stats_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;

    /* Only the conditions that have a counter behind them. Arming one
     * against an id this channel does not own leaves the job waiting on
     * something nothing will raise, and host1x eventually kills the
     * channel -- which is exactly what happened. */
    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    g[n++] = OP_NONINCR(0x000, 1); g[n++] = (4u << 8) | sp_mem;
    if (sp_stats) { g[n++] = OP_NONINCR(0x000, 1); g[n++] = (5u << 8) | sp_stats; }
    if (sp_loadv) { g[n++] = OP_NONINCR(0x000, 1); g[n++] = (6u << 8) | sp_loadv; }

    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    g[n++] = OP_NONINCR(0x00C, 1); g[n++] = trigger;

    /* Park the job on the frame's own completion and on statistics, the way
     * stock's post-frame submit does. The relocation pins then last exactly
     * as long as the writes they guard, and the keepalive flood that papered
     * over the gap is gone. The threshold is read at submit time: nothing
     * else on this exclusive channel moves 36 or 37 between here and the
     * frame, so +1 is unambiguous. If the sensor never delivers, the job
     * sits until its timeout and the channel dies -- the same price every
     * wedged run paid. */
    {
        uint32_t want_mem = syncpt_read(sp_mem) + 1;
        g[n++] = OP_SETCLASS(HOST1X_CLASS_ID);
        g[n++] = OP_INCR(HOST1X_WAIT_SYNCPT, 1);
        g[n++] = (sp_mem << 24) | (want_mem & 0xFFFFFF);
        if (sp_stats) {
            uint32_t want_stats = syncpt_read(sp_stats) + 1;
            g[n++] = OP_SETCLASS(HOST1X_CLASS_ID);
            g[n++] = OP_INCR(HOST1X_WAIT_SYNCPT, 1);
            g[n++] = (sp_stats << 24) | (want_stats & 0xFFFFFF);
        }
    }

    g[n++] = OP_IMM(0, sp);
    (void)hold_sp; (void)hold_at;

    nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);
    free(g);

    struct nvhost_reloc rel[4] = {
        { cmd_h, (uint32_t)y_word * 4, out_h, 0 },
        { cmd_h, (uint32_t)u_word * 4, out_h, u_off },
        { cmd_h, (uint32_t)v_word * 4, out_h, v_off },
        { cmd_h, (uint32_t)stats_word * 4, stats_h, 0 },
    };
    struct nvhost_reloc_shift sh[4] = { { 0 }, { 0 }, { 0 }, { 0 } };
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
    sa.num_relocs = 4;
    /* This job is parked on a counter we raise after the whole capture, so
     * three seconds -- the default -- is shorter than its own life. It ran
     * out and host1x killed the ISP channel, which costs a reboot. */
    sa.timeout = 60000;
    sa.syncpt_incrs = (uint32_t)(uintptr_t)&si;
    sa.cmdbufs = (uint32_t)(uintptr_t)&cb;
    sa.relocs = (uint32_t)(uintptr_t)rel;
    sa.reloc_shifts = (uint32_t)(uintptr_t)sh;
    sa.class_ids = (uint32_t)(uintptr_t)&cls;
    sa.fences = (uint32_t)(uintptr_t)&fence;

    errno = 0;
    int rc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
    printf("ISP frame: %d words, %ux%u fmt 0x%08x, strides %u/%u,"
           " trigger 0x%02x, rc=%d (%s)\n", n, W, H, fmt, stride_y,
           stride_uv, trigger, rc, rc == 0 ? "ok" : strerror(errno));
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
    if (pre_wait) {
        printf("  waiting %u ms after the mode set\n", pre_wait);
        usleep(pre_wait * 1000);
    }
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
    unsigned W = 2592, H = 1944, port = 1, vi_height = 0;
    const char *sensor = "ov5693";
    int use_sensor = 1, dump = 0, front = 1;
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
    uint32_t frame_length = 2064, coarse_time = 2000, gain = 16;
    /* The front sensor is on CIL E. The reference dump that had this word
     * at zero came from a session driving CIL A and B -- the other brick
     * entirely -- so it says nothing about ours. Back to the value that
     * brings E up, which is the one that reads 0x110 back from it. */
    uint32_t phy_cil_cmd = 0x12020000;   /* brick E, one lane */
    /* One frame. Every extra one queues another job behind a block that may
     * already be stuck, and when it is, the channel dies and only a reboot
     * brings the camera back -- so the cost of asking for more is paid by
     * hand, every time. Two is the ceiling; anything larger is clamped. */
    int tpg = 0, shots = 1, piggyback = 0;
    int hold = 0, dump_regs = 0, scan_cil = 0, refill = 0, scan_cond = 0;
    uint32_t seq_sp = 0;    /* --seq-sp: the syncpoint our own jobs ride on */
    int settle = 200;
    int fast_arm = 0;   /* --fast-arm: arm the next ISP frame right after the last completes */
    uint32_t isp_clk = 384000000;   /* --isp-clk=HZ */
    /* The memory-side rate the channel asks for. The driver does not set a
     * number here at all -- it computes an isochronous bandwidth from the
     * frame size and sets a latency allowance, neither of which we can reach
     * from userspace. This is the nearest lever we have. */
    unsigned long emc_rate = 528000000;
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
    uint32_t isp_fmt = 0x00000043, isp_e03 = 0;
    uint32_t isp_enable = 0x04040007, isp_trigger = ISP_TRIGGER_SENSOR;
    /* Which of the two routing writes go through host1x methods rather than
     * registers: bit 0 the ISP interface, bit 1 the image definition. */
    unsigned isp_route = 3;
    int demosaic_zero = 0;
    uint32_t rt_luma = 1;
    uint32_t ccm_word = 0;
    /* Which blocks of the driver's streaming init to leave out. None of
     * them appear in the stock per-frame trace, and whether every one of
     * them is right is not settled: bit 0 the luma weights at 0x400, bit 1
     * the GPP at 0x600, bit 2 the input stage, bit 3 the statistics, bit 4
     * the second processing channel. */
    unsigned isp_skip = 0;
    /* The input pixel format, if we choose to name it. 0x10200024 is Bayer
     * BGGR at ten bits in a sixteen-bit container, which is what this
     * sensor sends. */
    uint32_t isp_in_fmt = 0;
    uint32_t gpp_gain = 0x3fff0000;
    int luma_lo = 0;
    uint32_t in_dims = 0, in_mode = 1, in_phase = 0;
    int in_dims_set = 0, in_phase_set = 0;
    /* The channel-to-ISP interface. Three is the only value anything names,
     * and what the rest of the field means has never been looked at -- it
     * is the one register on the VI side that describes the handover. */
    uint32_t ispintf = ISPINTF_CONFIG_ENABLE;
    /* Clear the block the way stock clears it before anything else. On by
     * default now: it is what the camera on this device actually does. */
    int zero_init = 1;
    int isp_apply = 1;
    /* The demosaic group only, by default. Sending the whole table at once
     * took the statistics path down with it; the coefficients are what we
     * came for, so start with those alone and add a group at a time. */
    unsigned stock_cfg = STOCK_DEMOSAIC;
    uint32_t opt_u_off = 0, opt_v_off = 0;
    /* The kind to allocate the ISP's output as. Zero means an ordinary
     * pitch-linear buffer; 0xFE is what the block-linear format wants. */
    unsigned out_kind = 0;
    /* Configuring the other block the way stock does turns out to stop ours
     * writing at all, so it is off unless asked for. */
    int init_a = 0;
    /* Send the calibration with every frame rather than once, as stock
     * does. The init then carries only the clearing pass. */
    int per_frame_cal = 1;
    int out_iovmm = 0;
    int isp_only = 0;
    uint32_t stats_ctrl = 0;
    uint32_t proc_flags = 0;

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
        else if (strncmp(a, "--hold=", 7) == 0)   hold = atoi(a + 7);
        else if (strcmp(a, "--dump-regs") == 0)   dump_regs = 1;
        else if (strcmp(a, "--scan-cil") == 0)    scan_cil = 1;
        else if (strncmp(a, "--shots=", 8) == 0) {
            shots = atoi(a + 8);
            if (shots > 2) {
                printf("shots: asked for %d, taking 2 -- more than that"
                       " queues jobs behind a block that may already be"
                       " stuck, and that costs a reboot\n", shots);
                shots = 2;
            }
            if (shots < 1) shots = 1;
        }
        else if (strcmp(a, "--refill") == 0)      refill = 1;
        else if (strncmp(a, "--settle=", 9) == 0) settle = atoi(a + 9);
        else if (strcmp(a, "--fast-arm") == 0)    fast_arm = 1;
        else if (strncmp(a, "--isp-clk=", 10) == 0) isp_clk = (uint32_t)strtoul(a + 10, 0, 0);
        else if (strncmp(a, "--emc-bw=", 9) == 0) emc_bw = strtoul(a + 9, 0, 0);
        /* The stock's per-frame gather is some forty words; ours carried the
         * whole calibration (lookup tables, shading) with every frame. */
        else if (strcmp(a, "--no-per-frame-cal") == 0) per_frame_cal = 0;
        /* The stock's order: warm-ups on the opening's placeholders -- no
         * shading, warm-up windows, zero coefficients, 0x5555 shifts,
         * 0x650 = 3, the constant in 0x800/0x820, no work pointer -- and
         * the working values only afterwards (coefficient job, colour
         * job, real pass). Also drops the live-shadow blocks from the
         * opening rounds, which is where the real coefficients came in. */
        else if (strcmp(a, "--bare-warmup") == 0) { bare_warmup = 1; stock_cfg = 0; }
        else if (strcmp(a, "--stock-vi") == 0)    stock_vi = 1;
        else if (strcmp(a, "--no-isp") == 0)      no_isp = 1;
        else if (strncmp(a, "--attempts=", 11) == 0)
            attempts = (int)strtoul(a + 11, 0, 0);
        else if (strncmp(a, "--pre-wait=", 11) == 0)
            pre_wait = (unsigned)strtoul(a + 11, 0, 0);
        else if (strcmp(a, "--sensor-early") == 0) sensor_late = 0;
        else if (strcmp(a, "--sensor-twice") == 0) sensor_twice = 1;
        else if (strcmp(a, "--no-cal") == 0)       no_cal = 1;
        else if (strcmp(a, "--plane-rev") == 0)   plane_rev = 1;
        else if (strncmp(a, "--dm-after=", 11) == 0)
            dm_after = atoi(a + 11);
        else if (strcmp(a, "--real-pass") == 0)   use_real_pass = 1;
        else if (strcmp(a, "--arm-stats") == 0)   arm_stats = 1;
        else if (strcmp(a, "--own-scratch") == 0) own_scratch = 1;
        else if (strcmp(a, "--warmup") == 0)      do_warmup = 1;
        else if (strcmp(a, "--ccm") == 0)         ccm = 3;
        else if (strcmp(a, "--geo-blocks") == 0)  geo_blocks = 1;
        else if (strcmp(a, "--stream-xfer") == 0) stream_xfer = 1;
        else if (strncmp(a, "--seq-sp=", 9) == 0)  seq_sp = (uint32_t)atoi(a + 9);
        else if (strncmp(a, "--wb=", 5) == 0) {     /* --wb=R,B in hex 4.12 */
            wb_r = (uint32_t)strtoul(a + 5, 0, 16);
            const char *comma = strchr(a + 5, ',');
            if (comma) wb_b = (uint32_t)strtoul(comma + 1, 0, 16);
        }
        else if (strncmp(a, "--ccm=", 6) == 0)    ccm = atoi(a + 6);
        else if (strcmp(a, "--enable-late") == 0) { do_warmup = 1;
                                                    enable_late = 1; }
        else if (strncmp(a, "--rgb2y=", 8) == 0)
            rgb2y = (uint32_t)strtoul(a + 8, 0, 16);
        else if (strncmp(a, "--stats-kb=", 11) == 0)
            stats_kb = (unsigned)strtoul(a + 11, 0, 0);
        else if (strncmp(a, "--work-kb=", 10) == 0)
            work_kb = (unsigned)strtoul(a + 10, 0, 0);
        else if (strncmp(a, "--coarse=", 9) == 0)
            coarse_time = (uint32_t)strtoul(a + 9, 0, 0);
        else if (strncmp(a, "--vi-height=", 12) == 0)
            vi_height = (unsigned)strtoul(a + 12, 0, 0);
        else if (strncmp(a, "--emc=", 6) == 0)
            emc_rate = strtoul(a + 6, 0, 0);
        else if (strncmp(a, "--isp-fmt=", 10) == 0)
            isp_fmt = (uint32_t)strtoul(a + 10, 0, 16);
        else if (strncmp(a, "--isp-e03=", 10) == 0)
            isp_e03 = (uint32_t)strtoul(a + 10, 0, 16);
        else if (strncmp(a, "--isp-enable=", 13) == 0)
            isp_enable = (uint32_t)strtoul(a + 13, 0, 16);
        else if (strncmp(a, "--isp-trigger=", 14) == 0)
            isp_trigger = (uint32_t)strtoul(a + 14, 0, 16);
        else if (strncmp(a, "--isp-route=", 12) == 0)
            isp_route = (unsigned)strtoul(a + 12, 0, 0);
        else if (strcmp(a, "--demosaic-zero") == 0) demosaic_zero = 1;
        else if (strncmp(a, "--isp-luma=", 11) == 0)
            rt_luma = (uint32_t)strtoul(a + 11, 0, 0);
        else if (strncmp(a, "--ccm=", 6) == 0)
            ccm_word = (uint32_t)strtoul(a + 6, 0, 16);
        else if (strncmp(a, "--isp-skip=", 11) == 0)
            isp_skip = (unsigned)strtoul(a + 11, 0, 0);
        else if (strncmp(a, "--in-fmt=", 9) == 0)
            isp_in_fmt = (uint32_t)strtoul(a + 9, 0, 16);
        else if (strncmp(a, "--gpp=", 6) == 0)
            gpp_gain = (uint32_t)strtoul(a + 6, 0, 16);
        else if (strcmp(a, "--luma-lo") == 0) luma_lo = 1;
        else if (strncmp(a, "--in-dims=", 10) == 0)
            { in_dims = (uint32_t)strtoul(a + 10, 0, 16); in_dims_set = 1; }
        else if (strncmp(a, "--in-mode=", 10) == 0)
            in_mode = (uint32_t)strtoul(a + 10, 0, 16);
        else if (strncmp(a, "--in-phase=", 11) == 0)
            { in_phase = (uint32_t)strtoul(a + 11, 0, 16); in_phase_set = 1; }
        else if (strncmp(a, "--ispintf=", 10) == 0)
            ispintf = (uint32_t)strtoul(a + 10, 0, 16);
        else if (strcmp(a, "--no-zero-init") == 0) zero_init = 0;
        else if (strcmp(a, "--no-apply") == 0)     isp_apply = 0;
        else if (strcmp(a, "--no-stock-cfg") == 0) stock_cfg = 0;
        else if (strncmp(a, "--stock=", 8) == 0)
            stock_cfg = (unsigned)strtoul(a + 8, 0, 0);
        else if (strncmp(a, "--u-off=", 8) == 0)
            opt_u_off = (uint32_t)strtoul(a + 8, 0, 16);
        else if (strncmp(a, "--v-off=", 8) == 0)
            opt_v_off = (uint32_t)strtoul(a + 8, 0, 16);
        else if (strncmp(a, "--out-kind=", 11) == 0)
            out_kind = (unsigned)strtoul(a + 11, 0, 16);
        else if (strcmp(a, "--init-a") == 0)       init_a = 1;
        else if (strcmp(a, "--out-iovmm") == 0)    out_iovmm = 1;
        else if (strcmp(a, "--isp-only") == 0)     { isp_only = 1;
                                                     use_sensor = 0; }
        else if (strncmp(a, "--stats-ctrl=", 13) == 0)
            stats_ctrl = (uint32_t)strtoul(a + 13, 0, 16);
        else if (strncmp(a, "--proc-flags=", 13) == 0)
            proc_flags = (uint32_t)strtoul(a + 13, 0, 16);
        else if (strcmp(a, "--scan-cond") == 0)   scan_cond = 1;
        else if (strcmp(a, "--carveout") == 0)    alloc_heap = NVMAP_HEAP_CARVEOUT_GENERIC;
        else if (strcmp(a, "--tpg") == 0)         { tpg = 1; use_sensor = 0; }
        else if (strcmp(a, "--both") == 0)
            image_def |= IMAGE_DEF_DEST_MEM;
        else if (strcmp(a, "--piggyback") == 0)   { piggyback = 1; use_sensor = 0; }
        else if (strncmp(a, "--phy-cil=", 10) == 0)
            phy_cil_cmd = (uint32_t)strtoul(a + 10, 0, 16);
        else if (strncmp(a, "--gain=", 7) == 0)
            gain = (uint32_t)strtoul(a + 7, 0, 0);
        else { printf("unknown option %s\n", a); return 1; }
    }

    /* The resolution-dependent set, keyed to the exact size: the stock
     * camera refuses to run when these do not agree with the frame. */
    const struct geom_cfg *geo = geom_for(W, H);
    if (W != 2592 && W != 1280)
        printf("WARNING: no measured geometry for %ux%u -- using the"
               " nearest captured set\n", W, H);
    if (!in_dims_set) in_dims = geo->in_dims;
    if (!in_phase_set) in_phase = geo->in_phase;

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
    uint32_t sp_mw = sp_id;
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
    unsigned OH = vi_height ? vi_height : H;
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

    uint32_t u_off = opt_u_off ? opt_u_off
                               : ((stride_y * rows_y + 0xFFFF) & ~0xFFFFu);
    uint32_t v_off = opt_v_off ? opt_v_off
                               : ((u_off + stride_uv * rows_uv + 0xFFFF)
                                  & ~0xFFFFu);
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
                   " buffer %u, 4K alignment required --"
                   " fix --u-off/--v-off\n",
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
        if (seq_sp) isp_sp = seq_sp;

        /* And arm the other two conditions, which stock arms on every real
         * frame: 0x424 is condition four on 36, 0x525 is five on 37, 0x627
         * is six on 39, and it then waits on both 36 and 37. We had these
         * off because arming them once wedged the channel -- but that was
         * while our own submits were also sequenced on 37, which is the
         * counter the hardware raises for the statistics stage. With the
         * sequencing moved to 38 the conflict is gone, and a pipeline whose
         * statistics stage is never asked to finish is a fair suspect for
         * one that never finishes the write either. */
        sp_stats = arm_stats ? sps[1] : 0;
        sp_loadv = arm_stats ? sps[3] : 0;

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
        int crc1 = ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &ic);
        /* This kernel has no GET_CLK_RATE (it logs "unrecognized ioctl"), so
         * the rate actually granted is only visible in
         * /sys/kernel/debug/clock/ispb/rate while the channel is busy. */
        printf("ISP clock request: %u Hz -> rc=%d (%s); EMC bandwidth %lu B/s -> rc=%d\n",
               isp_clk, crc0, crc0 ? strerror(cerr0) : "ok", emc_bw, crc1);
        /* The ISP write client's latency allowance, as the stock sets it
         * at every opening. Without it, on a fresh boot, the ISP never
         * finished even an 8x8 warm-up; after one run of the stock camera
         * -- whose setting outlives everything but a reboot -- it did. */
        /* Not on the channel: the channel's ioctl handler refuses every
         * magic but its own with EFAULT. The ISP driver's ioctls live on
         * its control node, /dev/nvhost-ctrl-isp.1 for ISP-B. */
        struct isp_emc_info ei = { 0, 81600, 0, 16 };
        int lfd = open("/dev/nvhost-ctrl-isp.1", O_RDWR);
        errno = 0;
        int lrc = lfd < 0 ? -1 : ioctl(lfd, NVHOST_ISP_IOCTL_SET_EMC, &ei);
        printf("ISP latency allowance (clk 81600 kHz, 16 bpp out, 162 MB/s HARD) -> rc=%d%s%s\n",
               lrc, lrc ? " " : "", lrc ? strerror(errno) : "");
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
             * them is 0xFC=0x20 -- 0x54 never appears. We had been writing
             * the pipeline mode there on the strength of a driver comment,
             * so pass --isp-enable=0 to do as stock does and leave it alone. */
            /* And when the enable is to go where stock puts it -- inside
             * the first warm-up frame, once -- it does not go here at all. */
            uint32_t off[2] = { 0xFC, 0x54 };
            uint32_t val[2] = { 0x20, isp_enable };
            struct regrdwr_args ra;
            memset(&ra, 0, sizeof ra);
            ra.id = 0;
            ra.num_offsets = (isp_enable && !enable_late) ? 2 : 1;
            ra.block_size = 4;
            ra.offsets = (uint32_t)(uintptr_t)off;
            ra.values = (uint32_t)(uintptr_t)val;
            ra.write = 1;
            errno = 0;
            int rrc = ioctl(isp_fd, NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR, &ra);
            printf("ISP registers: 0xFC=0x20, 0x54=0x%08x, rc=%d (%s)\n",
                   isp_enable, rrc, rrc == 0 ? "ok" : strerror(errno));
        }

        /* A scratch buffer the ISP wants for its own working state. The
         * reprocess tool calls it required for a cold start. */
        /* Big enough for the offsets the runtime configuration hands out --
         * it reaches 0x3f4a0 into this buffer. */
        work_h = nvmap_create(work_kb * 1024);
        if (work_h && nvmap_alloc(work_h) == 0) work_iova = nvmap_pin(work_h);

        /* Stock configures the other block first and in full. Do the same
         * before touching ours -- a separate channel, its own wake-up
         * write and its own syncpoint. */
        if (init_a) {
            int afd = open("/dev/nvhost-isp", O_RDWR);
            if (afd < 0) printf("open /dev/nvhost-isp: %s\n", strerror(errno));
            else {
                struct nvhost_set_nvmap_fd_args anf = { (uint32_t)nvmap_fd };
                ioctl(afd, NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD, &anf);
                struct nvhost_get_param_arg ap = { .param = 0, .value = 0 };
                uint32_t asp = 0;
                if (ioctl(afd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &ap) == 0)
                    asp = ap.value;
                struct nvhost_clk_rate_args ac;
                ac.moduleid = 0; ac.rate = 384000000;
                ioctl(afd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &ac);
                uint32_t aoff = 0xFC, aval = 0x20;
                struct regrdwr_args ara;
                memset(&ara, 0, sizeof ara);
                ara.id = 0; ara.num_offsets = 1; ara.block_size = 4;
                ara.offsets = (uint32_t)(uintptr_t)&aoff;
                ara.values = (uint32_t)(uintptr_t)&aval;
                ara.write = 1;
                ioctl(afd, NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR, &ara);
                if (asp) isp_init_a(afd, asp);
                close(afd);
            }
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
            /* The contiguous heap is what VI needs, but the ISP's output at
             * full resolution is twenty megabytes and the write faults part
             * way through -- five hundred rows in. Whether that heap can
             * hand out a run that long is a fair question, so the output
             * can come from the scattered one instead. */
            uint32_t save = alloc_heap;
            if (out_iovmm) alloc_heap = NVMAP_HEAP_IOVMM;
            int ok = out_kind ? nvmap_alloc_kind(out_h, out_kind)
                              : nvmap_alloc(out_h);
            alloc_heap = save;
            if (ok == 0) out_iova = nvmap_pin(out_h);
        }
        printf("ISP-B channel fd=%d, syncpoints %u/%u/%u/%u, output %u bytes"
               " at 0x%08x (U at +0x%x, V at +0x%x)\n", isp_fd, isp_sp,
               sp_mem, sp_stats, sp_loadv, out_bytes, out_iova, u_off, v_off);
        if (work_h)
            isp_init(isp_fd, work_h, isp_enable, isp_sp,
                     work_iova, stats_iova, demosaic_zero, rt_luma, ccm_word,
                     isp_skip, gpp_gain, luma_lo, in_dims, in_mode,
                     in_phase, zero_init, isp_apply, stats_ctrl, stock_cfg,
                     geo);
        if (out_h) {
            uint32_t chunk = 65536;
            void *p = malloc(chunk);
            memset(p, 0x5A, chunk);
            for (uint32_t o = 0; o < out_bytes; o += chunk)
                nvmap_rw(out_h, o, p,
                         out_bytes - o < chunk ? out_bytes - o : chunk, 1);
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
    if (isp_route && (image_def & (IMAGE_DEF_DEST_ISP_A |
                                   IMAGE_DEF_DEST_ISP_B))) {
        uint32_t cmd_h = nvmap_create(4096);
        nvmap_alloc(cmd_h);
        uint32_t g[10];
        int n = 0;
        g[n++] = OP_SETCLASS(VI_CLASS_ID);
        if (isp_route & 1) { g[n++] = OP_INCR(0x099, 1);
                             g[n++] = ispintf; }
        if (isp_route & 2) { g[n++] = OP_INCR(0x282, 1);
                             g[n++] = image_def; }
        g[n++] = OP_IMM(0, sp_cmd);
        nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);

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
        printf("VI routing via host1x (mask %u): %d words, rc=%d (%s)\n",
               isp_route, n, rc, rc == 0 ? "ok" : strerror(errno));
        ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    }

    int sfd = -1;
    if (use_sensor) {
        char sn[64];
        snprintf(sn, sizeof sn, "/dev/%s", sensor);
        sfd = open(sn, O_RDWR);
        if (sfd < 0) { printf("open %s: %s\n", sn, strerror(errno)); return 1; }
        /* --sensor-twice: power the sensor on, off, and on again before the
         * mode set. The board's power-on releases the reset two
         * microseconds after enabling the core rail; from cold (a minute or
         * more since the last power-off) the rail is still ramping and the
         * part comes up answering on I2C but never streaming, which is
         * what the first run after every boot looked like. A second
         * power-on within a second of the first is a warm one. */
        if (sensor_twice) {
            usleep(50000);
            close(sfd);
            usleep(100000);
            sfd = open(sn, O_RDWR);
            if (sfd < 0) { printf("reopen %s: %s\n", sn, strerror(errno)); return 1; }
            printf("sensor power-cycled once before the mode set\n");
        }
        if (front) {
            /* Opening the node already powered it -- but only just. The log
             * puts the mode ioctl twenty-five microseconds after the power
             * sequence returns, and the part answers neither of the two
             * writes that follow: "no acknowledge from address 0x36". The
             * driver's power-on does not wait for the sensor to come out of
             * reset, so the wait has to be here. */
            usleep(50000);
            /* The stream itself starts after the receiver is up (see
             * sensor_start_front); --sensor-early keeps the old order. */
            if (!sensor_late)
                sensor_start_front(sfd, W, H, frame_length, coarse_time, gain);
            else
                printf("sensor powered; streaming deferred until the receiver is up\n");
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
    /* --piggyback: touch nothing that is already running. The stock camera
     * brings this receiver up correctly, so let it, and change only where
     * the pixels go -- the destination field and the surface. If a frame
     * lands, the capture path is ours; if it does not, the fault is in the
     * write itself and not in any of the bring-up we have been repeating.
     *
     * It also corrects a wrong reading: the frame-start syncpoint counts
     * our own shots, not arriving frames -- it advances by the same amount
     * with no sensor at all. */
    if (piggyback) {
        printf("piggyback: leaving the running configuration alone\n");
        vi_wr(base + VI_CSI_IMAGE_DEF,
              (1u << BYPASS_PXL_TRANSFORM_OFFSET) |
              (IMAGE_FORMAT_T_R16_I << IMAGE_DEF_FORMAT_OFFSET) |
              IMAGE_DEF_DEST_MEM);
        vi_wr(base + VI_CSI_SURFACE0_OFFSET_MSB, 0);
        vi_wr(base + VI_CSI_SURFACE0_OFFSET_LSB, iova);
        vi_wr(base + VI_CSI_SURFACE0_STRIDE, stride);
        vi_wr(base + VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);
        vi_flush("piggyback");
        usleep(500000);
        dump_regs = 1;      /* this is the state where capture works */
        goto readback;
    }

    /* --isp-only: arm the ISP and touch nothing else. The kernel's own
     * pattern facility drives VI -- it powers the block, enables the
     * generator, sets the routing and fires the shot -- so this lets the
     * colour path be tested on a synthetic frame, with the sensor and our
     * whole receiver bring-up out of the picture. If the mosaic survives
     * that too, the question is not about how we describe the sensor. */
    if (isp_only) goto isp_wait;

    pmc_dpd_release(front ? PMC_DPD_BIT_CSIE : (1u << 0) /* CSIA */);
    car_enable_csi_clocks();

    /* The driver writes this the moment VI comes up and we never wrote it at
     * all. It governs VI's internal clock gating, so with it unset the
     * registers still answer -- nvhost powers the module for an ioctl -- and
     * the write engine has no clock to run on. */
    vi_wr(TEGRA_VI_CFG_CG_CTRL, 1);

    if (scan_cil) {
        /* Which brick does this sensor actually arrive on? Nothing we have
         * answers it: the board file leaves the port to userspace, and the
         * device tree's prose and its own property disagree. So ask the
         * hardware -- the sensor is streaming by now, so bring every pad
         * out of power-down, enable both halves of the brick command, and
         * read all five lane interfaces. The one carrying the sensor is the
         * one whose status stops reading zero. */
        static const struct { const char *name; unsigned pad, phy, st, cst; }
        cil[] = {
            { "A", 0x92C, 0x934, 0x93C, 0x940 },
            { "B", 0x960, 0x968, 0x970, 0x974 },
            { "C", 0x994, 0x99C, 0x9A4, 0x9A8 },
            { "D", 0x9C8, 0x9D0, 0x9D8, 0x9DC },
            { "E", 0xA08, 0xA10, 0xA18, 0xA1C },
        };
        pmc_dpd_release(0x1 | 0x2 | 0x4 | 0x8 | PMC_DPD_BIT_CSIE);
        vi_wr(T124_CSI_CLKEN_OVERRIDE, 0);
        for (unsigned i = 0; i < 5; i++) {
            vi_wr(cil[i].pad, 0x00000005);
            vi_wr(cil[i].phy, 0x00000002);
            vi_wr(cil[i].st, 0xFFFFFFFF);
            vi_wr(cil[i].cst, 0xFFFFFFFF);
        }
        vi_wr(T124_CSI_PHY_CIL_COMMAND, 0x12021202);
        vi_flush("scan bring-up");

        for (int pass = 0; pass < 3; pass++) {
            usleep(200000);
            printf("pass %d:", pass);
            for (unsigned i = 0; i < 5; i++)
                printf("  %s=%08x/%08x", cil[i].name,
                       vi_rd(cil[i].st), vi_rd(cil[i].cst));
            printf("\n");
        }
        printf("parser A=%08x B=%08x\n",
               vi_rd(T124_PP_A_PIXEL_PARSER_STATUS),
               vi_rd(T124_PP_B_PIXEL_PARSER_STATUS));
        goto done;
    }

    if (front) {
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
        /* The stock camera never writes CILC's pad register for the front
         * sensor: only CILA/CILB (zero) and CILE (zero, then THS 9). The
         * 4x brick mode here is what the R21.5 V4L2 driver did. */
        if (!stock_vi) vi_wr(T124_CILC_PAD_CONFIG0, 0x00010000);
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
        vi_wr(T124_PP_B_PIXEL_STREAM_CONTROL0, stock_vi ? 0x080301f1 : 0x280301f1);
        vi_wr(T124_PP_B_PIXEL_STREAM_PP_COMMAND, 0x0000f005);
        vi_wr(T124_PP_B_PIXEL_STREAM_CONTROL1, stock_vi ? 0 : 0x00000011);
        vi_wr(T124_PP_B_PIXEL_STREAM_GAP, 0x00140000);
        vi_wr(T124_PP_B_PIXEL_STREAM_EXPECTED_FRAME, 0x0);
        vi_wr(T124_PP_B_INPUT_STREAM_CONTROL, stock_vi ? 0x007f0014 : 0x003f0000);

        /* Only the upper half of the brick command is ours; the lower half
         * belongs to the rear path and has to survive our write. The stock
         * enables brick E alone (0x10000000) and leaves C and D untouched. */
        vi_wr(T124_CSI_PHY_CIL_COMMAND,
              (vi_rd(T124_CSI_PHY_CIL_COMMAND) & 0x0000FFFF) |
              (stock_vi ? 0x10000000 : phy_cil_cmd));
        vi_wr(T124_CSI_DEBUG_CONTROL, T124_CSI_DEBUG_COUNTER_CFG);
        /* --tpg: let the receiver make its own picture. This splits the
         * problem in half -- if the pattern lands in the buffer then VI,
         * the parser and the write path are all sound and the fault is on
         * the wire; if it does not, the fault is in our channel setup and
         * the sensor was never the question. */
        if (tpg) {
            printf("  pattern generator on (port B)\n");
            vi_wr(TPG_B_BASE + TPG_CTRL, ((1u - 1) << PG_MODE_OFFSET) | PG_ENABLE);
            vi_wr(TPG_B_BASE + TPG_PHASE, 0);
            vi_wr(TPG_B_BASE + TPG_RED_FREQ, (0x10u << 16) | 0x10u);
            vi_wr(TPG_B_BASE + TPG_RED_FREQ_RATE, 0);
            vi_wr(TPG_B_BASE + TPG_GREEN_FREQ, (0x10u << 16) | 0x10u);
            vi_wr(TPG_B_BASE + TPG_GREEN_FREQ_RATE, 0);
            vi_wr(TPG_B_BASE + TPG_BLUE_FREQ, (0x10u << 16) | 0x10u);
            vi_wr(TPG_B_BASE + TPG_BLUE_FREQ_RATE, 0);
        }

        vi_flush("CSI bring-up");
        /* --no-cal: skip our MIPI calibration. The 24.1 kernel driver never
         * calibrates the one-lane front path at all (its lane mask covers
         * 2, 4 and 8 lanes only) and enables just the bias pad -- and its
         * pictures are right. Ours are garbage on a fresh boot and right
         * after one run of the stock camera, whose own calibration leaves
         * the pads in a state that outlives everything but a reboot. */
        if (!tpg && !no_cal) mipi_calibrate_csie();
        else if (no_cal) {
            mipi_upd(MIPI_BIAS_PAD_CFG2, BIAS_PDVREG, 0);
            mipi_upd(MIPI_BIAS_PAD_CFG0, BIAS_E_VCLAMP_REF, 0);
            printf("  MIPI calibration skipped; bias pad enabled (PDVREG 0, VCLAMP_REF 0)\n");
        }
        printf("  CILE pad0 after bring-up and calibration: 0x%08x\n",
               vi_rd(T124_CILE_PAD_CONFIG0));
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
        /* VI's memory side goes through its own ioctl: a bandwidth in
         * KB/s that the driver turns into the latency allowance for the
         * VI write client (and an isomgr reservation). The stock's trace
         * shows it at 162 MB/s. The "moduleid = 1" clock request this
         * replaced matched no pdata entry and set the VI clock again. */
        /* The stock's figure, 162 MB/s; 528 MB/s came back ENOMEM. */
        unsigned vi_bw_kbps = 162000;
        /* On the VI control node, not the channel (see the ISP one). */
        int vcfd = open("/dev/nvhost-ctrl-vi.1", O_RDWR);
        errno = 0;
        int a2 = vcfd < 0 ? -1 : ioctl(vcfd, NVHOST_VI_IOCTL_SET_EMC_INFO, &vi_bw_kbps);
        printf("clock/bandwidth request: module rc=%d, VI bandwidth %u KB/s rc=%d%s%s\n",
               a1, vi_bw_kbps, a2, a2 ? " " : "", a2 ? strerror(errno) : "");
        if (vcfd >= 0) close(vcfd);
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
    if (stock_vi) {
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
    vi_wr(base + VI_CSI_ISPINTF_CONFIG, ispintf);
    /* What the capture has for this group, and what we were leaving to
     * chance. */
    vi_wr(base + VI_CSI_RGB2Y_CTRL, rgb2y);
    vi_wr(base + VI_CSI_MEM_TILING, 0);
    vi_wr(base + VI_CSI_IMAGE_DT, IMAGE_DT_RAW10);
    vi_wr(base + VI_CSI_IMAGE_SIZE_WC, wc);
    /* The sensor keeps its own mode; this is only how many of its lines VI
     * is told to take. Asking for fewer than arrive is how we find out
     * whether a truncated capture is the link running out or the write. */
    vi_wr(base + VI_CSI_IMAGE_SIZE,
          ((vi_height ? vi_height : H) << IMAGE_SIZE_HEIGHT_OFFSET) | W);
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

    /* Receiver and VI are configured: now the sensor may start streaming
     * (see sensor_start_front for why not earlier). */
    if (sensor_late && sfd >= 0 && front && use_sensor)
        sensor_start_front(sfd, W, H, frame_length, coarse_time, gain);
    if (front)
        printf("  CILE pad0 after the sensor start: 0x%08x\n",
               vi_rd(T124_CILE_PAD_CONFIG0));

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
    /* Two different syncpoints, as the driver uses: one for the frame
     * start and a separate one for the memory-write acknowledge. We had
     * been arming both conditions against the same id. */
    /* Both acknowledge conditions. The header says outright that these
     * event numbers were found by trial rather than derived, so which of
     * the two belongs to this port is worth not assuming. */
    /* Only the frame start is armed here. The driver arms the memory-write
     * acknowledge only after the frame start has fired -- once the DMA is
     * already running -- so arming it up front was our invention. */
    vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT,
          (front ? T124_PPB_FRAME_START : T124_PPA_FRAME_START) << 8 | sp_id);

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
        /* Sent together they reproduce exactly what the driver's note
         * records: the frame start never arrives. Which of the two does it
         * is worth knowing, so each is separately selectable. */
        if (isp_route & 1) {
            g[n++] = OP_INCR(0x099, 1);
            g[n++] = ispintf;
        }
        if (isp_route & 2) {
            g[n++] = OP_INCR(0x282, 1);
            g[n++] = image_def;
        }

        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_SURFACE0_OFFSET_MSB), 1);
        g[n++] = 0;
        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_SURFACE0_OFFSET_LSB), 1);
        addr_word = n;
        g[n++] = 0;                       /* the kernel fills this in */
        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_SINGLE_SHOT), 1);
        g[n++] = SINGLE_SHOT_CAPTURE;

        /* Park the job so the mapping outlives the submit. Without this the
         * memory controller faults on the buffer's own base part way
         * through the capture and the picture stops there. */
        hold_thresh = syncpt_read(sp_mw) + 1;
        g[n++] = OP_SETCLASS(HOST1X_CLASS_ID);
        g[n++] = OP_INCR(HOST1X_WAIT_SYNCPT, 1);
        g[n++] = (sp_mw << 24) | (hold_thresh & 0xFFFFFF);
        g[n++] = OP_SETCLASS(VI_CLASS_ID);
        /* Retire the command buffer on a syncpoint of its own. It used to
         * share one with the frame-start condition, so that counter moved
         * once per submit whether or not a frame ever started -- which is
         * exactly the reading we were treating as evidence. */
        g[n++] = OP_IMM(0, sp_cmd);
        nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);

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

        /* Which event numbers does this hardware actually raise? The two we
         * use for the write acknowledge came from a header that says
         * outright they were found by trial, and neither of them fires. So
         * arm every condition in turn against one counter, take a shot, and
         * let the ones that move name themselves. */
        if (scan_cond) {
            uint8_t fill[64], tail[64];
            memset(fill, 0xA5, sizeof fill);
            for (uint32_t cond = 0; cond < 32; cond++) {
                /* Each pass is a real capture, carried to the bottom of the
                 * frame. The first sweep judged a condition on 150ms of
                 * waiting, which is less than a frame and less than the
                 * first shot needs to start at all -- so it was reporting
                 * which events arrive early, not which ones arrive. */
                nvmap_rw(buf_h, frame - sizeof fill, fill, sizeof fill, 1);
                uint32_t fs0 = syncpt_read(sp_id), v0 = syncpt_read(sp_mw);

                vi_wr(base + VI_CSI_SW_RESET, 0xF);
                vi_wr(base + VI_CSI_SW_RESET, 0x0);
                vi_wr(pp, (0xFu << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
                          CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_ENABLE);
                vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT,
                      (front ? T124_PPB_FRAME_START : T124_PPA_FRAME_START)
                      << 8 | sp_id);
                vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, cond << 8 | sp_mw);
                vi_wr(base + VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);
                vi_flush(0);

                int w = 0, done = 0;
                while (w < 2500 && !done) {
                    nvmap_rw(buf_h, frame - sizeof tail, tail, sizeof tail, 0);
                    for (unsigned i = 0; i < sizeof tail; i++)
                        if (tail[i] != 0xA5) { done = 1; break; }
                    if (!done) { usleep(1000); w++; }
                }
                printf("  condition %2u: %-5s  (frame %s, %dms)\n", cond,
                       syncpt_read(sp_mw) != v0 ? "FIRES" : "-",
                       done ? "whole" : (syncpt_read(sp_id) != fs0
                                         ? "started only" : "never started"),
                       w);
            }
            goto readback;
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
                  (front ? T124_PPB_FRAME_START : T124_PPA_FRAME_START)       \
                  << 8 | sp_id);                                              \
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
        if (do_warmup && isp_fd >= 0) {
            warm_h = nvmap_create(64 * 1024);
            if (warm_h && nvmap_alloc(warm_h) == 0) {
                nvmap_pin(warm_h);
                warm_left = 2;
            } else {
                warm_h = 0;
            }
        }

        for (int shot = 0; shot < shots; shot++) {
            int attempt = 0, done = 0, started = 0, waited = 0, mwaited = 0;

            /* Two at most. The first trigger aligns, the second captures;
             * more than that only stretches the run, and a long run is what
             * took the parked job past the timeout the kernel allows. */
            while (attempt < attempts && !done) {
                uint32_t fs0 = syncpt_read(sp_id);
                attempt++;

                /* Wiping is a diagnostic, not part of a capture: it is ten
                 * megabytes through a hundred and fifty ioctls, it runs
                 * while the hardware is writing, and the holes it leaves in
                 * the picture are its own. Off by default for that reason --
                 * and the row-to-row jumps that looked like tearing were
                 * those holes, since the fill pattern reads as 42405 against
                 * a picture whose values sit near 25. */
                if (refill && attempt == 2) {
                    uint32_t chunk = 64 * 1024;
                    void *p = malloc(chunk);
                    memset(p, 0xA5, chunk);
                    for (uint32_t o = 0; o < frame; o += chunk)
                        nvmap_rw(buf_h, o, p,
                                 frame - o < chunk ? frame - o : chunk, 1);
                    free(p);
                } else {
                    nvmap_rw(buf_h, frame - sizeof fill, fill, sizeof fill, 1);
                }

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
                /* After the first frame, in its own job, the way the
                 * capture shows the stock stack doing it. */
                if (isp_fd >= 0 && out_iova && shot >= dm_after && !dm_sent) {
                    isp_demosaic(isp_fd, isp_sp, out_h, stats_h,
                                 u_off, v_off, work_iova);
                    dm_sent = 1;
                }
                /* And the working configuration after the first frame, in
                 * the place the capture puts it. */
                if (isp_fd >= 0 && out_iova && shot > 0 && !real_sent
                    && use_real_pass) {
                    isp_real_pass(isp_fd, isp_sp, work_iova);
                    real_sent = 1;
                }

                /* The warm-up needs pixels, which is what running it before
                 * the capture ever started was missing. So it goes here
                 * instead, in place of a real frame, while the receiver is
                 * armed and the sensor is delivering -- the way stock runs
                 * it, between frames of a live stream. */
                if (warm_left > 0 && isp_fd >= 0 && warm_h) {
                    isp_base_mem = syncpt_read(sp_mem);
                    isp_warmup(isp_fd, isp_sp, warm_h, stats_h,
                               warm_left == 2, isp_enable, W, OH);
                    if (warm_left == 2 && !dm_sent) {
                        isp_demosaic(isp_fd, isp_sp, out_h, stats_h,
                                     u_off, v_off, work_iova);
                        dm_sent = 1;
                    }
                }
                else if (isp_fd >= 0 && out_iova && stats_h) {
                    isp_base_mem = syncpt_read(sp_mem);
                    isp_frame(isp_fd, out_h, stats_h, W, OH, isp_fmt, isp_e03,
                              isp_trigger, u_off, v_off,
                              sp_mem, sp_stats, sp_loadv, isp_sp, 0, 0,
                              isp_in_fmt, work_iova, per_frame_cal,
                              proc_flags);
                }

                vi_wr(base + VI_CSI_SURFACE0_OFFSET_MSB, 0);
                vi_wr(base + VI_CSI_SURFACE0_OFFSET_LSB, iova);
                vi_wr(base + VI_CSI_SURFACE0_STRIDE, stride);
                if (front && !cile_rewritten) {
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
                vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT,
                      (front ? T124_PPB_FRAME_START : T124_PPA_FRAME_START)
                      << 8 | sp_id);
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
                    while (syncpt_read(sp_mem) == isp_base_mem && w2 < 2500) {
                        usleep(2000);
                        w2 += 2;
                    }
                    /* A moment beyond the condition, because the last
                     * transfer may still be draining when it fires. */
                    if (!fast_arm) usleep(20000);
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
                if (!fast_arm) usleep((useconds_t)period * 500 + settle * 1000);
                mwaited = settle;
                done = started;
            }
            printf("  frame %d: %s after %d attempt%s"
                   " (start %dms, bottom %dms), parser %08x\n",
                   shot, done ? "whole" : (started ? "SHORT" : "NEVER STARTED"),
                   attempt, attempt == 1 ? "" : "s", waited, mwaited,
                   vi_rd(front ? T124_PP_B_PIXEL_PARSER_STATUS
                               : T124_PP_A_PIXEL_PARSER_STATUS));

            /* A warm-up round is not one of the frames that were asked
             * for, so it does not spend one. */
            if (warm_left > 0) {
                printf("  (that was a warm-up round, output condition %+d)\n",
                       (int)(syncpt_read(sp_mem) - isp_base_mem));
                if (--warm_left == 0) {
                    /* Where the capture puts it: after the warm-up and the
                     * coefficients, once, and never in the opening round. */
                    if (ccm) isp_colour(isp_fd, isp_sp, work_iova, W, OH);
                    if (use_real_pass && !real_sent) {
                        isp_real_pass(isp_fd, isp_sp, work_iova);
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

        /* Stop the capture before reading a single byte. The trigger bit
         * stays set once written and the acknowledge kept firing at once,
         * frame after frame -- so while we sat waiting, VI was overwriting
         * the buffer from the top with a newer frame and leaving the tail of
         * an older one below. That is the tear: not a write that stopped
         * short, but one that had started again. */
        vi_wr(pp, 0);
        vi_wr(base + VI_CSI_SINGLE_SHOT, 0);
        vi_wr(base + VI_CSI_SW_RESET, 0xF);
        vi_wr(base + VI_CSI_SW_RESET, 0x0);
        vi_flush("capture stopped");
        if (isp_fd >= 0 && out_iova) isp_stop(isp_fd, isp_sp);

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
    goto after_readback;

isp_wait:
    /* Arm the ISP, then sit still while something else drives VI. */
    if (isp_fd >= 0 && out_iova && stats_h) {
        isp_base_mem = syncpt_read(sp_mem);
        isp_frame(isp_fd, out_h, stats_h, W, OH, isp_fmt, isp_e03,
                  isp_trigger, u_off, v_off, sp_mem, sp_stats, sp_loadv,
                  isp_sp, 0, 0, isp_in_fmt, work_iova, per_frame_cal,
                              proc_flags);
        int w3 = 0;
        while (syncpt_read(sp_mem) == isp_base_mem && w3 < 4000) {
            usleep(5000);
            w3 += 5;
        }
        printf("ISP armed and waiting: wrote %s after %dms\n",
               syncpt_read(sp_mem) != isp_base_mem ? "yes" : "NO", w3);
        isp_stop(isp_fd, isp_sp);
    }
    goto after_readback;

after_readback:
    if (isp_only) goto readback;

    printf("readback: IMAGE_DEF=0x%08x DT=0x%08x SIZE=0x%08x WC=0x%08x\n",
           vi_rd(base + VI_CSI_IMAGE_DEF), vi_rd(base + VI_CSI_IMAGE_DT),
           vi_rd(base + VI_CSI_IMAGE_SIZE), vi_rd(base + VI_CSI_IMAGE_SIZE_WC));
    printf("readback: SURFACE0=0x%08x STRIDE=0x%08x PP=0x%08x\n",
           vi_rd(base + VI_CSI_SURFACE0_OFFSET_LSB),
           vi_rd(base + VI_CSI_SURFACE0_STRIDE), vi_rd(pp));

readback:
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
    printf("CIL status: E=0x%08x CILE=0x%08x parser=0x%08x\n",
           vi_rd(front ? 0xA18 : T124_CSI_CIL_A_STATUS),
           vi_rd(front ? 0xA1C : T124_CSI_CILA_STATUS),
           vi_rd(front ? T124_PP_B_PIXEL_PARSER_STATUS
                       : T124_PP_A_PIXEL_PARSER_STATUS));

    /* The channel is exclusive, so nothing outside this process can read
     * the aperture while we hold it -- and picking ranges by hand is how a
     * difference gets missed. Dump the lot and diff it against a stock
     * session offline. */
    if (dump_regs) {
        printf("=== VI aperture ===\n");
        /* Sixteen at a time: one register per ioctl turned a dump of the
         * aperture into hundreds of round trips and never finished. */
        for (uint32_t o = 0; o < 0xC00; o += 64) {
            uint32_t offs[16], vals[16];
            for (int i = 0; i < 16; i++) { offs[i] = o + i * 4; vals[i] = 0; }
            struct regrdwr_args a;
            memset(&a, 0, sizeof a);
            a.id = 0;
            a.num_offsets = 16;
            a.block_size = 4;
            a.offsets = (uint32_t)(uintptr_t)offs;
            a.values = (uint32_t)(uintptr_t)vals;
            a.write = 0;
            if (ioctl(vi_fd, NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR, &a) < 0)
                continue;
            for (int i = 0; i < 16; i++)
                if (vals[i]) printf("  +0x%03x = 0x%08x\n", offs[i], vals[i]);
        }
        printf("=== end ===\n");
    }

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

    if (hold > 0) {
        printf("holding the sensor open for %d s -- probe it now\n", hold);
        fflush(stdout);
        sleep((unsigned)hold);
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
    goto shutdown;

done:
    if (sfd >= 0) close(sfd);
shutdown:
    nvmap_unpin(buf_h);
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)buf_h);
    close(vi_fd);
    close(nvmap_fd);
    printf("=== done ===\n");
    return 0;
}
