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
#include "isp_b_cal.h"
#include "isp_stock.h"
#include "isp_demosaic.h"
#include "isp_real.h"
#include "stock_opening_720p.h"

/* The opening and the per-frame calibration both carry isp_b_cal_data,
 * read out of a 2592-wide stock session: its 0xd00 block and its 0xd0b
 * mesh are that raster's. At 1280 wide they are swapped for the stock's
 * 720p words -- the same ones the working configuration sends -- else
 * every frame re-applies a shading mesh laid out for a different raster
 * on top of the working configuration's, which is what impl-1's gather
 * diff found (gather-diff-720p.md §3). --cal-2592 keeps the old table. */
static unsigned cal_fit_w = 2592;
static int cal_2592 = 0;
static void cal_fit_width(uint32_t *g, unsigned words)
{
    if (cal_fit_w != 1280 || cal_2592) return;
    for (unsigned i = 0; i + 1 < words; i++) {
        if (g[i] == 0x1d00000a && i + 10 < words) {
            memcpy(&g[i + 1], isp_real_d00_720, 10 * 4); i += 10;
        } else if (g[i] == 0x2d0b01e0 && i + 480 < words) {
            memcpy(&g[i + 1], isp_real_d0b_720, 480 * 4); i += 480;
        }
    }
}

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
/* Which group of the table a block belongs to (--stock-groups=MASK).
 *
 * The demosaic group alone is the recipe that gave a picture. Sending the
 * whole table at once cost a capture and a reboot: the statistics blocks
 * went in with it, ours had been configured differently, and the frame then
 * waited on a statistics syncpoint that never moved. The colour group
 * carries the four 257-entry tone tables (0x651..0x657) and the shading
 * tables, which nothing of ours writes -- on a fresh boot they hold
 * whatever reset left, after a stock session they hold the stock's curves.
 * 0x700/0x750 carry the stock process's own addresses and go out only when
 * asked for. */
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
    case 0x200: case 0x202: case 0x205:
        return STOCK_INPUT;
    case 0x300: case 0x304:
        return STOCK_CCM;
    case 0x700: case 0x750:
        return STOCK_CHAN;
    default:
        return 0;
    }
}

