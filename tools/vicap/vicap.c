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

/* The receiver's own clocks. Reading the clock controller shows csi (bit
 * 20 of the H group) and cile (bit 18 of W) switched off, while vi_sensor
 * is on -- so the registers accept everything, because nvhost powers VI for
 * the duration of an ioctl, and the lane interface still cannot receive.
 * Enabling them from outside does not hold: the kernel gates them again
 * during the run. So they go on here, in the same window as the shot.
 * Numbers from the stock kernel's clock table; the set registers only set
 * bits, which is why they are used rather than a read-modify-write. */
#define CAR_BASE            0x60006000UL
#define CAR_ENB_SET_H       0x328
#define CAR_ENB_SET_W       0x448
#define CAR_CSI_BIT_H       (1u << 20)
#define CAR_CILE_BIT_W      (1u << 18)
#define CAR_ENB_SET_X       0x284
#define CAR_MIPICAL_BIT_H   (1u << 24)
#define CAR_CLK72M_BIT_X    (1u << 17)
#define CAR_ENB_SET_L       0x320
#define CAR_RST_CLR_L       0x304
#define CAR_VI_BIT_L        (1u << 20)
#define CAR_RST_CLR_H       0x30C
#define CAR_RST_CLR_W       0x43C

static int mem_wr(unsigned long addr, uint32_t val, uint32_t *before)
{
    long page = sysconf(_SC_PAGESIZE);
    unsigned long base = addr & ~(unsigned long)(page - 1);
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return -1;
    void *m = mmap(0, (size_t)page * 2, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, (off_t)base);
    if (m == MAP_FAILED) { close(fd); return -1; }
    volatile uint32_t *r = (volatile uint32_t *)((char *)m + (addr - base));
    if (before) *before = *r;
    *r = val;
    __sync_synchronize();
    munmap(m, (size_t)page * 2);
    close(fd);
    return 0;
}

static int mem_rd(unsigned long addr, uint32_t *out)
{
    long page = sysconf(_SC_PAGESIZE);
    unsigned long base = addr & ~(unsigned long)(page - 1);
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) return -1;
    void *m = mmap(0, (size_t)page * 2, PROT_READ, MAP_SHARED, fd, (off_t)base);
    if (m == MAP_FAILED) { close(fd); return -1; }
    *out = *(volatile uint32_t *)((char *)m + (addr - base));
    munmap(m, (size_t)page * 2);
    close(fd);
    return 0;
}

