/*
 * ispinit -- raise the ISP by hand and give it back, from a shell.
 *
 * The first thing we do ourselves instead of watching. The sequence is
 * minimal on purpose (the contract from the lead, 2026-08-30):
 *
 *   NvRmOpen(&dev)         -- stub in libnvrm.so: writes 1 to *out, returns 0.
 *                             So dev is the VALUE 1, not an address; the live
 *                             hook log confirms the stock passes r0=1 into
 *                             NvIspOpen.
 *   NvIspOpen(dev, 1, &h)  -- instance 1 for the first instance (stock opens
 *                             1 then 2). On failure it cleans up after
 *                             itself; we free nothing by hand, NvIspClose
 *                             owns the teardown, including the host1x
 *                             channel.
 *   NvIspSetIspClockRate(h, 0x003fffff) -- nonzero on purpose. impl-2: on a
 *                             fresh context the rate cache is zero, so
 *                             GetStatus(6) alone would read that same zero
 *                             and we could not tell a raised ISP from an
 *                             unraised one. The stock itself calls this with
 *                             r1=0, which is a no-op by cache equality.
 *   NvIspGetStatus(h, 6, &v, &size) -- size=4 in, actual size out; after the
 *                             clock call it must print the written rate,
 *                             not zero.
 *   NvIspClose(h)          -- nothing released manually: a manual release
 *                             would be a double free.
 *
 * Every step prints its result BEFORE the next call that may not return;
 * stdout is unbuffered, so whatever ran reaches the shell even mid-crash.
 *
 * Exit is a normal return from main. Nobody has established that the
 * kernel reclaims the channel behind a killed process, so we do not kill
 * the process and do not leave via a signal.
 *
 * Built WITH crt0 and libc (build-ispinit.sh), not -nostdlib: the -nostdlib
 * build died inside the linker on the very first dlopen -- loading a
 * library whose dependencies include libc runs constructors against a
 * process whose libc was never initialised. This is a separate process, so
 * the mediaserver rule (no symbol versioning, ever) does not extend here:
 * if it does not load, it does not load, nothing breaks.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#ifndef HOST_ARGTEST
#include <sys/ucontext.h>
#endif

#include "bayer_gen.h"

/*
 * dlopen flags are the bionic 4.4 values, NOT the NDK header values: on
 * this device RTLD_NOW is 0 and RTLD_GLOBAL is 2, while the r21e header
 * constants encode modern bionic and would read as RTLD_GLOBAL here.
 * Verified on the device; see the long comment in shim/src/nvisp_shim.c.
 * We want NOW | LOCAL, which is 0.
 */
#define ISPINIT_DLOPEN_FLAGS 0

static const char nvrm_path[] = "libnvrm.so";
static const char nvisp_path[] = "/system/vendor/lib/libnvisp_v3.real.so";

/*
 * The stock's configuration words, verbatim from the live dump
 * (out/hook-config.txt, identical for both instances):
 *   mode 1, size 64: sixteen words of format selectors;
 *   mode 2, size 4:  the single word 2.
 * Cross-checked twice: the hook dump and the lead's mail agree word for
 * word. A typo here is a wrong format with no visible cause.
 */
static const unsigned cfg_mode1_arr[16] = {
    0x00000001, 0x00000007, 0x00000009, 0x0000000a,
    0x00000003, 0x00000000, 0x00000006, 0x00000008,
    0x00000011, 0x0000000f, 0x0000000c, 0x0000000e,
    0x0000000b, 0x00000000, 0x00000010, 0x0000000d,
};
static const unsigned cfg_mode2 = 2;


/* --- the five calls --- */

typedef int (*NvRmOpen_fn)(void **out);
typedef int (*NvRmMemHandleCreate_fn)(unsigned param0, void **out,
                                      unsigned size);
typedef int (*NvRmMemHandleAllocAttr_fn)(unsigned size, void *attrs,
                                         void **out);
typedef int (*NvRmMemWriteStrided_fn)(unsigned handle, void *bufPtr,
                                      unsigned bufStride, unsigned offset,
                                      unsigned memStride, unsigned widthBytes,
                                      unsigned numRows, unsigned x8);
typedef int (*NvRmMemReadStrided_fn)(unsigned handle, void *bufPtr,
                                     unsigned bufStride, unsigned offset,
                                     unsigned memStride, unsigned widthBytes,
                                     unsigned numRows, unsigned x8);
typedef int (*NvRmMemRW_fn)(unsigned handle, void *ptr, unsigned a3,
                            unsigned a4);
typedef int (*NvIspOpen_fn)(unsigned devHandle, unsigned instance,
                            unsigned *hIsp);
typedef int (*NvIspSetConfiguration_fn)(unsigned hIsp, unsigned mode,
                                        const void *buf, unsigned *size);
typedef int (*NvIspSetIspClockRate_fn)(unsigned hIsp, unsigned rate);
typedef int (*NvIspHwSettingsCreate_fn)(unsigned devHandle, void *p1,
                                        unsigned size, void *p2);
typedef int (*NvIspHwSettingsSetAttribute_fn)(unsigned hSettings,
                                              unsigned blockId,
                                              unsigned index, void *buf,
                                              unsigned *size);
typedef int (*NvIspSetStats_fn)(unsigned hIsp, unsigned type,
                                unsigned subidx, void *buf,
                                unsigned *size);
typedef int (*NvIspHwSettingsApply_fn)(unsigned hSettings, void *mapped,
                                       unsigned a3, unsigned flags);
typedef int (*NvIspHwSettingsDestroy_fn)(unsigned hSettings, unsigned size,
                                         void *p3, unsigned devHandle);
typedef int (*NvIspSetAttribute_fn)(unsigned hIsp, unsigned attrId,
                                    void *inVal, void *inSize);
typedef unsigned (*NvRmMemGetAddress_fn)(unsigned hMem, unsigned offset);
typedef unsigned (*NvRmMemPin_fn)(unsigned hMem, unsigned flag);
/* pinning / device address: exports VERIFIED in libnvrm.so
   (NvRmMemPin 0x6459 sz26, NvRmMemGetAddress 0x66b9 sz16,
   NvRmMemPinMult 0x6449 sz16). GetAddress is (hMem, offset) returning
   the device address; Pin RETURNS the address too (measured: 0x800c0000
   in the return slot was an address, not an error code). */
typedef unsigned (*NvRmMemGetAddress_fn)(unsigned hMem, unsigned offset);
typedef unsigned (*NvRmMemPin_fn)(unsigned hMem, unsigned flag);
/* the four stages, per the 0x1784 frame read (lead + impl-2):
   st.4 takes a FIFTH argument on the stack (+0x1c) */
typedef int (*Stage1_fn)(unsigned hIsp, void *pkt, unsigned inDesc);
typedef int (*Stage2_fn)(unsigned hIsp, void *pkt, unsigned inDesc,
                         unsigned *local);
typedef int (*Stage3_fn)(unsigned hIsp, unsigned inDesc, unsigned counter,
                         unsigned local);
typedef int (*Stage4_fn)(unsigned hIsp, unsigned mode, unsigned outDesc,
                         unsigned a4, unsigned stack5);
typedef int (*NvIspGetStatus_fn)(unsigned hIsp, unsigned statusId,
                                 void *value, unsigned *size);
typedef int (*NvIspClose_fn)(unsigned hIsp);

/*
 * ProcessFrame: 4 register args + 9 stack words. The hook's live stack
 * (caller-sp coordinates) shows the five pointers at +0x10..+0x20 and
 * four words the library never reads at +0x00..+0x0c -- in C terms the
 * stack argument area starts at +0x00, so the descriptors are
 * parameters 9..13 and the unread words are parameters 5..8, mirrored
 * as zeros here (the stock's 3280/2460 there were session sensor
 * geometry; we encode nothing of the session).
 */
typedef int (*NvIspProcessFrame_fn)(unsigned hIsp, unsigned mode,
                                    unsigned r2, unsigned r3,
                                    unsigned s5, unsigned s6,
                                    unsigned s7, unsigned s8,
                                    unsigned inDesc, unsigned aux1,
                                    unsigned aux2, unsigned aux3,
                                    unsigned aux4);

/*
 * First n 32-bit LE words of a byte buffer, tagged. Used for the three
 * provenance states of the submission experiment: output-after-alloc,
 * input-after-write, output-after-submit. Twice-taken emptiness or
 * twice-taken garbage must be DISTINGUISHABLE, not assumed.
 */
static void print_first_words(const char *tag, const unsigned char *b,
                              int n)
{
    int m;

    printf("%s:", tag);
    for (m = 0; m < n; m++)
        printf(" %08x",
               (unsigned)b[m * 4] | ((unsigned)b[m * 4 + 1] << 8) |
               ((unsigned)b[m * 4 + 2] << 16) |
               ((unsigned)b[m * 4 + 3] << 24));
    printf("\n");
}

/*
 * Slot-kind resolution: what a configurable stack slot (+0x08/+0x10/
 * +0x14/+0x18) actually contains for this run. Defaults live in
 * slot_kind[]; kinds: descin/descout (surface descriptors with the
 * respective nvmap handles), descnohandle (frame description without
 * memory), buf (our zeroed per-slot buffer), zero.
 */
static unsigned resolve_slot(const char *kind, unsigned descin,
                             unsigned descout, unsigned descno,
                             unsigned buf)
{
    if (strcmp(kind, "descin") == 0)
        return descin;
    if (strcmp(kind, "descout") == 0)
        return descout;
    if (strcmp(kind, "descnohandle") == 0)
        return descno;
    if (strcmp(kind, "buf") == 0)
        return buf;
    return 0; /* zero */
}

/*
 * Post-call state: the words themselves plus WHICH words changed.
 * A library answer written into one word of one buffer is easy to miss
 * in sixteen lines -- the diff makes it unmissable.
 */
static void print_state_diff(const char *tag, const unsigned *before,
                             const unsigned *after, int n)
{
    int k, c = 0;

    printf("%s:", tag);
    for (k = 0; k < n; k++)
        printf(" +%02x:%08x", k * 4, after[k]);
    printf(" | changed:");
    for (k = 0; k < n; k++)
        if (before[k] != after[k]) {
            printf(" +%02x", k * 4);
            c++;
        }
    if (c == 0)
        printf(" none");
    printf("\n");
}

/*
 * Stage-table interception. The ISP handler table lives in the context;
 * entries are function pointers (thumb bit set). stage=<off>:on swaps
 * an entry for one of our hooks; the hook prints entry args (r0-r3
 * only -- arity unestablished), calls the original, prints its return
 * code, and returns it unchanged. Nothing else is touched. Fired /
 * never-called prints at the end.
 *
 * The hooks are ARM/Thumb and exist only in the device build; the host
 * argument-test build compiles with -DHOST_ARGTEST and gets a stub
 * table instead.
 */
#define STAGE_SLOTS 8

#ifndef HOST_ARGTEST

extern __attribute__((visibility("hidden"))) void stage_cont(void);
extern void stage_hook_0(void);
extern void stage_hook_1(void);
extern void stage_hook_2(void);
extern void stage_hook_3(void);
extern void stage_hook_4(void);
extern void stage_hook_5(void);
extern void stage_hook_6(void);
extern void stage_hook_7(void);
static const void *stage_hook_syms[STAGE_SLOTS] = {
    stage_hook_0, stage_hook_1, stage_hook_2, stage_hook_3,
    stage_hook_4, stage_hook_5, stage_hook_6, stage_hook_7
};
static unsigned stage_hook_orig[STAGE_SLOTS];   /* table value we replaced */
static unsigned stage_hook_fired[STAGE_SLOTS];
static unsigned stage_installed[STAGE_SLOTS];
static unsigned stage_installed_off[STAGE_SLOTS];
static unsigned stage_cur;                      /* slot currently entered */

/*
 * Called from asm with (slot, &saved[r0..r3]). Prints the entry args
 * and returns the ORIGINAL address; the asm tail-jumps to it with
 * lr = the shared continuation.
 */
__attribute__((used, noinline))
static void *stage_pre(unsigned slot, unsigned *saved)
{
    printf("[stage %u] enter r0=0x%x r1=0x%x r2=0x%x r3=0x%x\n",
           slot, saved[0], saved[1], saved[2], saved[3]);
    saved[4] = (unsigned)stage_cont; /* the hook reads it into lr */
    return (void *)stage_hook_orig[slot];
}

/* the shared continuation: a pure-asm symbol (defined nowhere in C),
   its address is handed to the hook by stage_pre through saved[4] --
   the C-side reference keeps the relocation in compiler territory,
   which the 4.9 linker accepts, unlike hand-written movw/movt. */

__attribute__((used, noinline))
static void stage_post(unsigned rc)
{
    stage_hook_fired[stage_cur] = 1;
    printf("[stage %u] leave rc=0x%x\n", stage_cur, rc);
}

/* the continuation body, entered from the original's return with
   rc in r0 and sp = S (the parked caller lr sits at S-4) */
__asm__(
    ".text\n"
    ".thumb\n"
    ".thumb_func\n"
    ".hidden stage_cont\n"
    "stage_cont:\n"
    "  push {r0, r1}\n"
    "  bl   stage_post\n"
    "  ldr  r0, [sp]\n"
    "  add  sp, #8\n"
    "  ldr  r1, [sp, #-4]\n"
    "  bx   r1\n");

#else /* HOST_ARGTEST: the parser in main is still exercised */

static const void *stage_hook_syms[STAGE_SLOTS] = { 0 };
static unsigned stage_hook_orig[STAGE_SLOTS];
static unsigned stage_hook_fired[STAGE_SLOTS];
static unsigned stage_installed[STAGE_SLOTS];
static unsigned stage_installed_off[STAGE_SLOTS];
static unsigned stage_cur;

#endif /* HOST_ARGTEST */

/* the stage-3 gate field, printed after every init-chain step */
static void print_gate(const char *tag, unsigned hIsp)
{
    unsigned g;

    if (hIsp == 0)
        return;
    g = *(unsigned *)(hIsp + 0x1318);
    printf("%s: gate ctx1318=0x%x", tag, g);
    if (g != 0)
        printf(" obj0=0x%x", *(unsigned *)g);
    else
        printf(" obj0=null");
    printf("\n");
}

/* crash isolation: a stage that faults must name itself before the
   process goes -- the whole point of per-stage calls is knowing WHO
   failed. Unbuffered stdout plus a controlled _exit(70). */
static const char *stage_now;

/* module ranges for PC/LR attribution; scanned once at startup so the
   handler itself does no file I/O */
#ifndef HOST_ARGTEST
static unsigned mod_start[3], mod_end[3];
static const char *mod_names[3] = {
    "libnvisp_v3.so", "libnvrm.so", "libnvrm_graphics.so"
};

static void scan_modules(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];

    if (f == 0)
        return;
    while (fgets(line, sizeof(line), f) != 0) {
        unsigned lo, hi;
        int i;
        if (sscanf(line, "%x-%x", &lo, &hi) != 2)
            continue;
        for (i = 0; i < 3; i++) {
            if (strstr(line, mod_names[i]) == 0)
                continue;
            if (mod_end[i] == 0) {
                mod_start[i] = lo;
                mod_end[i] = hi;
            } else {
                if (lo < mod_start[i])
                    mod_start[i] = lo;
                if (hi > mod_end[i])
                    mod_end[i] = hi;
            }
        }
    }
    fclose(f);
}
#endif

static void hex_write(unsigned v)
{
    static const char dig[] = "0123456789abcdef";
    int i;

    for (i = 28; i >= 0; i -= 4) {
        char c = dig[(v >> i) & 0xf];
        write(1, &c, 1);
    }
}

/* which loaded module an address belongs to: PC inside libnvisp_v3 ->
   plain data deref (pc - base = file offset); PC == 0 -> call through
   a null function pointer, and LR says who called; PC inside
   libnvrm_graphics -> the fault is inside NvRmStream* */
static void print_named(unsigned a)
{
#ifndef HOST_ARGTEST
    int i;
    for (i = 0; i < 3; i++) {
        if (mod_end[i] != 0 && a >= mod_start[i] && a < mod_end[i]) {
            static const char lb[] = " (";
            write(1, lb, sizeof(lb) - 1);
            write(1, mod_names[i], strlen(mod_names[i]));
            write(1, "+0x", 3);
            hex_write(a - mod_start[i]);
            write(1, ")", 1);
            return;
        }
    }
#else
    (void)a;
#endif
}

static void stage_segv(int sig, siginfo_t *info, void *uc)
{
    static const char m1[] = "[stage-crash] ";
    static const char m2[] = " CRASHED (fatal signal) fault-addr=0x";
    static const char nl[] = "\n";
#ifndef HOST_ARGTEST
    ucontext_t *u = (ucontext_t *)uc;
    static const char rlbl[] = " pc=0x";
    static const char lbl2[] = " lr=0x";
    static const char lbl3[] = " sp=0x";
    static const char lbl0[] = " r0=0x";
    static const char lbl1[] = " r1=0x";
    static const char lbl2x[] = " r2=0x";
    static const char lbl3x[] = " r3=0x";
    unsigned pc = u != 0 ? u->uc_mcontext.arm_pc : 0;
    unsigned lr = u != 0 ? u->uc_mcontext.arm_lr : 0;
    unsigned sp = u != 0 ? u->uc_mcontext.arm_sp : 0;
    unsigned r0 = u != 0 ? u->uc_mcontext.arm_r0 : 0;
    unsigned r1 = u != 0 ? u->uc_mcontext.arm_r1 : 0;
    unsigned r2 = u != 0 ? u->uc_mcontext.arm_r2 : 0;
    unsigned r3 = u != 0 ? u->uc_mcontext.arm_r3 : 0;
#else
    (void)info;
    (void)uc;
#endif
    static const char m3[] = " -- controlled exit\n";
    static const char dig[] = "0123456789abcdef";
    unsigned a = info != 0 ? (unsigned)(unsigned long)info->si_addr : 0;
    int i;

    write(1, m1, sizeof(m1) - 1);
    if (stage_now != 0)
        write(1, stage_now, strlen(stage_now));
    write(1, m2, sizeof(m2) - 1);
    for (i = 28; i >= 0; i -= 4) {
        char c = dig[(a >> i) & 0xf];
        write(1, &c, 1);
    }
#ifndef HOST_ARGTEST
    write(1, rlbl, sizeof(rlbl) - 1);
    hex_write(pc);
    print_named(pc);
    if (pc == 0) {
        static const char z[] = " [call through NULL]";
        write(1, z, sizeof(z) - 1);
    }
    write(1, lbl2, sizeof(lbl2) - 1);
    hex_write(lr);
    print_named(lr);
    write(1, lbl3, sizeof(lbl3) - 1);
    hex_write(sp);
    write(1, lbl0, sizeof(lbl0) - 1);
    hex_write(r0);
    write(1, lbl1, sizeof(lbl1) - 1);
    hex_write(r1);
    write(1, lbl2x, sizeof(lbl2x) - 1);
    hex_write(r2);
    write(1, lbl3x, sizeof(lbl3x) - 1);
    hex_write(r3);
#endif
    write(1, nl, sizeof(nl) - 1);
    write(1, m3, sizeof(m3) - 1);
    _exit(70);
}

