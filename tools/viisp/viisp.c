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
/* Allocation that names the memory's kind. Stock's output format is the
 * block-linear one and it will not complete against an ordinary buffer --
 * the ISP simply never writes. This is how a block-linear surface is asked
 * for; the kind byte is what the format's top byte refers to. */
struct nvmap_alloc_kind_handle {
    uint32_t handle, heap_mask, flags, align;
    uint8_t kind, comp_tags;
};
#define NVMAP_IOC_ALLOC_KIND \
    _IOW(NVMAP_IOC_MAGIC, 100, struct nvmap_alloc_kind_handle)
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
struct nvhost_ctrl_syncpt_incr_args { uint32_t id; };
#define NVHOST_IOCTL_CTRL_SYNCPT_INCR \
    _IOW(NVHOST_IOCTL_MAGIC, 2, struct nvhost_ctrl_syncpt_incr_args)

/* host1x's own class, and the method that parks a channel until a syncpoint
 * reaches a threshold. A parked job holds everything it pinned, which is how
 * the buffer stays mapped for longer than the submit itself. */
#define HOST1X_CLASS_ID             0x01
#define HOST1X_WAIT_SYNCPT          0x08
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
#define OP_NONINCR(m, n)   ((2u << 28) | ((m) << 16) | (n))
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

static uint32_t nvmap_create(uint32_t size);
static int nvmap_alloc(uint32_t h);
static int nvmap_rw(uint32_t h, uint32_t off, void *p, uint32_t len, int wr);

/* Put the ISP in the state where it takes a frame off the VI interface and
 * writes the result to our buffer. The geometry, format and plane addresses
 * are the same registers the reprocess tool drives; what changes is that no
 * input descriptor is armed -- there is no source buffer to describe -- and
 * the trigger says sensor rather than memory.
 *
 * Submitted once, before the shot. Whether the ISP then needs re-arming per
 * frame is one of the things this is meant to find out. */
/* The calibration gather, taken verbatim from the 24.1 ISP driver, which in
 * turn captured it off the stock camera on this device. Fifteen hundred
 * words of it, and there is no reconstructing that from a register list --
 * the three or four registers we had been sending in its place were never
 * going to stand in for it.
 *
 * The driver patches the last two words before sending: 0x053 takes 1 and
 * 0x054 takes 0, and there is deliberately no trigger at the end. */
#include "isp_b_cal.h"