static void car_enable_csi_clocks(void)
{
    uint32_t b1 = 0, b2 = 0, b3 = 0;
    /* csi and cile for the receiver; mipi-cal and clk72mhz for the
     * calibration block, which cannot finish without them -- it reported
     * "not done" while both of those were switched off. */
    /* VI itself, which we had never switched on: its clock reads off in
     * the L group. Registers answer regardless, because nvhost powers the
     * module for the length of an ioctl -- but the parser and the write
     * engine need the clock to actually run. */
    mem_wr(CAR_BASE + CAR_ENB_SET_L, CAR_VI_BIT_L, 0);
    mem_wr(CAR_BASE + CAR_RST_CLR_L, CAR_VI_BIT_L, 0);

    mem_wr(CAR_BASE + CAR_ENB_SET_H, CAR_CSI_BIT_H | CAR_MIPICAL_BIT_H, &b1);
    mem_wr(CAR_BASE + CAR_ENB_SET_W, CAR_CILE_BIT_W, &b2);
    mem_wr(CAR_BASE + CAR_ENB_SET_X, CAR_CLK72M_BIT_X, &b3);

    /* Enabling a clock is only half of what the kernel's helper does: it
     * also takes the block out of reset. A module left in reset accepts
     * register writes and does nothing, without complaining -- which is
     * what a calibration that starts and never finishes looks like. */
    mem_wr(CAR_BASE + CAR_RST_CLR_H, CAR_CSI_BIT_H | CAR_MIPICAL_BIT_H, 0);
    mem_wr(CAR_BASE + CAR_RST_CLR_W, CAR_CILE_BIT_W, 0);

    printf("  receiver clocks on and out of reset: H 0x%08x, W 0x%08x, X 0x%08x\n",
           b1, b2, b3);
}

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
#define MIPI_CAL_BASE       0x700E3000UL
#define MIPI_CAL_CTRL       0x00
#define MIPI_CAL_STATUS     0x08
#define MIPI_CAL_CILA_CFG   0x14
#define MIPI_CAL_CILB_CFG   0x18
#define MIPI_CAL_CILC_CFG   0x1c
#define MIPI_CAL_CILD_CFG   0x20
#define MIPI_CAL_CILE_CFG   0x24
#define MIPI_CAL_DSIA_CFG   0x38
#define MIPI_CAL_DSIB_CFG   0x3c
#define MIPI_BIAS_PAD_CFG0  0x58
#define MIPI_BIAS_PAD_CFG2  0x60
#define MIPI_CAL_DSIA_CFG2  0x64
#define MIPI_CAL_DSIB_CFG2  0x68
#define MIPI_CAL_CILC_CFG2  0x6c
#define MIPI_CAL_CILD_CFG2  0x70
#define MIPI_CAL_CSIE_CFG2  0x74
#define MIPI_CAL_CIL_SEL    (1u << 21)
#define MIPI_CAL_CLKSEL     (1u << 21)
#define MIPI_CAL_DSI_SEL    (1u << 21)
#define BIAS_E_VCLAMP_REF   (1u << 0)
#define BIAS_PDVREG         (1u << 1)
#define MIPI_CAL_DONE       (1u << 16)
#define MIPI_CAL_ACTIVE     (1u << 0)
#define MIPI_CAL_CLKEN_OVR  (1u << 4)
#define MIPI_CAL_START      (0xau << 26 | 0x2u << 24 | 1u << 4 | 1u << 0)

/* Read-modify-write, because most of the sequence touches one bit of a
 * register whose other fields carry production trim we must not lose. */
static void mipi_upd(unsigned off, uint32_t mask, uint32_t val)
{
    uint32_t cur = 0;
    if (mem_rd(MIPI_CAL_BASE + off, &cur) < 0) return;
    mem_wr(MIPI_CAL_BASE + off, (cur & ~mask) | (val & mask), 0);
}

