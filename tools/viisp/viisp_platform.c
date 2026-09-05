/*
 * viisp_platform — the plumbing: /dev/mem register pokes, the VI register
 * batch writer, syncpoint reads and the nvmap allocator ioctls.
 */
#include "viisp.h"

int nvmap_fd = -1, vi_fd = -1;
/* The output can come from the scattered heap or the carveout; VI needs
 * physically contiguous memory, the ISP does not care. */
uint32_t alloc_heap = NVMAP_HEAP_CARVEOUT_GENERIC;

int dm_sent;
int real_sent;
/* The working recipe, as defaults: two 8x8 warm-ups with the enable inside
 * the first, the statistics conditions armed, the geometry blocks for the
 * frame size, the real pass after the warm-ups, the stock's streaming
 * transfer block, the calibration with every frame. */
int use_real_pass = 1;
int arm_stats = 1;
int do_warmup = 1;
int per_frame_cal = 1;
int geo_blocks = 1;
int stream_xfer = 1;
uint32_t work_word_override; int work_word_set;   /* --work-word=HEX */
unsigned stock_groups = STOCK_DEMOSAIC;  /* --stock-groups=MASK: which groups of the stock table isp_init sends */
int stock_vi;       /* --stock-vi: the stock camera's VI and parser words instead of the R21.5 driver's */
int no_isp;         /* --no-isp: never open the ISP channel; VI to memory alone */
int sensor_late;     /* 0: the sensor streams before the receiver comes up (the MIPI calibration needs the clock lane live). The other order is kept as code only. */
int cile_rewritten;  /* the CILE pad re-write before the first shot has been done */
unsigned long emc_bw = 163200000;   /* --emc-bw: the ISP's EMC bandwidth request, bytes/s (the stock's 163.2 MB/s) */
int isp_job_timeout_ms = 60000;     /* host1x timeout of the ISP frame job; stream_run lowers it */
int isp_wait_ms = 2500;             /* --isp-wait: how long to wait for the ISP's output write per frame */
int stream_n;                       /* --stream=N: N frames by the stock's protocol instead of single shots */
unsigned isp_emc_clk = 81600;       /* --isp-emc-clk: the isp_clk (kHz) in the ISP SET_EMC ioctl; the stock's 81600 -> 162 MB/s ISO */
uint32_t wb_r, wb_b;    /* --wb: white-balance gains for 0x705 / 0x70b, 4.12 */
int ccm = 3;
unsigned stats_kb = 512, work_kb = 512;

int mem_wr(unsigned long addr, uint32_t val, uint32_t *before)
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

int mem_rd(unsigned long addr, uint32_t *out)
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

/* Memory bandwidth. On a fresh boot EMC sits at the DVFS floor (PLLP/2,
 * ~204 MHz) and the ISP starves on it: noise-like output, writes that land
 * late, memory faults after unmap. The stock camera's session lifts EMC
 * onto PLLM and it stays there a while, which is the whole of "it works
 * after the stock camera". The one knob that moves EMC here is the CPU
 * governor -- cpu.emc follows the CPU rate -- so the run pins it to
 * performance and puts the old governor back at exit. */
#define CPU0_GOVERNOR "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
static char saved_governor[32];

void emc_pin_high(void)
{
    FILE *f = fopen(CPU0_GOVERNOR, "r");
    if (!f) { printf("  EMC: no cpufreq governor node\n"); return; }
    if (!fgets(saved_governor, sizeof saved_governor, f)) saved_governor[0] = 0;
    fclose(f);
    char *nl = strchr(saved_governor, '\n'); if (nl) *nl = 0;
    if (strcmp(saved_governor, "performance") == 0) { saved_governor[0] = 0; printf("  EMC: governor already performance\n"); return; }
    f = fopen(CPU0_GOVERNOR, "w");
    if (!f) { printf("  EMC: cannot set governor: %s\n", strerror(errno)); saved_governor[0] = 0; return; }
    fputs("performance\n", f);
    fclose(f);
    usleep(200000);   /* the shared bus follows within a DVFS tick */
    uint32_t src = 0;
    mem_rd(CAR_BASE + 0x19c, &src);
    printf("  EMC: governor %s -> performance, EMC source 0x%08x%s\n", saved_governor, src,
           (src >> 29) == 4 ? " (PLLM)" : " (NOT PLLM -- the ISP will starve)");
}

void emc_unpin(void)
{
    if (!saved_governor[0]) return;
    FILE *f = fopen(CPU0_GOVERNOR, "w");
    if (!f) return;
    fputs(saved_governor, f); fputs("\n", f);
    fclose(f);
    printf("  EMC: governor back to %s\n", saved_governor);
}

void car_enable_csi_clocks(void)
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
    /* CILE and the C/D/E brick's shared clock CILCD: the 24.1 driver notes
     * that CSI-E through CILE needs both, and during the stock's stream
     * both bits (W 17 and 18) are on where we had only 18. */
    mem_wr(CAR_BASE + CAR_ENB_SET_W, CAR_CILE_BIT_W | CAR_CILCD_BIT_W, &b2);
    mem_wr(CAR_BASE + CAR_ENB_SET_X, CAR_CLK72M_BIT_X, &b3);

    /* Enabling a clock is only half of what the kernel's helper does: it
     * also takes the block out of reset. A module left in reset accepts
     * register writes and does nothing, without complaining -- which is
     * what a calibration that starts and never finishes looks like. */
    mem_wr(CAR_BASE + CAR_RST_CLR_H, CAR_CSI_BIT_H | CAR_MIPICAL_BIT_H, 0);
    mem_wr(CAR_BASE + CAR_RST_CLR_W, CAR_CILE_BIT_W | CAR_CILCD_BIT_W, 0);

    printf("  receiver clocks on and out of reset: H 0x%08x, W 0x%08x, X 0x%08x\n",
           b1, b2, b3);
}

