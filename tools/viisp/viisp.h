/*
 * viisp — shared definitions for the VI/ISP single-shot tool.
 *
 * Split out of the original monolith: register maps and ioctl shapes here,
 * the nvmap/host1x plumbing in viisp_platform.c, the ISP job builders in
 * isp_jobs.c, and the run flow in viisp.c.
 */
#ifndef VIISP_H
#define VIISP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* The CSI pads sit in deep power down until something asks the PMC to let
 * them out. Offsets from the stock kernel's pmc.c; CSIE is bit 12 of the
 * second request register per the driver's own table. */
#define PMC_BASE            0x7000E400UL
#define PMC_IO_DPD2_REQ     0x1C0
#define PMC_DPD_CODE_OFF    0x40000000u
#define PMC_DPD_BIT_CSIE    (1u << 12)

/* The receiver's own clocks. Numbers from the stock kernel's clock table;
 * the set registers only set bits, which is why they are used rather than
 * a read-modify-write. */
#define CAR_BASE            0x60006000UL
#define CAR_ENB_SET_H       0x328
#define CAR_ENB_SET_W       0x448
#define CAR_CSI_BIT_H       (1u << 20)
#define CAR_CILE_BIT_W      (1u << 18)
#define CAR_CILCD_BIT_W     (1u << 17)   /* the C/D/E brick's shared clock; the stock has it on with CILE */
#define CAR_ENB_SET_X       0x284
#define CAR_MIPICAL_BIT_H   (1u << 24)
#define CAR_CLK72M_BIT_X    (1u << 17)
#define CAR_ENB_SET_L       0x320
#define CAR_RST_CLR_L       0x304
#define CAR_VI_BIT_L        (1u << 20)
#define CAR_RST_CLR_H       0x30C
#define CAR_RST_CLR_W       0x43C

/* MIPI calibration: five lane interfaces, a bias pad, and the command word
 * that starts a calibration cycle. */
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
#define NVHOST_IOCTL_CHANNEL_GET_CLK_RATE \
    _IOWR(NVHOST_IOCTL_MAGIC, 9, struct nvhost_clk_rate_args)
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

/* The ISP side: class ids as host1x knows them, and the registers the
 * reprocess tool already drives. 0xE30 arms the memory input port and
 * 0x00C takes 0x0B for a memory pass, while a frame arriving from VI is
 * 0x05 and no input descriptor at all. */
#define ISP_CLASS_A                 0x32
#define ISP_CLASS_B                 0x34
#define ISP_TRIGGER_SENSOR          0x05
#define VI1_ISPB_SYNCPT             46      /* the stock arms VI condition 0xf onto it with every shot */
#define ISP_ENABLE_WORD             0x04040007  /* method 0x015, once per session */
#define ISP_TRIGGER_MEMORY          0x0B

/* Port B's command register is 0x87C -- 0x86C is its INPUT_STREAM_CONTROL. */
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
#define T124_CILA_STATUS                     0x940
#define T124_CILB_PAD_CONFIG0                0x960
#define T124_CSI_CIL_B_INT_MASK              0x96C
#define T124_CSI_CIL_B_STATUS                0x970
#define T124_CILB_STATUS                     0x974
#define T124_CILC_PAD_CONFIG0                0x994
#define T124_PHY_CILC_CONTROL0               0x99C
#define T124_CSI_CIL_C_INT_MASK              0x9A0
#define T124_CSI_CIL_C_STATUS                0x9A4
#define T124_CILC_STATUS                     0x9A8
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

/* The pattern generator, at the parser base plus 0x18C. */
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
/* The CSI block sits at 0x838 in the VI aperture. */
#define T124_CSI_CLKEN_OVERRIDE              (0x838 + 0x218)
#define T124_CSI_DEBUG_CONTROL               (0x838 + 0x21C)

#define TEGRA_VI_CFG_VI_INCR_SYNCPT     0x000
#define TEGRA_VI_CFG_VI_INCR_SYNCPT_ERROR 0x008
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