static void mipi_calibrate_csie(void)
{
    uint32_t st = 0;

    /* 1. Override the block's own clock gating. */
    mipi_upd(MIPI_CAL_CTRL, MIPI_CAL_CLKEN_OVR, MIPI_CAL_CLKEN_OVR);

    /* 2. Clear the status bits. */
    mem_wr(MIPI_CAL_BASE + MIPI_CAL_STATUS, 0xF1F10000, 0);

    /* 3. The display lanes are not ours; drop them. */
    mipi_upd(MIPI_CAL_DSIA_CFG, MIPI_CAL_DSI_SEL, 0);
    mipi_upd(MIPI_CAL_DSIB_CFG, MIPI_CAL_DSI_SEL, 0);

    /* 4. The bias the pads calibrate against -- the step we had missing. */
    mipi_upd(MIPI_BIAS_PAD_CFG0, BIAS_E_VCLAMP_REF, BIAS_E_VCLAMP_REF);
    mipi_upd(MIPI_BIAS_PAD_CFG2, BIAS_PDVREG, 0);

    /* 5. Deselect every lane and every clock, so only ours is left in. */
    mipi_upd(MIPI_CAL_CILA_CFG, MIPI_CAL_CIL_SEL, 0);
    mipi_upd(MIPI_CAL_DSIA_CFG2, MIPI_CAL_CLKSEL, 0);
    mipi_upd(MIPI_CAL_CILB_CFG, MIPI_CAL_CIL_SEL, 0);
    mipi_upd(MIPI_CAL_DSIB_CFG2, MIPI_CAL_CLKSEL, 0);
    mipi_upd(MIPI_CAL_CILC_CFG, MIPI_CAL_CIL_SEL, 0);
    mipi_upd(MIPI_CAL_CILC_CFG2, MIPI_CAL_CLKSEL, 0);
    mipi_upd(MIPI_CAL_CILD_CFG, MIPI_CAL_CIL_SEL, 0);
    mipi_upd(MIPI_CAL_CILD_CFG2, MIPI_CAL_CLKSEL, 0);
    mipi_upd(MIPI_CAL_CILE_CFG, MIPI_CAL_CIL_SEL, 0);
    mipi_upd(MIPI_CAL_CSIE_CFG2, MIPI_CAL_CLKSEL, 0);

    /* 6. Ours: lane E with its clock. */
    mipi_upd(MIPI_CAL_CILE_CFG, MIPI_CAL_CIL_SEL, MIPI_CAL_CIL_SEL);
    mipi_upd(MIPI_CAL_CSIE_CFG2, MIPI_CAL_CLKSEL, MIPI_CAL_CLKSEL);

    /* 7. Trim and trigger in one word. */
    mem_wr(MIPI_CAL_BASE + MIPI_CAL_CTRL, MIPI_CAL_START, 0);

    /* 8. The driver polls up to five hundred times at a couple of hundred
     * microseconds; a single short sleep was not giving it time. */
    int tries = 500;
    while (tries--) {
        if (mem_rd(MIPI_CAL_BASE + MIPI_CAL_STATUS, &st) < 0) break;
        if (st & MIPI_CAL_DONE) break;
        usleep(300);
    }
    printf("  MIPI calibration: status 0x%08x after %d polls (%s%s)\n",
           st, 500 - tries, (st & MIPI_CAL_DONE) ? "done" : "NOT done",
           (st & MIPI_CAL_ACTIVE) ? ", still active" : "");
    {
        /* Read back what the block actually holds. A calibration that stays
         * active tells us nothing about which of the writes landed. */
        uint32_t ctrl = 0, b0 = 0, b2 = 0, ce = 0, c2 = 0;
        mem_rd(MIPI_CAL_BASE + MIPI_CAL_CTRL, &ctrl);
        mem_rd(MIPI_CAL_BASE + MIPI_BIAS_PAD_CFG0, &b0);
        mem_rd(MIPI_CAL_BASE + MIPI_BIAS_PAD_CFG2, &b2);
        mem_rd(MIPI_CAL_BASE + MIPI_CAL_CILE_CFG, &ce);
        mem_rd(MIPI_CAL_BASE + MIPI_CAL_CSIE_CFG2, &c2);
        printf("    ctrl=%08x bias0=%08x bias2=%08x cile=%08x csie2=%08x\n",
               ctrl, b0, b2, ce, c2);
    }

    /* 9. Leave the selection as the driver leaves it. */
    mipi_upd(MIPI_CAL_CILE_CFG, MIPI_CAL_CIL_SEL, 0);
    mipi_upd(MIPI_CAL_CSIE_CFG2, MIPI_CAL_CLKSEL, 0);
}

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
#define NVMAP_HEAP_CARVEOUT_GENERIC (1 << 0)
#define NVMAP_HANDLE_WRITE_COMBINE 2

/* ---- nvhost register access ---- */
#define NVHOST_IOCTL_MAGIC 'H'
struct regrdwr_args {
    uint32_t id, num_offsets, block_size, offsets, values, write;
};
struct nvhost_get_param_arg { uint32_t param, value; };
struct nvhost_set_nvmap_fd_args { uint32_t fd; };
struct nvhost_ctrl_syncpt_read_args { uint32_t id, value; };
struct nvhost_clk_rate_args { uint32_t rate, moduleid; };
#define NVHOST_IOCTL_CHANNEL_SET_CLK_RATE \
    _IOW(NVHOST_IOCTL_MAGIC, 10, struct nvhost_clk_rate_args)
#define NVHOST_IOCTL_CTRL_SYNCPT_READ \
    _IOWR(NVHOST_IOCTL_MAGIC, 1, struct nvhost_ctrl_syncpt_read_args)