/*
 * Syncpoint visibility. The kernel ioctl is source-verified
 * (include/linux/nvhost_ioctl.h):
 *   NVHOST_IOCTL_CTRL_SYNCPT_READ = _IOWR('H', 1,
 *       struct nvhost_ctrl_syncpt_read_args { u32 id; u32 value; })
 * The divergence (min != max in dmesg) is otherwise invisible: debugfs
 * is closed on the shipped kernel. Printed on EVERY run -- divergence
 * is a validity condition of everything else we measure.
 */
static int sp_fd = -1;

static int read_syncpt(unsigned id, unsigned *value)
{
    unsigned args[2] = { id, 0 };

    if (sp_fd < 0)
        return -1;
    if (ioctl(sp_fd, 0xC0084801u, args) != 0)
        return -1;
    *value = args[1];
    return 0;
}

static void print_syncpts(const char *tag, unsigned *out /* may be 0 */)
{
    static const unsigned ids[4] = { 32, 33, 34, 35 };
    static const char *nm[4] = {
        "ispa_memory", "ispa_stats", "ispa_stream", "ispa_loadv"
    };
    int i;

    printf("%s:", tag);
    for (i = 0; i < 4; i++) {
        unsigned v = 0;
        if (read_syncpt(ids[i], &v) == 0) {
            printf(" %s(%u)=%u", nm[i], ids[i], v);
            if (out != 0)
                out[i] = v;
        } else {
            printf(" %s(%u)=ERR", nm[i], ids[i]);
        }
    }
    printf("\n");
}

/*
 * Stock settings replay. The shim dump (mail-1045) writes lines
 *   id=<block> size=<bytes> buf=<hex32 words, comma separated>
 * to /data/local/tmp/stock_settings.txt. ispinit loads that file and,
 * per-block key on, replays the real values instead of zeros.
 * Stats buffers replay from the same file format (id = stats type).
 */
#define MAX_BLK_ID 16
#define MAX_STAT_TYPE 4
static unsigned blk_val[MAX_BLK_ID + 1][44], blk_size[MAX_BLK_ID + 1];
static int blk_have[MAX_BLK_ID + 1], blk_on[MAX_BLK_ID + 1];
static unsigned st_val[MAX_STAT_TYPE + 1][32], st_size[MAX_STAT_TYPE + 1];
static int st_have[MAX_STAT_TYPE + 1], st_on[MAX_STAT_TYPE + 1];

static const char *stock_file = "/data/local/tmp/stock_settings.txt";

static void load_stock_settings(void)
{
    FILE *f = fopen(stock_file, "r");
    char line[1024];
    int blocks = 0, stats = 0;

    if (f == 0) {
        printf("stock settings file: absent -- zeros will be used\n");
        return;
    }
    while (fgets(line, sizeof(line), f) != 0) {
        unsigned id, size;
        const char *b;
        if (sscanf(line, "id=%u size=%u", &id, &size) != 2)
            continue;
        b = strstr(line, "buf=");
        if (b == 0)
            continue;
        b += 4;
        if (id <= MAX_BLK_ID) {
            unsigned *dst = blk_val[id];
            unsigned n = 0;
            while (*b != '\0' && *b != '\n' && n < 44) {
                dst[n++] = (unsigned)strtoul(b, 0, 16);
                while (*b != ',' && *b != '\0' && *b != '\n')
                    b++;
                if (*b == ',')
                    b++;
            }
            blk_size[id] = size;
            blk_have[id] = 1;
            blocks++;
        } else if (id >= 1 && id <= MAX_STAT_TYPE) {
            unsigned *dst = st_val[id];
            unsigned n = 0;
            while (*b != '\0' && *b != '\n' && n < 32) {
                dst[n++] = (unsigned)strtoul(b, 0, 16);
                while (*b != ',' && *b != '\0' && *b != '\n')
                    b++;
                if (*b == ',')
                    b++;
            }
            st_size[id] = size;
            st_have[id] = 1;
            stats++;
        }
    }
    fclose(f);
    printf("stock settings file: %d blocks, %d stats types loaded\n",
           blocks, stats);
}

/* the nested float table for SetStats type 4 (+0x10 field points here):
   256 words of slack, zeroed -- the real table size is unknown; the
   first run's goal is only that the +0x10 deref stops faulting */
static unsigned stat4_floats[256];

/* obj state after a SetStats step: the gate pointer, its first word,
   and the +0x1660 window that feeds the memory-write configuration.
   obj[0] is a BITMASK (type1->bit0, 2->bit2, 3->bit4, 4->bit5;
   stock 0x37 = all four loaded), and the library memcmps: rc=0 means
   "nothing changed" as often as "accepted". The CHANGED list is the
   real signal -- rc alone cannot tell a write from a no-op. */
static unsigned last_obj0, last_win[16];
static int last_valid;

static void print_obj_state(const char *tag, unsigned hIsp)
{
    unsigned g = *(unsigned *)(hIsp + 0x1318);
    unsigned o0 = g != 0 ? *(unsigned *)g : 0;
    unsigned win[16];
    int i, changed = 0;

    if (hIsp == 0)
        return;
    printf("%s: ctx1318=0x%x obj0=0x%x\n", tag, g, o0);
    printf("%s: obj+0x1660:", tag);
    for (i = 0; i < 16; i++) {
        win[i] = g != 0 ? *(unsigned *)(g + 0x1660 + i * 4) : 0;
        printf(" +%x:%08x", 0x1660 + i * 4, win[i]);
    }
    if (last_valid) {
        if (o0 != last_obj0) {
            printf(" | obj0 was 0x%x", last_obj0);
            changed = 1;
        }
        for (i = 0; i < 16; i++)
            if (win[i] != last_win[i]) {
                printf(" | +%x was 0x%x", 0x1660 + i * 4, last_win[i]);
                changed = 1;
            }
        if (changed == 0)
            printf(" | no changes in the observed area");
    } else {
        printf(" | first sample");
    }
    printf("\n");
    last_obj0 = o0;
    for (i = 0; i < 16; i++)
        last_win[i] = win[i];
    last_valid = 1;
}

/* settings-object diff: settings blocks write into the object AT
   hset, not into the +0x1660 window (that belongs to SetStats). Only
   CHANGED words print; the label states only what was measured --
   "memcmp no-op" would be a claim we have not tested here. */
static unsigned set_snap[256];
static int set_snap_valid;

static void set_snap_take(unsigned hset)
{
    int i;
    for (i = 0; i < 256; i++)
        set_snap[i] = *(unsigned *)(hset + i * 4);
    set_snap_valid = 1;
}

static void print_set_diff(const char *tag, unsigned hset)
{
    int i, c = 0;

    if (hset == 0)
        return;
    if (set_snap_valid == 0)
        set_snap_take(hset);
    printf("%s:", tag);
    for (i = 0; i < 256; i++) {
        unsigned cur = *(unsigned *)(hset + i * 4);
        if (cur != set_snap[i]) {
            printf(" +%03x:%08x(was %08x)", i * 4, cur, set_snap[i]);
            set_snap[i] = cur;
            c++;
        }
    }
    if (c == 0)
        printf(" no changes in the observed area (256 words)");
    printf("\n");
}