unsigned isp_stock_emit(uint32_t *g, unsigned n, uint32_t work_iova)
{
    unsigned count = sizeof isp_stock_blocks / sizeof isp_stock_blocks[0];
    for (unsigned b = 0; b < count; b++) {
        const struct isp_block *bl = &isp_stock_blocks[b];
        const uint32_t *d = bl->data;

        /* Skipped deliberately: its second word is where the stock process
         * kept its scratch buffer, and that address means nothing here. */
        if (bl->method == 0x053) continue;
        if (!(stock_group(bl->method) & stock_groups)) continue;

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
    g[n++] = OP_INCR(0x053, 2); g[n++] = 1; g[n++] = work_iova;
    return n;
}

int isp_init(int isp_fd, uint32_t work_h, uint32_t sp,
             uint32_t work_iova, uint32_t stats_iova,
             const struct geom_cfg *geo)
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
    /* The three words after the enable are 75, 147 and 34 -- the BT.601
     * luma weights in the high half of each word, which is what folding
     * three channels into one looks like. */
    g[n++] = OP_INCR(0x400, 12);
    g[n++] = 0x00000001;
    g[n++] = 0x004b0000;
    g[n++] = 0x00930000;
    g[n++] = 0x00220000;
    g[n++] = 0x2ff01000; g[n++] = 0x2ff01000;
    g[n++] = 0x2ff01000; g[n++] = 0x2ff01000;
    g[n++] = 0x00030000; g[n++] = 0x00000000;
    g[n++] = 0x00020000; g[n++] = 0x00000000;

    /* The stock has the constant 0x85001000 in the first word here; the
     * statistics buffer's address in its place works at both widths. */
    g[n++] = OP_INCR(0x800, 3);
    g[n++] = stats_iova; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_INCR(0x820, 3);
    g[n++] = stats_iova; g[n++] = 0; g[n++] = 0;

    /* The 720 capture's real histogram windows: word 0 is 0x1d and the
     * four window words describe the 1280x720 frame -- the zeros-and-
     * 0x70000 tail was the 8x8 warm-up's values, never the running
     * configuration. */
    /* --no-x930 leaves this block out: no stock session at either size
     * writes it (impl-1), the words came from a shadow read of one 720
     * session, and a scene with a window in it -- high dynamic range --
     * is where our output turns to saturated contours. */
    if (!no_x930) {
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
    }

    g[n++] = OP_INCR(0xC00, 3);
    g[n++] = 0x00000101; g[n++] = 0x00000000; g[n++] = 0x00100000;

    /* The input stage: dimensions, then the enable, then stride and format. */
    /* 0x203 and 0x204 carry 0x00780078 in the driver for this sensor and
     * 0x02000200 for the other -- a pair of sixteen-bit fields that have
     * nothing to do with either sensor's dimensions, so what they mean
     * is a guess. Both they and the pipeline mode in 0x200 are knobs:
     * the April notes say a non-zero mode is what takes the block out of
     * minimal processing, and minimal processing is exactly what we
     * measure -- the mosaic arrives at the output intact. */
    g[n++] = OP_INCR(0x202, 3);
    g[n++] = 0x00000001; g[n++] = geo->in_dims; g[n++] = geo->in_dims;
    g[n++] = OP_INCR(0x200, 2);
    g[n++] = 0x00000001; g[n++] = 0x00000000;
    /* The last word of this block is the one place in the driver where
     * the two sensors differ in a way that looks like Bayer order:
     * 0x3333 for the rear camera, zero for this one -- and the two
     * sensors are RGGB and BGGR. A zero there could as easily mean "no
     * mosaic", which would explain a demosaic stage that never runs. */
    g[n++] = OP_INCR(0x205, 4);
    g[n++] = 0x00000000; g[n++] = 0x000600c8;
    g[n++] = 0x000f000f; g[n++] = geo->in_phase;

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
    /* The shift fields, and ours were a step too high. Injected into the
     * stock camera in place of its own they made its picture brighter --
     * 137.9 against 124.6 by measurement, and visibly so -- which is what
     * one extra step of scaling does to every demosaic coefficient. */
    g[n++] = OP_INCR(0x904, 2);
    g[n++] = 0x00004444; g[n++] = 0x00000001;
    g[n++] = OP_INCR(0x908, 1); g[n++] = 0x00004334;

    /* Three of these words are addresses in the stock process's space -- a
     * base at 0x10000000 and two windows into it. They stay verbatim: the
     * working recipe carries them as the capture had them. */
    g[n++] = OP_INCR(0x920, 10);
    g[n++] = 0x00000002;
    g[n++] = 0x10001660;
    g[n++] = 0x00000000;
    g[n++] = 0x1000f4a0;
    g[n++] = 0x0000fa80;
    g[n++] = 0x10000000;
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

    /* Demosaic: the tool's own coefficients; the stock's follow in the
     * table below and stand over these. */
    g[n++] = OP_INCR(0x506, 9);
    g[n++] = 0x3f3fcff3; g[n++] = 0x00000000;
    g[n++] = 0x04c1304c; g[n++] = 0x08220882;
    g[n++] = 0x00000000; g[n++] = 0x03d0f43d;
    g[n++] = 0x08621886; g[n++] = 0x01204812;
    g[n++] = 0x06e1b86e;

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
    g[n++] = 0x3fff0000; g[n++] = 0x3fff0000;
    g[n++] = 0x3fff0000; g[n++] = 0x10001000;

    /* One, which is what the stock camera has here. We were sending three,
     * and writing coefficients into a stage set up differently from the way
     * their owner set it up is a fair account of a block that stalls
     * part-way through the stream rather than faulting on anything. */
    g[n++] = OP_INCR(0x650, 1); g[n++] = 0x00000001;
    g[n++] = OP_INCR(0x651, 1); g[n++] = 0x00000000;


    memcpy(&g[n], isp_b_cal_data, words * 4);
    cal_fit_width(&g[n], words);
    n += words;
    /* The blob ends with 0x053 and 0x054 -- the work buffer's enable and its
     * address. The driver patches a zero into the address and calls that
     * what stock does, but the stock streaming trace carries a real pointer
     * there. A pipeline handed a null scratch buffer has every reason to
     * fall back to the least it can do, which is what we see. */
    g[n - 2] = 0x00000001;                /* 0x053 */
    g[n - 1] = work_iova;                 /* 0x054 */

    /* And then the real thing, last, so it stands over everything above:
     * the configuration read out of the stock camera while it was running.
     * The demosaic coefficients live here and nowhere else we could reach. */
    n = isp_stock_emit(g, n, work_iova);

    /* No enable here: stock writes it exactly once for a whole session,
     * inside the first warm-up frame, after everything else is configured. */

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
    g[n++] = OP_NONINCR(0x00C, 1); g[n++] = 0x0F;
    g[n++] = OP_IMM(0, sp);

    nvmap_rw(cmd_h, 0, g, n * 4, 1);
    gather_log("isp-opening", g, (unsigned)n);
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
    printf("ISP calibration: %u words, rc=%d (%s)\n",
           n, rc, rc == 0 ? "ok" : strerror(errno));
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    (void)work_h;
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
    gather_log("demosaic", g, (unsigned)n);

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
static int stock_stats_addr = 0;   /* --stock-stats-addr: the stock's literal 0x85001000 in 0x800/0x820 */

int isp_real_pass(int isp_fd, uint32_t sp, uint32_t work_iova, uint32_t stats_iova,
                  unsigned W)
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
        if ((blk[b].m == 0x800 || blk[b].m == 0x820) && stats_iova && !stock_stats_addr)
            g[first] = stats_iova;

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
        /* --blc-tail: the low halves of words 9 and 11 of 0x400 -- 0x3f in
         * every stock capture, the only subtract-shaped constants in the
         * set (63 = 64 - 1) while the sensor's pedestal measures ~16. A
         * sweep on a dark scene says whether they are the black level. */
        if (blk[b].m == 0x400 && blc_tail >= 0) {
            g[first + 9]  = (g[first + 9]  & 0xffff0000u) | (uint32_t)blc_tail;
            g[first + 11] = (g[first + 11] & 0xffff0000u) | (uint32_t)blc_tail;
        }
        /* --x400-word=IDX,HEX: one whole word of the 0x400 block replaced,
         * for sweeping the other subtract-shaped constants there (word 8
         * low half 0x10 = the sensor's pedestal, word 10 low half 0x2c). */
        if (blk[b].m == 0x400 && x400_idx >= 0 && x400_idx < 12)
            g[first + x400_idx] = x400_val;
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
           w720 ? "720p" : "2592", stock_stats_addr || !stats_iova ? "the stock's 0x85001000" : "ours",
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
    {
        unsigned w8 = n;
        g[n++] = geo->p400_w8;
        g[n++] = blc_tail >= 0 ? (geo->p400_w9 & 0xffff0000u) | (uint32_t)blc_tail : geo->p400_w9;
        g[n++] = geo->p400_w10;
        g[n++] = blc_tail >= 0 ? (geo->p400_w11 & 0xffff0000u) | (uint32_t)blc_tail : geo->p400_w11;
        if (x400_idx >= 8 && x400_idx < 12) g[w8 + x400_idx - 8] = x400_val;
    }

    g[n++] = OP_INCR(0x600, 16);
    g[n++] = 0x00000005; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0xf9500800;
    g[n++] = 0x0000fec0; g[n++] = 0x096004c0;
    g[n++] = 0x000001d0; g[n++] = 0xfac0fd50;
    g[n++] = 0x00000800; g[n++] = 0x00000000;
    g[n++] = 0x3fff0000; g[n++] = 0x3fff0000;
    g[n++] = 0x3fff0000; g[n++] = 0x10001000;
    g[n++] = OP_INCR(0x053, 2); g[n++] = 1; g[n++] = work_iova;
    g[n++] = OP_IMM(0, sp);

    nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);
    gather_log("colour", g, (unsigned)n);

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

/* Run-level verdict state: rounds in which the block never wrote, whether
 * the stop was taken, and where the sequencing counter stood when the run
 * began. */
static int isp_nowrite = 0, isp_stop_acked = -1;
static uint32_t isp_seq_base = 0;
/* Off by default: with both halves in, the ISP wrote nothing at 720p
 * (2026-09-06, twice: 15:10 and 15:13, parser 0x30/0xb4, first warm-up
 * never written), while either half alone changed nothing visible. The
 * stock writes these zeros before its sensor streams; we write them with
 * the wire already live, which is not the same experiment. */