static int isp_init(int isp_fd, uint32_t work_h, uint32_t enable, uint32_t sp,
                    uint32_t work_iova, uint32_t stats_iova, int demosaic_zero,
                    uint32_t rt_luma, uint32_t ccm_word, unsigned skip,
                    uint32_t gpp_gain, int luma_lo,
                    uint32_t in_dims, uint32_t in_mode, uint32_t in_phase,
                    int zero_init, int apply)
{
    unsigned words = sizeof isp_b_cal_data / sizeof isp_b_cal_data[0];
    /* Room for the zero-init as well as the blob: that clearing pass alone
     * is fourteen hundred words. */
    /* Two clearing passes plus the runtime block and the blob. */
    uint32_t bytes = (words + 6144) * 4;
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
            { 0x910, 9, 0 }, { 0x919, 1, 0 }, { 0x91A, 9, 1 },
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
            /* The two words stock leaves set in the whole of that pass. */
            g[n++] = OP_NONINCR(0x91A, 9);
            for (int i = 0; i < 8; i++) g[n++] = 0;
            g[n++] = 0x00000200;
            g[n++] = OP_INCR(0x91F, 1); g[n++] = 0x00000002;

            /* Apply, then the transfer configuration. Its last word is two
             * on the first pass and one on the second -- the difference the
             * reconstruction collapsed into a single value. */
            g[n++] = OP_NONINCR(0x00C, 1); g[n++] = 0x0F;
            g[n++] = OP_INCR(0x018, 5);
            g[n++] = 0x00000000; g[n++] = 0x00000400;
            g[n++] = 0x00000000; g[n++] = 0x00000200;
            g[n++] = pass ? 0x00000001 : 0x00000002;
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

    g[n++] = OP_INCR(0x800, 3);
    g[n++] = stats_iova; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_INCR(0x820, 3);
    g[n++] = stats_iova; g[n++] = 0; g[n++] = 0;

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
    g[n++] = 0x00000000; g[n++] = 0x00001a40;
    g[n++] = 0x00000000; g[n++] = 0x10000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
    g[n++] = 0x00001000; g[n++] = 0x00001a00;
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
    g[n++] = 0x00001101; g[n++] = 0x00000000;
    g[n++] = 0x00210000; g[n++] = 0x00210000;
    g[n++] = 0x00210000; g[n++] = 0x00210000;

    g[n++] = OP_INCR(0x900, 2);
    g[n++] = 0x00000001; g[n++] = 0x00000001;
    g[n++] = OP_INCR(0x904, 2);
    g[n++] = 0x00005555; g[n++] = 0x00000001;
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
    g[n++] = 0x00000030; g[n++] = 0x1400002a;
    g[n++] = 0x3c00002b;

    g[n++] = OP_INCR(0x910, 9);
    g[n++] = 0x00000003; g[n++] = 0x00000028;
    g[n++] = 0x01480029; g[n++] = 0x0003030b;
    g[n++] = 0x00990030; g[n++] = 0x00000800;
    g[n++] = 0x007b0666; g[n++] = 0x00000036;
    g[n++] = 0x00001f1f;

    g[n++] = OP_INCR(0x91B, 1); g[n++] = 0x00000000;
    g[n++] = OP_NONINCR(0x91C, 9);
    g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = 0x00000001; g[n++] = 0x00000025;
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
    g[n++] = OP_INCR(0x600, 16);
    g[n++] = 0x00000005; g[n++] = 0x00000000;
    g[n++] = 0x00000000; g[n++] = 0x00000000;
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

    g[n++] = OP_INCR(0x650, 1); g[n++] = 0x00000003;
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
    g[n - 1] = work_iova;                 /* 0x054 */
    g[n++] = OP_INCR(0x015, 1); g[n++] = enable;

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
static int isp_init_a(int fd, uint32_t sp)
{
    uint32_t cmd_h = nvmap_create(8192);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    uint32_t g[512];
    unsigned n = 0;
    g[n++] = OP_SETCLASS(ISP_CLASS_A);

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
 * So instead: a short job that carries the same relocations and nothing
 * else, submitted again and again. Each one is over in moments and cannot
 * strand anything, and between them the mapping never lapses. */
static int isp_keepalive(int fd, uint32_t out_h, uint32_t stats_h,
                         uint32_t u_off, uint32_t v_off, uint32_t sp)
{
    uint32_t cmd_h = nvmap_create(4096);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    uint32_t g[24];
    int n = 0, y_word, u_word, v_word, stats_word;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    g[n++] = OP_INCR(0xE04, 3);
    y_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_INCR(0xE07, 3);
    u_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_INCR(0xE0A, 3);
    v_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_INCR(0x100, 4);
    stats_word = n; g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;
    g[n++] = OP_IMM(0, sp);
    nvmap_rw(cmd_h, 0, g, (uint32_t)n * 4, 1);

    struct nvhost_reloc rel[4] = {
        { cmd_h, (uint32_t)y_word * 4, out_h, 0 },
        { cmd_h, (uint32_t)u_word * 4, out_h, u_off },
        { cmd_h, (uint32_t)v_word * 4, out_h, v_off },
        { cmd_h, (uint32_t)stats_word * 4, stats_h, 0 },
    };
    struct nvhost_reloc_shift sh[4] = { { 0 }, { 0 }, { 0 }, { 0 } };
    struct nvhost_cmdbuf cb = { cmd_h, 0, (uint32_t)n };
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
    int rc = ioctl(fd, NVHOST32_IOCTL_CHANNEL_SUBMIT, &sa);
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
    return rc;
}

/* Put the block back to sleep. Without this it stays armed and writes a
 * later frame into a buffer we have already let go -- the memory controller
 * faults on it after the sensor has powered down, which is exactly where
 * the fault lands in the log. */
static void isp_stop(int isp_fd, uint32_t sp)
{
    uint32_t cmd_h = nvmap_create(4096);
    if (!cmd_h || nvmap_alloc(cmd_h)) return;

    uint32_t g[10];
    int n = 0;
    g[n++] = OP_SETCLASS(ISP_CLASS_B);
    g[n++] = OP_INCR(0x015, 1); g[n++] = 0x00000000;
    g[n++] = OP_NONINCR(0x00C, 1); g[n++] = 0x00000000;
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
    printf("ISP stopped: rc=%d (%s)\n", rc, rc == 0 ? "ok" : strerror(errno));
    ioctl(nvmap_fd, NVMAP_IOC_FREE, (unsigned long)cmd_h);
}

/* The per-frame gather, word for word as the stock camera sends it: output
 * geometry, three plane triplets for planar YUV, the processing block, the
 * stats buffer, three conditional syncpoint increments and the streaming
 * trigger. There are no input registers -- in streaming mode the pixels
 * arrive from VI and there is nothing to describe. */
static int isp_frame(int isp_fd, uint32_t out_h, uint32_t stats_h,
                     unsigned W, unsigned H, uint32_t fmt, uint32_t e03,
                     uint32_t trigger, uint32_t u_off, uint32_t v_off,
                     uint32_t sp_mem, uint32_t sp_stats, uint32_t sp_loadv,
                     uint32_t sp, uint32_t hold_sp, uint32_t hold_at,
                     uint32_t in_fmt, uint32_t work_iova, int per_frame_cal)
{
    /* Room for the calibration too, now that it rides with every frame. */
    uint32_t cmd_h = nvmap_create(16384);
    if (!cmd_h || nvmap_alloc(cmd_h)) return -1;

    /* Planar YUV takes a byte per luma sample and half-width chroma; the
     * packed forms take four bytes a pixel in one plane. */
    int planar = (fmt & 0xFF) == 0xE6;
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

    g[n++] = OP_INCR(0xE04, 3);
    y_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_y;
    g[n++] = OP_INCR(0xE07, 3);
    u_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_uv;
    g[n++] = OP_INCR(0xE0A, 3);
    v_word = n; g[n++] = 0; g[n++] = 0; g[n++] = stride_uv;

    g[n++] = OP_INCR(0x500, 6);
    g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0; g[n++] = 0;
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

    /* No parked job here. Twice now a host1x wait held this channel past
     * the timeout the kernel allows and killed it, and each time cost a
     * reboot. It was never the right cure anyway: the buffer went
     * unmapped because the ISP was still writing after our program had
     * torn everything down, and waiting for the block to report the write
     * before we tear down fixes that without holding anything. */
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

static uint32_t nvmap_create(uint32_t size) {
    struct nvmap_create_handle ch = { .size = size };
    if (ioctl(nvmap_fd, NVMAP_IOC_CREATE, &ch) < 0) { perror("nvmap create"); return 0; }
    return ch.handle;
}
/* VI reaches memory through its own translation context, and an address
 * that nvmap hands us for the virtual heap need not mean anything there --
 * a write would go nowhere, silently, however right the rest is. Carveout
 * is physically contiguous, so the address is the address. */
/* Contiguous by default. Out of the scattered heap the address nvmap hands
 * back is not one VI can reach: the memory controller logs a translation
 * fault on the buffer's own base the moment the capture runs long enough to
 * outlive the host1x job that mapped it, and the picture stops at whatever
 * row that happened on. Out of the carveout heap those faults do not occur
 * at all. --iovmm asks for the old behaviour. */
static uint32_t alloc_heap = NVMAP_HEAP_CARVEOUT_GENERIC;

static int nvmap_alloc(uint32_t h) {
    struct nvmap_alloc_handle ah = { h, alloc_heap,
                                     NVMAP_HANDLE_WRITE_COMBINE, 4096 };
    if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah) < 0) { perror("nvmap alloc"); return -1; }
    return 0;
}

/* The same, but naming the memory's kind. Stock's output format is
 * block-linear and the ISP never writes a byte against a surface that is
 * not, which is why that format has always come back untouched here. */
static int nvmap_alloc_kind(uint32_t h, unsigned kind) {
    struct nvmap_alloc_kind_handle ak;
    memset(&ak, 0, sizeof ak);
    ak.handle = h;
    ak.heap_mask = alloc_heap;
    ak.flags = NVMAP_HANDLE_WRITE_COMBINE;
    ak.align = 4096;
    ak.kind = (uint8_t)kind;
    if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC_KIND, &ak) < 0) {
        printf("nvmap alloc kind 0x%02x: %s\n", kind, strerror(errno));
        return -1;
    }
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
    uint32_t image_def = (0u << BYPASS_PXL_TRANSFORM_OFFSET) |
                         (IMAGE_FORMAT_T_R16_I << IMAGE_DEF_FORMAT_OFFSET) |
                         IMAGE_DEF_DEST_ISP_B;
    uint32_t frame_length = 2064, coarse_time = 2000, gain = 16;
    /* The front sensor is on CIL E. The reference dump that had this word
     * at zero came from a session driving CIL A and B -- the other brick
     * entirely -- so it says nothing about ours. Back to the value that
     * brings E up, which is the one that reads 0x110 back from it. */
    uint32_t phy_cil_cmd = 0x12020000;   /* brick E, one lane */
    int tpg = 0, shots = 8, piggyback = 0;
    int hold = 0, dump_regs = 0, scan_cil = 0, refill = 0, scan_cond = 0;
    int settle = 200;
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
    uint32_t in_dims = 0x00780078, in_mode = 1, in_phase = 0;
    /* The channel-to-ISP interface. Three is the only value anything names,
     * and what the rest of the field means has never been looked at -- it
     * is the one register on the VI side that describes the handover. */
    uint32_t ispintf = ISPINTF_CONFIG_ENABLE;
    /* Clear the block the way stock clears it before anything else. On by
     * default now: it is what the camera on this device actually does. */
    int zero_init = 1;
    int isp_apply = 1;
    uint32_t opt_u_off = 0, opt_v_off = 0;
    /* The kind to allocate the ISP's output as. Zero means an ordinary
     * pitch-linear buffer; 0xFE is what the block-linear format wants. */
    unsigned out_kind = 0;
    /* Configure the other block first, the way stock does. */
    int init_a = 1;
    /* Send the calibration with every frame rather than once, as stock
     * does. The init then carries only the clearing pass. */
    int per_frame_cal = 1;

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
        else if (strcmp(a, "--refill") == 0)      refill = 1;
        else if (strncmp(a, "--settle=", 9) == 0) settle = atoi(a + 9);
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
            in_dims = (uint32_t)strtoul(a + 10, 0, 16);
        else if (strncmp(a, "--in-mode=", 10) == 0)
            in_mode = (uint32_t)strtoul(a + 10, 0, 16);
        else if (strncmp(a, "--in-phase=", 11) == 0)
            in_phase = (uint32_t)strtoul(a + 11, 0, 16);
        else if (strncmp(a, "--ispintf=", 10) == 0)
            ispintf = (uint32_t)strtoul(a + 10, 0, 16);
        else if (strcmp(a, "--no-zero-init") == 0) zero_init = 0;
        else if (strcmp(a, "--no-apply") == 0)     isp_apply = 0;
        else if (strncmp(a, "--u-off=", 8) == 0)
            opt_u_off = (uint32_t)strtoul(a + 8, 0, 16);
        else if (strncmp(a, "--v-off=", 8) == 0)
            opt_v_off = (uint32_t)strtoul(a + 8, 0, 16);
        else if (strncmp(a, "--out-kind=", 11) == 0)
            out_kind = (unsigned)strtoul(a + 11, 0, 16);
        else if (strcmp(a, "--no-init-a") == 0)    init_a = 0;
        else if (strcmp(a, "--scan-cond") == 0)   scan_cond = 1;
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
    uint32_t stride_y = isp_planar ? ((W + 63) & ~63u) : W * 4;
    uint32_t stride_uv = ((W / 2) + 63) & ~63u;
    /* Stock's own plane offsets for this sensor are 0x540000 and 0x6a0000 --
     * a wider gap than the planes need, and not what rounding the sizes up
     * produces. Overridable for that reason. */
    uint32_t u_off = opt_u_off ? opt_u_off
                               : ((stride_y * OH + 0xFFFF) & ~0xFFFFu);
    uint32_t v_off = opt_v_off ? opt_v_off
                               : ((u_off + stride_uv * (OH / 2) + 0xFFFF)
                                  & ~0xFFFFu);
    uint32_t out_bytes = isp_planar ? v_off + stride_uv * (OH / 2)
                                    : stride_y * OH;
    int isp_fd = open("/dev/nvhost-isp.1", O_RDWR);
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
        struct nvhost_get_param_arg ip = { .param = 0, .value = 0 };
        if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &ip) == 0)
            sp_mem = ip.value;
        ip.param = 1; ip.value = 0;
        if (ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_GET_SYNCPOINT, &ip) == 0)
            isp_sp = ip.value;
        if (!isp_sp) isp_sp = sp_mem;
        sp_stats = 0;
        sp_loadv = 0;

        /* The ISP's own clocks, at the rates the reprocess tool uses. */
        struct nvhost_clk_rate_args ic;
        ic.moduleid = 0; ic.rate = 384000000;
        ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &ic);
        ic.moduleid = 1; ic.rate = 768000000;
        ioctl(isp_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &ic);

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
            uint32_t off[2] = { 0xFC, 0x54 };
            uint32_t val[2] = { 0x20, isp_enable };
            struct regrdwr_args ra;
            memset(&ra, 0, sizeof ra);
            ra.id = 0;
            ra.num_offsets = isp_enable ? 2 : 1;
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
        work_h = nvmap_create(512 * 1024);
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

        stats_h = nvmap_create(64 * 1024);
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
            int ok = out_kind ? nvmap_alloc_kind(out_h, out_kind)
                              : nvmap_alloc(out_h);
            if (ok == 0) out_iova = nvmap_pin(out_h);
        }
        printf("ISP-B channel fd=%d, syncpoints %u/%u/%u/%u, output %u bytes"
               " at 0x%08x (U at +0x%x, V at +0x%x)\n", isp_fd, isp_sp,
               sp_mem, sp_stats, sp_loadv, out_bytes, out_iova, u_off, v_off);
        if (work_h)
            isp_init(isp_fd, work_h, isp_enable, isp_sp,
                     work_iova, stats_iova, demosaic_zero, rt_luma, ccm_word,
                     isp_skip, gpp_gain, luma_lo, in_dims, in_mode,
                     in_phase, zero_init, isp_apply);
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
    if (isp_route && !(image_def & IMAGE_DEF_DEST_MEM)) {
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
        /* The driver asks for the maximum, 600MHz, whenever a channel starts
         * streaming. We had been asking for 408. */
        c.moduleid = 0; c.rate = 600000000;
        int a1 = ioctl(vi_fd, NVHOST_IOCTL_CHANNEL_SET_CLK_RATE, &c);
        c.moduleid = 1; c.rate = emc_rate;        /* memory */
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
    /* The interface between the channel and the ISP. Nothing reaches the
     * ISP with this at zero, whatever the destination bits say. */
    vi_wr(base + VI_CSI_ISPINTF_CONFIG, ispintf);
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
            isp_base_stats = syncpt_read(sp_stats);
            isp_base_loadv = syncpt_read(sp_loadv);
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

        for (int shot = 0; shot < shots; shot++) {
            int attempt = 0, done = 0, started = 0, waited = 0, mwaited = 0;

            /* Two at most. The first trigger aligns, the second captures;
             * more than that only stretches the run, and a long run is what
             * took the parked job past the timeout the kernel allows. */
            while (attempt < 2 && !done) {
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
                if (isp_fd >= 0 && out_iova && stats_h) {
                    isp_base_mem = syncpt_read(sp_mem);
                    isp_frame(isp_fd, out_h, stats_h, W, OH, isp_fmt, isp_e03,
                              isp_trigger, u_off, v_off,
                              sp_mem, sp_stats, sp_loadv, isp_sp, 0, 0,
                              isp_in_fmt, work_iova, per_frame_cal);
                }

                vi_wr(base + VI_CSI_SURFACE0_OFFSET_MSB, 0);
                vi_wr(base + VI_CSI_SURFACE0_OFFSET_LSB, iova);
                vi_wr(base + VI_CSI_SURFACE0_STRIDE, stride);
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
                if (isp_fd >= 0 && out_iova) {
                    int w2 = 0;
                    while (syncpt_read(sp_mem) == isp_base_mem && w2 < 600) {
                        /* Re-map while we wait. Without this the larger
                         * frames fault on the output buffer part way
                         * through and never complete. */
                        isp_keepalive(isp_fd, out_h, stats_h, u_off, v_off,
                                      isp_sp);
                        usleep(2000);
                        w2 += 2;
                    }
                    printf("  ISP wrote after %dms%s\n", w2,
                           syncpt_read(sp_mem) != isp_base_mem ? "" : " (NO)");
                }

                /* One whole frame from the start, plus what the caller asks
                 * for on top. */
                usleep((useconds_t)period * 500 + settle * 1000);
                mwaited = settle;
                done = started;
            }
            printf("  frame %d: %s after %d attempt%s"
                   " (start %dms, bottom %dms), parser %08x\n",
                   shot, done ? "whole" : (started ? "SHORT" : "NEVER STARTED"),
                   attempt, attempt == 1 ? "" : "s", waited, mwaited,
                   vi_rd(front ? T124_PP_B_PIXEL_PARSER_STATUS
                               : T124_PP_A_PIXEL_PARSER_STATUS));
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
        (void)isp_base_stats; (void)isp_base_loadv;

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