struct nvhost_syncpt_incr { uint32_t syncpt_id, syncpt_incrs; };
struct nvhost_cmdbuf { uint32_t mem, offset, words; };
struct nvhost_reloc { uint32_t cmdbuf_mem, cmdbuf_offset, target, target_offset; };
struct nvhost_reloc_shift { uint32_t shift; };
struct nvhost_fence { uint32_t syncpt_id, value; };
struct nvhost32_submit_args {
    uint32_t submit_version, num_syncpt_incrs, num_cmdbufs, num_relocs,
             num_waitchks, timeout, syncpt_incrs, cmdbufs, relocs,
             reloc_shifts, waitchks, waitbases, class_ids, pad[2],
             fences, fence;
} __attribute__((packed));
#define NVHOST32_IOCTL_CHANNEL_SUBMIT \
    _IOWR(NVHOST_IOCTL_MAGIC, 15, struct nvhost32_submit_args)

/* Host1x opcodes and the VI class. The surface address cannot be written
 * by hand: it is an address in VI's own translation context, and the only
 * way to get one is to let the kernel relocate a handle for us -- which
 * means a real submit rather than a register poke.
 *
 * Method numbers are not the register offsets. Two known pairs from the
 * kernel's own VI gather fix them: the image definition is offset 0x10C on
 * channel 0 and method 0x242, and 0x20C on channel 1 and method 0x282. So
 * method = offset/4 + 0x1FF, and both pairs agree. */
#define OP_SETCLASS(c)     ((0u << 28) | ((c) << 6))
#define OP_INCR(m, n)      ((1u << 28) | ((m) << 16) | (n))
#define OP_IMM(m, v)       ((4u << 28) | ((m) << 16) | ((v) & 0xffff))
#define VI_CLASS_ID        0x30
#define VI_METHOD(off)     (((off) >> 2) + 0x1FF)
#define NVHOST_IOCTL_CHANNEL_SET_NVMAP_FD \
    _IOW(NVHOST_IOCTL_MAGIC, 5, struct nvhost_set_nvmap_fd_args)
#define NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT \
    _IOWR(NVHOST_IOCTL_MAGIC, 16, struct nvhost_get_param_arg)
#define NVHOST32_IOCTL_CHANNEL_MODULE_REGRDWR \
    _IOWR(NVHOST_IOCTL_MAGIC, 14, struct regrdwr_args)

/* ---- VI / CSI registers (24.1 registers.h, t124_registers.h) ---- */
#define VI_CSI_BASE(n)              (0x100 + (n) * 0x100)
#define VI_CSI_SW_RESET             0x000
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
#define IMAGE_DEF_DEST_MEM          0x1
#define IMAGE_DEF_DEST_ISP_A        0x2
#define IMAGE_DEF_DEST_ISP_B        0x4

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
#define TEGRA_VI_CFG_CG_CTRL            0x0B8
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