static int ping_only = 0, syncpts_only = 0, stock_zero = 0;
static int stock_opening = 0, declare_conds = 1;

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
        } else if (so->kind == 1 && g[i] == OP_INCR(0x100, 4) && i + 1 < so->n) {
            rel[nrel].cmdbuf_mem = cmd_h; rel[nrel].cmdbuf_offset = (i + 1) * 4;
            rel[nrel].target = stats_h; rel[nrel].target_offset = 0; nrel++;
            g[i + 1] = 0;
        } else if (so->kind == 0 && g[i] == 0x85001000 && stats_iova && !stock_stats_addr) {
            g[i] = stats_iova; swapped++;
        }
    }
    g[n++] = OP_IMM(0, sp);
    nvmap_rw(cmd_h, 0, g, n * 4, 1);
    char what[32]; snprintf(what, sizeof what, "stock-opening-%u", idx);
    gather_log(what, g, n);

    struct nvhost_cmdbuf cb = { cmd_h, 0, n };
    struct nvhost_syncpt_incr si[4] = { { sp, 1 }, { sp_mem, 1 }, { sp_mem + 1, 1 }, { sp_mem + 3, 1 } };
    unsigned nsi = (so->kind == 1 && declare_conds) ? 4 : 1;
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

int isp_stop(int isp_fd, uint32_t sp)
{
    uint32_t cmd_h = nvmap_create(4096);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    /* No 0x00C=0: the stock camera never writes a zero trigger, and a write
     * the hardware has no meaning for is one more thing to differ on. */
    uint32_t g[8];
    int n = 0;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    g[n++] = OP_INCR(0x015, 1); g[n++] = 0x00000000;
    g[n++] = OP_IMM(0, sp);
    nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);
    gather_log("stop", g, (unsigned)n);

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
    return syncpt_read(sp) != was;
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
                     uint32_t sp, uint32_t hold_sp, uint32_t hold_at,
                     uint32_t park_mem, uint32_t park_stats,
                     uint32_t work_iova, int per_frame_cal)
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
        cal_fit_width(&g[n], cal_words);
        n += cal_words;
        g[n - 2] = 0x00000001;
        g[n - 1] = work_iova;
    }

    /* 0x053/0x054 every frame, as the stock's rounds carry them -- but the
     * second word is NOT the work buffer's address. The stock's steady state
     * has a per-resolution constant there (odd at 720p, so no pointer) and
     * never relocates it; with our pointer there the 2592 pass stopped at
     * ~46% of the frame and the channel died, with the stock's own literal
     * at 31%, and with zero the frame completes and the channel lives.
     * 720p is indifferent. Zero it is, until the register's meaning is
     * known; --work-word=HEX puts something else there for experiments. */
    g[n++] = OP_INCR(0x053, 2);
    g[n++] = 0x00000001; g[n++] = work_word_iova ? work_iova : work_word(0);

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
        uint32_t want_mem = park_mem ? park_mem : syncpt_read(sp_mem) + 1 + hold_at;
        g[n++] = OP_SETCLASS(HOST1X_CLASS_ID);
        g[n++] = OP_INCR(HOST1X_WAIT_SYNCPT, 1);
        g[n++] = (sp_mem << 24) | (want_mem & 0xFFFFFF);
        /* The statistics counter (37): the stream does not park on it -- a
         * black scene left it unmet once and the parked job took the
         * channel down -- because a flush job follows every stream frame.
         * The single capture is different: the stop job (0x015 = 0) comes
         * right behind it, and with the job retired at the output counter
         * alone the stop lands while the ISP is still writing chroma and
         * statistics: half-empty buffers, a memory fault at exit, garbage.
         * So the single capture keeps the stats wait (hold_sp != 0). */
        if (sp_stats && hold_sp) {
            uint32_t want_stats = park_stats ? park_stats : syncpt_read(sp_stats) + 1 + hold_at;
            g[n++] = OP_SETCLASS(HOST1X_CLASS_ID);
            g[n++] = OP_INCR(HOST1X_WAIT_SYNCPT, 1);
            g[n++] = (sp_stats << 24) | (want_stats & 0xFFFFFF);
        }
    }
    (void)hold_at;

    nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);
    gather_log("frame", g, (unsigned)n);
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

/* --stream=N: N frames in a row by the stock's protocol, after the warm-ups
 * and the coefficient/colour/real passes have gone out. Per frame:
 *   1. the ISP frame job (isp_frame: config, trigger 0x05, the channel
 *      counter's increment, then the parking waits on the frame's own
 *      output and statistics counters -- the stock's two gathers in one);
 *   2. a VI gather on the VI channel: WAIT on the ISP channel's counter for
 *      that job, the parser's single-shot command, the single-shot itself,
 *      and the frame-start increment -- so the shot is fired by host1x the
 *      moment the ISP is armed, with no CPU in between;
 *   3. the CPU waits for the output counter, notes the timing and the parser
 *      status, and reads the frame out.
 * In the single-shot loop the output of a job at 2592 wide arrived only when
 * the next job was submitted; here the next job is always queued right
 * behind, parked on the previous frame's completion.
 * Two buffer sets: job k writes outs[k & 1] / stats[k & 1], so the job
 * queued behind it never writes into memory the running one still owns and
 * the CPU reads frame k while frame k+1 lands elsewhere. The last frame is
 * copied into outs[0] at the end so the ordinary dump sees it. */