/* Read-modify-write, because most of the sequence touches one bit of a
 * register whose other fields carry production trim we must not lose. */
void mipi_upd(unsigned off, uint32_t mask, uint32_t val)
{
    uint32_t cur = 0;
    if (mem_rd(MIPI_CAL_BASE + off, &cur) < 0) return;
    mem_wr(MIPI_CAL_BASE + off, (cur & ~mask) | (val & mask), 0);
}

void mipi_calibrate_csie(void)
{
    uint32_t st = 0;

    /* 1. Override the block's own clock gating. */
    mipi_upd(MIPI_CAL_CTRL, MIPI_CAL_CLKEN_OVR, MIPI_CAL_CLKEN_OVR);

    /* 2. Clear the status bits. */
    /* The stock (NvViCsiCalibrateT12x in libnvvicsi_v3, decompiled) writes
     * 0 here; the 24.1 kernel writes 0xF1F10000. We follow the stock. */
    mem_wr(MIPI_CAL_BASE + MIPI_CAL_STATUS, 0x00000000, 0);

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
    /* The stock waits 10 us after the selects and then sets the start bit
     * alone, leaving the noise filter and prescale fields as they are;
     * the 24.1 kernel writes the whole word (0xa<<26 | 2<<24 | CLKEN_OVR
     * | START). We follow the stock. */
    usleep(10);
    mipi_upd(MIPI_CAL_CTRL, 1u, 1u);

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
    /* The 24.1 kernel's calibration ends by putting the clock-lane select
     * back to 1 on the pads it is not calibrating -- DSIA, DSIB, CILC,
     * CILD -- and the stock's register state after a run shows the same
     * bits set where a fresh boot has them clear. We had cleared them at
     * the start and never restored them; on a fresh boot the picture was
     * garbage until the stock camera had run once. */
    mipi_upd(MIPI_CAL_DSIA_CFG2, MIPI_CAL_CLKSEL, MIPI_CAL_CLKSEL);
    mipi_upd(MIPI_CAL_DSIB_CFG2, MIPI_CAL_CLKSEL, MIPI_CAL_CLKSEL);
    mipi_upd(MIPI_CAL_CILC_CFG2, MIPI_CAL_CLKSEL, MIPI_CAL_CLKSEL);
    mipi_upd(MIPI_CAL_CILD_CFG2, MIPI_CAL_CLKSEL, MIPI_CAL_CLKSEL);
}

int pmc_dpd_release(uint32_t bit)
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

void vi_wr(uint32_t off, uint32_t val)
{
    if (batch_n >= VI_BATCH_MAX) { printf("  batch full, dropping 0x%03x\n", off); return; }
    batch_off[batch_n] = off;
    batch_val[batch_n] = val;
    batch_n++;
}

/* The syncpoint counters live behind the control node, not the channel. */
uint32_t syncpt_read(uint32_t id)
{
    struct nvhost_ctrl_syncpt_read_args r = { id, 0 };
    int fd = open("/dev/nvhost-ctrl", O_RDWR);
    if (fd < 0) return 0;
    ioctl(fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &r);
    close(fd);
    return r.value;
}

int vi_flush(const char *what)
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

uint32_t vi_rd(uint32_t off)
{
    uint32_t v = 0;
    if (vi_reg(off, &v, 0)) return 0xdeadbeef;
    return v;
}

uint32_t nvmap_create(uint32_t size) {
    struct nvmap_create_handle ch = { .size = size };
    if (ioctl(nvmap_fd, NVMAP_IOC_CREATE, &ch) < 0) { perror("nvmap create"); return 0; }
    return ch.handle;
}

int nvmap_alloc(uint32_t h) {
    struct nvmap_alloc_handle ah = { h, alloc_heap,
                                     NVMAP_HANDLE_WRITE_COMBINE, 4096 };
    if (ioctl(nvmap_fd, NVMAP_IOC_ALLOC, &ah) < 0) { perror("nvmap alloc"); return -1; }
    return 0;
}

uint32_t nvmap_pin(uint32_t h) {
    struct nvmap_pin_handle ph = { h, 0, 1 };
    if (ioctl(nvmap_fd, NVMAP_IOC_PIN_MULT, &ph) < 0) { perror("nvmap pin"); return 0; }
    return (uint32_t)ph.addr;
}
void nvmap_unpin(uint32_t h) {
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

void nvmap_invalidate(uint32_t h, uint32_t len) {
    struct nvmap_cache_op c = { 0, h, len, NVMAP_CACHE_OP_INV };
    ioctl(nvmap_fd, NVMAP_IOC_CACHE, &c);
}

int nvmap_rw(uint32_t h, uint32_t off, void *d, uint32_t n, int wr) {
    struct nvmap_rw_handle rw = { (unsigned long)d, h, off, n, n, n, 1 };
    return ioctl(nvmap_fd, wr ? NVMAP_IOC_WRITE : NVMAP_IOC_READ, &rw);
}