/* The syncpoint counters live behind the control node, not the channel. */
static uint32_t syncpt_read(uint32_t id)
{
    struct nvhost_ctrl_syncpt_read_args r = { id, 0 };
    int fd = open("/dev/nvhost-ctrl", O_RDWR);
    if (fd < 0) return 0;
    ioctl(fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &r);
    close(fd);
    return r.value;
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
    /* A null label means the caller reports for itself -- the per-shot loop
     * runs this often enough that a line each would bury the result. */
    if (what)
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
/* VI reaches memory through its own translation context, and an address
 * that nvmap hands us for the virtual heap need not mean anything there --
 * a write would go nowhere, silently, however right the rest is. Carveout
 * is physically contiguous, so the address is the address. */
static uint32_t alloc_heap = NVMAP_HEAP_IOVMM;

static int nvmap_alloc(uint32_t h) {
    struct nvmap_alloc_handle ah = { h, alloc_heap,
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
/* If the engine wrote through a different mapping than the one we read
 * back through, the data can be sitting there while our read returns what
 * was in cache -- indistinguishable from a capture that never happened. */
struct nvmap_cache_op {
    unsigned long addr;
    uint32_t handle, len;
    int32_t op;
};
#define NVMAP_IOC_CACHE   _IOW(NVMAP_IOC_MAGIC, 12, struct nvmap_cache_op)
#define NVMAP_CACHE_OP_INV 1

static void nvmap_invalidate(uint32_t h, uint32_t len) {
    struct nvmap_cache_op c = { 0, h, len, NVMAP_CACHE_OP_INV };
    ioctl(nvmap_fd, NVMAP_IOC_CACHE, &c);
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
    /* Destination is in the low bits: 1 memory, 2 ISP-A, 4 ISP-B. Stock
     * reads 0x00200004 because it sends pixels to the ISP, not to memory
     * -- copying its value told our VI to do the same, which is why the
     * buffer stayed untouched and nothing ever reported an error. For a
     * memory write it is the format, the transform bypass, and DEST_MEM. */
    uint32_t image_def = (1u << BYPASS_PXL_TRANSFORM_OFFSET) |
                         (IMAGE_FORMAT_T_R16_I << IMAGE_DEF_FORMAT_OFFSET) |
                         IMAGE_DEF_DEST_MEM;
    uint32_t frame_length = 2064, coarse_time = 2000, gain = 16;
    /* The front sensor is on CIL E. The reference dump that had this word
     * at zero came from a session driving CIL A and B -- the other brick
     * entirely -- so it says nothing about ours. Back to the value that
     * brings E up, which is the one that reads 0x110 back from it. */
    uint32_t phy_cil_cmd = 0x12020000;   /* brick E, one lane */
    int tpg = 0, shots = 8, piggyback = 0;
    int hold = 0, dump_regs = 0, scan_cil = 0;

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
        else if (strncmp(a, "--shots=", 8) == 0)  shots = atoi(a + 8);
        else if (strcmp(a, "--carveout") == 0)    alloc_heap = NVMAP_HEAP_CARVEOUT_GENERIC;
        else if (strcmp(a, "--tpg") == 0)         { tpg = 1; use_sensor = 0; }
        else if (strcmp(a, "--piggyback") == 0)   { piggyback = 1; use_sensor = 0; }
        else if (strncmp(a, "--phy-cil=", 10) == 0)
            phy_cil_cmd = (uint32_t)strtoul(a + 10, 0, 16);
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
            /* Opening the node already powered it -- but only just. The log
             * puts the mode ioctl twenty-five microseconds after the power
             * sequence returns, and the part answers neither of the two
             * writes that follow: "no acknowledge from address 0x36". The
             * driver's power-on does not wait for the sensor to come out of
             * reset, so the wait has to be here. */
            usleep(50000);
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
        vi_wr(T124_CILC_PAD_CONFIG0, 0x00010000);
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
        vi_wr(T124_PP_B_PIXEL_STREAM_CONTROL0, 0x280301f1);
        vi_wr(T124_PP_B_PIXEL_STREAM_PP_COMMAND, 0x0000f005);
        vi_wr(T124_PP_B_PIXEL_STREAM_CONTROL1, 0x00000011);
        vi_wr(T124_PP_B_PIXEL_STREAM_GAP, 0x00140000);
        vi_wr(T124_PP_B_PIXEL_STREAM_EXPECTED_FRAME, 0x0);
        vi_wr(T124_PP_B_INPUT_STREAM_CONTROL, 0x003f0000);

        /* Only the upper half of the brick command is ours; the lower half
         * belongs to the rear path and has to survive our write. */
        vi_wr(T124_CSI_PHY_CIL_COMMAND,
              (vi_rd(T124_CSI_PHY_CIL_COMMAND) & 0x0000FFFF) | phy_cil_cmd);
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
        if (!tpg) mipi_calibrate_csie();
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
        c.moduleid = 0; c.rate = 408000000;
        int a1 = ioctl(vi_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &c);
        c.moduleid = 1; c.rate = 528000000;      /* memory */
        int a2 = ioctl(vi_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &c);
        printf("clock/bandwidth request: module rc=%d, memory rc=%d\n", a1, a2);
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
    /* Measured off a live stock session on this channel: 0x00200004. The
     * transform-bypass bit is CLEAR there, where we had been setting it,
     * and the low nibble carries a 4 we had left at zero. Bypass off is
     * what the driver does whenever the ISP is in the path. */
    vi_wr(base + VI_CSI_IMAGE_DEF, image_def);
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

    /* Surface address and trigger through host1x, so the buffer is mapped
     * into VI's context and the address written is one it can reach. */
    {
        uint32_t cmd_h = nvmap_create(4096);
        nvmap_alloc(cmd_h);
        uint32_t g[10];
        int n = 0, addr_word;
        g[n++] = OP_SETCLASS(VI_CLASS_ID);
        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_SURFACE0_OFFSET_MSB), 1);
        g[n++] = 0;
        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_SURFACE0_OFFSET_LSB), 1);
        addr_word = n;
        g[n++] = 0;                       /* the kernel fills this in */
        g[n++] = OP_INCR(VI_METHOD(base + VI_CSI_SINGLE_SHOT), 1);
        g[n++] = SINGLE_SHOT_CAPTURE;
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
        sa.timeout = 3000;
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

        /* Now the shots, as the driver does them: re-arm the parser, arm the
         * frame-start condition, write the trigger, and see whether the
         * counter moves. Each attempt reports for itself. */
        for (int shot = 0; shot < shots; shot++) {
            uint32_t fs0 = syncpt_read(sp_id);

            vi_wr(pp, (0xFu << CSI_PP_START_MARKER_FRAME_MAX_OFFSET) |
                      CSI_PP_SINGLE_SHOT_ENABLE | CSI_PP_ENABLE);
            vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT,
                  (front ? T124_PPB_FRAME_START : T124_PPA_FRAME_START) << 8
                  | sp_id);
            vi_wr(base + VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);
            vi_flush(0);

            /* Wait for the frame to start, then -- and only then -- arm the
             * acknowledge, which is what the driver's two threads do between
             * them. Firing and sleeping a fixed time was leaving the buffer
             * half written, because the shot lands wherever the sensor
             * happens to be in its frame. */
            int waited = 0;
            while (syncpt_read(sp_id) == fs0 && waited < 400) {
                usleep(1000);
                waited++;
            }
            int started = syncpt_read(sp_id) != fs0;

            uint32_t mw0 = syncpt_read(sp_mw), mwa0 = syncpt_read(sp_cmd);
            vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, T124_MWB_ACK_DONE << 8 | sp_mw);
            vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT, T124_MWA_ACK_DONE << 8 | sp_cmd);
            vi_flush(0);

            int mwaited = 0;
            while (syncpt_read(sp_mw) == mw0 &&
                   syncpt_read(sp_cmd) == mwa0 && mwaited < 400) {
                usleep(1000);
                mwaited++;
            }
            printf("  shot %d: start %s (%dms), write B %s A %s (%dms),"
                   " parser %08x\n",
                   shot, started ? "yes" : "NO", waited,
                   syncpt_read(sp_mw) != mw0 ? "yes" : "no",
                   syncpt_read(sp_cmd) != mwa0 ? "yes" : "no", mwaited,
                   vi_rd(front ? T124_PP_B_PIXEL_PARSER_STATUS
                               : T124_PP_A_PIXEL_PARSER_STATUS));
        }

        /* Now that the shots are away, arm the acknowledge -- the driver's
         * order, and it costs nothing if the DMA has already finished. */
        vi_wr(TEGRA_VI_CFG_VI_INCR_SYNCPT,
              (front ? T124_MWB_ACK_DONE : T124_MWA_ACK_DONE) << 8 | sp_mw);
        vi_flush("arm memory-write acknowledge");
        ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    }
    usleep(1500000);

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