/* ---- shared state ---- */
extern int nvmap_fd, vi_fd;
extern uint32_t alloc_heap;
extern int dm_sent, real_sent, use_real_pass;
extern int arm_stats, do_warmup;
extern int per_frame_cal, geo_blocks, ccm, stream_xfer;
/* Groups of the stock configuration table (isp_stock.h), for --stock-groups. */
#define STOCK_DEMOSAIC 1    /* 0x900..0x908, 0x506 */
#define STOCK_COLOUR   2    /* 0x600, 0x650 + the four tone tables, 0xd00.. shading */
#define STOCK_STATS    4    /* 0x909..0x920 */
#define STOCK_INPUT    8    /* 0x200, 0x202, 0x205 */
#define STOCK_CCM      16   /* 0x300, 0x304 */
#define STOCK_CHAN     32   /* 0x700, 0x750: the stock process's own addresses */
extern unsigned stock_groups;
/* --work-word=HEX: what goes into 0x054 in the PER-FRAME calibration only,
 * in place of the default zero; the opening and the working configuration
 * keep the work buffer's address. The stock's steady-state rounds carry a
 * constant there per resolution (0x02016b4c at 2592, 0x003e0e8f at 720p --
 * odd, so not an address) and never relocate it, while its opening has
 * {0,0} and its working configuration {1,0}. One variable per cell. */
extern uint32_t work_word_override;
extern int work_word_set;
static inline uint32_t work_word(uint32_t iova) { return work_word_set ? work_word_override : iova; }
extern int stock_vi, no_isp, sensor_late, cile_rewritten;
extern unsigned long emc_bw;
extern int isp_wait_ms, stream_n, isp_job_timeout_ms, shot_delay_ms, no_emc_pin;
extern unsigned isp_emc_clk;
extern uint32_t wb_r, wb_b;
extern unsigned stats_kb, work_kb;

/* ---- platform ---- */
int   mem_wr(unsigned long addr, uint32_t val, uint32_t *before);
int   mem_rd(unsigned long addr, uint32_t *out);
void  car_enable_csi_clocks(void);
void  emc_pin_high(void);
void  emc_unpin(void);
void  mipi_upd(unsigned off, uint32_t mask, uint32_t val);
void  mipi_calibrate_csie(void);
int   pmc_dpd_release(uint32_t bit);
uint32_t syncpt_read(uint32_t id);
void  vi_wr(uint32_t off, uint32_t val);
int   vi_flush(const char *what);
uint32_t vi_rd(uint32_t off);
uint32_t nvmap_create(uint32_t size);
int   nvmap_alloc(uint32_t h);
uint32_t nvmap_pin(uint32_t h);
void  nvmap_unpin(uint32_t h);
void  nvmap_invalidate(uint32_t h, uint32_t len);
int   nvmap_rw(uint32_t h, uint32_t off, void *p, uint32_t len, int wr);

/* ---- geometry, measured off the stock captures ---- */
struct geom_cfg {
    uint32_t in_dims;               /* 0x202 words 1-2 */
    uint32_t in_phase;              /* 0x205 word 3 */
    uint32_t d20_mode, d20_step;    /* 0xd20 word 0, words 2-5 */
    uint32_t s909_w4;               /* 0x909 word 3 */
    uint32_t s910_w3, s910_w7, s910_w8; /* 0x910 words 3, 7, 8 */
    uint32_t s91c_w5;               /* 0x91c word 5 */
    uint32_t x700_w5, x700_w11;     /* 0x700 words 5, 11 */
    uint32_t c00_w0, c00_w2_hi;     /* 0xc00 word 0, high half of word 2 */
    uint32_t d00[8];                /* 0xd00 words 1-8 */
    uint32_t p400_w8, p400_w9, p400_w10, p400_w11; /* 0x400 tail */
};
const struct geom_cfg *geom_for(unsigned W, unsigned H);

/* ---- ISP jobs ---- */
unsigned isp_stock_emit(uint32_t *g, unsigned n, uint32_t work_iova);
int isp_init(int isp_fd, uint32_t work_h, uint32_t sp,
             uint32_t work_iova, uint32_t stats_iova,
             const struct geom_cfg *geo);
int isp_demosaic(int isp_fd, uint32_t sp, uint32_t out_h,
                 uint32_t stats_h, uint32_t u_off, uint32_t v_off,
                 uint32_t work_iova);
int isp_real_pass(int isp_fd, uint32_t sp, uint32_t work_iova);
int isp_warmup(int isp_fd, uint32_t sp, uint32_t warm_h,
               uint32_t stats_h, int write_enable,
               unsigned W, unsigned H);
int isp_colour(int isp_fd, uint32_t sp, uint32_t work_iova,
               unsigned W, unsigned H);
void isp_stop(int isp_fd, uint32_t sp);
int isp_frame(int isp_fd, uint32_t out_h, uint32_t stats_h,
              unsigned W, unsigned H, uint32_t fmt,
              uint32_t u_off, uint32_t v_off,
              uint32_t sp_mem, uint32_t sp_stats, uint32_t sp_loadv,
              uint32_t sp, uint32_t hold_sp, uint32_t hold_at,
              uint32_t park_mem, uint32_t park_stats,
              uint32_t work_iova, int per_frame_cal);

#endif /* VIISP_H */