static int stream_run(int isp_fd, int vi_fd, uint32_t base,
                      uint32_t sp_id, uint32_t sp_mem, uint32_t sp_stats,
                      uint32_t sp_loadv, uint32_t isp_sp,
                      const uint32_t outs[2], const uint32_t stats[2],
                      unsigned W, unsigned OH,
                      uint32_t isp_fmt, uint32_t u_off, uint32_t v_off,
                      uint32_t work_iova,
                      uint32_t iova, uint32_t stride, uint32_t out_bytes,
                      int n_frames)
{
    /* A gather on the VI channel (class 0x30 through host1x) hung the
     * device outright on its first try, so the VI is armed through its
     * registers here, as in the single-shot loop. What is kept of the
     * stock's protocol is the queue: the next ISP job is submitted while
     * the current frame is still running, parked on the frame after it. */
    (void)vi_fd;
    vi_wr(base + VI_CSI_SURFACE0_OFFSET_MSB, 0);
    vi_wr(base + VI_CSI_SURFACE0_OFFSET_LSB, iova);
    vi_wr(base + VI_CSI_SURFACE0_STRIDE, stride);
    vi_flush("stream: VI surface");

    uint32_t pp_reg = PP_B_PIXEL_STREAM_PP_COMMAND;
    uint32_t pp_cmd = (0xFu << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
                      CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_ENABLE;
    uint32_t fs_cond = T124_PPB_FRAME_START;
    uint8_t *img = malloc(out_bytes);
    int whole = 0;

    isp_job_timeout_ms = 3000;
    /* Every frame moves 36 and 37 by exactly one, so job k parks on
     * start + k + 1 -- counted, not read back, see isp_frame. */
    uint32_t start36 = syncpt_read(sp_mem), start37 = syncpt_read(sp_stats);
    /* And the channel's own counter: job k has been armed -- its
     * configuration loaded and its trigger given -- once it reads
     * start38 + k + 1. A shot before that hands the frame to an ISP that
     * is not yet taking lines; that was the miss behind every "2 shots". */
    uint32_t start38 = syncpt_read(isp_sp);
    /* The stock arms VI condition 0xf onto syncpoint 46 (vi1_ispb) with
     * every shot and waits on it before the next: one increment per frame,
     * at the parser's frame end (impl-2's reading). Armed here to measure
     * where that edge falls against the ISP's output-done and the shot. */
    uint32_t start46 = syncpt_read(VI1_ISPB_SYNCPT);
    /* Job 0 alone in the queue: it parks on the next frame. */
    int rc = isp_frame(isp_fd, outs[0], stats[0], W, OH, isp_fmt, u_off, v_off,
                       sp_mem, sp_stats, sp_loadv, isp_sp, 0, 0,
                       start36 + 1, start37 + 1,
                       work_iova, per_frame_cal);
    printf("  stream: job 0 armed, rc=%d\n", rc);

    for (int k = 0; k < n_frames; k++) {
        uint32_t base36 = syncpt_read(sp_mem);
        uint32_t base_fs = syncpt_read(sp_id);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        /* Shoot frame k only once job k is armed -- and, past the first
         * frame, only after --shot-delay: the previous output-done falls
         * while the sensor is already inside the next frame, so a shot
         * right then takes a partial one (2 shots, 200 ms per frame). */
        if (k > 0 && shot_delay_ms > 0) usleep((useconds_t)shot_delay_ms * 1000);
        {
            int wa = 0;
            while ((int32_t)(syncpt_read(isp_sp) - (start38 + (uint32_t)k + 1)) < 0 && wa < 500) {
                usleep(1000); wa++;
            }
            if (wa >= 500) printf("  stream: job %d not armed within 500 ms\n", k);
        }
        vi_wr(pp_reg, pp_cmd);
        vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, (fs_cond << 8) | sp_id);
        vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, (0x0fu << 8) | VI1_ISPB_SYNCPT);
        vi_wr(base + VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);
        vi_flush(0);
        long fe_us = -1;                     /* when 46 moved, from the shot */

        /* Queue job k+1 right behind, parked on frame k+1 (depth 1). Its
         * own counter moves only once frame k is done and it is armed, and
         * isp_frame waits for that -- so when the call returns frame k is
         * complete without a single CPU poll on 36. */
        int w_out = 0;
        if (k + 1 < n_frames) {
            rc = isp_frame(isp_fd, outs[(k + 1) & 1], stats[(k + 1) & 1],
                           W, OH, isp_fmt, u_off, v_off,
                           sp_mem, sp_stats, sp_loadv, isp_sp, 0, 1,
                           start36 + (uint32_t)k + 2, start37 + (uint32_t)k + 2,
                           work_iova, per_frame_cal);
        } else {
            /* The last frame's output lands only with a job queued behind
             * it -- every stream so far died on its last frame and nowhere
             * else. An 8x8 job with no parking of its own stands behind it,
             * writing into the buffer set the last frame does not use: that
             * set stays mapped until exit, and the job does take a frame if
             * a retrigger shot follows it (a freed 64 KB buffer here drew a
             * burst of SMMU faults). */
            rc = isp_warmup(isp_fd, isp_sp, outs[(k + 1) & 1], stats[(k + 1) & 1], 0, W, OH);
        }
        int shots = 1;
        while (syncpt_read(sp_mem) == base36 && w_out < isp_wait_ms) {
            usleep(1000); w_out++;
            if (fe_us < 0 && syncpt_read(VI1_ISPB_SYNCPT) != start46 + (uint32_t)k) {
                struct timespec tf; clock_gettime(CLOCK_MONOTONIC, &tf);
                fe_us = (tf.tv_sec - t0.tv_sec) * 1000000L + (tf.tv_nsec - t0.tv_nsec) / 1000;
            }
            /* No output two periods after the shot: the shot fell inside a
             * frame, the parser handed the ISP a partial one and the ISP is
             * holding it. Fire again -- the job stays parked -- so the next,
             * whole frame reaches it. The stock never has this problem
             * because its hardware parking fires every shot at a frame
             * boundary. Three extra shots at most. */
            if (w_out % 100 == 0 && shots < 4) {   /* 1.5 periods at 15 fps: the phase steps by half a frame */
                vi_wr(pp_reg, pp_cmd);
                vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, (fs_cond << 8) | sp_id);
                vi_wr(base + VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);
                vi_flush(0);
                shots++;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        int got = syncpt_read(sp_mem) != base36;
        uint32_t parser = vi_rd(T124_PP_B_PIXEL_PARSER_STATUS);
        printf("  stream frame %d: frame start %s, output %s (%ld ms, %d shot%s),"
               " frame end (46) %s%ld ms 46=%u/%u,"
               " parser %08x, 36=%u/%u 37=%u/%u 38=%u, next job rc=%d\n",
               k, syncpt_read(sp_id) != base_fs ? "seen" : "NOT seen",
               got ? "done" : "NONE",
               (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000,
               shots, shots == 1 ? "" : "s",
               fe_us < 0 ? "not seen " : "at ", fe_us < 0 ? 0L : fe_us / 1000,
               syncpt_read(VI1_ISPB_SYNCPT) - start46, (unsigned)k + 1,
               parser,
               syncpt_read(sp_mem) - start36, (unsigned)k + 1,
               syncpt_read(sp_stats) - start37, (unsigned)k + 1,
               syncpt_read(isp_sp), rc);
        if (!got) {
            printf("  stream: no output for frame %d, stopping\n", k);
            break;
        }
        whole++;
        if (k == 0) {
            /* The first shot lands at a random phase of the sensor and the
             * parser takes a frame without its start; the error bits it
             * accumulates are sticky. Clear them here, once, so what the
             * aligned frames that follow add is visible on its own. */
            vi_wr(T124_PP_B_PIXEL_PARSER_STATUS, 0xFFFFFFFF);
            vi_flush(0);
        }
        /* Frame k, out of its own buffer, while frame k+1 lands in the other. */
        if (n_frames <= 6 && nvmap_rw(outs[k & 1], 0, img, out_bytes, 0) == 0) {
            char path[64];
            snprintf(path, sizeof path, "/data/local/tmp/stream_%02d.raw", k);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(img, 1, out_bytes, f); fclose(f); }
        }
    }
    printf("stream: %d of %d frames produced output\n", whole, n_frames);
    isp_job_timeout_ms = 10000;
    /* The last frame into outs[0], where the run's dump and readback look. */
    if (whole > 0 && ((whole - 1) & 1) && outs[1] != outs[0]
        && nvmap_rw(outs[1], 0, img, out_bytes, 0) == 0)
        nvmap_rw(outs[0], 0, img, out_bytes, 1);
    free(img);
    return whole == n_frames ? 0 : -1;
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
    int use_sensor = 1, dump = 0;
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
    /* One frame. Every extra one queues another job behind a block that may
     * already be stuck, and when it is, the channel dies and only a reboot
     * brings the camera back -- so the cost of asking for more is paid by
     * hand, every time. Two is the ceiling; anything larger is clamped. */
    int shots = 1;
    int hold = 0, dump_regs = 0;
    int pp_trace_ms = 0;    /* --pp-trace=MS: log the parser status bit 5 edges before any shot */
    uint32_t seq_sp = 0;    /* --seq-sp: the syncpoint our own jobs ride on */
    int settle = 200;
    int fast_arm = 1;   /* arm the next ISP frame right after the last completes; --slow-arm paces by the frame period instead */
    uint32_t isp_clk = 384000000;   /* --isp-clk=HZ */
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
        else if (strcmp(a, "--no-sensor") == 0)   use_sensor = 0;
        else if (strcmp(a, "--dump") == 0)        dump = 1;
        else if (strncmp(a, "--frame-length=", 15) == 0)
            frame_length = (uint32_t)strtoul(a + 15, 0, 0);
        else if (strncmp(a, "--coarse=", 9) == 0)
            coarse_time = (uint32_t)strtoul(a + 9, 0, 0);
        else if (strncmp(a, "--hold=", 7) == 0)   hold = atoi(a + 7);
        else if (strcmp(a, "--dump-regs") == 0)   dump_regs = 1;
        else if (strncmp(a, "--pp-trace=", 11) == 0) pp_trace_ms = atoi(a + 11);
        else if (strncmp(a, "--shot-delay=", 13) == 0) shot_delay_ms = atoi(a + 13);
        else if (strcmp(a, "--emc-pin") == 0) emc_pin = 1;
        else if (strcmp(a, "--kernel-csi") == 0) kernel_csi = 1;
        else if (strcmp(a, "--no-x930") == 0) no_x930 = 1;
        else if (strcmp(a, "--release-csib") == 0) release_csib = 1;
        else if (strcmp(a, "--no-stock-zero") == 0) stock_zero = 0;
        else if (strcmp(a, "--stock-stats-addr") == 0) stock_stats_addr = 1;
        else if (strcmp(a, "--cal-2592") == 0) cal_2592 = 1;
        else if (strcmp(a, "--stock-opening") == 0) stock_opening = 1;
        else if (strcmp(a, "--no-declare-conds") == 0) declare_conds = 0;
        else if (strncmp(a, "--stock-zero=", 13) == 0) stock_zero = (int)strtoul(a + 13, 0, 0);
        else if (strcmp(a, "--ping") == 0) ping_only = 1;
        else if (strcmp(a, "--syncpts") == 0) syncpts_only = 1;
        else if (strncmp(a, "--blc-tail=", 11) == 0) blc_tail = (int)strtol(a + 11, 0, 16);
        else if (strncmp(a, "--x400-word=", 12) == 0) {
            x400_idx = atoi(a + 12);
            const char *comma = strchr(a + 12, ',');
            x400_val = comma ? (uint32_t)strtoul(comma + 1, 0, 16) : 0;
        }
        else if (strcmp(a, "--no-emc-bw") == 0) no_emc_bw = 1;
        else if (strcmp(a, "--no-set-emc") == 0) no_set_emc = 1;
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
        else if (strncmp(a, "--settle=", 9) == 0) settle = atoi(a + 9);
        else if (strcmp(a, "--slow-arm") == 0)    fast_arm = 0;
        else if (strncmp(a, "--isp-clk=", 10) == 0) isp_clk = (uint32_t)strtoul(a + 10, 0, 0);
        else if (strncmp(a, "--emc-bw=", 9) == 0) emc_bw = strtoul(a + 9, 0, 0);
        else if (strncmp(a, "--isp-wait=", 11) == 0) isp_wait_ms = (int)strtoul(a + 11, 0, 0);
        else if (strncmp(a, "--stream=", 9) == 0) stream_n = (int)strtoul(a + 9, 0, 0);
        else if (strncmp(a, "--isp-emc-clk=", 14) == 0) isp_emc_clk = (unsigned)strtoul(a + 14, 0, 0);
        else if (strcmp(a, "--stock-vi") == 0)    stock_vi = 7;
        else if (strcmp(a, "--work-word=iova") == 0) work_word_iova = 1;   /* the old behaviour: our work buffer's address per frame */
        else if (strncmp(a, "--work-word=", 12) == 0)
            { work_word_override = (uint32_t)strtoul(a + 12, 0, 16); work_word_set = 1; }
        else if (strncmp(a, "--stock-groups=", 15) == 0)
            stock_groups = (unsigned)strtoul(a + 15, 0, 0);
        else if (strncmp(a, "--stock-vi=", 11) == 0)
            /* bit 0: DVFS + SINGLE_SHOT_STATE_UPDATE; bit 1: the parser
             * words; bit 2: PHY_CIL_COMMAND E-only and no CILC pad. */
            stock_vi = (int)strtoul(a + 11, 0, 0);
        else if (strcmp(a, "--no-isp") == 0)      no_isp = 1;
        else if (strncmp(a, "--seq-sp=", 9) == 0)  seq_sp = (uint32_t)atoi(a + 9);
        else if (strncmp(a, "--wb=", 5) == 0) {     /* --wb=R,B in hex 4.12 */
            wb_r = (uint32_t)strtoul(a + 5, 0, 16);
            const char *comma = strchr(a + 5, ',');
            if (comma) wb_b = (uint32_t)strtoul(comma + 1, 0, 16);
        }
        else if (strncmp(a, "--ccm=", 6) == 0)    ccm = atoi(a + 6);
        else if (strncmp(a, "--stats-kb=", 11) == 0)
            stats_kb = (unsigned)strtoul(a + 11, 0, 0);
        else if (strncmp(a, "--work-kb=", 10) == 0)
            work_kb = (unsigned)strtoul(a + 10, 0, 0);
        else if (strncmp(a, "--gain=", 7) == 0)
            gain = (uint32_t)strtoul(a + 7, 0, 0);
        else { printf("unknown option %s\n", a); return 1; }
    }

    /* --no-isp: the frame goes to memory instead of the ISP, so the receiver
     * and VI can be judged on their own. */
    if (no_isp)
        image_def = (image_def & ~(IMAGE_DEF_DEST_MEM | IMAGE_DEF_DEST_ISP_A |
                                   IMAGE_DEF_DEST_ISP_B)) | IMAGE_DEF_DEST_MEM;
    /* The resolution-dependent set, keyed to the exact size: the stock
     * camera refuses to run when these do not agree with the frame. */
    const struct geom_cfg *geo = geom_for(W, H);
    if (W != 2592 && W != 1280)
        printf("WARNING: no measured geometry for %ux%u -- using the"
               " nearest captured set\n", W, H);

    uint32_t base = VI_CSI_BASE(1);          /* port B: the front sensor */
    uint32_t stride = W * 2;                 /* RAW10 lands in 16-bit words */
    uint32_t frame = stride * H;
    uint32_t wc = W * 10 / 8;                /* core.c: width * bpp / 8 */

    if (syncpts_only) { syncpt_table(); return 0; }
    cal_fit_w = W;
    if (ping_only) printf("=== viisp --ping: is the ISP-B channel taking work? (no sensor, no VI) ===\n");
    else printf("=== viisp: ov5693 %ux%u, CSI port B ===\n", W, H);
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
    uint32_t out_h2 = 0, stats_h2 = 0;   /* --stream: the second buffer set */
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
        if (seq_sp) isp_sp = seq_sp;

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
        /* --no-emc-bw leaves this lever out, for attributing the EMC rise
         * between it and the SET_EMC reservation below: by the kernel's
         * arithmetic neither asks for more than ~10 MHz, yet with both sent
         * EMC sits on PLLM while the ISP is busy (impl-1, §2.2). */
        int crc1 = no_emc_bw ? 0 : ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &ic);
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
        int lrc = no_set_emc ? 0 : lfd < 0 ? -1 : ioctl(lfd, NVHOST_ISP_IOCTL_SET_EMC, &ei);
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
        /* --stream: a second output and statistics set. Job k+1 is queued
         * while frame k is still being written, and the CPU reads frame k
         * out while frame k+1 runs -- one buffer for both is a write into
         * memory another job still owns, which is what the SMMU fault on
         * the U plane looked like. The jobs alternate between the two. */
        if (stream_n > 0) {
            out_h2 = nvmap_create(out_bytes);
            if (!out_h2 || nvmap_alloc(out_h2) || !nvmap_pin(out_h2)) out_h2 = 0;
            stats_h2 = nvmap_create(stats_kb * 1024);
            if (!stats_h2 || nvmap_alloc(stats_h2) || !nvmap_pin(stats_h2)) stats_h2 = 0;
            if (stats_h2) {
                void *p = malloc(64 * 1024);
                memset(p, 0x3C, 64 * 1024);
                nvmap_rw(stats_h2, 0, p, 64 * 1024, 1);
                free(p);
            }
            printf("stream: second buffer set %s\n",
                   out_h2 && stats_h2 ? "allocated" : "NOT allocated -- single-buffered");
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
        if (stock_opening) {
            /* Submits 0..10: two zero passes, the shading tables and value
             * rounds, the ticks and the 0x053 job, as the stock sends them. */
            for (unsigned i = 0; i <= 10; i++)
                stock_open_submit(isp_fd, isp_sp, sp_mem, i, 0, stats_h, stats_iova);
        } else if (work_h)
            isp_init(isp_fd, work_h, isp_sp, work_iova, stats_iova, geo);
        if (out_h) {
            uint32_t chunk = 65536;
            void *p = malloc(chunk);
            memset(p, 0x5A, chunk);
            for (uint32_t o = 0; o < out_bytes; o += chunk) {
                uint32_t len = out_bytes - o < chunk ? out_bytes - o : chunk;
                nvmap_rw(out_h, o, p, len, 1);
                if (out_h2) nvmap_rw(out_h2, o, p, len, 1);
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
    if (use_sensor) {
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
        /* The stream itself starts here, before the receiver comes up (the
         * MIPI calibration needs the clock lane live); sensor_late keeps
         * the other order as code. */
        if (!sensor_late)
            sensor_start_front(sfd, W, H, frame_length, coarse_time, gain);
        else
            printf("sensor powered; streaming deferred until the receiver is up\n");
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
    if (release_csib) pmc_dpd_release_reg(PMC_IO_DPD_REQ, PMC_DPD_BIT_CSIB);
    pmc_dpd_release(PMC_DPD_BIT_CSIE);
    if (emc_pin) emc_pin_high();
    car_enable_csi_clocks();

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

    /* What the stock camera writes first, at the start of every front
     * session (impl-1, vi-state-diff-stock-vs-viisp.md): the A and B
     * bricks' pad and control words to zero -- they belong to the rear
     * sensor, and a boot leaves 7/2 in them -- and the port-B pattern
     * generator block to zero, clearing whatever a rear session left. We
     * had never touched either; every dump of ours carried the leftovers.
     * Its own batch, so the bring-up below keeps its one call. */
    /* With both in, the ISP wrote nothing at 720p (2026-09-06 15:10,
     * parser 0x30) where the same run without them gave its usual
     * picture -- so --stock-zero=MASK splits them: 1 = the A/B bricks'
     * pad and control words, 2 = the PG_B block; --no-stock-zero = 0. */
    if (!kernel_csi && (stock_zero & 1)) {
        vi_wr(T124_CILA_PAD_CONFIG0, 0);
        vi_wr(T124_PHY_CILA_CONTROL0, 0);
        vi_wr(T124_CILB_PAD_CONFIG0, 0);
        vi_wr(T124_PHY_CILB_CONTROL0, 0);
    }
    if (stock_zero & 2) {
        static const uint32_t pg_b[] = {
            0xA68, 0xA6C, 0xA70, 0xA74, 0xA78, 0xA7C, 0xA80, 0xA84, 0xA88,
            0xA9C, 0xAA0, 0xAA4, 0xAA8, 0xAAC, 0xAB0, 0xAB4, 0xAB8, 0xABC,
        };
        for (unsigned i = 0; i < sizeof pg_b / sizeof pg_b[0]; i++)
            vi_wr(pg_b[i], 0);
    }
    vi_flush("stock session-start zeroing (CILA/CILB, PG_B)");

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
    if (!(stock_vi & 4)) vi_wr(T124_CILC_PAD_CONFIG0, 0x00010000);
    if (kernel_csi) {
        /* --kernel-csi: the registers the kernel's own port-B path writes
         * and this reconstruction had left to reset defaults (impl-1,
         * csi-bringup-diff.md): the A/B pads and masks, the full PHY_CIL
         * command word, THS 0xa on CIL E, a parser reset before use,
         * CONTROL1 0x11, and the kernel's teardown at exit. */
        vi_wr(T124_CILA_PAD_CONFIG0, 0x00010000);
        vi_wr(T124_CILB_PAD_CONFIG0, 0x00000000);
    }
    vi_wr(T124_CILD_PAD_CONFIG0, 0x00000000);
    vi_wr(T124_CILE_PAD_CONFIG0, 0x00000000);

    if (kernel_csi) { vi_wr(T124_CSI_CIL_A_INT_MASK, 0x0); vi_wr(T124_CSI_CIL_B_INT_MASK, 0x0); }
    vi_wr(T124_CSI_CIL_C_INT_MASK, 0x0);
    vi_wr(T124_CSI_CIL_D_INT_MASK, 0x0);
    vi_wr(T124_CSI_CIL_E_INT_MASK, 0x0);
    vi_wr(T124_PHY_CILE_CONTROL0, kernel_csi ? 0x0000000a : 0x00000009);

    /* Reset the parser, configure it, then enable -- the command word
     * is written twice on purpose, and the single-shot bit rides along
     * both times. */
    vi_wr(T124_PP_B_PIXEL_STREAM_PP_COMMAND, 0x0000f007);
    if (kernel_csi) vi_wr(T124_PP_B_PIXEL_STREAM_PP_COMMAND, 0x0000f007);   /* the kernel resets twice */
    vi_wr(T124_PP_B_PIXEL_STREAM_PP_INT_MASK, 0x0);
    /* --stock-vi: the words the stock camera (an R19-era stack) writes
     * through its VI channel, read out of its gathers -- PAD_FRAME 0
     * where the R21.5 driver has NOPAD, CONTROL1 clear, the packet-skip
     * threshold 0x7f with two extra low bits in INPUT_STREAM_CONTROL.
     * The same words at 2592 and 1280 wide. */
    vi_wr(T124_PP_B_PIXEL_STREAM_CONTROL0, (stock_vi & 2) ? 0x080301f1 : 0x280301f1);
    vi_wr(T124_PP_B_PIXEL_STREAM_PP_COMMAND, 0x0000f005);
    vi_wr(T124_PP_B_PIXEL_STREAM_CONTROL1, kernel_csi ? 0x00000011 : (stock_vi & 2) ? 0 : 0x00000011);
    vi_wr(T124_PP_B_PIXEL_STREAM_GAP, 0x00140000);
    vi_wr(T124_PP_B_PIXEL_STREAM_EXPECTED_FRAME, 0x0);
    vi_wr(T124_PP_B_INPUT_STREAM_CONTROL, (stock_vi & 2) ? 0x007f0014 : 0x003f0000);

    /* Only the upper half of the brick command is ours; the lower half
     * belongs to the rear path and has to survive our write. The stock
     * enables brick E alone (0x10000000) and leaves C and D untouched. */
    if (kernel_csi) {
        /* Whole words, as the kernel writes them: everything off, then port
         * B on one lane -- the low half is not to be trusted after a boot. */
        vi_wr(T124_CSI_PHY_CIL_COMMAND, 0x22020202);
        vi_wr(T124_CSI_PHY_CIL_COMMAND, 0x12020202);
    } else
    vi_wr(T124_CSI_PHY_CIL_COMMAND,
          (vi_rd(T124_CSI_PHY_CIL_COMMAND) & 0x0000FFFF) |
          ((stock_vi & 4) ? 0x10000000 : 0x12020000 /* brick E, one lane */));
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
    if (stock_vi & 1) {
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

    /* Receiver and VI are configured: now the sensor may start streaming
     * (see sensor_start_front for why not earlier). */
    if (sensor_late && sfd >= 0 && use_sensor)
        sensor_start_front(sfd, W, H, frame_length, coarse_time, gain);
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

    if (pp_trace_ms > 0) {
        /* Looking for a live sign of the sensor's frame timing before any
         * shot: the parser status only moves once a single-shot is armed,
         * so sample the CIL E status words too -- between frames the lanes
         * drop to LP-11, and the lane interface may show it. Every change
         * of any of the three words is printed with its time. */
        static const uint32_t regs[3] = { 0xA18, 0xA1C, T124_PP_B_PIXEL_PARSER_STATUS };
        static const char *names[3] = { "CIL_E", "CILE", "PP_B" };
        uint32_t prev[3];
        struct timespec t0, t;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int r = 0; r < 3; r++) prev[r] = vi_rd(regs[r]);
        /* And the frame-start condition, re-armed after every increment: if
         * it ticks at the frame period with no shot armed, it is a clock. */
        uint32_t fs_prev = syncpt_read(sp_id);
        long fs_last = 0; int fs_ticks = 0;
        printf("live trace %d ms: CIL_E 0x%08x CILE 0x%08x PP_B 0x%08x, FS syncpt %u = %u",
               pp_trace_ms, prev[0], prev[1], prev[2], sp_id, fs_prev);
        int changes = 0;
        for (;;) {
            clock_gettime(CLOCK_MONOTONIC, &t);
            long us = (t.tv_sec - t0.tv_sec) * 1000000L + (t.tv_nsec - t0.tv_nsec) / 1000;
            if (us > pp_trace_ms * 1000L) break;
            for (int r = 0; r < 3; r++) {
                uint32_t v = vi_rd(regs[r]);
                if (v != prev[r]) {
                    if (changes < 60)
                        printf("%s%ld.%03ld %s %08x>%08x", changes % 3 ? "  " : "\n  ",
                               us / 1000, us % 1000, names[r], prev[r], v);
                    changes++; prev[r] = v;
                }
            }
            uint32_t fs = syncpt_read(sp_id);
            if (fs != fs_prev) {
                if (fs_ticks < 40)
                    printf("%s%ld.%03ld FS+%u (%ld us)", fs_ticks % 4 ? "  " : "\n  ",
                           us / 1000, us % 1000, fs - fs_prev, us - fs_last);
                fs_ticks++; fs_last = us; fs_prev = fs;
                vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, T124_PPB_FRAME_START << 8 | sp_id);
                vi_flush(0);
            }
            usleep(300);
        }
        printf("\n  %d register changes, %d frame-start ticks in %d ms\n",
               changes, fs_ticks, pp_trace_ms);
    }

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
        if (do_warmup && isp_fd >= 0) {
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
            /* After the first frame, in its own job, the way the
             * capture shows the stock stack doing it. */
            if (isp_fd >= 0 && out_iova && shot > 0 && !dm_sent) {
                isp_demosaic(isp_fd, isp_sp, out_h, stats_h,
                             u_off, v_off, work_iova);
                dm_sent = 1;
            }
            /* And the working configuration after the first frame, in
             * the place the capture puts it. */
            if (isp_fd >= 0 && out_iova && shot > 0 && !real_sent
                && use_real_pass) {
                isp_real_pass(isp_fd, isp_sp, work_iova, stats_iova, W);
                real_sent = 1;
            }

            /* The warm-up needs pixels, which is what running it before
             * the capture ever started was missing. So it goes here
             * instead, in place of a real frame, while the receiver is
             * armed and the sensor is delivering -- the way stock runs
             * it, between frames of a live stream. */
            if (warm_left > 0 && isp_fd >= 0 && warm_h && stock_opening) {
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
                dm_sent = 1;
            }
            else if (warm_left > 0 && isp_fd >= 0 && warm_h) {
                isp_base_mem = syncpt_read(sp_mem);
                isp_warmup(isp_fd, isp_sp, warm_h, stats_h,
                           warm_left == 2, W, OH);
                if (warm_left == 2) {
                    /* The ISP module is busy from here: its "emc" shared-bus
                     * user counts only while the module's clocks are on, so
                     * this is where the bandwidth request shows in the CAR. */
                    uint32_t src = 0;
                    mem_rd(CAR_BASE + 0x19c, &src);
                    printf("  EMC source with the ISP busy: 0x%08x%s\n", src,
                           (src >> 29) == 4 ? " (PLLM)" : (src >> 29) == 2 ? " (PLLP)" : "");
                }
                if (warm_left == 2 && !dm_sent) {
                    isp_demosaic(isp_fd, isp_sp, out_h, stats_h,
                                 u_off, v_off, work_iova);
                    dm_sent = 1;
                }
            }
            else if (isp_fd >= 0 && out_iova && stats_h && stream_n > 0) {
                /* The real frames go out by the stock's protocol instead
                 * of this single-shot machinery. */
                uint32_t outs[2] = { out_h, out_h2 ? out_h2 : out_h };
                uint32_t stats2[2] = { stats_h, stats_h2 ? stats_h2 : stats_h };
                stream_run(isp_fd, vi_fd, base, sp_id, sp_mem,
                           sp_stats, sp_loadv, isp_sp, outs, stats2,
                           W, OH, isp_fmt, u_off, v_off, work_iova,
                           iova, stride, out_bytes, stream_n);
                goto after_shots;
            }
            else if (isp_fd >= 0 && out_iova && stats_h) {
                isp_base_mem = syncpt_read(sp_mem);
                isp_frame(isp_fd, out_h, stats_h, W, OH, isp_fmt, u_off, v_off,
                          sp_mem, sp_stats, sp_loadv, isp_sp, 0, 0, 0, 0,
                          work_iova, per_frame_cal);
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
                vi_wr(T124_PHY_CILE_CONTROL0, kernel_csi ? 0x0000000a : 0x00000009);
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
                if (!fast_arm) usleep(20000);
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
            if (!fast_arm) usleep((useconds_t)period * 500 + settle * 1000);
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
                    if (stock_opening)
                        stock_open_submit(isp_fd, isp_sp, sp_mem, 15, warm_h, stats_h, stats_iova);
                    if (ccm && !stock_opening) isp_colour(isp_fd, isp_sp, work_iova, W, OH);
                    if (use_real_pass && !real_sent) {
                        isp_real_pass(isp_fd, isp_sp, work_iova, stats_iova, W);
                        real_sent = 1;
                    }
                }
                shot--;
            }
        }
    after_shots:
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
        if (kernel_csi) {
            /* The kernel's stop: parser disabled, clock gating on, the
             * channel reset fully, CIL E switched off. */
            vi_wr(pp, 0x0000f002);
            vi_wr(TEGRA_VI_CFG_CG_CTRL, 1);
            vi_wr(base + VI_CSI_SW_RESET, 0x1F);
            vi_wr(base + VI_CSI_SW_RESET, 0x0);
            vi_wr(T124_CSI_PHY_CIL_COMMAND, 0x22020201);
            vi_flush("capture stopped (kernel form)");
        } else {
        vi_wr(pp, 0);
        vi_wr(base + VI_CSI_SINGLE_SHOT, 0);
        vi_wr(base + VI_CSI_SW_RESET, 0xF);
        vi_wr(base + VI_CSI_SW_RESET, 0x0);
        /* And the stock's own teardown, which ours had left out: the
         * routing cleared and brick E switched off, so the next session
         * -- ours or the stock's -- starts from what the stock leaves,
         * not from our streaming state (impl-1's diff, rows 0x20c and
         * 0x908). The low half of the brick command belongs to the rear
         * path and survives. */
        vi_wr(base + VI_CSI_IMAGE_DEF, 0);
        vi_wr(T124_CSI_PHY_CIL_COMMAND,
              (vi_rd(T124_CSI_PHY_CIL_COMMAND) & 0x0000FFFF) | 0x20000000);
        vi_flush("capture stopped");
        }
        if (isp_fd >= 0 && out_iova) isp_stop_acked = isp_stop(isp_fd, isp_sp);

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
    emc_unpin();
    /* The line the wrapper reads. A job still owed on the sequencing
     * counter, or a stop the block never took, is a dead channel: the
     * kernel will print its timeout within isp_job_timeout_ms, and nothing
     * this run produced is evidence of anything. */
    int rc_final = 0;
    if (isp_fd >= 0 && isp_sp) {
        uint32_t mn = syncpt_read(isp_sp), mx = syncpt_read_max(isp_sp);
        unsigned owed = mx - mn, done = mn - isp_seq_base;
        if (owed == 0 && isp_stop_acked != 0) {
            printf("ISP VERDICT: ALIVE -- counter %u: %u job(s) this run, all retired%s%s\n",
                   isp_sp, done,
                   isp_stop_acked > 0 ? ", stop acknowledged" : "",
                   isp_nowrite ? " -- BUT the block produced no write in some round(s), see above" : "");
        } else {
            printf("ISP VERDICT: DEAD -- counter %u value %u promised %u: %u job(s) still owed%s;"
                   " the kernel times the channel out within %d s;"
                   " THIS RUN IS NOT EVIDENCE -- reboot before the next\n",
                   isp_sp, mn, mx, owed,
                   isp_stop_acked == 0 ? ", stop never acknowledged" : "",
                   isp_job_timeout_ms / 1000 + 1);
            rc_final = 3;
        }
        if (isp_nowrite)
            printf("ISP NOTE: %d round(s) ended without the ISP's output write\n", isp_nowrite);
    }
    printf("=== done ===\n");
    return rc_final;
}