int main(int argc, char **argv)
{
    void *nvrm;
    void *nvisp;
    NvRmOpen_fn nvRmOpen;
    NvRmMemHandleCreate_fn nvRmMemHandleCreate;
    NvRmMemHandleAllocAttr_fn nvRmMemHandleAllocAttr;
    NvRmMemWriteStrided_fn nvRmMemWriteStrided;
    NvRmMemReadStrided_fn nvRmMemReadStrided;
    NvRmMemRW_fn nvRmMemWrite;
    NvRmMemRW_fn nvRmMemRead;
    NvIspOpen_fn nvIspOpen;
    NvIspSetConfiguration_fn nvIspSetConfiguration;
    NvIspHwSettingsCreate_fn nvIspHwSettingsCreate;
    NvIspHwSettingsSetAttribute_fn nvIspHwSettingsSetAttribute;
    NvIspSetStats_fn nvIspSetStats;
    NvIspHwSettingsApply_fn nvIspHwSettingsApply;
    NvIspHwSettingsDestroy_fn nvIspHwSettingsDestroy;
    NvIspSetAttribute_fn nvIspSetAttribute;
    NvRmMemGetAddress_fn nvRmMemGetAddress;
    NvRmMemPin_fn nvRmMemPin;
    NvIspProcessFrame_fn nvIspProcessFrame;
    NvIspSetIspClockRate_fn nvIspSetIspClockRate;
    NvIspGetStatus_fn nvIspGetStatus;
    NvIspClose_fn nvIspClose;
    void *dev = 0;
    unsigned hIsp = 0;
    unsigned value = 0;
    unsigned size = 4;
    unsigned rate = 0x003fffffu;
    unsigned order;
    unsigned a1_val;         /* AllocAttr's first argument override; the
                                 working value is the device VALUE (1 in
                                 our runs), used when unset */
    int a1_set = 0;
    /* round-trip control knobs, rt=<key>:<val> on the command line */
    int rt_on = 1;
    unsigned rt_w = 8, rt_h = 8;          /* pixels */
    unsigned rt_bs = 0x100, rt_ms = 0x100;/* host / nvmap row stride */
    unsigned rt_wb = 0, rt_nr = 0;        /* 0 = follow w*h and h */
    unsigned rt_off = 0;                  /* offset into the nvmap buffer */
    unsigned rt_x8 = 0;                   /* 0 = auto: memStride*numRows */
    /* simple (hops) is the DEFAULT: it is the verified-working path;
       the strided form stays available under rt=strided for signature
       re-checks. Its rc=0x100 rejection on every run was pure noise. */
    int rt_simple = 1;
    unsigned pf_mode = 1;   /* ProcessFrame's 2nd arg: mode=<val>.
                               Default 1 -- the memory path is the goal;
                               2 is the stock's sensor mode. The mode=2
                               control run returned 0xa: modes differ. */
    int slot14_aux = 0;     /* slot14=aux: put a plain aux buffer into
                               +0x14 instead of the output descriptor
                               (impl-2 once called it a Flush token --
                               never tested) */
    unsigned wait_ms = 0;                 /* wait=<ms>: delay before the
                                             post-submit output read */
    int obj0_set = 0;
    unsigned obj0_val = 0;
    /* objout: write the OUTPUT surface handle into the frame-record
       relocation field -- the only path a surface address can reach
       the hardware through (libnvisp_v3 has no physical-address
       imports at all, impl-2) */
    int objout_on = 0;
    int objout_manual = 0;
    unsigned objout_val = 0;
    /* objload=<path>: clone the stock gate object from a file captured
       by objread, into OUR object after Open (the object lives in our
       own address space, plain memcpy). objlen caps the write. */
    int objload_on = 0;
    const char *objload_path = "/data/local/tmp/stock_obj.bin";
    unsigned objlen = 0x1000; /* start small; 0x4000 is the full dump */
    int objdump_on = 0;
    const char *objdump_path = "/data/local/tmp/our_obj.bin";
    int pin_on = 0;        /* pin=on: pin the output buffer */
    int outaddr_on = 0;    /* outaddr=on|<off>:on|<hex> */
    int outaddr_manual = 0;
    unsigned outaddr_val = 0;
    unsigned outaddr_off = 0x1674; /* rec0 last word */
    unsigned out_devaddr = 0;
    int out_devaddr_valid = 0;
    /* outblock=on: arm /proc/isp_patch_override with the missing
       INCR(0xE04,3) output-plane block. Requires kernel tracing ON
       (isp_patch counts submits inside the trace hook). */
    int outblock_on = 0;
    unsigned outblock_submit = 6;   /* our frame is submit #6 */
    unsigned outblock_off = 0x1674; /* rec0 last word: device address */
    unsigned outblock_addr = 0;     /* override the pinned address */
    int outblock_addr_set = 0;

    int objdump0_on = 0;
    const char *objdump0_path = "/data/local/tmp/our_obj_early.bin";
    unsigned objset_off[16], objset_val[16];
    int objset_n = 0;
    unsigned final_s = 0;   /* final=<sec>: settle-wait, then re-read
                               syncpoints -- timeouts land late */

    unsigned retry_n = 1;                 /* submissions per run */
    unsigned a10_val[44], a14_val[44];
    unsigned params_fill = 0x3f000000u; /* params filler word, fill= key */
    unsigned char a10_set[44] = {{0}}, a14_set[44] = {{0}};
    unsigned st_fill_w[5] = {0};   /* stat=<type>:fill:<word> */
    int st_fill_g[5] = {0};        /* fill given for this type */
    int st4_on = 0;                /* type 4 faults by default */
    unsigned sf_val[256];                 /* sf=<idx>:<val> float words */
    unsigned char sf_set[256] = {{0}};
    unsigned rt_size = 0;                 /* simple-mode byte count */
    char rt_order[5] = "hops";            /* simple-mode argument order:
        NvRmMemWrite(handle, offset, ptr, size) -- found by enumerating
        the six h-first permutations, hpos/hosp/hpso/hsop refused,
        hops PASSED at 64/512/2048 bytes byte-identical */
    unsigned attr_val[8];
    unsigned attr_set[8] = {0};
    /* seven stack slots: +00 +04 +08 +0c +18 +1c +20. The first four
       default OFF (zeros -- the historical behavior, their role is the
       open question), the last three default ON. */
    int aux_on[7] = {0, 0, 0, 0, 1, 1, 1};
    unsigned aux_val[7][44];
    unsigned char aux_set[7][44] = {{0}};
    unsigned char aux_isptr[7][44] = {{0}}; /* value was 'ptr' */
    unsigned cfg_val[16];                 /* cfg=<idx>:<val> overrides */
    unsigned char cfg_set[16] = {{0}};
    unsigned cfg2_val = cfg_mode2;        /* mode-2 word, stock value */
    int cfg2_set = 0;
    unsigned nvisp_base;
    unsigned hset = 0;   /* settings handle, if Create produces one */
    /* where the handle comes from: p1 = Create's p1[0] VALUE (measured
       -- the library wrote a heap pointer there); p2a = the address
       &p2[1], the earlier theory, kept for enumeration; or an explicit
       hex value */
    char hset_mode[8] = "p1";
    unsigned hset_manual = 0;
    /* stock init-chain steps, each toggleable: init=<name>:off */
    /* per-slot content keys, independent: slot08/slot10/slot14/slot18
       = descin | descout | descnohandle | buf | zero */
    char slot_kind[4][16] = {
        "descnohandle", "descin", "descout", "buf"
    };
    unsigned sp_pre[4];  /* syncpoint values before the submission */
    int st_create = 1, st_sattr = 1, st_stats = 1;
    unsigned inst = 1;    /* inst=<n>: ISP instance; stock opens 1 and 2.
                             isp.0 (instance 1, rear) is dead on this
                             kernel; isp.1 (front) works. */
    unsigned cfgmode1_arg = 1;  /* cfgmode1=<n>: SetConfiguration mode 1 */
    unsigned cfgmode2_arg = 2;  /* cfgmode2=<n>: SetConfiguration mode 2 */
    unsigned create_size = 0x24;   /* createsize=: HwSettingsCreate size */
    unsigned status_id = 6;        /* status=<n>: GetStatus id */
    unsigned apply_a2 = 5, apply_a4 = 0;  /* apply=<a2>:<a4> */
    unsigned setattr_id = 4;       /* setattr=<n>: SetAttribute id */
    int st_apply = 1, st_setattr = 1, st_destroy = 1;
    unsigned din_val[44], dout_val[44];  /* descriptor word overrides */
    /* ctx=<off>:<count> -- ISP-context dumps, default none */
    unsigned ctx_off[4], ctx_cnt[4];
    int ctx_n = 0;
    unsigned ctxp_off[4], ctxp_cnt[4];
    int ctxp_n = 0;
    unsigned stage_off[STAGE_SLOTS];
    int stage_n = 0;
    /* per-stage submission: stages=0 whole call (default), stages=1234
       runs stages individually; pkt2/pkt3 are ProcessFrame's own
       register arguments 3 and 4 */
    char stages_spec[8] = "0";
    unsigned pkt2 = 0, pkt3 = 0;
    struct sigaction sa;
    /* n<slot>=<val> -- put the NUMBER itself into stack slot +00/+04/
       +08/+0c (the live log shows the stock passes sensor geometry
       there as numbers: 3280/2460). Defaults for +08/+0c follow the
       frame geometry; +00/+04 default to zero. */
    unsigned n_val[4];
    int n_set[4] = {0, 0, 0, 0};
    unsigned char din_set[44] = {{0}};
    unsigned char dout_set[44] = {{0}};
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);

    /*
     * Optional rate argument: "ispinit [rate]". Control experiment -- the
     * first run read 600000 (0x927c0) after we asked for the max, which
     * proves life only if a smaller request comes back as requested.
     * strtol, no frills; anything unparseable keeps the default.
     */
    if (argc > 1) {
        char *end = 0;
        long v = strtol(argv[1], &end, 0);
        if (end != argv[1] && *end == '\0' && v > 0)
            rate = (unsigned)v;
    }

    /* mosaic order, CLI-switchable like the rate: our sensor is SRGGB10
       per the kernel, but the ISP-visible order depends on readout
       mirroring, so we will be cycling through them. Default RGGB when
       the argument is absent; an explicit but unknown name is an error. */
    order = 0;
    if (argc > 2) {
        int k, found = 0;
        for (k = 0; k < 4; k++) {
            if (strcmp(argv[2], order_names[k]) == 0) {
                order = (unsigned)k;
                found = 1;
            }
        }
        if (!found) {
            printf("[0] bad order '%s', use rggb|bggr|grbg|gbrg\n", argv[2]);
            return 1;
        }
    }

    /* [attr=]idx:val allocation-attribute overrides and the buffer SIZE,
       several overrides allowed (hex or decimal). A token WITH ':' is an
       attribute override (idx 0..7; attrs[0] is a pointer -- overridable
       but almost certainly wrong); a token WITHOUT ':' is the buffer
       size. Unspecified values stay per the recipe. */
    for (int ai = 3; ai < argc; ai++) {
        char *tok = argv[ai];
        char *colon;
        char *e1 = 0, *e2 = 0;
        long idx, val;

        if (strncmp(tok, "rt=", 3) == 0) {
            /* rt=on|off, or rt=<key>:<val> with keys
               w,h (pixels), bs (host row stride), ms (nvmap row
               stride), wb (widthBytes), nr (numRows), off (nvmap
               offset), x8 (unknown 8th arg; 0 = auto ms*nr) */
            char *rest = tok + 3;
            char *colon;
            char *e3 = 0;
            long v;
            unsigned *dst = 0;
            if (strcmp(rest, "on") == 0) { rt_on = 1; continue; }
            if (strcmp(rest, "off") == 0) { rt_on = 0; continue; }
            if (strcmp(rest, "simple") == 0) { rt_simple = 1; continue; }
            if (strcmp(rest, "strided") == 0) { rt_simple = 0; continue; }
            if (strncmp(rest, "order:", 6) == 0) {
                /* simple-mode argument order, a permutation of hpos:
                   h=handle p=ptr o=offset s=size. Lives BEFORE the
                   generic numeric path: the value is a word, not a
                   number, and the colon must survive for the match. */
                const char *o = rest + 6;
                if (strlen(o) != 4 || strspn(o, "hpos") != 4 ||
                    o[0] == o[1] || o[0] == o[2] || o[0] == o[3] ||
                    o[1] == o[2] || o[1] == o[3] || o[2] == o[3]) {
                    printf("[0] bad rt order '%s', permutation of hpos\n",
                           o);
                    return 1;
                }
                memcpy(rt_order, o, 5);
                continue;
            }
            colon = strchr(rest, ':');
            if (colon == 0) {
                printf("[0] bad rt '%s', use rt=on|off or rt=<key>:<val>\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            v = strtol(colon + 1, &e3, 0);
            if (e3 == colon + 1 || *e3 != '\0' || v < 0) {
                printf("[0] bad rt value in '%s'\n", argv[ai]);
                return 1;
            }
            if (strcmp(rest, "size") == 0)
                dst = &rt_size;
            else if (strcmp(rest, "w") == 0) dst = &rt_w;
            else if (strcmp(rest, "h") == 0) dst = &rt_h;
            else if (strcmp(rest, "bs") == 0) dst = &rt_bs;
            else if (strcmp(rest, "ms") == 0) dst = &rt_ms;
            else if (strcmp(rest, "wb") == 0) dst = &rt_wb;
            else if (strcmp(rest, "nr") == 0) dst = &rt_nr;
            else if (strcmp(rest, "off") == 0) dst = &rt_off;
            else if (strcmp(rest, "x8") == 0) dst = &rt_x8;
            else {
                printf("[0] bad rt key '%s'\n", argv[ai]);
                return 1;
            }
            *dst = (unsigned)v;
            continue;
        }
        if (tok[0] == 'a' && tok[3] == '=' &&
            ((tok[1] >= '0' && tok[1] <= '9') ||
             (tok[1] >= 'a' && tok[1] <= 'f')) &&
            ((tok[2] >= '0' && tok[2] <= '9') ||
             (tok[2] >= 'a' && tok[2] <= 'f'))) {
            /* a<slot>=<idx>:<val|ptr> -- set one word of an aux buffer
               (slot two hex digits: 00 04 08 0c 18 1c 20); several
               allowed; idx decimal or hex, 0..43; 'ptr' puts the
               address of a shared zeroed buffer into the word (for the
               stock's pointer at +14 of the +1c structure) */
            static const unsigned aux_slots[7] = {
                0x00, 0x04, 0x08, 0x0c, 0x18, 0x1c, 0x20
            };
            char *colon;
            long idx, val, slot;
            int which = -1;
            for (int wi = 0; wi < 7; wi++)
                if (aux_slots[wi] ==
                    (unsigned)strtol(tok + 1, 0, 16))
                    which = wi;
            if (which < 0) {
                printf("[0] unknown aux slot in '%s'\n", argv[ai]);
                return 1;
            }
            slot = 0; /* reserved */
            (void)slot;
            colon = strchr(tok + 4, ':');
            if (colon == 0) {
                printf("[0] bad aux word '%s', use a<slot>=<idx>:<val|ptr>\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            idx = strtol(tok + 4, &e1, 0);
            if (e1 == tok + 4 || *e1 != '\0' || idx < 0 || idx > 43) {
                printf("[0] bad aux word index '%s'\n", argv[ai]);
                return 1;
            }
            if (strcmp(colon + 1, "ptr") == 0) {
                aux_isptr[which][idx] = 1;
                aux_set[which][idx] = 1;
            } else {
                val = strtol(colon + 1, &e2, 0);
                if (e2 == colon + 1 || *e2 != '\0') {
                    printf("[0] bad aux word value '%s'\n", argv[ai]);
                    return 1;
                }
                aux_val[which][idx] = (unsigned)val;
                aux_set[which][idx] = 1;
            }
            continue;
        }
        if (strncmp(tok, "slot08=", 7) == 0 || strncmp(tok, "slot10=", 7) == 0 ||
            strncmp(tok, "slot14=", 7) == 0 || strncmp(tok, "slot18=", 7) == 0) {
            /* slot<NN>=<kind>, independent per slot:
               descin (surface descriptor with input handle),
               descout (surface descriptor with output handle),
               descnohandle (frame description, no memory),
               buf (zeroed buffer), zero (pass NULL) */
            static const char kinds[6][14] = {
                "descin", "descout", "descnohandle", "buf", "zero",
                "params"
            };
            int which = (tok[4] == '0') ? 0 : (tok[4] == '1') ?
                        (tok[5] == '4' ? 2 : 3) : 1;
            int k2, hit = -1;
            const char *val = tok + 7;
            for (k2 = 0; k2 < 6; k2++)
                if (strcmp(val, kinds[k2]) == 0)
                    hit = k2;
            if (hit < 0) {
                printf("[0] bad slot kind '%s', use descin|descout|"
                       "descnohandle|buf|zero\n", argv[ai]);
                return 1;
            }
            snprintf(slot_kind[which], sizeof(slot_kind[0]), "%s",
                     kinds[hit]);
            continue;
        }
        if (strncmp(tok, "mode=", 5) == 0) {
            char *e6 = 0;
            long v = strtol(tok + 5, &e6, 0);
            if (e6 == tok + 5 || *e6 != '\0' || v < 0) {
                printf("[0] bad mode '%s', use mode=<val>\n", argv[ai]);
                return 1;
            }
            pf_mode = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "slot14=", 7) == 0) {
            if (strcmp(tok + 7, "aux") == 0)
                slot14_aux = 1;
            else if (strcmp(tok + 7, "desc") == 0)
                slot14_aux = 0;
            else {
                printf("[0] bad slot14 '%s', use slot14=desc|aux\n",
                       argv[ai]);
                return 1;
            }
            continue;
        }
        if (strncmp(tok, "ctx=", 4) == 0) {
            /* ctx=<off>:<count> -- dump <count> words of the ISP
               context from <off>; several regions allowed, up to four.
               The context is ours, valid between Open and Close,
               read-only. Default: no dump. */
            char *colon;
            char *e5 = 0;
            long off, cnt;
            colon = strchr(tok + 4, ':');
            if (colon == 0) {
                printf("[0] bad ctx '%s', use ctx=<off>:<count>\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            off = strtol(tok + 4, &e5, 0);
            cnt = strtol(colon + 1, &e5, 0);
            if (e5 == colon + 1 || *e5 != '\0' || off < 0 || cnt <= 0 ||
                cnt > 256) {
                printf("[0] bad ctx region in '%s' (count 1..256)\n",
                       argv[ai]);
                return 1;
            }
            if (ctx_n == 4) {
                printf("[0] too many ctx regions (max 4)\n");
                return 1;
            }
            ctx_off[ctx_n] = (unsigned)off;
            ctx_cnt[ctx_n] = (unsigned)cnt;
            ctx_n++;
            continue;
        }
        if (strncmp(tok, "n08=", 4) == 0 || strncmp(tok, "n0c=", 4) == 0 ||
            strncmp(tok, "n00=", 4) == 0 || strncmp(tok, "n04=", 4) == 0) {
            /* numeric slot value, see n_val above */
            char *e10 = 0;
            long v;
            int which = (tok[1] == '0') ? (tok[2] == '0' ? 0 : tok[2] == '4' ? 1 : 2)
                                        : 3;
            v = strtol(tok + 4, &e10, 0);
            if (e10 == tok + 4 || *e10 != '\0' || v < 0) {
                printf("[0] bad n-slot '%s', use n<slot>=<number>\n",
                       argv[ai]);
                return 1;
            }
            n_val[which] = (unsigned)v;
            n_set[which] = 1;
            continue;
        }
        if (strncmp(tok, "stage=", 6) == 0) {
            /* stage=<off>:on -- replace the handler-table entry at
               ctx+<off> with one of our hooks (prints args, calls the
               original, prints the return code, returns it unchanged).
               Several entries allowed; enumeration of the table is the
               point, so any word-aligned offset is accepted. */
            char *colon;
            char *e8 = 0;
            long off;
            colon = strchr(tok + 6, ':');
            if (colon == 0 || strcmp(colon + 1, "on") != 0) {
                printf("[0] bad stage '%s', use stage=<off>:on\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            off = strtol(tok + 6, &e8, 0);
            if (e8 == tok + 6 || *e8 != '\0' || off < 0 || (off & 3) != 0) {
                printf("[0] bad stage offset in '%s' (word-aligned)\n",
                       argv[ai]);
                return 1;
            }
            if (stage_n == STAGE_SLOTS) {
                printf("[0] too many stage hooks (max %d)\n", STAGE_SLOTS);
                return 1;
            }
            stage_off[stage_n++] = (unsigned)off;
            continue;
        }
        if (strncmp(tok, "ctxp=", 5) == 0) {
            /* ctxp=<off>:<count> -- take the WORD at ctx+<off>, treat
               it as an address, print <count> words from there. One
               level of dereference only; a null pointer prints as null
               instead of crashing. The pointer comes from our own
               context, valid between Open and Close. */
            char *colon;
            char *e7 = 0;
            long off, cnt;
            colon = strchr(tok + 5, ':');
            if (colon == 0) {
                printf("[0] bad ctxp '%s', use ctxp=<off>:<count>\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            off = strtol(tok + 5, &e7, 0);
            cnt = strtol(colon + 1, &e7, 0);
            if (e7 == colon + 1 || *e7 != '\0' || off < 0 || cnt <= 0 ||
                cnt > 256) {
                printf("[0] bad ctxp region in '%s' (count 1..256)\n",
                       argv[ai]);
                return 1;
            }
            if (ctxp_n == 4) {
                printf("[0] too many ctxp regions (max 4)\n");
                return 1;
            }
            ctxp_off[ctxp_n] = (unsigned)off;
            ctxp_cnt[ctxp_n] = (unsigned)cnt;
            ctxp_n++;
            continue;
        }
        if (strncmp(tok, "hset=", 5) == 0) {
            if (strcmp(tok + 5, "p1") == 0) {
                snprintf(hset_mode, sizeof(hset_mode), "p1");
            } else if (strcmp(tok + 5, "p2a") == 0) {
                snprintf(hset_mode, sizeof(hset_mode), "p2a");
            } else {
                char *e13 = 0;
                long v = strtol(tok + 5, &e13, 0);
                if (e13 == tok + 5 || *e13 != '\0' || v <= 0) {
                    printf("[0] bad hset '%s', use hset=p1|p2a|<hex>\n",
                           argv[ai]);
                    return 1;
                }
                snprintf(hset_mode, sizeof(hset_mode), "manual");
                hset_manual = (unsigned)v;
            }
            continue;
        }
        if (strncmp(tok, "init=", 5) == 0) {
            /* init=<name>:on|off -- toggle a stock init-chain step.
               Names: create, sattr (14x HwSettingsSetAttribute),
               stats (4x SetStats), apply, setattr (the debug-flag
               SetAttribute), destroy. Default all on. */
            char *colon;
            char *e9 = 0;
            int *flag = 0;
            colon = strchr(tok + 5, ':');
            if (colon == 0) {
                printf("[0] bad init '%s', use init=<name>:on|off\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            if (strcmp(tok + 5, "create") == 0) flag = &st_create;
            else if (strcmp(tok + 5, "sattr") == 0) flag = &st_sattr;
            else if (strcmp(tok + 5, "stats") == 0) flag = &st_stats;
            else if (strcmp(tok + 5, "apply") == 0) flag = &st_apply;
            else if (strcmp(tok + 5, "setattr") == 0) flag = &st_setattr;
            else if (strcmp(tok + 5, "destroy") == 0) flag = &st_destroy;
            else {
                printf("[0] unknown init step '%s'\n", argv[ai]);
                return 1;
            }
            if (strcmp(colon + 1, "on") == 0)
                *flag = 1;
            else if (strcmp(colon + 1, "off") == 0)
                *flag = 0;
            else {
                printf("[0] bad init state '%s'\n", colon + 1);
                return 1;
            }
            continue;
        }
        if (strncmp(tok, "stages=", 7) == 0) {
            /* stages=0 whole ProcessFrame (default); stages=1234 runs
               stages individually, ascending, each printed separately */
            const char *sp = tok + 7;
            if (strcmp(sp, "0") == 0)
                continue;
            if (*sp == '\0') {
                printf("[0] empty stages spec\n");
                return 1;
            }
            {
                const char *q = sp;
                char prev = '0';
                while (*q) {
                    if (*q < '1' || *q > '4' || *q <= prev) {
                        printf("[0] bad stages '%s': ascending digits "
                               "1..4, or 0\n", sp);
                        return 1;
                    }
                    prev = *q;
                    q++;
                }
            }
            snprintf(stages_spec, sizeof(stages_spec), "%s", sp);
            continue;
        }
        if (strncmp(tok, "pkt2=", 5) == 0) {
            char *e11 = 0;
            long v = strtol(tok + 5, &e11, 0);
            if (e11 == tok + 5 || *e11 != '\0' || v < 0) {
                printf("[0] bad pkt2 '%s'\n", argv[ai]);
                return 1;
            }
            pkt2 = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "pkt3=", 5) == 0) {
            char *e12 = 0;
            long v = strtol(tok + 5, &e12, 0);
            if (e12 == tok + 5 || *e12 != '\0' || v < 0) {
                printf("[0] bad pkt3 '%s'\n", argv[ai]);
                return 1;
            }
            pkt3 = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "final=", 6) == 0) {
            char *e16 = 0;
            long v = strtol(tok + 6, &e16, 0);
            if (e16 == tok + 6 || *e16 != '\0' || v < 0 || v > 60) {
                printf("[0] bad final '%s', use final=<seconds 0..60>\n",
                       argv[ai]);
                return 1;
            }
            final_s = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "blk=", 4) == 0 || strncmp(tok, "stat=", 5) == 0) {
            /* blk=<id>:on|off -- replay stock values for settings block
               <id> (needs the stock settings file on the device);
               stat=<type>:on|off|fill:<word> -- stats buffers: on/off
               replays stock values from the file, fill:<word> fills the
               WHOLE buffer with a nonzero word so the library's memcmp
               sees a change and sets the type's bit in obj[0] (zeros
               compare equal and the call is a silent no-op).
               Type 4 is DISABLED by default: it faults with our data
               (stat=4:on or stat=4:fill:<w> to enable). */
            int is_stat = (tok[1] == 't');
            char *colon;
            char *e18 = 0;
            long id;
            colon = strchr(tok + (is_stat ? 5 : 4), ':');
            if (colon == 0) {
                printf("[0] bad %s '%s', use <id>:on|off\n",
                       is_stat ? "stat" : "blk", argv[ai]);
                return 1;
            }
            *colon = '\0';
            id = strtol(tok + (is_stat ? 5 : 4), &e18, 0);
            if (e18 == tok + (is_stat ? 5 : 4) || *e18 != '\0' ||
                id < 1 || id > (is_stat ? MAX_STAT_TYPE : MAX_BLK_ID)) {
                printf("[0] bad id in '%s'\n", argv[ai]);
                return 1;
            }
            if (is_stat && strncmp(colon + 1, "fill:", 5) == 0) {
                char *e21 = 0;
                long fv = strtol(colon + 6, &e21, 0);
                if (e21 == colon + 6 || *e21 != '\0' || fv < 0) {
                    printf("[0] bad fill value in '%s'\n", argv[ai]);
                    return 1;
                }
                st_fill_w[id] = (unsigned)fv;
                st_fill_g[id] = 1;
                st_on[id] = 1;
                if (id == 4)
                    st4_on = 1;
                continue;
            }
            if (strcmp(colon + 1, "on") == 0) {
                if (is_stat) {
                    st_on[id] = 1;
                    if (id == 4)
                        st4_on = 1;
                } else
                    blk_on[id] = 1;
            } else if (strcmp(colon + 1, "off") == 0) {
                if (is_stat)
                    st_on[id] = 0;
                else
                    blk_on[id] = 0;
            } else {
                printf("[0] bad state '%s', use on|off|fill:<word>\n",
                       colon + 1);
                return 1;
            }
            continue;
        }
        if (strncmp(tok, "a10=", 4) == 0 || strncmp(tok, "a14=", 4) == 0) {
            /* a10=/a14=<idx>:<val> -- word overrides for the +10/+14
               slot buffers (same shape as a08=/a18=) */
            char *colon;
            char *e19 = 0;
            long idx, val;
            int is14 = (tok[1] == '1' && tok[2] == '4');
            colon = strchr(tok + 4, ':');
            if (colon == 0) {
                printf("[0] bad a10/a14 '%s', use <idx>:<val>\n", argv[ai]);
                return 1;
            }
            *colon = '\0';
            idx = strtol(tok + 4, &e19, 0);
            if (e19 == tok + 4 || *e19 != '\0' || idx < 0 || idx > 43) {
                printf("[0] bad index in '%s'\n", argv[ai]);
                return 1;
            }
            val = strtol(colon + 1, &e19, 0);
            if (e19 == colon + 1 || *e19 != '\0' || val < 0) {
                printf("[0] bad value in '%s'\n", argv[ai]);
                return 1;
            }
            if (is14) {
                a14_val[idx] = (unsigned)val;
                a14_set[idx] = 1;
            } else {
                a10_val[idx] = (unsigned)val;
                a10_set[idx] = 1;
            }
            continue;
        }
        if (strncmp(tok, "fill=", 5) == 0) {
            char *e20 = 0;
            long v = strtol(tok + 5, &e20, 0);
            if (e20 == tok + 5 || *e20 != '\0' || v < 0) {
                printf("[0] bad fill '%s'\n", argv[ai]);
                return 1;
            }
            params_fill = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "obj0=", 5) == 0) {
            /* obj0=<val> -- manually write this value into the first
               word of the object at [hIsp+0x1318] before the submission.
               Experiment: stage 4 issues the memory PushIncr only when
               that word is nonzero (the stock has 0x37 there, we 0).
               No default write -- only by key. */
            char *e14 = 0;
            long v = strtol(tok + 5, &e14, 0);
            if (e14 == tok + 5 || *e14 != '\0' || v < 0) {
                printf("[0] bad obj0 '%s', use obj0=<value>\n", argv[ai]);
                return 1;
            }
            obj0_set = 1;
            obj0_val = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "sf=", 3) == 0) {
            /* sf=<idx>:<val> -- one word of the SetStats type-4 nested
               float table; idx 0..255, decimal or hex */
            char *colon;
            char *e17 = 0;
            long idx, val;
            colon = strchr(tok + 3, ':');
            if (colon == 0) {
                printf("[0] bad sf '%s', use sf=<idx>:<val>\n", argv[ai]);
                return 1;
            }
            *colon = '\0';
            idx = strtol(tok + 3, &e17, 0);
            if (e17 == tok + 3 || *e17 != '\0' || idx < 0 || idx > 255) {
                printf("[0] bad sf index in '%s'\n", argv[ai]);
                return 1;
            }
            val = strtol(colon + 1, &e17, 0);
            if (e17 == colon + 1 || *e17 != '\0' || val < 0) {
                printf("[0] bad sf value in '%s'\n", argv[ai]);
                return 1;
            }
            sf_val[idx] = (unsigned)val;
            sf_set[idx] = 1;
            continue;
        }
        if (strncmp(tok, "retry=", 6) == 0) {
            /* retry=<n> -- on rc 0xa, resubmit the frame the same way
               (fix-and-repeat): the library writes what it wants into
               the +1c state buffer, and the resubmit hands it back */
            char *e15 = 0;
            long v = strtol(tok + 6, &e15, 0);
            if (e15 == tok + 6 || *e15 != '\0' || v < 1 || v > 10) {
                printf("[0] bad retry '%s', use retry=<1..10>\n", argv[ai]);
                return 1;
            }
            retry_n = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "objload=", 8) == 0) {
            objload_path = tok + 8;
            objload_on = 1;
            continue;
        }
        if (strncmp(tok, "objlen=", 7) == 0) {
            char *e23 = 0;
            long v = strtol(tok + 7, &e23, 0);
            if (e23 == tok + 7 || *e23 != '\0' || v < 0x40 || v > 0x4000) {
                printf("[0] bad objlen '%s', use 0x40..0x4000 bytes\n",
                       argv[ai]);
                return 1;
            }
            objlen = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "objdump=", 8) == 0) {
            /* objdump=<path> -- write OUR gate object (objlen bytes)
               to a file after the load/set operations, for the
               word-level diff against the stock capture */
            objdump_path = tok + 8;
            objdump_on = 1;
            continue;
        }
        if (strncmp(tok, "objdump0=", 9) == 0) {
            /* objdump0=<path> -- EARLY dump, right after Open/load,
               before the init chain: the state BEFORE statistics and
               the frame. The mail-1028 companion of objdump=. */
            objdump0_path = tok + 9;
            objdump0_on = 1;
            continue;
        }
        if (strncmp(tok, "objset=", 7) == 0) {
            /* objset=<off>:<val> -- write one word into our object at
               byte offset <off>; bounds-checked, word-aligned */
            char *colon;
            char *e24 = 0;
            long off, val;
            colon = strchr(tok + 7, ':');
            if (colon == 0) {
                printf("[0] bad objset '%s', use objset=<off>:<val>\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            off = strtol(tok + 7, &e24, 0);
            if (e24 == tok + 7 || *e24 != '\0' || off < 0 ||
                (off & 3) != 0 || off > 0x3ffc) {
                printf("[0] bad objset offset in '%s'\n", argv[ai]);
                return 1;
            }
            val = strtol(colon + 1, &e24, 0);
            if (e24 == colon + 1 || *e24 != '\0' || val < 0) {
                printf("[0] bad objset value in '%s'\n", argv[ai]);
                return 1;
            }
            if (objset_n < 16) {
                objset_off[objset_n] = (unsigned)off;
                objset_val[objset_n] = (unsigned)val;
                objset_n++;
            }
            continue;
        }
        if (strncmp(tok, "heapi=", 6) == 0 || strncmp(tok, "heapo=", 6) == 0) {
            /* heap selection per surface is NOT IMPLEMENTED: the attrs
               layout is unestablished (tag/value pairs vs fixed fields),
               and a guessed word crashed libnvrm. Kernel heap masks for
               reference: NVMAP_HEAP_IOVMM = 1<<30, CARVEOUT_GENERIC = 1,
               CARVEOUT_IVM = 2 (include/linux/nvmap.h). */
            printf("[0] heap selection not implemented -- see the attrs "
                   "layout note in the source\n");
            return 1;
        }
        if (strncmp(tok, "cfgmode1=", 9) == 0) {
            char *e30 = 0;
            long v = strtol(tok + 9, &e30, 0);
            if (e30 == tok + 9 || *e30 != '\0' || v < 0) { bad: printf("[0] bad value in '%s'\n", argv[ai]); return 1; }
            cfgmode1_arg = (unsigned)v; continue;
        }
        if (strncmp(tok, "cfgmode2=", 9) == 0) {
            char *e30 = 0;
            long v = strtol(tok + 9, &e30, 0);
            if (e30 == tok + 9 || *e30 != '\0' || v < 0) goto bad;
            cfgmode2_arg = (unsigned)v; continue;
        }
        if (strncmp(tok, "createsize=", 11) == 0) {
            char *e30 = 0;
            long v = strtol(tok + 11, &e30, 0);
            if (e30 == tok + 11 || *e30 != '\0' || v <= 0) goto bad;
            create_size = (unsigned)v; continue;
        }
        if (strncmp(tok, "status=", 7) == 0) {
            char *e30 = 0;
            long v = strtol(tok + 7, &e30, 0);
            if (e30 == tok + 7 || *e30 != '\0' || v < 0) goto bad;
            status_id = (unsigned)v; continue;
        }
        if (strncmp(tok, "apply=", 6) == 0) {
            char *colon9 = strchr(tok + 6, ':');
            char *e30 = 0;
            long v1, v2 = 0;
            if (colon9 == 0) goto bad;
            *colon9 = '\0';
            v1 = strtol(tok + 6, &e30, 0);
            if (e30 != colon9) goto bad;
            v2 = strtol(colon9 + 1, &e30, 0);
            if (v1 < 0 || v2 < 0) goto bad;
            apply_a2 = (unsigned)v1; apply_a4 = (unsigned)v2; continue;
        }
        if (strncmp(tok, "setattr=", 8) == 0) {
            char *e30 = 0;
            long v = strtol(tok + 8, &e30, 0);
            if (e30 == tok + 8 || *e30 != '\0' || v < 0) goto bad;
            setattr_id = (unsigned)v; continue;
        }
        if (strncmp(tok, "objout=", 7) == 0) {
            /* objout=on -- write the output nvmap handle into the
               frame-record relocation field; objout=<hex> -- write an
               arbitrary value there instead. Default off. */
            if (strcmp(tok + 7, "on") == 0) {
                objout_on = 1;
            } else {
                char *e32 = 0;
                long v = strtol(tok + 7, &e32, 0);
                if (e32 == tok + 7 || *e32 != '\0' || v <= 0) {
                    printf("[0] bad objout '%s', use objout=on|<hex>\n",
                           argv[ai]);
                    return 1;
                }
                objout_on = 1;
                objout_manual = 1;
                objout_val = (unsigned)v;
            }
            continue;
        }
        if (strncmp(tok, "pin=", 4) == 0) {
            /* pin=on -- pin the output buffer via NvRmMemPin. Pinning
               may be required on its own: unpinned memory has no
               device address at all. */
            if (strcmp(tok + 4, "on") == 0)
                pin_on = 1;
            else {
                printf("[0] bad pin '%s', use pin=on\n", argv[ai]);
                return 1;
            }
            continue;
        }
        if (strncmp(tok, "outaddr=", 8) == 0) {
            /* outaddr=on -- write the device address of our output
               buffer into the channel-record last word (rec0, +0x1674);
               outaddr=<off>:on -- same at an explicit offset (rec
               enumeration); outaddr=<hex> -- arbitrary value. */
            if (strcmp(tok + 8, "on") == 0) {
                outaddr_on = 1;
            } else {
                char *colon2 = strchr(tok + 8, ':');
                if (colon2 != 0 && strcmp(colon2 + 1, "on") == 0) {
                    char *e25 = 0;
                    long o2;
                    *colon2 = '\0';
                    o2 = strtol(tok + 8, &e25, 0);
                    if (e25 == tok + 8 || *e25 != '\0' || o2 < 0x400 ||
                        o2 > 0x3ffc || (o2 & 3) != 0) {
                        printf("[0] bad outaddr offset '%s'\n", argv[ai]);
                        return 1;
                    }
                    outaddr_off = (unsigned)o2;
                    outaddr_on = 1;
                } else {
                    char *e26 = 0;
                    long v2 = strtol(tok + 8, &e26, 0);
                    if (e26 == tok + 8 || *e26 != '\0' || v2 <= 0) {
                        printf("[0] bad outaddr '%s', use on|<off>:on|"
                               "<hex>\n", argv[ai]);
                        return 1;
                    }
                    outaddr_on = 1;
                    outaddr_manual = 1;
                    outaddr_val = (unsigned)v2;
                }
            }
            continue;
        }
        if (strncmp(tok, "inst=", 5) == 0) {
            /* inst=<n> -- ISP instance for NvIspOpen. Stock opens 1 and
               2 (isp.0 rear, isp.1 front); on this kernel only isp.1
               works. Default 1 for comparability with previous runs. */
            char *e29 = 0;
            long v = strtol(tok + 5, &e29, 0);
            if (e29 == tok + 5 || *e29 != '\0' || v < 1 || v > 4) {
                printf("[0] bad inst '%s', use 1..4\n", argv[ai]);
                return 1;
            }
            inst = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "outblock=", 9) == 0) {
            if (strcmp(tok + 9, "on") == 0)
                outblock_on = 1;
            else if (strcmp(tok + 9, "off") == 0)
                outblock_on = 0;
            else {
                printf("[0] bad outblock '%s', use on|off\n", argv[ai]);
                return 1;
            }
            continue;
        }
        if (strncmp(tok, "outsubmit=", 10) == 0) {
            char *e31 = 0;
            long v = strtol(tok + 10, &e31, 0);
            if (e31 == tok + 10 || *e31 != '\0' || v < 0) {
                printf("[0] bad outsubmit '%s'\n", argv[ai]);
                return 1;
            }
            outblock_submit = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "outaddr=", 8) == 0) {
            /* objout=on -- write the output nvmap handle into the
               frame-record relocation field; objout=<hex> -- write an
               arbitrary value there instead. Default off. */
            if (strcmp(tok + 7, "on") == 0) {
                objout_on = 1;
            } else {
                char *e22 = 0;
                long v = strtol(tok + 7, &e22, 0);
                if (e22 == tok + 7 || *e22 != '\0' || v <= 0) {
                    printf("[0] bad objout '%s', use objout=on|<hex>\n",
                           argv[ai]);
                    return 1;
                }
                objout_on = 1;
                objout_manual = 1;
                objout_val = (unsigned)v;
            }
            continue;
        }
        if (strncmp(tok, "wait=", 5) == 0) {
            /* wait=<ms> -- delay before the post-submit output read.
               A crutch for the async hypothesis: if the output content
               changes with delay and not without, processing is
               asynchronous and the real answer is a fence wait. */
            char *e4 = 0;
            long v = strtol(tok + 5, &e4, 0);
            if (e4 == tok + 5 || *e4 != '\0' || v < 0) {
                printf("[0] bad wait '%s', use wait=<milliseconds>\n",
                       argv[ai]);
                return 1;
            }
            wait_ms = (unsigned)v;
            continue;
        }
        if (strncmp(tok, "cfg2=", 5) == 0) {
            /* cfg2=<val> -- the single mode-2 configuration word
               (stock sends 2) */
            char *e5 = 0;
            long v = strtol(tok + 5, &e5, 0);
            if (e5 == tok + 5 || *e5 != '\0' || v < 0) {
                printf("[0] bad cfg2 '%s', use cfg2=<val>\n", argv[ai]);
                return 1;
            }
            cfg2_val = (unsigned)v;
            cfg2_set = 1;
            continue;
        }
        if (strncmp(tok, "cfg=", 4) == 0) {
            /* cfg=<idx>:<val> -- override one of the sixteen mode-1
               configuration words; defaults are the stock words
               (1,7,9,a,3,0,6,8,11,f,c,e,b,0,10,d). impl-2 read word 0
               from {0,1,2}: possibly the frame SOURCE for the session
               -- the stock only walks the sensor path, and our
               memory-mode submission may need a different value. */
            char *colon;
            long idx, val;
            colon = strchr(tok + 4, ':');
            if (colon == 0) {
                printf("[0] bad cfg '%s', use cfg=<idx>:<val>\n", argv[ai]);
                return 1;
            }
            *colon = '\0';
            idx = strtol(tok + 4, &e1, 0);
            if (e1 == tok + 4 || *e1 != '\0' || idx < 0 || idx > 15) {
                printf("[0] bad cfg index in '%s'\n", argv[ai]);
                return 1;
            }
            val = strtol(colon + 1, &e2, 0);
            if (e2 == colon + 1 || *e2 != '\0' || val < 0) {
                printf("[0] bad cfg value in '%s'\n", argv[ai]);
                return 1;
            }
            cfg_val[idx] = (unsigned)val;
            cfg_set[idx] = 1;
            continue;
        }
        if (strncmp(tok, "din=", 4) == 0 || strncmp(tok, "dout=", 5) == 0) {
            /* din=<idx>:<val>, dout=... -- descriptor word overrides;
               several allowed, idx 0..43. Word 5 (the memory handle) is
               PROGRAM-SET and rejected here: an override there is not a
               configurable, it is a broken run. */
            int is_out = (tok[1] == 'o');
            char *colon;
            long idx, val;
            colon = strchr(tok + (is_out ? 5 : 4), ':');
            if (colon == 0) {
                printf("[0] bad descriptor override '%s', use "
                       "din=<idx>:<val>\n", argv[ai]);
                return 1;
            }
            *colon = '\0';
            idx = strtol(tok + (is_out ? 5 : 4), &e1, 0);
            if (e1 == tok + (is_out ? 5 : 4) || *e1 != '\0' ||
                idx < 0 || idx > 43) {
                printf("[0] bad descriptor index in '%s'\n", argv[ai]);
                return 1;
            }
            if (idx == 5) {
                printf("[0] %s=5 rejected: the memory handle is "
                       "program-set\n", is_out ? "dout" : "din");
                return 1;
            }
            val = strtol(colon + 1, &e2, 0);
            if (e2 == colon + 1 || *e2 != '\0' || val < 0) {
                printf("[0] bad descriptor value in '%s'\n", argv[ai]);
                return 1;
            }
            if (is_out) {
                dout_val[idx] = (unsigned)val;
                dout_set[idx] = 1;
            } else {
                din_val[idx] = (unsigned)val;
                din_set[idx] = 1;
            }
            continue;
        }
        if (strncmp(tok, "aux=", 4) == 0) {
            /* aux=<hexslot>:on|off -- fill one of the three remaining
               stack slots (+18, +1c, +20) with a zeroed buffer, or pass
               a deliberate NULL; default is all on */
            char *colon = strchr(tok + 4, ':');
            long slot;
            int *flag;
            static const unsigned aux_slots[7] = {
                0x00, 0x04, 0x08, 0x0c, 0x18, 0x1c, 0x20
            };
            int wi, hit = -1;
            if (colon == 0) {
                printf("[0] bad aux '%s', use aux=<hexslot>:on|off\n",
                       argv[ai]);
                return 1;
            }
            *colon = '\0';
            slot = strtol(tok + 4, &e1, 16); /* slots are hex offsets
                                                ("0x" prefix ok too) */
            if (e1 == tok + 4 || *e1 != '\0') {
                printf("[0] bad aux slot '%s'\n", argv[ai]);
                return 1;
            }
            for (wi = 0; wi < 7; wi++)
                if (aux_slots[wi] == (unsigned)slot)
                    hit = wi;
            if (hit < 0) {
                printf("[0] unknown aux slot 0x%lx, use "
                       "00|04|08|0c|18|1c|20\n", slot);
                return 1;
            }
            flag = &aux_on[hit];
            if (strcmp(colon + 1, "on") == 0)
                *flag = 1;
            else if (strcmp(colon + 1, "off") == 0)
                *flag = 0;
            else {
                printf("[0] bad aux state '%s', use on|off\n", colon + 1);
                return 1;
            }
            continue;
        }
        if (strncmp(tok, "attr=", 5) == 0)
            tok += 5;
        colon = strchr(tok, ':');
        if (colon == 0) {
            /* no colon: override of AllocAttr's first argument */
            val = strtol(tok, &e2, 0);
            if (e2 == tok || *e2 != '\0' || val <= 0) {
                printf("[0] bad value '%s', use a positive number\n",
                       argv[ai]);
                return 1;
            }
            a1_val = (unsigned)val;
            a1_set = 1;
            continue;
        }
        *colon = '\0';
        idx = strtol(tok, &e1, 0);
        val = strtol(colon + 1, &e2, 0);
        if (e1 == tok || *e1 != '\0' || e2 == colon + 1 || *e2 != '\0' ||
            idx < 0 || idx > 7 || val < 0 || val > 0xffffffffl) {
            printf("[0] bad attr '%s', use [attr=]idx:val, idx 0..7\n",
                   argv[ai]);
            return 1;
        }
        attr_set[idx] = 1;
        attr_val[idx] = (unsigned)val;
    }
    printf("[0] requested rate = 0x%x, order = %s\n", rate,
           order_names[order]);
#ifndef HOST_ARGTEST
    scan_modules();
#endif
    load_stock_settings();

    /*
     * [gen] Generator self-test on a HOST staging buffer: FIRST, before
     * anything that can crash -- it needs nothing but malloc, and its
     * whole point is to be visible when everything else dies. 8x8
     * stride 256 bpp 2 (the dummy geometry, deliberate): for RGGB the
     * first four u32 words must be
     *   00220011 00220011 01220111 01220111
     * (row 0 spans four WORDS because stride is 256, not 16: pixels 4-7
     * sit in quadrant 1, +0x100) and the checksum must be 0x6AA0 --
     * permutation invariant, the same for all four orders. The numbers
     * are asserted on the host by bayer_test.sh.
     */
    {
        unsigned char *stage = malloc(4096);
        if (stage == 0) {
            printf("[gen] malloc failed -- generator untested\n");
        } else {
            unsigned sum = bayer_fill(stage, 8, 8, 256, 2, order);
            unsigned w[4];
            int k;
            for (k = 0; k < 4; k++)
                w[k] = (unsigned)stage[k * 4] |
                       ((unsigned)stage[k * 4 + 1] << 8) |
                       ((unsigned)stage[k * 4 + 2] << 16) |
                       ((unsigned)stage[k * 4 + 3] << 24);
            printf("[gen] bayer_fill(8x8, stride=256, bpp=2, order=%s) "
                   "checksum=0x%x first=0x%08x 0x%08x 0x%08x 0x%08x\n",
                   order_names[order], sum, w[0], w[1], w[2], w[3]);
            free(stage);
        }
    }

    /* [1] libnvrm -- gives us NvRmOpen, the only way to a device handle */
    printf("[1] dlopen(\"%s\") -> ", nvrm_path);
    nvrm = dlopen(nvrm_path, ISPINIT_DLOPEN_FLAGS);
    printf("%p\n", nvrm);
    if (nvrm == 0) {
        printf("    dlerror: %s\n", dlerror());
        return 1;
    }

    /* [2] the stock ISP library, by its explicit deployed path
       (the shim occupies libnvisp_v3.so; the stock one is .real) */
    printf("[2] dlopen(\"%s\") -> ", nvisp_path);
    nvisp = dlopen(nvisp_path, ISPINIT_DLOPEN_FLAGS);
    printf("%p\n", nvisp);
    if (nvisp == 0) {
        printf("    dlerror: %s\n", dlerror());
        return 1;
    }

    /* [3] the symbols; a miss here is printed and fatal */
    nvRmOpen = (NvRmOpen_fn)dlsym(nvrm, "NvRmOpen");
    nvRmMemHandleCreate =
        (NvRmMemHandleCreate_fn)dlsym(nvrm, "NvRmMemHandleCreate");
    nvRmMemHandleAllocAttr =
        (NvRmMemHandleAllocAttr_fn)dlsym(nvrm, "NvRmMemHandleAllocAttr");
    nvRmMemWriteStrided =
        (NvRmMemWriteStrided_fn)dlsym(nvrm, "NvRmMemWriteStrided");
    nvRmMemReadStrided =
        (NvRmMemReadStrided_fn)dlsym(nvrm, "NvRmMemReadStrided");
    nvRmMemWrite = (NvRmMemRW_fn)dlsym(nvrm, "NvRmMemWrite");
    nvRmMemRead = (NvRmMemRW_fn)dlsym(nvrm, "NvRmMemRead");
    nvIspOpen = (NvIspOpen_fn)dlsym(nvisp, "NvIspOpen");
    nvIspSetConfiguration =
        (NvIspSetConfiguration_fn)dlsym(nvisp, "NvIspSetConfiguration");
    nvIspProcessFrame =
        (NvIspProcessFrame_fn)dlsym(nvisp, "NvIspProcessFrame");
    nvIspHwSettingsCreate =
        (NvIspHwSettingsCreate_fn)dlsym(nvisp, "NvIspHwSettingsCreate");
    nvIspHwSettingsSetAttribute = (NvIspHwSettingsSetAttribute_fn)dlsym(
        nvisp, "NvIspHwSettingsSetAttribute");
    nvIspSetStats = (NvIspSetStats_fn)dlsym(nvisp, "NvIspSetStats");
    nvIspHwSettingsApply =
        (NvIspHwSettingsApply_fn)dlsym(nvisp, "NvIspHwSettingsApply");
    nvIspHwSettingsDestroy =
        (NvIspHwSettingsDestroy_fn)dlsym(nvisp, "NvIspHwSettingsDestroy");
    nvIspSetAttribute = (NvIspSetAttribute_fn)dlsym(nvisp, "NvIspSetAttribute");
    nvRmMemGetAddress =
        (NvRmMemGetAddress_fn)dlsym(nvrm, "NvRmMemGetAddress");
    nvRmMemPin = (NvRmMemPin_fn)dlsym(nvrm, "NvRmMemPin");
    nvIspSetIspClockRate =
        (NvIspSetIspClockRate_fn)dlsym(nvisp, "NvIspSetIspClockRate");
    nvIspGetStatus = (NvIspGetStatus_fn)dlsym(nvisp, "NvIspGetStatus");
    nvIspClose = (NvIspClose_fn)dlsym(nvisp, "NvIspClose");

    printf("[3] dlsym: NvRmOpen=%p HandleCreate=%p AllocAttr=%p NvIspOpen=%p "
           "SetConfiguration=%p ProcessFrame=%p SetIspClockRate=%p "
           "GetStatus=%p Close=%p\n",
           nvRmOpen, nvRmMemHandleCreate, nvRmMemHandleAllocAttr, nvIspOpen,
           nvIspSetConfiguration,
           nvIspProcessFrame, nvIspSetIspClockRate, nvIspGetStatus,
           nvIspClose);
    /*
     * Base of libnvisp_v3: dlsym address minus the symbol's static
     * offset (NvIspProcessFrame st_value = 0x1784 + thumb bit, per the
     * snapshot). Table pointers read from the context translate to
     * file offsets by subtracting this -- impl-2 gets exact addresses
     * instead of computed ones.
     */
    nvisp_base = ((unsigned)nvIspProcessFrame & ~1u) - 0x1784u;
    printf("[3] libnvisp_v3 base = 0x%x\n", nvisp_base);

    if (nvRmOpen == 0 || nvRmMemHandleCreate == 0 ||
        nvRmMemHandleAllocAttr == 0 || nvIspOpen == 0 ||
        nvIspSetConfiguration == 0 || nvIspProcessFrame == 0 ||
        nvIspSetIspClockRate == 0 || nvIspGetStatus == 0 ||
        nvIspClose == 0 || nvIspHwSettingsCreate == 0 ||
        nvIspHwSettingsSetAttribute == 0 || nvIspSetStats == 0 ||
        nvIspHwSettingsApply == 0 || nvIspHwSettingsDestroy == 0 ||
        nvIspSetAttribute == 0) {
        printf("    dlerror: %s\n", dlerror());
        return 1;
    }

    /* [4] device handle: the VALUE lands in dev (the stub writes 1) */
    printf("[4] NvRmOpen(&dev) -> ");
    rc = nvRmOpen(&dev);
    printf("rc=%d dev=%p\n", rc, dev);
    if (rc != 0)
        return 1;

    /*
     * [5] ROUND TRIP: allocate, write a known pattern, read it back,
     * compare. Touches NO ISP state -- this validates the memory path
     * alone and settles empirically whether an explicit mapping is
     * needed for direct Write/Read (impl-2 says the stock maps; if the
     * plain Strided calls agree without one, we do not need it).
     *
     * Expected and received words both print, so a mismatch shows its
     * SHAPE -- a shift, a byte order, a stride -- instead of a bare
     * verdict. Everything is CLI-tunable: rt=w:8 rt=h:8 rt=bs:0x100
     * rt=ms:0x100 rt=wb:16 rt=nr:8 rt=off:0 rt=x8:... (x8 unset in the
     * decode; wrappers pass the size there, so 0 means auto = ms*nr).
     */
    if (rt_on) {
        static NvRmMemHandleAllocAttr_fn alloc_fn;
        unsigned attrs_rt[8] = { 0, 0, 0x20, 2, 0x40000u, 0, 0, 0 };
        void *memh_rt = 0;
        unsigned char *stage = 0;
        unsigned char *back = 0;
        unsigned wb = rt_wb ? rt_wb : rt_w * 2;
        unsigned nr = rt_nr ? rt_nr : rt_h;
        unsigned x8 = rt_x8 ? rt_x8 : rt_ms * nr;
        unsigned region = rt_ms * nr;
        unsigned bad = 0, first_bad = 0xffffffffu;
        unsigned cmp_region;
        int k;

        if (rt_wb == 0 && wb > rt_bs)
            wb = rt_bs; /* the pattern cannot write past its own row */
        for (k = 0; k < 8; k++)
            if (attr_set[k])
                attrs_rt[k] = attr_val[k];

        printf("[5] round trip (%s): w=%u h=%u bs=0x%x ms=0x%x wb=%u "
               "nr=%u off=0x%x x8=0x%x size=%u order=%s\n",
               rt_simple ? "simple" : "strided",
               rt_w, rt_h, rt_bs, rt_ms, wb, nr, rt_off, x8,
               rt_size ? rt_size : region, rt_order);

        if (nvRmMemWriteStrided == 0 || nvRmMemReadStrided == 0) {
            printf("[5] Strided symbols missing -- round trip skipped\n");
        } else {
            alloc_fn = nvRmMemHandleAllocAttr;
            printf("[5] NvRmMemHandleAllocAttr(dev=0x%x, attrs, &memh_rt) "
                   "-> ", (unsigned)dev);
            rc = alloc_fn((unsigned)dev, attrs_rt, &memh_rt);
            printf("rc=0x%x memh_rt=%p\n", (unsigned)rc, memh_rt);

            stage = malloc(region > rt_bs * nr ? region : rt_bs * nr + 16);
            back = malloc(region + 16);
            if (memh_rt == 0 || rc != 0 || stage == 0 || back == 0) {
                printf("[5] allocation failed -- round trip skipped\n");
            } else {
                unsigned sum = bayer_fill(stage, rt_w, rt_h, rt_bs, 2, order);
                printf("[5] pattern: bayer_fill(%s) checksum=0x%x\n",
                       order_names[order], sum);

                if (rt_simple) {
                    /* plain Write/Read, four arguments; the order of
                       ptr/offset/size is not certain, so it is a CLI
                       permutation: rt=order:hpos etc. */
                    unsigned aw[4], ar[4], m;
                    if (nvRmMemWrite == 0 || nvRmMemRead == 0) {
                        printf("[5] NvRmMemWrite/Read symbols missing -- "
                               "simple mode unavailable\n");
                        free(stage);
                        free(back);
                        goto round_trip_end;
                    }
                    for (m = 0; m < 4; m++) {
                        if (rt_order[m] == 'h') aw[m] = (unsigned)memh_rt;
                        else if (rt_order[m] == 'p') aw[m] = (unsigned)stage;
                        else if (rt_order[m] == 'o') aw[m] = rt_off;
                        else aw[m] = rt_size ? rt_size : region;
                    }
                    rc = nvRmMemWrite(aw[0], (void *)aw[1], aw[2], aw[3]);
                    printf("[5] NvRmMemWrite(%s) -> rc=0x%x\n", rt_order,
                           (unsigned)rc);

                    memset(back, 0xEE, region);
                    for (m = 0; m < 4; m++) {
                        if (rt_order[m] == 'h') ar[m] = (unsigned)memh_rt;
                        else if (rt_order[m] == 'p') ar[m] = (unsigned)back;
                        else if (rt_order[m] == 'o') ar[m] = rt_off;
                        else ar[m] = rt_size ? rt_size : region;
                    }
                    rc = nvRmMemRead(ar[0], (void *)ar[1], ar[2], ar[3]);
                    printf("[5] NvRmMemRead(%s) -> rc=0x%x\n", rt_order,
                           (unsigned)rc);
                } else {
                    rc = nvRmMemWriteStrided((unsigned)memh_rt, stage,
                                             rt_bs, rt_off, rt_ms, wb, nr,
                                             x8);
                    printf("[5] NvRmMemWriteStrided -> rc=0x%x\n",
                           (unsigned)rc);

                    memset(back, 0xEE, region);
                    rc = nvRmMemReadStrided((unsigned)memh_rt, back, rt_bs,
                                            rt_off, rt_ms, wb, nr, x8);
                    printf("[5] NvRmMemReadStrided -> rc=0x%x\n",
                           (unsigned)rc);
                }

                cmp_region = rt_simple && rt_size ? rt_size : region;
                for (k = 0; k < (int)cmp_region; k++) {
                    if (stage[k] != back[k]) {
                        bad++;
                        if (first_bad == 0xffffffffu)
                            first_bad = (unsigned)k;
                    }
                }

                {
                    unsigned ew[8], rw[8];
                    int m;
                    for (m = 0; m < 8; m++) {
                        int b = m * 4;
                        ew[m] = (unsigned)stage[b] |
                                ((unsigned)stage[b + 1] << 8) |
                                ((unsigned)stage[b + 2] << 16) |
                                ((unsigned)stage[b + 3] << 24);
                        rw[m] = (unsigned)back[b] |
                                ((unsigned)back[b + 1] << 8) |
                                ((unsigned)back[b + 2] << 16) |
                                ((unsigned)back[b + 3] << 24);
                    }
                    printf("[5] expected:");
                    for (m = 0; m < 8; m++)
                        printf(" %08x", ew[m]);
                    printf("\n");
                    printf("[5] received:");
                    for (m = 0; m < 8; m++)
                        printf(" %08x", rw[m]);
                    printf("\n");
                }

                if (bad == 0) {
                    printf("[5] ROUND TRIP PASSED: %u bytes identical\n",
                           cmp_region);
                } else {
                    printf("[5] ROUND TRIP FAILED: %u of %u bytes differ, "
                           "first at byte 0x%x (row %u, col %u)\n",
                           bad, cmp_region, first_bad,
                           first_bad / rt_ms, (first_bad % rt_ms) / 4);
                }
            }
        }
    }

round_trip_end:;
    /* [5] open the chosen instance (inst=, default 1). On failure the
       library cleans up after itself; we release nothing here, that
       would be a double free. Which /dev/nvhost-isp* device this maps
       to is visible in the kernel log / open interposition. */
    printf("[5] NvIspOpen(dev=%p, instance=%u, &hIsp) -> ", dev, inst);
    rc = nvIspOpen((unsigned)dev, inst, &hIsp);
    printf("rc=0x%x hIsp=0x%x\n", (unsigned)rc, hIsp);
    if (rc != 0)
        return 1;

    /* [5b] the stage table, address AND file offset (base printed at
       [3]). Expected offsets per the frame read: st1 0x3044, st2 0x2af0,
       st3 0x4380, st4 0x1eb8. A mismatch means we are not calling the
       functions we think we are. */
    {
        static const unsigned soffs[4] = { 0x130c, 0x1308, 0x12d4, 0x1304 };
        static const unsigned sexp[4] = { 0x3044, 0x2af0, 0x4380, 0x1eb8 };
        static const char snames[4][4] = { "st1", "st2", "st3", "st4" };
        int q;
        printf("[5b] stage table (base 0x%x):\n", nvisp_base);
        for (q = 0; q < 4; q++) {
            unsigned p = *(unsigned *)((unsigned)hIsp + soffs[q]);
            unsigned off = p ? ((p & ~1u) - nvisp_base) : 0;
            printf("[5b]   %s ctx+0x%x = 0x%08x file+0x%x -- expected "
                   "file+0x%x %s\n",
                   snames[q], soffs[q], p, off, sexp[q],
                   off == sexp[q] ? "MATCH" : "MISMATCH");
        }
    }

    /* [5c] syncpoint read channel (kernel ioctl, source-verified) */
    sp_fd = open("/dev/nvhost-ctrl", O_RDWR);
    if (sp_fd < 0)
        sp_fd = open("/dev/nvhost-ctrl", O_RDONLY);
    printf("[5c] /dev/nvhost-ctrl fd=%d\n", sp_fd);
    print_syncpts("[5c] syncpoints at open", 0);

    /*
     * [5d] object operations, all on OUR object (hIsp is ours, plain
     * memory): objload=<path> clones a captured stock object into it
     * (handles inside are FOREIGN -- per mail-1073 this mode is for
     * offline comparison only, never for submission); objset=<off>:
     * <val> writes single words (bounds-checked); objdump=<path>
     * writes our object out for the word-level diff against the stock
     * capture. The frame counter, if loaded, CONTINUES from the
     * stock's value by design -- do not reset it.
     */
    if ((objload_on || objdump_on || objset_n != 0) && hIsp != 0) {
        unsigned objp = *(unsigned *)((unsigned)hIsp + 0x1318);
        if (objp == 0) {
            printf("[5d] object ops: [hIsp+0x1318] is null -- "
                   "skipped\n");
        } else {
            if (objload_on) {
                FILE *f = fopen(objload_path, "rb");
                if (f == 0) {
                    printf("[5d] objload: cannot open %s -- "
                           "skipped\n", objload_path);
                } else {
                    unsigned n = fread((void *)objp, 1, objlen, f);
                    fclose(f);
                    printf("[5d] objload: %u bytes -> obj@0x%x from "
                           "%s (FOREIGN HANDLES INSIDE -- do not "
                           "submit with this)\n",
                           n, objp, objload_path);
                }
            }
            {
                int k5;
                for (k5 = 0; k5 < objset_n; k5++) {
                    unsigned was =
                        *(unsigned *)(objp + objset_off[k5]);
                    *(unsigned *)(objp + objset_off[k5]) =
                        objset_val[k5];
                    printf("[5d] objset +0x%x: 0x%x -> 0x%x\n",
                           objset_off[k5], was, objset_val[k5]);
                }
            }
            if (objdump0_on) {
                FILE *f = fopen(objdump0_path, "wb");
                if (f == 0) {
                    printf("[5d] objdump0: cannot open %s\n",
                           objdump0_path);
                } else {
                    fwrite((void *)objp, 1, objlen, f);
                    fclose(f);
                    printf("[5d] objdump0: %u bytes from obj@0x%x -> "
                           "%s (EARLY: before init chain)\n",
                           objlen, objp, objdump0_path);
                }
            }
            printf("[5d] obj[0]=0x%x\n", *(unsigned *)objp);
            {
                int i3;
                printf("[5d] obj+0x1660:");
                for (i3 = 0; i3 < 16; i3++)
                    printf(" +%x:%08x", 0x1660 + i3 * 4,
                           *(unsigned *)(objp + 0x1660 + i3 * 4));
                printf("\n");
            }
        }
    }

    /* crash isolation for the per-stage calls */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = stage_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGILL, &sa, 0);

    /* [6] configure, mode 1: the stock's sixteen selector words
       (CLI-overridable, cfg=), size 64 in. The size AFTER the call
       matters: rc 10 means the library disagreed and wrote the size it
       wants -- the fix-and-retry mechanism. We print what it said
       instead of guessing. */
    {
        unsigned cfgw[16];
        int k;
        for (k = 0; k < 16; k++)
            cfgw[k] = cfg_set[k] ? cfg_val[k] : cfg_mode1_arr[k];
        printf("[6] cfg words:");
        for (k = 0; k < 16; k++)
            printf(" [%d]=0x%x%s", k, cfgw[k], cfg_set[k] ? "*" : "");
        printf("\n");
        size = 64;
        printf("[6] NvIspSetConfiguration(hIsp=0x%x, mode=%u, cfg16, "
               "&size=64) -> ", hIsp, cfgmode1_arg);
        rc = nvIspSetConfiguration(hIsp, cfgmode1_arg, cfgw, &size);
        printf("rc=0x%x size-after=%u\n", (unsigned)rc, size);
    }

    /* [7] configure, mode 2: the single word (stock 2, cfg2= to
       override), size 4 in */
    size = 4;
    printf("[7] NvIspSetConfiguration(hIsp=0x%x, mode=%u, cfg2=0x%x%s, "
           "&size=4) -> ", hIsp, cfgmode2_arg, cfg2_val, cfg2_set ? "*" : "");
    rc = nvIspSetConfiguration(hIsp, cfgmode2_arg, &cfg2_val, &size);
    printf("rc=0x%x size-after=%u\n", (unsigned)rc, size);

    /*
     * [7b] the stock init chain, the calls we never made. Order and
     * arities are from the live hook log (out/hook-init-chain.txt),
     * not derived. After EACH step the stage-3 gate field prints:
     * if the gate turns non-zero, the step that did it is the answer;
     * if nothing ever does, the init chain is not the source.
     */
    if (st_create) {
        /* headroom IN FRONT: Destroy's second buffer is p1-4, so p1
           must point into our memory with one word before it */
        unsigned carea1[17] = {0}, carea2[17] = {0};
        unsigned *cs1 = carea1 + 1; /* 0x24 region, one word of slack */
        unsigned *cs2 = carea2 + 1;
        printf("[7b] NvIspHwSettingsCreate(hIsp=0x%x, &p1, size=%u, &p2) "
               "-> ", (unsigned)hIsp, create_size);
        stage_now = "HwSettingsCreate";
        rc = nvIspHwSettingsCreate((unsigned)hIsp, cs1, create_size, cs2);
        /* the live chain: SetAttribute/Apply/Destroy receive r0 =
           r3 + 4 -- the settings OBJECT lives inside the p2 buffer at
           offset 4, the handle is that ADDRESS */
        if (strcmp(hset_mode, "p1") == 0)
            hset = cs1[0];               /* measured: the library wrote
                                            a heap pointer here */
        else if (strcmp(hset_mode, "p2a") == 0)
            hset = (unsigned)&cs2[1];    /* the p2+4 theory */
        else
            hset = hset_manual;
        printf("rc=0x%x hset=0x%x (mode %s)\n", (unsigned)rc, hset,
               hset_mode);
        stage_now = 0;
        print_first_words("[7b] p1 first words", (unsigned char *)cs1, 8);
        print_first_words("[7b] p2 first words", (unsigned char *)cs2, 8);
        if (hset != 0)
            set_snap_take(hset);
        print_gate("[7b] after HwSettingsCreate", hIsp);
    }

    /* [8] set the requested rate: the zero cache would answer the next
       read with zero and hide whether the ISP is alive at all; the max
       is clipped by the clock tree (600000 of 0x3fffff), and a smaller
       request must come back as requested -- that is the control */
    printf("[8] NvIspSetIspClockRate(hIsp=0x%x, rate=0x%x) -> ", hIsp, rate);
    rc = nvIspSetIspClockRate(hIsp, rate);
    printf("rc=0x%x\n", (unsigned)rc);

    /* [9] status id 6, 4 bytes in, actual size out; must be the rate
       just written, not zero */
    size = 4;
    printf("[9] NvIspGetStatus(hIsp=0x%x, id=%u, &value, size=4) -> ",
           hIsp, status_id);
    rc = nvIspGetStatus(hIsp, status_id, &value, &size);
    printf("rc=0x%x size=%u value=0x%x\n", (unsigned)rc, size, value);
    print_gate("[9] after GetStatus", hIsp);

    /* [9b2] HwSettingsSetAttribute x14: the stock's block walk. Order
       and (id,index) pairs from the live hook log (the first four are
       log-observed, the rest follow the lead's canonical list).
       Buffers are zeroed 256 bytes -- if the library wants content or
       a size, rc will say so. */
    if (st_sattr && hset == 0)
        printf("[9b2] skipped: hset==0 (Create did not produce one)\n");
    if (st_sattr && hset != 0) {
        static const unsigned blocks[14][2] = {
            {5, 0}, {1, 0}, {2, 0}, {2, 1}, {4, 0}, {6, 0}, {7, 0},
            {8, 0}, {9, 0}, {0xa, 0}, {0xb, 0}, {0xd, 0}, {0xe, 0},
            {0x10, 0},
        };
        int q;
        for (q = 0; q < 14; q++) {
            unsigned id = blocks[q][0];
            unsigned bufsize = 256; /* our buffer; the library corrects
                                       it via rc 10 if it wants less */
            unsigned size_out = bufsize;
            unsigned char *zbuf = malloc(bufsize);
            unsigned char *rbuf = 0;
            unsigned wrc;
            char tag[32];
            if (zbuf == 0) {
                printf("[9b2] malloc failed for id %u\n", blocks[q][0]);
                continue;
            }
            memset(zbuf, 0, bufsize);
            /* stock replay, per-block key: real values instead of
               zeros -- the memcmp makes zero-on-zero a silent no-op */
            if (blk_on[id] != 0 && blk_have[id] != 0) {
                unsigned n2;
                for (n2 = 0; n2 < 44; n2++) {
                    zbuf[n2 * 4] = (unsigned char)(blk_val[id][n2]);
                    zbuf[n2 * 4 + 1] = (unsigned char)(blk_val[id][n2] >> 8);
                    zbuf[n2 * 4 + 2] = (unsigned char)(blk_val[id][n2] >> 16);
                    zbuf[n2 * 4 + 3] = (unsigned char)(blk_val[id][n2] >> 24);
                }
                size_out = blk_size[id];
            }
            snprintf(tag, sizeof(tag), "HwSettingsSetAttribute id=%u",
                     blocks[q][0]);
            stage_now = tag;
            /* FIVE arguments: the fifth (&size) travels on the stack --
               the 0x1eb8 crash was the library reading a stale stack
               word as the size pointer */
            wrc = nvIspHwSettingsSetAttribute(hset, blocks[q][0],
                                              blocks[q][1], zbuf,
                                              &size_out);
            stage_now = 0;
            snprintf(tag, sizeof(tag), "[9b2] SetAttr id=%u idx=%u",
                     blocks[q][0], blocks[q][1]);
            printf("[9b2] HwSettingsSetAttribute(hset=0x%x, id=%u, idx=%u, "
                   "buf, &size=%u) -> rc=0x%x size-out=%u\n",
                   hset, blocks[q][0], blocks[q][1], bufsize,
                   (unsigned)wrc, size_out);
            if (wrc == 10 && size_out != bufsize && size_out != 0) {
                rbuf = malloc(size_out);
                if (rbuf != 0) {
                    memset(rbuf, 0, size_out);
                    wrc = nvIspHwSettingsSetAttribute(hset, blocks[q][0],
                                                      blocks[q][1], rbuf,
                                                      &size_out);
                    printf("[9b2]   retry with size=%u -> rc=0x%x\n",
                           size_out, (unsigned)wrc);
                }
            }
            {
                char btag[48];
                snprintf(btag, sizeof(btag),
                         "[9b2] buf-after id=%u idx=%u", blocks[q][0],
                         blocks[q][1]);
                print_first_words(btag, (const unsigned char *)zbuf, 16);
            }
            print_set_diff(tag, hset);
            free(zbuf);
            free(rbuf);
        }
    }

    /* [9b3] SetStats x4: (type,index) pairs from the live log; FIVE
       arguments -- the fifth is &size (impl-2): (2,0)->0x68,
       (1,0)->0x20, (1,1)->0x20, (4,0)->0x48. rc 10 is the
       fix-and-repeat protocol: the library writes the size it wants
       into our word; we print it and retry once with that size. */
    if (st_stats) {
        static const unsigned stats[4][3] = {
            {2, 0, 0x68}, {1, 0, 0x20}, {1, 1, 0x20}, {4, 0, 0x48},
        };
        int q;
        /* stock replay, per-type key (stat=<type>:on); per-type fill
           (stat=<type>:fill:<word>) makes the buffer DIFFER from the
           object -- the memcmp no-op only fires on identical zeros */
        for (q = 0; q < 4; q++) {
            unsigned stype = stats[q][0];
            unsigned bufsize = stats[q][2];
            unsigned size_out = bufsize;
            if (stype == 4 && st4_on == 0) {
                printf("[9b3] SetStats type 4 skipped by default "
                       "(faults with our data; stat=4:on enables)\n");
                continue;
            }
            unsigned char *zbuf = malloc(bufsize);
            unsigned char *rbuf = 0;
            unsigned wrc;
            if (zbuf == 0) {
                printf("[9b3] malloc failed for type %u\n", stats[q][0]);
                continue;
            }
            if (st_fill_g[stype] != 0) {
                /* nonzero fill: memcmp sees a difference and the type's
                   bit goes into obj[0] */
                unsigned *wf = (unsigned *)zbuf;
                unsigned wn;
                for (wn = 0; wn < bufsize / 4; wn++)
                    wf[wn] = st_fill_w[stype];
                printf("[9b3] buffer filled with 0x%08x (%u words)\n",
                       st_fill_w[stype], bufsize / 4);
            } else {
                memset(zbuf, 0, bufsize);
            }
            if (st_on[stype] != 0 && st_have[stype] != 0) {
                unsigned n2;
                bufsize = st_size[stype];
                size_out = bufsize;
                for (n2 = 0; n2 < 32; n2++) {
                    zbuf[n2 * 4] = (unsigned char)(st_val[stype][n2]);
                    zbuf[n2 * 4 + 1] =
                        (unsigned char)(st_val[stype][n2] >> 8);
                    zbuf[n2 * 4 + 2] =
                        (unsigned char)(st_val[stype][n2] >> 16);
                    zbuf[n2 * 4 + 3] =
                        (unsigned char)(st_val[stype][n2] >> 24);
                }
            }
            stage_now = stats[q][0] == 2 ? "SetStats(type 2)"
                                         : stats[q][0] == 4 ? "SetStats(type 4)"
                                         : "SetStats(type 1)";
            wrc = nvIspSetStats((unsigned)hIsp, stats[q][0], stats[q][1],
                                zbuf, &size_out);
            printf("[9b3] NvIspSetStats(hIsp=0x%x, id=%u, idx=%u, "
                   "buf=%uB fill=0x%x) -> rc=0x%x size-out=%u\n",
                   (unsigned)hIsp, stats[q][0], stats[q][1], bufsize,
                   st_fill_g[stype] ? st_fill_w[stype] : 0,
                   (unsigned)wrc, size_out);
            if (wrc == 10 && size_out != bufsize && size_out != 0) {
                rbuf = malloc(size_out);
                if (rbuf != 0) {
                    memset(rbuf, 0, size_out);
                    wrc = nvIspSetStats((unsigned)hIsp, stats[q][0],
                                        stats[q][1], rbuf, &size_out);
                    printf("[9b3]   retry with size=%u -> rc=0x%x\n",
                           size_out, (unsigned)wrc);
                    free(rbuf);
                }
            }
            /* per-type obj state: WHICH type fills the +0x1660 window */
            stage_now = 0;
            print_obj_state("[9b3] after SetStats", hIsp);
            free(zbuf);
        }
        stage_now = 0;
    }

    /* [9b4] HwSettingsApply: (hset, mapped?, 5, 0xb61bf1bf) per the
       live log; the mapped pointer is opaque -- zero for now */
    if (st_apply && hset == 0)
        printf("[9b4] skipped: hset==0\n");
    if (st_apply && hset != 0) {
        /* the live log's r3 (0xb61bf1bf etc.) is a SESSION POINTER that
           moves between runs -- never a constant; zero until we know
           what it points at */
        stage_now = "HwSettingsApply";
        rc = nvIspHwSettingsApply(hset, 0, apply_a2, apply_a4);
        printf("[9b4] NvIspHwSettingsApply(hset=0x%x, 0, %u, %u) -> "
               "rc=0x%x\n", hset, apply_a2, apply_a4, (unsigned)rc);
        /* stage 3 takes its relocation handle from here; zero means it
           falls over before doing any work */
        stage_now = 0;
        printf("[9b4] ctx+0x123c = 0x%x\n",
               *(unsigned *)((unsigned)hIsp + 0x123c));
        print_gate("[9b4] after HwSettingsApply", hIsp);
    }

    /* [9b] ISP-context dumps, only where requested (ctx=<off>:<count>).
       The context is ours, valid between Open and Close, read-only.
       Table pointers translate to file offsets via the base printed
       at [3]. */
    for (int ci = 0; ci < ctx_n; ci++) {
        unsigned *cp = (unsigned *)((unsigned)hIsp + ctx_off[ci]);
        printf("[9b] ctx+0x%x (%u words):", ctx_off[ci], ctx_cnt[ci]);
        for (unsigned k2 = 0; k2 < ctx_cnt[ci]; k2++) {
            if (k2 != 0 && k2 % 8 == 0)
                printf("\n[9b]   ");
            printf(" +%x:%08x", ctx_off[ci] + k2 * 4, cp[k2]);
        }
        printf("\n");
    }

    /* [9b] pointer-following dumps: one level deep, null-safe */
    for (int cj = 0; cj < ctxp_n; cj++) {
        unsigned ptr = *(unsigned *)((unsigned)hIsp + ctxp_off[cj]);
        printf("[9b] ctxp+0x%x -> 0x%x", ctxp_off[cj], ptr);
        if (ptr == 0) {
            printf(" (null)\n");
            continue;
        }
        {
            unsigned *cp = (unsigned *)ptr;
            printf(" (%u words):", ctxp_cnt[cj]);
            for (unsigned k2 = 0; k2 < ctxp_cnt[cj]; k2++) {
                if (k2 != 0 && k2 % 8 == 0)
                    printf("\n[9b]   ");
                printf(" +%x:%08x", k2 * 4, cp[k2]);
            }
            printf("\n");
        }
    }

    /* [9c] install stage hooks, if any were requested */
    if (stage_n != 0) {
#ifndef HOST_ARGTEST
        int k, i;
        for (k = 0; k < stage_n; k++) {
            unsigned *entry = (unsigned *)((unsigned)hIsp + stage_off[k]);
            int slot = -1;
            for (i = 0; i < STAGE_SLOTS; i++)
                if (stage_installed[i] == 0) {
                    slot = i;
                    break;
                }
            if (slot < 0)
                break;
            stage_hook_orig[slot] = *entry;
            *entry = (unsigned)stage_hook_syms[slot];
            stage_installed[slot] = 1;
            stage_installed_off[slot] = stage_off[k];
            printf("[9c] stage: ctx+0x%x hooked (slot %u, orig 0x%08x)\n",
                   stage_off[k], slot, stage_hook_orig[slot]);
        }
#else
        printf("[9c] host build: stage interception unavailable\n");
#endif
    }

    /*
     * [10..12] submit the placeholder frame. First reproduce what the
     * hardware already accepted (the stock's 8x8 dummy: format 0x105a500c
     * from the stock pool, stride 256, one plane), with OUR buffer and
     * OUR handle; geometry and format change one step at a time after
     * this works. The numbers 8x8 are copied deliberately as known-good,
     * the handle is ours from the allocation -- never copied, it lives
     * one run.
     */

    /* [13b] the debug-flag SetAttribute the stock sends (id 4, value 0,
       &size): the shim gate showed the stock passes a zero */
    if (st_setattr) {
        unsigned flag = 0;
        unsigned fsize = 4;
        stage_now = "NvIspSetAttribute";
        rc = nvIspSetAttribute(hIsp, setattr_id, &flag, &fsize);
        printf("[13b] NvIspSetAttribute(hIsp=0x%x, id=%u, &flag, &size) "
               "-> rc printed with gate below\n", hIsp, setattr_id);
        printf("[13b] NvIspSetAttribute(hIsp=0x%x, id=4, &flag=0, "
               "&size=4) -> rc=0x%x\n", hIsp, (unsigned)rc);
        stage_now = 0;
        print_gate("[13b] after SetAttribute", hIsp);
    }

    /*
     * [10] the WORKING ATTRIBUTES -- read from INTERCEPTION, not derived.
     * nvrmlog caught the stock's live calls in our own process (ten
     * CREATE/ALLOC/MMAP triples, rc=0, identical attrs every time):
     *
     *   arg1      = 0x1      the device VALUE. Three derived answers --
     *                        pointer, Create handle, bare size -- were
     *                        all wrong; this one reproduced rc=0 first try.
     *   attrs[0]  = 0        no tag list at all
     *   attrs[1]  = 0        not the tagged path
     *   attrs[2]  = 0x20     alignment
     *   attrs[3]  = 2        memory type (2, not 1)
     *   attrs[4]  = 0x40000  256 KiB -- the size DOES live in attrs[4]
     *
     * CLI overrides still land on top of these defaults; they exist for
     * exactly this kind of one-run experiment. (The two-step Create+Alloc
     * experiment lives in git history, commit be257b4.)
     */
    {
        unsigned attrs_base[8] = { 0, 0, 0x20, 2, 0x40000u, 0, 0, 0 };
        unsigned attrs_in[8];
        unsigned attrs_out[8];
        void *memh_in = 0;
        void *memh_out = 0;
        unsigned char *pat_out = 0;   /* the 0xDEADBEEF reference */
        unsigned desc_in[44] = {0};  /* 0xb0 bytes each, the stock
                                        record size */
        unsigned desc_out[44] = {0};
        unsigned gh, gw, gf, gt, gs, gp;
        unsigned bufs[7][44] = {{0}}; /* one buffer per stack slot */
        unsigned desc_no[44] = {0}; /* frame description, no handle */
        unsigned fenceA[16] = {0};  /* fence-return capture (slot kind) */
        unsigned fenceB[16] = {0};  /* fence-return capture (slot kind) */
        unsigned sbuf10[44] = {0};  /* buffer behind slot10 */
        unsigned sbuf14[44] = {0};  /* buffer behind slot14 */
        /* pre-call snapshots for the post-call diff */
        unsigned snap_in[16], snap_out[16], snap_no[16];
        unsigned snap_aux[7][16];
        unsigned snap_s08[16], snap_s10[16], snap_s14[16], snap_s18[16];
        unsigned ptr_target[44] = {0}; /* shared target for word=ptr */
        unsigned a1;
        unsigned sv[7];   /* stack slot values, +00..+20 in slot order */
        unsigned s08, s10, s14, s18;
        int i, k;
        static const char aux_names[7][3] = {
            "00", "04", "08", "0c", "18", "1c", "20"
        };

        /* stock-shape defaults for the two structured slots, built
           from OUR session values (the stock's 3280/2460 were its
           sensor geometry; ours are the frame's), then the a<slot>=
           word overrides land on top:
           +14 (slot14=aux): +1c mode, +30 width, +34 height,
              +38.. 0x3f000000 (float 0.5) to the end;
           +1c: 3, width, height, 1, 0, hIsp at +14, 2 at +38. */
        /* mode 1: +0x08 is a POINTER to a structure whose first two
           words are the frame geometry (ldm {r8,r10} at 0x3386) --
           default-built here so the bare run is well-formed; the
           a08= word overrides land on top. In mode 2 the slot is a
           number (see the sv[] default below). */
        bufs[2][0] = din_set[1] ? din_val[1] : 8; /* width */
        bufs[2][1] = din_set[0] ? din_val[0] : 8; /* height */
        bufs[5][0] = 3;
        bufs[5][1] = din_set[1] ? din_val[1] : 8;
        bufs[5][2] = din_set[0] ? din_val[0] : 8;
        bufs[5][3] = 1;
        bufs[5][4] = 0;
        bufs[5][5] = (unsigned)hIsp;
        bufs[5][14] = 2;
        desc_no[0] = din_set[0] ? din_val[0] : 8;
        desc_no[1] = din_set[1] ? din_val[1] : 8;
        desc_no[2] = din_set[2] ? din_val[2] : 0x105a500cu;
        desc_no[3] = din_set[3] ? din_val[3] : 1;
        desc_no[4] = din_set[4] ? din_val[4] : 256;
        desc_no[9] = din_set[9] ? din_val[9] : 1;

        for (i = 0; i < 7; i++) {
            for (k = 0; k < 44; k++) {
                if (aux_isptr[i][k])
                    bufs[i][k] = (unsigned)ptr_target;
                else if (aux_set[i][k])
                    bufs[i][k] = aux_val[i][k];
            }
        }

        /* +00..+0c: each slot takes a NUMBER (n<slot>=<val>) or a
           POINTER (aux=<slot>:on -> zeroed buffer), CLI-switchable --
           the stock's +08/+0c = 3280/2460 numbers were read in mode 2,
           and mode 1 may treat the same slots differently, so both
           readings stay enumerable. Precedence: explicit number wins,
           then pointer, then defaults (+08 = width number, +0c =
           height number, +00/+04 = zero). */
        {
            unsigned gw_r = din_set[1] ? din_val[1] : 8;
            unsigned gh_r = din_set[0] ? din_val[0] : 8;
            for (i = 0; i < 4; i++) {
                if (n_set[i])
                    sv[i] = n_val[i];
                else if (i == 2)
                    sv[i] = 0; /* set after the descriptors are built */
                else if (aux_on[i])
                    sv[i] = (unsigned)bufs[i];
                else
                    sv[i] = (i == 2) ? gw_r : (i == 3) ? gh_r : 0;
            }
            sv[4] = aux_on[4] ? (unsigned)bufs[4] : 0;
            sv[5] = aux_on[5] ? (unsigned)bufs[5] : 0;
            sv[6] = aux_on[6] ? (unsigned)bufs[6] : 0;
            printf("[12] slot values:");
            for (i = 0; i < 7; i++)
                printf(" +%s=%s0x%x", aux_names[i],
                       (i < 4 && n_set[i]) ? "num " :
                       (i < 4 && aux_on[i]) ? "ptr " : "    ", sv[i]);
            printf("\n");
            /* числа не снимаются диффом -- дифф только для буферных слотов */
            for (i = 0; i < 7; i++)
                for (k = 0; k < 16; k++)
                    snap_aux[i][k] = bufs[i][k];

        }

        for (k = 0; k < 8; k++) {
            attrs_in[k] = attrs_base[k];
            attrs_out[k] = attrs_base[k];
        }
        /* attrs are the STOCK-CAPTURED values and are not modified by
           default. heap_i/heap_o keys exist but heap selection is NOT
           implemented: we do not yet know the attrs array layout
           (pairs "tag,value" vs fixed fields) -- writing a guessed
           word crashed libnvrm (mail-1087 report, reverted). The
           kernel heap masks for reference: NVMAP_HEAP_IOVMM = 1<<30,
           CARVEOUT_GENERIC = 1, CARVEOUT_IVM = 2. */
        for (k = 0; k < 8; k++) {
            if (attr_set[k]) {
                attrs_in[k] = attr_val[k];
                attrs_out[k] = attr_val[k];
            }
        }

        printf("[10] attrs in|out:");
        for (k = 0; k < 8; k++)
            printf(" [%d]=0x%x|0x%x%s", k, attrs_in[k], attrs_out[k],
                   attr_set[k] ? "*" : "");
        printf("\n");

        a1 = a1_set ? a1_val : (unsigned)dev;
        printf("[10] NvRmMemHandleAllocAttr(a1=0x%x, attrs, &memh_in) "
               "[input] -> ", a1);
        rc = nvRmMemHandleAllocAttr(a1, attrs_in, &memh_in);
        printf("rc=0x%x memh_in=%p\n", (unsigned)rc, memh_in);

        if (rc == 0 && memh_in != 0) {
            printf("[10] NvRmMemHandleAllocAttr(a1=0x%x, attrs, &memh_out) "
                   "[output] -> ", a1);
            rc = nvRmMemHandleAllocAttr(a1, attrs_out, &memh_out);
            printf("rc=0x%x memh_out=%p\n", (unsigned)rc, memh_out);
        }

        if (rc != 0 || memh_in == 0 || memh_out == 0) {
            printf("    alloc failed -- skipping submission, closing\n");
        } else {
            /* pin the output buffer and read its device address
               (exports verified in libnvrm.so; both calls PRINT their
               results -- a wrong arity shows in print, not in a crash) */
            if (pin_on && memh_out != 0 && nvRmMemPin != 0) {
                unsigned pa;
                stage_now = "NvRmMemPin";
                /* measured: Pin RETURNS the device address (0x800c0000
                   in the field was an address, not an error code) */
                pa = nvRmMemPin((unsigned)memh_out, 0);
                stage_now = 0;
                printf("[11a] NvRmMemPin(memh_out=0x%x) -> addr=0x%x\n",
                       (unsigned)memh_out, pa);
                if (pa != 0) {
                    out_devaddr = pa;
                    out_devaddr_valid = 1;
                }
            }
            if (nvRmMemGetAddress != 0 && memh_out != 0) {
                unsigned ga = nvRmMemGetAddress((unsigned)memh_out, 0);
                printf("[11a] NvRmMemGetAddress(memh_out=0x%x, 0) -> "
                       "0x%x\n",
                       (unsigned)memh_out, ga);
                if (out_devaddr_valid == 0) {
                    out_devaddr = ga;
                    out_devaddr_valid = 1;
                }
            }

            /* [11] two descriptors, stock dummy layout with seven live
               words. Geometry/format fields are CLI-overridable
               (din=/dout=); the geometry drives the pattern fill and
               the write size so all three never disagree. The handle
               word is program-set LAST, an override there is rejected
               at parse time. */
            gh = din_set[0] ? din_val[0] : 8;
            gw = din_set[1] ? din_val[1] : 8;
            gf = din_set[2] ? din_val[2] : 0x105a500cu;
            gt = din_set[3] ? din_val[3] : 1;
            gs = din_set[4] ? din_val[4] : 256;
            gp = din_set[9] ? din_val[9] : 1;

            desc_in[0] = gh;
            desc_in[1] = gw;
            desc_in[2] = gf;
            desc_in[3] = gt;
            desc_in[4] = gs;
            desc_in[9] = gp;
            desc_out[0] = dout_set[0] ? dout_val[0] : gh;
            desc_out[1] = dout_set[1] ? dout_val[1] : gw;
            desc_out[2] = dout_set[2] ? dout_val[2] : gf;
            desc_out[3] = dout_set[3] ? dout_val[3] : gt;
            desc_out[4] = dout_set[4] ? dout_val[4] : gs;
            desc_out[9] = dout_set[9] ? dout_val[9] : gp;
            for (k = 0; k < 44; k++) {
                if (din_set[k] && k != 5)
                    desc_in[k] = din_val[k];
                if (dout_set[k] && k != 5)
                    desc_out[k] = dout_val[k];
            }
            desc_in[5] = (unsigned)memh_in;   /* OUR input handle */
            desc_out[5] = (unsigned)memh_out; /* OUR output handle */

            /*
             * Output pre-fill: 0xDEADBEEF repeated, written through the
             * same Write path. Pages come back NOT zeroed (measured),
             * so post-submit content proves nothing by itself -- but
             * with a known pattern in place the post-submit rule is
             * simple: everything that DIFFERS from the pattern, the
             * ISP wrote. Zero diffs = untouched; partial = started and
             * stopped; full = frame written. The pattern buffer is
             * kept alive for that comparison.
             */
            {
                unsigned pbytes = gs * gh;
                unsigned wq;
                pat_out = malloc(pbytes);
                if (pat_out == 0) {
                    printf("[11] output pre-fill malloc failed -- "
                           "post-submit diff will be blind\n");
                } else {
                    for (wq = 0; wq + 4 <= pbytes; wq += 4) {
                        pat_out[wq] = (unsigned char)0xEF;
                        pat_out[wq + 1] = (unsigned char)0xBE;
                        pat_out[wq + 2] = (unsigned char)0xAD;
                        pat_out[wq + 3] = (unsigned char)0xDE;
                    }
                    rc = nvRmMemWrite((unsigned)memh_out, (void *)0,
                                      (unsigned)pat_out, pbytes);
                    printf("[11] output pre-fill 0xDEADBEEF: %u bytes -> "
                           "rc=0x%x\n", pbytes, (unsigned)rc);
                }
            }

            printf("[11] desc_in=%p (h=%u w=%u fmt=0x%x type=%u stride=%u "
                   "memh=0x%x planes=%u)\n",
                   desc_in, desc_in[0], desc_in[1], desc_in[2], desc_in[3],
                   desc_in[4], desc_in[5], desc_in[9]);
            printf("[11] desc_out=%p (h=%u w=%u fmt=0x%x type=%u stride=%u "
                   "memh=0x%x planes=%u)\n",
                   desc_out, desc_out[0], desc_out[1], desc_out[2],
                   desc_out[3], desc_out[4], desc_out[5], desc_out[9]);

            /* now that the descriptors exist: in mode 1 the +0x08 slot
               is the INPUT surface descriptor (measured: stage 1 reads
               geometry from it, stage 2 the format); in mode 2 it is a
               number. n08= overrides both readings. */
            if (n_set[2] == 0)
                sv[2] = (pf_mode == 1) ? (unsigned)desc_in
                                       : (din_set[1] ? din_val[1] : 8);

            /* the params kind, stock shape (live capture): zeros, mode
               at +1c, geometry at +30/+34, then the coefficient array
               0.5 (0x3f000000) to the end. Word overrides land on top. */
            {
                unsigned *pb[4];
                pb[0] = bufs[2];
                pb[1] = sbuf10;
                pb[2] = sbuf14;
                pb[3] = bufs[4];
                for (i = 0; i < 4; i++) {
                    if (strcmp(slot_kind[i], "params") != 0)
                        continue;
                    unsigned *b = pb[i];
                    unsigned q3;
                    for (q3 = 0; q3 < 44; q3++)
                        b[q3] = 0;
                    b[7] = pf_mode;  /* +1c: mode */
                    b[12] = din_set[1] ? din_val[1] : 8; /* +30: width */
                    b[13] = din_set[0] ? din_val[0] : 8; /* +34: height */
                    for (q3 = 14; q3 < 44; q3++)
                        b[q3] = params_fill;
                }
            }

            /* state 1 of 3: the OUTPUT buffer as allocation handed it,
               before anything else. Same junk as after the submission
               means reused memory; zeros there but content after means
               the ISP wrote. */
            {
                unsigned pbytes = gs * gh;
                unsigned char *chk = malloc(pbytes);
                unsigned rrc;
                if (chk == 0) {
                    printf("[11] output-after-alloc: malloc failed\n");
                } else {
                    memset(chk, 0xEE, pbytes);
                    rrc = nvRmMemRead((unsigned)memh_out, (void *)0,
                                      (unsigned)chk, pbytes);
                    print_first_words("[11] output-after-alloc first words",
                                      chk, 8);
                    printf("[11] output-after-alloc: rc=0x%x, "
                           "non-poison bytes %u of %u\n",
                           (unsigned)rrc,
                           pbytes - chk[0] * 0, pbytes); /* count below */
                    {
                        unsigned nz = 0;
                        int q;
                        for (q = 0; q < (int)pbytes; q++)
                            if (chk[q] != 0xEE)
                                nz++;
                        printf("[11] output-after-alloc: bytes differing "
                               "from poison: %u of %u\n", nz, pbytes);
                    }
                    free(chk);
                }
            }

            /* [12] the submission itself. The intent line goes out
               before the call: if the call never returns (a fence wait,
               for instance), the log shows exactly where it stopped. */
            for (i = 0; i < 7; i++) {
                if (!aux_on[i])
                    continue;
                printf("[11] aux%s=", aux_names[i]);
                for (k = 0; k < 16; k++)
                    printf("%s%02x:%08x", k != 0 ? "," : "+", k * 4,
                           bufs[i][k]);
                printf("\n");
            }
            /*
             * [11b] fill the INPUT buffer with the generated Bayer --
             * the buffer the ISP actually reads. Found order hops =
             * (handle, offset, ptr, size), offset 0, size stride*height,
             * so rows land in nvmap as bayer_fill laid them out. Then
             * state 2 of 3: read it back and compare byte-for-byte
             * with what we sent. (Commit 2b521e1 claimed this block;
             * a silent patch failure left it out -- the presence rule
             * in argtest.sh now catches that class.)
             */
            {
                unsigned pat_bytes = gs * gh;
                unsigned char *pat = malloc(pat_bytes);
                unsigned char *chk = malloc(pat_bytes);
                unsigned wrc, rrc, diff = 0;
                int q;

                if (pat == 0 || chk == 0) {
                    printf("[11] pattern malloc failed -- input buffer "
                           "stays as allocated\n");
                } else {
                    bayer_fill(pat, gw, gh, gs, 2, order);
                    wrc = nvRmMemWrite((unsigned)memh_in, (void *)0,
                                       (unsigned)pat, pat_bytes);
                    printf("[11] NvRmMemWrite(hops) memh_in <- %u pattern "
                           "bytes -> rc=0x%x\n", pat_bytes, (unsigned)wrc);

                    memset(chk, 0xEE, pat_bytes);
                    rrc = nvRmMemRead((unsigned)memh_in, (void *)0,
                                      (unsigned)chk, pat_bytes);
                    for (q = 0; q < (int)pat_bytes; q++)
                        if (chk[q] != pat[q])
                            diff++;
                    print_first_words(
                        "[11] input-after-write first words", chk, 8);
                    printf("[11] input-after-write: rc=0x%x, %u of %u "
                           "bytes differ from pattern\n",
                           (unsigned)rrc, diff, pat_bytes);
                }
                free(pat);
                free(chk);
            }

            for (i = 0; i < 7; i++)
                for (k = 0; k < 16; k++)
                    snap_aux[i][k] = bufs[i][k];
            for (k = 0; k < 16; k++) {
                snap_in[k] = desc_in[k];
                snap_out[k] = desc_out[k];
                snap_no[k] = desc_no[k];
                snap_s08[k] = bufs[2][k];
                snap_s10[k] = sbuf10[k];
                snap_s14[k] = sbuf14[k];
                snap_s18[k] = bufs[4][k];
            }

            print_syncpts("[sp pre-submit]", sp_pre);
            {
                static const unsigned ids[4] = { 32, 33, 34, 35 };
                int q;
                for (q = 0; q < 4; q++)
                    read_syncpt(ids[q], &sp_pre[q]);
            }

            /* resolve the four configurable slots (+08/+10/+14/+18) */
            s08 = resolve_slot(slot_kind[0], (unsigned)desc_in,
                               (unsigned)desc_out, (unsigned)desc_no,
                               (unsigned)bufs[2]);
            s10 = resolve_slot(slot_kind[1], (unsigned)desc_in,
                               (unsigned)desc_out, (unsigned)desc_no,
                               (unsigned)sbuf10);
            s14 = resolve_slot(slot_kind[2], (unsigned)desc_in,
                               (unsigned)desc_out, (unsigned)desc_no,
                               (unsigned)sbuf14);
            s18 = resolve_slot(slot_kind[3], (unsigned)desc_in,
                               (unsigned)desc_out, (unsigned)desc_no,
                               (unsigned)bufs[4]);
            /* word overrides re-applied after the params build: a08=
               for the +08 buffer (params may have overwritten it),
               a10=/a14= for their own buffers; a18= was applied earlier
               and bufs[4] is untouched by params */
            for (k = 0; k < 44; k++)
                if (aux_set[2][k])
                    bufs[2][k] = aux_val[2][k];
            for (k = 0; k < 44; k++) {
                if (a10_set[k])
                    sbuf10[k] = a10_val[k];
                if (a14_set[k])
                    sbuf14[k] = a14_val[k];
            }
            if (n_set[2])
                s08 = n_val[2]; /* explicit number wins (mode-2 form) */
            printf("[12] slots: +08=%s(0x%x) +10=%s(0x%x) +14=%s(0x%x) "
                   "+18=%s(0x%x)\n",
                   slot_kind[0], s08, slot_kind[1], s10,
                   slot_kind[2], s14, slot_kind[3], s18);
            /*
             * outblock=on: arm /proc/isp_patch_override with the
             * output-control block the library never pushes
             * (INCR(0xE04,3) = plane address, 0, stride).
             * Base form: the stock's own working frame gather
             * (trace submit#16 G0, 40 words), hardcoded -- its stale
             * addresses are fine because runtime RELOCs patch them
             * after the override. Word[10] (the E04 plane address) is
             * replaced with OUR pinned output address.
             * Requires kernel tracing ON: the submit counter lives in
             * the trace hook.
             */
            if (outblock_on) {
                /*
                 * Base = OUR OWN frame gather (46 words, captured from
                 * our library's submission), extended with the missing
                 * output-control block. Exactly two edits vs the
                 * original, both required:
                 *   [28] 0x11000004 -> 0x11000001: the stats command
                 *        loses three zero data words, freeing space;
                 *   [30..33] the output block: INCR(0xE04,3), plane
                 *        address, 0, stride 0x100.
                 * Word [29] = 0x80f40000 is UNTOUCHABLE: the kernel
                 * relocates the stats buffer address into it at a
                 * fixed offset. Length is exactly 46: the override is
                 * clipped to the original gather length. Stale input
                 * addresses are fine -- runtime RELOCs patch after.
                 * Requires kernel tracing ON (submit counter lives in
                 * the trace hook).
                 */
                static const unsigned base[46] = {
                    0x00000d00, 0x1e000001, 0x00070000, 0x1e010001,
                    0x00070000, 0x1e020001, 0x010000c9, 0x1e030001,
                    0x00000000, 0x15000006, 0x00000000, 0x00000000,
                    0x00000000, 0x00000000, 0x00000000, 0x00080008,
                    0x00000d00, 0x1e310001, 0x00080008, 0x1e330001,
                    0x310000c9, 0x1e320001, 0x00000008, 0x10150001,
                    0x00000007, 0x1e300001, 0x00000001, 0x00000d00,
                    0x11000001, 0x80f40000, 0x1e040003, 0xaaaaaaaa,
                    0x00000000, 0x00000100, 0x00000d00, 0x20000001,
                    0x00000424, 0x20000001, 0x00000525, 0x20000001,
                    0x00000627, 0x00000d00, 0x200c0001, 0x00000009,
                    0x200c0001, 0x0000000b,
                };
                unsigned words[46];
                unsigned addr = outblock_addr_set != 0 ? outblock_addr
                                                       : out_devaddr;
                unsigned q6;
                FILE *po;

                for (q6 = 0; q6 < 46; q6++)
                    words[q6] = base[q6];
                if (outblock_addr_set != 0)
                    words[31] = outblock_addr;
                else if (out_devaddr_valid != 0)
                    words[31] = out_devaddr; /* E04 plane: OURS */
                printf("[11c] outblock: 46 words, E04 addr=0x%x "
                       "(submit=%u gather=0)\n",
                       words[31], outblock_submit);
                for (q6 = 0; q6 < 46; q6 += 8) {
                    unsigned j;
                    printf("[11c]  ");
                    for (j = 0; j < 8 && q6 + j < 46; j++)
                        printf(" %08x", words[q6 + j]);
                    printf("\n");
                }

                {
                    FILE *tf = fopen("/proc/isp_trace/enable", "r");
                    if (tf == 0) {
                        printf("[11c] WARNING: cannot read "
                               "/proc/isp_trace/enable -- override may "
                               "not fire\n");
                    } else {
                        char tb[16] = {0};
                        if (fgets(tb, sizeof(tb), tf) != 0 && tb[0] == '0')
                            printf("[11c] WARNING: /proc/isp_trace/enable"
                                   " is 0 -- override will NOT fire\n");
                        fclose(tf);
                    }
                }

                po = fopen("/proc/isp_patch_override", "w");
                if (po == 0) {
                    printf("[11c] outblock: cannot open "
                           "/proc/isp_patch_override\n");
                } else {
                    fprintf(po, "reset_counter\n");
                    fclose(po);
                    po = fopen("/proc/isp_patch_override", "w");
                    if (po != 0) {
                        fprintf(po, "data");
                        for (q6 = 0; q6 < 46; q6++)
                            fprintf(po, " %08x", words[q6]);
                        fprintf(po, "\n");
                        fclose(po);
                    }
                    po = fopen("/proc/isp_patch_override", "w");
                    if (po != 0) {
                        fprintf(po, "submit=%u gather=0\n",
                                outblock_submit);
                        fclose(po);
                    }
                    printf("[11c] outblock: reset_counter, data(46), "
                           "submit=%u gather=0 written\n",
                           outblock_submit);
                }
            }

            /* manual gate write, only when obj0=<val> is given */
            if (obj0_set && hIsp != 0) {
                unsigned gptr = *(unsigned *)((unsigned)hIsp + 0x1318);
                if (gptr != 0) {
                    unsigned was = *(unsigned *)gptr;
                    *(unsigned *)gptr = obj0_val;
                    printf("[11c] obj0: wrote 0x%x at [0x%x] (was 0x%x)\n",
                           obj0_val, gptr, was);
                } else {
                    printf("[11c] obj0: [hIsp+0x1318] is null -- nothing "
                           "to write\n");
                }
                print_gate("[11c] after obj0 write", hIsp);
            }

            /* the output-surface relocation: stage 3 reads its target
               handle from [obj + 0x250*n + 0x24c], obj = [ctx+0x1318],
               n = the frame counter at obj+0x0c. No instruction in the
               whole binary writes that field -- nothing outside the
               decoded paths does -- so it stays zero and the memory
               job is never ordered. This key writes OUR output handle
               there (or an arbitrary value for probing). Null checks
               at every level; the counter is sanity-capped at 16. */
            if (objout_on && memh_out != 0 && hIsp != 0) {
                unsigned objp = *(unsigned *)((unsigned)hIsp + 0x1318);
                if (objp == 0) {
                    printf("[11c] objout: [hIsp+0x1318] is null -- "
                           "skipped\n");
                } else {
                    unsigned n = *(unsigned *)(objp + 0x0c);
                    if (n > 16) {
                        printf("[11c] objout: frame counter %u > 16 -- "
                               "skipped\n", n);
                    } else {
                        unsigned field = objp + 0x250 * n + 0x24c;
                        unsigned was = *(unsigned *)field;
                        unsigned val = objout_manual
                                           ? objout_val
                                           : (unsigned)memh_out;
                        *(unsigned *)field = val;
                        printf("[11c] objout: obj=0x%x n=%u field=0x%x "
                               "was=0x%x new=0x%x (out memh=0x%x)\n",
                               objp, n, field, was, val, memh_out);
                    }
                }
            }

            printf("[12] aux slots:");
            for (i = 0; i < 7; i++)
                printf(" +%s=%p(%s)", aux_names[i],
                       aux_on[i] ? (void *)bufs[i] : (void *)0,
                       aux_on[i] ? "on" : "off");
            printf("\n");
            printf("[12] NvIspProcessFrame(hIsp=0x%x, mode=%u, "
                   "slots: +08=%s +10=%s +14=%s +18=%s)\n",
                   hIsp, pf_mode, slot_kind[0], slot_kind[1],
                   slot_kind[2], slot_kind[3]);

            unsigned attempt = 0;
            int stop_all = 0;
            do {
                attempt++;
                if (attempt > 1) {
                    printf("[12] retry attempt %u of %u: +1c[0] was 0x%x "
                           "(library-written), resubmitting\n",
                           attempt, retry_n, bufs[5][0]);
                }
            if (stages_spec[0] != '0') {
                /* per-stage submission: call the stages the library
                   would call, in its order, printing each return code.
                   Packet = {mode, r2, r3}; st.2 writes into our local;
                   st.3 gets the frame counter from ctx+0x1254 plus the
                   local's value; st.4 takes the fifth argument on the
                   stack (C handles it with a 5-param prototype). A
                   nonzero rc stops the chain where the library would. */
                /*
                 * The packet is NOT three words: the library reads the
                 * block [packet .. caller args] as one array -- 0x3386
                 * dereferences pkt+0x14, which is caller arg +0x08 (the
                 * geometry pointer). Layout, frame-arithmetic verified:
                 *   +00 mode, +04 r2, +08 r3, +0c..+2c = the nine
                 *   caller stack slots (+00..+20). Sixteen words with a
                 *   zeroed tail so a slight overread stays in our
                 *   memory. desc_in ALSO travels as the third register
                 *   argument, as before.
                 */
                unsigned pkt[16] = {0};
                unsigned local_var = 0;
                unsigned counter = *(unsigned *)((unsigned)hIsp + 0x1254) + 1;
                int q, stop = 0;
                static const char sn2[5][4] = { "st1", "st2", "st3",
                                                "st4" };

                pkt[0] = pf_mode;
                pkt[1] = pkt2;
                pkt[2] = pkt3;
                pkt[3] = sv[0];
                pkt[4] = sv[1];
                pkt[5] = s08;
                pkt[6] = sv[3];
                pkt[7] = s10;
                pkt[8] = s14;
                pkt[9] = s18;
                pkt[10] = (unsigned)bufs[5];
                pkt[11] = (unsigned)bufs[6];
                printf("[12] per-stage run: stages=%s counter=%u\n",
                       stages_spec, counter);
                printf("[12] packet: mode=0x%x r2=0x%x r3=0x%x "
                       "+00=0x%x +04=0x%x +08=0x%x +0c=0x%x "
                       "+10=0x%x +14=0x%x +18=0x%x +1c=0x%x +20=0x%x\n",
                       pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5],
                       pkt[6], pkt[7], pkt[8], pkt[9], pkt[10], pkt[11]);
                for (q = 0; q < (int)strlen(stages_spec) && !stop; q++) {
                    int stg = stages_spec[q] - '0';
                    switch (stg) {
                    case 1:
                        stage_now = "st1";
                        rc = ((Stage1_fn)*(unsigned *)((unsigned)hIsp +
                              0x130c))((unsigned)hIsp, pkt,
                                       (unsigned)desc_in);
                        break;
                    case 2:
                        stage_now = "st2";
                        rc = ((Stage2_fn)*(unsigned *)((unsigned)hIsp +
                              0x1308))((unsigned)hIsp, pkt,
                                       (unsigned)desc_in, &local_var);
                        break;
                    case 3:
                        stage_now = "st3";
                        rc = ((Stage3_fn)*(unsigned *)((unsigned)hIsp +
                              0x12d4))((unsigned)hIsp, (unsigned)desc_in,
                                       counter, local_var);
                        break;
                    default:
                        stage_now = "st4";
                        rc = ((Stage4_fn)*(unsigned *)((unsigned)hIsp +
                              0x1304))((unsigned)hIsp, pkt[0],
                                       s14, s18, (unsigned)bufs[5]);
                        break;
                    }
                    printf("[12] %s -> rc=0x%x\n", sn2[stg - 1],
                           (unsigned)rc);
                    if (stg == 2)
                        printf("[12] st2 local = 0x%x\n", local_var);
                    if (rc != 0) {
                        printf("[12] library would stop here "
                               "(nonzero rc)\n");
                        stop = 1;
                    }
                }
                stage_now = 0;
            } else {
                rc = nvIspProcessFrame(hIsp, pf_mode, 0, 0,
                                       sv[0], sv[1], s08, sv[3],
                                       s10, s14, s18,
                                       (unsigned)bufs[5], (unsigned)bufs[6]);
                printf("    ProcessFrame returned rc=0x%x\n",
                       (unsigned)rc);
            }
                if (rc == 0xa && attempt < retry_n) {
                    stop_all = 0;
                    printf("[12] rc=0xa -- fix-and-repeat: retrying\n");
                } else {
                    stop_all = 1;
                }
            } while (!stop_all);

            /* post-call state of EVERY buffer we hand over, with the
               changed-word list: a "fix-and-repeat" answer (rc=10
               protocol) would be exactly one written word somewhere,
               invisible across sixteen lines of values. */
            print_state_diff("[12b] post desc_in ", snap_in, desc_in, 16);
            print_state_diff("[12b] post desc_out", snap_out, desc_out, 16);
            for (i = 0; i < 7; i++) {
                char tag[40];
                if (!aux_on[i])
                    continue;
                snprintf(tag, sizeof(tag), "[12b] post aux%s ",
                         aux_names[i]);
                print_state_diff(tag, snap_aux[i], bufs[i], 16);
            }
            print_state_diff("[12b] post desc_no ", snap_no, desc_no, 16);
            print_state_diff("[12b] post slot08buf ", snap_s08, bufs[2], 16);
            print_state_diff("[12b] post slot10buf ", snap_s10, sbuf10, 16);
            print_state_diff("[12b] post slot14buf ", snap_s14, sbuf14, 16);
            print_state_diff("[12b] post slot18buf ", snap_s18, bufs[4], 16);
            {
                /* fences start zeroed: anything non-zero here was
                   written by the library (fence ids and thresholds) */
                unsigned z16[16] = {0};
                print_state_diff("[12b] post fenceA ", z16, fenceA, 16);
                print_state_diff("[12b] post fenceB ", z16, fenceB, 16);
            }

            /* syncpoints after the call, per-id deltas, and the fence
               check: did the hardware reach the threshold the library
               ordered? */
            {
                static const unsigned ids[4] = { 32, 33, 34, 35 };
                unsigned post[4] = {0};
                int q, reached = 0, fences = 0;

                print_syncpts("[sp post-submit]", post);
                printf("[sp delta]:");
                for (q = 0; q < 4; q++)
                    printf(" id%u=%+d", ids[q],
                           (int)post[q] - (int)sp_pre[q]);
                printf("\n");

                unsigned *fslots[2];
                fslots[0] = (unsigned *)s14; /* whatever sits in +0x14 */
                fslots[1] = (unsigned *)s18; /* whatever sits in +0x18 */
                for (q = 0; q < 2; q++) {
                    unsigned *fb = fslots[q];
                    unsigned id, thresh, actual = 0;
                    if (fb == 0 || fb[0] == 0)
                        continue; /* a zero slot carries no fence */
                    id = fb[0];
                    thresh = fb[1];
                    fences++;
                    read_syncpt(id, &actual);
                    printf("[sp-check] slot%s: id=%u thresh=%u actual=%u "
                           "%s\n",
                           q == 0 ? "14" : "18", id, thresh, actual,
                           actual >= thresh ? "REACHED" : "NOT REACHED");
                    if (actual >= thresh)
                        reached++;
                }
                printf("[sp verdict]: %d of %d fences reached; deltas "
                       "above -- zeros everywhere means converged\n",
                       reached, fences);
                /* [12c] LATE object ops: after the submission (and
                   therefore after the init chain's SetStats/Apply, which
                   overwrite our writes). objset re-applied here, then
                   the late dump -- the same point the stock capture is
                   taken at. */
                if (objset_n != 0 || objdump_on || outaddr_on) {
                    unsigned objp2 =
                        *(unsigned *)((unsigned)hIsp + 0x1318);
                    int k5;
                    if (objp2 == 0) {
                        printf("[12c] obj null -- late ops skipped\n");
                    } else {
                        for (k5 = 0; k5 < objset_n; k5++) {
                            unsigned was = *(unsigned *)(objp2 +
                                objset_off[k5]);
                            *(unsigned *)(objp2 + objset_off[k5]) =
                                objset_val[k5];
                            printf("[12c] objset +0x%x: 0x%x -> 0x%x\n",
                                   objset_off[k5], was, objset_val[k5]);
                        }
                        if (outaddr_on != 0) {
                            /* the channel-record write: rec0/rec1/rec2
                               are 6-word records at +0x1660/78/90; the
                               last word is the device address. Print
                               the whole record before and after. */
                            unsigned q6;
                            int rci;
                            if (out_devaddr_valid == 0 &&
                                nvRmMemGetAddress != 0) {
                                out_devaddr = nvRmMemGetAddress(
                                    (unsigned)memh_out, 0);
                                out_devaddr_valid = 1;
                            }
                            printf("[12c] outaddr: rec before:");
                            for (q6 = 5; (int)q6 >= 0; q6--)
                                printf(" +%x:%08x",
                                       outaddr_off - q6 * 4,
                                       *(unsigned *)(objp2 + outaddr_off -
                                                    q6 * 4));
                            printf("\n");
                            rci = outaddr_manual
                                      ? 0
                                      : (out_devaddr_valid != 0);
                            *(unsigned *)(objp2 + outaddr_off) =
                                outaddr_manual ? outaddr_val
                                               : out_devaddr;
                            printf("[12c] outaddr: wrote 0x%x at +0x%x "
                                   "(rc-sim %d)\n",
                                   *(unsigned *)(objp2 + outaddr_off),
                                   outaddr_off, rci);
                            printf("[12c] outaddr: rec after:");
                            for (q6 = 5; (int)q6 >= 0; q6--)
                                printf(" +%x:%08x",
                                       outaddr_off - q6 * 4,
                                       *(unsigned *)(objp2 + outaddr_off -
                                                    q6 * 4));
                            printf("\n");
                        }
                        if (objdump_on) {
                            FILE *f = fopen(objdump_path, "wb");
                            if (f == 0) {
                                printf("[12c] objdump: cannot open "
                                       "%s\n", objdump_path);
                            } else {
                                fwrite((void *)objp2, 1, objlen, f);
                                fclose(f);
                                printf("[12c] objdump: %u bytes from "
                                       "obj@0x%x -> %s (LATE)\n",
                                       objlen, objp2, objdump_path);
                            }
                        }
                    }
                }

            /* [12b] read the OUTPUT buffer back whatever the rc was:
               the first submission that works must meet the read side
               already in place. Poisoned with 0xEE first, so untouched
               bytes are visible as such. */
            {
                unsigned pat_bytes = gs * gh;
                unsigned char *outb = malloc(pat_bytes);
                unsigned rrc, pat_diff = 0;
                int m, q;
                if (outb == 0) {
                    printf("[12] output malloc failed -- nothing to read\n");
                } else {
                    /* async hypothesis, crutch stage: wait=<ms> before
                       the read. Content that appears only with the
                       delay proves asynchronous processing. */
                    if (wait_ms != 0) {
                        struct timespec ts;
                        ts.tv_sec = (time_t)(wait_ms / 1000);
                        ts.tv_nsec = (long)(wait_ms % 1000) * 1000000L;
                        printf("[12] waiting %u ms before read\n", wait_ms);
                        nanosleep(&ts, 0);
                    }
                    memset(outb, 0xEE, pat_bytes);
                    rrc = nvRmMemRead((unsigned)memh_out, (void *)0,
                                      (unsigned)outb, pat_bytes);
                    printf("[12] output-after-submit: NvRmMemRead(hops) "
                           "memh_out %u bytes -> rc=0x%x\n",
                           pat_bytes, (unsigned)rrc);
                    print_first_words("[12] output-after-submit first words",
                                      outb, 8);
                    if (pat_out != 0) {
                        for (q = 0; q < (int)pat_bytes; q++)
                            if (outb[q] != pat_out[q])
                                pat_diff++;
                        printf("[12] post-submit: %u of %u bytes differ "
                               "from the 0xDEADBEEF pattern (%s)\n",
                               pat_diff, pat_bytes,
                               pat_diff == 0 ? "ISP did not touch it" :
                               pat_diff == pat_bytes ? "fully written" :
                               "partially written -- processing started");
                    }
                    for (m = 0; m < 8; m++)
                        printf(" %08x",
                               (unsigned)outb[m * 4] |
                               ((unsigned)outb[m * 4 + 1] << 8) |
                               ((unsigned)outb[m * 4 + 2] << 16) |
                               ((unsigned)outb[m * 4 + 3] << 24));
                    printf("\n");
                    free(outb);
                }
            }
        }
    }

    /* [14b] HwSettingsDestroy: (hset, 0xf92, ptr, dev) per the live
       log. The 0xf92 "page minus a word"-looking size is unexplained;
       passed verbatim. */
    if (st_destroy && hset == 0)
        printf("[14b] skipped: hset==0\n");
    if (st_destroy && hset != 0) {
        unsigned dscratch[16] = {0};
        stage_now = "HwSettingsDestroy";
        rc = nvIspHwSettingsDestroy(hset, 0xf92, dscratch,
                                    (unsigned)dev); /* 0xf92 stock size */
        printf("[14b] NvIspHwSettingsDestroy(hset=0x%x, 0xf92, ptr, dev) -> "
               "rc=0x%x\n", hset, (unsigned)rc);
        print_gate("[14b] after HwSettingsDestroy", hIsp);
    }

    /* [13] hand it back, completely: NvIspClose releases everything
       itself, including the host1x channel. */
    printf("[13] NvIspClose(hIsp=0x%x) -> ", hIsp);
    rc = nvIspClose(hIsp);
    printf("rc=0x%x\n", (unsigned)rc);

#ifndef HOST_ARGTEST
    {
        int k;
        for (k = 0; k < STAGE_SLOTS; k++)
            if (stage_installed[k] != 0)
                printf("[end] stage[%u] ctx+0x%x: %s\n", k,
                       stage_installed_off[k],
                       stage_hook_fired[k] ? "FIRED" : "never called");
    }
#endif

    /* [sp end] the four syncpoints at exit -- comparable across runs:
       if the entry state of run N+1 differs from the exit state of run
       N (or from a fresh boot), the runs are not comparable either */
    print_syncpts("[sp end]", 0);
    if (final_s != 0) {
        struct timespec ts;
        ts.tv_sec = (time_t)final_s;
        ts.tv_nsec = 0;
        printf("[sp] settling %u s (kernel timeouts land late)\n",
               final_s);
        nanosleep(&ts, 0);
        print_syncpts("[sp final]", 0);
    }

    printf("done\n");
    return 0;
}

} /* main */

/*
 * Hook frame discipline (mirrors the verified shim trampolines).
 * Entry: sp = S (8-aligned), args r0-r3, the library's own stack args
 * at [S..] -- untouched by us. Frame after push {r0-r3, r12, lr}
 * (24 bytes): sp+0..12 = r0..r3, sp+16 = scratch, sp+20 = caller lr.
 * stage_pre prints and returns the original, and writes the
 * continuation address into saved[4] (the frame's r12 slot). Args
 * reload from the frame, sp returns to S (the library's stack args
 * stay valid), and bx calls the original. stage_cont -- entered with
 * sp = S, the parked caller lr at S-4 -- prints the return code and
 * returns to the parked lr, leaving sp = S.
 */
#ifndef HOST_ARGTEST
__asm__(
    ".text\n"
    ".thumb\n"
    ".align 2\n");

#define STAGE_HOOK(N) \
    __asm__( \
        ".thumb\n" \
        ".align 2\n" \
        ".thumb_func\n" \
        ".hidden stage_hook_" #N "\n" \
        "stage_hook_" #N ":\n" \
        "  push {r0-r3, r12, lr}\n" \
        "  mov  r0, #" #N "\n" \
        "  add  r1, sp, #0\n" \
        "  bl   stage_pre\n" \
        "  mov  r12, r0\n" \
        "  ldr  lr, [sp, #16]\n" \
        "  ldr  r0, [sp, #0]\n" \
        "  ldr  r1, [sp, #4]\n" \
        "  ldr  r2, [sp, #8]\n" \
        "  ldr  r3, [sp, #12]\n" \
        "  add  sp, #24\n" \
        "  bx   r12\n");

STAGE_HOOK(0)
STAGE_HOOK(1)
STAGE_HOOK(2)
STAGE_HOOK(3)
STAGE_HOOK(4)
STAGE_HOOK(5)
STAGE_HOOK(6)
STAGE_HOOK(7)
#endif /* HOST_ARGTEST */
