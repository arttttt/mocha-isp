/*
 * nvisp_shim.c -- stage 1 passthrough wrapper for libnvisp_v3.
 *
 * This library impersonates libnvisp_v3.so: it is placed on the device under
 * the original path and forwards every call to the real library, which is
 * loaded lazily on the first forwarded call.
 *
 * Why lazy, not from a constructor: the wrapper is loaded as a DT_NEEDED
 * dependency of libnvmm_camera_v3, so a dlopen in our constructor ran the
 * loader re-entrantly (inside its own lock). The real library got mapped,
 * but its own constructors ran out of order and its state came up broken
 * (NvIspFlush crash, 30.08 session). Resolution now happens on the first
 * forwarded call, outside loader context.
 *
 * Call path:
 *   call -> stub -> slot = tramp_N -> trampoline: saves r0-r3/r12/lr,
 *          calls shim_resolve(N) -> real address, restores registers and
 *          sp byte for byte, rewrites the slot with the real address,
 *          enters the real function.
 *   Subsequent calls: stub -> slot -> real directly. Zero per-frame
 *   overhead after the first call.
 *
 * Transparency contract of the trampoline (verified line by line):
 *   - sp at `bx r12` equals sp at trampoline entry, byte for byte --
 *     stack-passed arguments land where the callee expects them;
 *   - r0-r3 restored (the first four arguments);
 *   - lr restored (the real function returns to the original caller);
 *   - the only clobbered register is r12 (AAPCS scratch).
 *
 * Thread safety, cheap variant: bionic serializes dlopen internally, and
 * two threads racing on the same slot both compute the same dlsym result
 * and store the same pointer -- the double store is idempotent.
 *
 * No libc is linked (-nostdlib). External symbols: dlopen and dlsym
 * (libdl.so on the device). shim_resolve and shim_trap are static: used
 * inside this object, never exported.
 */

#include "gen_passthrough.h"

/* libdl.so, resolved on the device */
void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);

/*
 * dlopen flags, bionic values (Android 4.4, 32-bit).
 *
 * Source of truth: bionic libc/include/dlfcn.h, android-4.4.4_r1:
 *   RTLD_NOW = 0, RTLD_LAZY = 1, RTLD_LOCAL = 0, RTLD_GLOBAL = 2.
 * Cross-checked on the device: libmediaplayerservice passes 0 when it means
 * RTLD_NOW. Do NOT copy glibc values (NOW=2, GLOBAL=1) -- on bionic that
 * combination reads as RTLD_LAZY | RTLD_GLOBAL.
 *
 * We want NOW (everything binds at load of the real library, no late
 * failures) and LOCAL (the real library exports the same names as this
 * wrapper; keeping them private makes it impossible for anyone to bind
 * past the wrapper). Both are zero, so the flags argument is 0.
 */
#define SHIM_DLOPEN_FLAGS 0

static const char real_path[] = "/system/vendor/lib/libnvisp_v3.real.so";

/*
 * The one binding whose r2 we may dereference: NvIspSetAttribute, arity
 * (h, attrId, in*, &size), input pointer, read by the library immediately
 * after us. The pair below is checked against the bindings table by
 * check-shim.sh (check 10). NOT NvIspHwSettingsSetAttribute (idx 16) --
 * near-identical name, different function: its r2 is an index (the live
 * log shows 0x0 and 0x1), and a read there would kill mediaserver.
 */
#define SHIM_DEREF_BINDING "NvIspSetAttribute"
#define SHIM_DEREF_IDX     6

/*
 * Second permitted dereference: NvIspSetConfiguration(handle, mode,
 * buf*, &size) -- r2 is the buffer, r3 the size pointer, both stack
 * addresses the library reads immediately after us (live log: pairs of
 * r1=1 then r1=2 per instance, r2/r3 in the stack range). We dump the
 * buffer CONTENT, the structure we must reproduce to raise the ISP
 * ourselves: one word through r3 gives the real size, then floor(size/4)
 * words through r2, never more than 16 words / 64 bytes, and never a
 * buffer read when the size is insane -- the size comes from the caller
 * and we do not control it.
 */
#define SHIM_DEREF_BINDING_CFG "NvIspSetConfiguration"
#define SHIM_DEREF_IDX_CFG     21

/*
 * NvIspProcessFrame, bindings-table index 5. Its r1 is the processing
 * mode: the stock always shows 2 (the ordinary path); the owner
 * suspects the from-memory mode is never used at all. To CHECK that
 * cheaply rather than believe it, a call with r1 != 2 bypasses the
 * budget and logs whatever the frame count is -- capped at 20 lines
 * per process so an unexpected stream cannot flood logcat.
 * On every logged call we also dump 13 words of the caller's stack
 * starting at the caller's original sp (saved[6]), each with its
 * offset in the line: the library reads five of them (+0x10..+0x20 in
 * the caller's frame, prologue delta 0x48), and the offsets in print
 * make a numbering shift impossible. The words are NOT dereferenced --
 * the stack is ours, we do not follow them further.
 */
#define SHIM_DEREF_BINDING_PF "NvIspProcessFrame"
#define SHIM_DEREF_IDX_PF     5

/*
 * Settings-content dumps: the stock fills the per-block configuration
 * buffers LIVE through HwSettingsSetAttribute (binding 16, fourteen
 * calls) and SetStats (binding 29, four calls). We replay zeros today;
 * dumping the real content lets the memory-mode run feed real values
 * instead. Budget for binding 16 raised to 14 so the WHOLE set passes
 * at least once.
 */
#define SHIM_IDX_HWSATTR 16
#define SHIM_IDX_SETSTATS 29
#define SHIM_HWSATTR_BUDGET 14
#define SHIM_SETSTATS_BUDGET 8
static unsigned pf_odd_logged;
static unsigned pf_dumped;

/*
 * --- the intervention gate -------------------------------------------------
 *
 * Writing into a stranger's memory (the debug flag behind r2) is toggled by
 * a file, not a constant: presence of /data/local/tmp/nvisp_shim_flags
 * turns the intervention on, deleting it turns it off -- after a
 * mediaserver restart, with no redeploy. Checked once at the first
 * SetAttribute log and cached; nothing reads the file per call.
 *
 * open/read/close come from libc.so the same way everything else does --
 * dlopen + dlsym -- so DT_NEEDED stays libdl.so. (Verified 2026-08-30:
 * libc.so in the snapshot exports all three.) O_RDONLY is 0 on Linux.
 */
static const char flags_path[] = "/data/local/tmp/nvisp_shim_flags";
static int flags_checked;   /* 0 = not yet, 1 = decided */
static int flags_present_;  /* the decision */
static int (*libc_open)(const char *, int);
static long (*libc_read)(int, void *, unsigned long);
static int (*libc_close)(int);

static int intervention_enabled(void)
{
    int fd;

    if (flags_checked)
        return flags_present_;
    flags_checked = 1;
    flags_present_ = 0;
    if (libc_open == 0) {
        void *h = dlopen("libc.so", SHIM_DLOPEN_FLAGS);
        if (h != 0) {
            libc_open = (int (*)(const char *, int))dlsym(h, "open");
            libc_read = (long (*)(int, void *, unsigned long))dlsym(h, "read");
            libc_close = (int (*)(int))dlsym(h, "close");
        }
    }
    if (libc_open == 0 || libc_read == 0 || libc_close == 0)
        return 0;
    fd = libc_open(flags_path, 0);
    if (fd < 0)
        return 0;
    libc_close(fd);
    flags_present_ = 1;
    return 1;
}

static void *real_handle;
static void *hook_real_cache[41];
static unsigned call_counter;
static unsigned shim_resolve_failures;
static unsigned shim_budget[41];
static unsigned seen_pairs[64];
static unsigned seen_pairs_n;

/*
 * Logging via liblog, loaded the same way as the real library -- dlopen +
 * dlsym -- so DT_NEEDED stays exactly libdl.so and nothing new is pulled
 * into mediaserver. The logger is optional: if liblog is unavailable the
 * shim keeps working silently (stage 1 must not depend on the log channel).
 */
static const char log_path[] = "liblog.so";
static const char log_sym[] = "__android_log_print";
static int (*log_print)(int prio, const char *tag, const char *text);

#define SHIM_LOG_PRIO_INFO 4
#define SHIM_LOG_PRIO_ERROR 6
static const char log_tag[] = "NVISP_SHIM";

static void shim_log(int prio, const char *text)
{
    if (log_print == 0) {
        void *h = dlopen(log_path, SHIM_DLOPEN_FLAGS);
        if (h != 0) {
            log_print = (int (*)(int, const char *, const char *))dlsym(h, log_sym);
        }
        if (log_print == 0) {
            return; /* log channel unavailable -- stay silent, keep working */
        }
    }
    log_print(prio, log_tag, text);
}

/*
 * shim_trap is referenced only from C (shim_resolve below), so a plain
 * static definition is safe: the compiler sees the use and keeps it.
 * shim_resolve is a different case -- see the attribute on its definition.
 */
static void shim_trap(void)
{
    __builtin_trap();
}

/*
 * Resolve binding idx: load the real library once, look the name up,
 * rewrite the slot with the real address (or the trap on failure).
 * Returns the address the slot now holds.
 *
 * used    : the only reference is `bl shim_resolve` from the trampolines --
 *           assembly the compiler does not see; without `used` it discards
 *           this function as unused and the reference dangles.
 * hidden  : plumbing, must stay out of the dynamic symbol table.
 * visible : a static function may be renamed by the compiler, which would
 *           break the assembly reference by name.
 */
__attribute__((used, visibility("hidden")))
void *shim_resolve(unsigned idx)
{
    const struct shim_binding *b = &shim_bindings[idx];
    void *t;
    char msg[112];

    if (real_handle == 0) {
        real_handle = dlopen(real_path, SHIM_DLOPEN_FLAGS);
    }
    if (real_handle == 0) {
        shim_log(SHIM_LOG_PRIO_ERROR,
                 "resolve: real library load failed, slot -> trap");
        *(b->slot) = (void *)shim_trap;
        return (void *)shim_trap;
    }
    t = dlsym(real_handle, b->name);
    if (t == 0) {
        shim_log(SHIM_LOG_PRIO_ERROR,
                 "resolve: dlsym FAILED, slot -> trap");
        *(b->slot) = (void *)shim_trap;
        return (void *)shim_trap;
    }
    *b->slot = t;

    /*
     * Log the resolution: "resolve [Name] -> 0xABCD". The name is a fixed
     * literal from the bindings table (longest is 46 characters), so a
     * 112-byte buffer cannot overflow.
     */
    {
        const char *p = "resolve [";
        char *w = msg;
        while (*p != 0) {
            *w++ = *p++;
        }
        p = b->name;
        while (*p != 0 && w < msg + 100) {
            *w++ = *p++;
        }
        *w++ = ']';
        *w = 0;
    }
    {
        /* " -> 0xABCD" -- адрес реальной функции */
        const char *hex = "0123456789abcdef";
        char *w = msg;
        while (*w != 0) {
            w++;
        }
        *w++ = ' ';
        *w++ = '-';
        *w++ = '>';
        *w++ = ' ';
        *w++ = '0';
        *w++ = 'x';
        unsigned long v = (unsigned long)t;
        for (int shift = 28; shift >= 0; shift -= 4) {
            *w++ = hex[(v >> shift) & 0xf];
        }
        *w = 0;
    }
    shim_log(SHIM_LOG_PRIO_INFO, msg);
    return t;
}

/*
 * Called by the stage-2 hook on every forwarded call to the hooked binding.
 * Logs the four argument registers and the call counter, then returns the
 * real function address so the caller can jump to it.
 *
 * used    : called only from assembly (the stage-2 hook stubs).
 * hidden  : plumbing, stays out of the dynamic symbol table.
 */
static char *shim_put_hex(char *w, unsigned long v)
{
    static const char hd[] = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4)
        *w++ = hd[(v >> i) & 0xf];
    return w;
}

static char *shim_put_dec(char *w, unsigned v)
{
    char buf[12];
    int n = 0;
    if (v == 0) { *w++ = '0'; return w; }
    while (v > 0) { buf[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0) { *w++ = buf[--n]; }
    return w;
}

__attribute__((used, visibility("hidden")))
void *shim_log_call(unsigned idx, unsigned *saved)
{
    static const char hexdig[] = "0123456789abcdef";
    /* 384: the base fields take ~90; the SetConfiguration dump adds up
       to " size=4294967295 cfg=" + 16 eight-digit words + 15 commas
       (~168). 192 could overflow on a full mode-1 dump. */
    char msg[384];
    char *w = msg;
    const char *p;
    int i;

    /* ensure the real library is loaded */
    if (real_handle == 0) {
        real_handle = dlopen(real_path, SHIM_DLOPEN_FLAGS);
    }

    /* resolve: dlsym the real address for this binding, cache it,
       fall back to trap if resolution fails */
    if (hook_real_cache[idx] == 0) {
        void *t = 0;
        if (real_handle != 0) {
            t = dlsym(real_handle, shim_bindings[idx].name);
        }
        if (t == 0) {
            t = (void *)shim_trap;
            shim_log(SHIM_LOG_PRIO_ERROR,
                     "resolve: dlsym FAILED, slot -> trap");
        }
        hook_real_cache[idx] = t;
    }

    /*
     * Budget: four logged calls per binding. The ProcessFrame exception:
     * r1 != 2 logs regardless of the budget -- the point is to catch a
     * nonstandard mode no matter how many ordinary frames ran before --
     * but at most 20 times per process.
     */
    if (shim_budget[idx] >=
        (idx == SHIM_IDX_HWSATTR ? SHIM_HWSATTR_BUDGET
                                 : idx == SHIM_IDX_SETSTATS
                                     ? SHIM_SETSTATS_BUDGET
                                     : 4)) {
        int bypass = (idx == SHIM_DEREF_IDX_PF && saved[1] != 2 &&
                      pf_odd_logged < 20);
        if (!bypass)
            return hook_real_cache[idx];
        pf_odd_logged++;
    }
    shim_budget[idx]++;

    /* build: "hook [Name] #<counter> r0=0x... r1=0x... r2=0x... r3=0x... sp0=0x..." */
    p = "hook [";
    while (*p) *w++ = *p++;
    p = shim_bindings[idx].name;
    while (*p) *w++ = *p++;
    *w++ = ']';
    *w++ = ' ';
    /* counter (decimal) */
    call_counter++;
    w = shim_put_dec(w, call_counter);
    p = " r0=0x";
    while (*p) *w++ = *p++;
    for (i = 28; i >= 0; i -= 4)
        *w++ = hexdig[(saved[0] >> i) & 0xf];
    p = " r1=0x";
    while (*p) *w++ = *p++;
    for (i = 28; i >= 0; i -= 4)
        *w++ = hexdig[(saved[1] >> i) & 0xf];
    p = " r2=0x";
    while (*p) *w++ = *p++;
    for (i = 28; i >= 0; i -= 4)
        *w++ = hexdig[(saved[2] >> i) & 0xf];
    /*
     * The one permitted dereference. r2 of NvIspSetAttribute is a
     * known-valid input pointer at call time: arity (h, attrId, in*,
     * &size), and the library reads through it right after us -- we read
     * exactly what it is about to read. Four bytes, this binding only,
     * inside the budget, never for any other binding: for the rest the
     * arity or the pointer guarantee is unestablished, and a read through
     * a non-pointer would kill mediaserver. The live log shows small
     * integers (0x0, 0x1) in r2 of the HwSettings lookalike -- exactly
     * why the binding is pinned by name and index above.
     */
    if (idx == SHIM_DEREF_IDX && saved[2] != 0) {
        /* print what we READ, before any write: "val=<as found>" then
           the arrow marks our write. Printing the post-write value (as
           this code once did) makes "was 0, we set 1" indistinguishable
           from "was 1, untouched" -- the print must precede the action. */
        unsigned v = *(const unsigned *)saved[2];
        int wrote = 0;

        /*
         * The intervention, first of its kind. Turn the library's debug
         * flag ON, under all three guards in code, not in argument:
         * only id 4 (the only id this call supports), only over a zero
         * (a one means somebody enabled it before us -- leave it), only
         * over a valid pointer, and only when the flag file exists.
         * Our change must be visible in the log as ours: val=0x0 -> 0x1.
         */
        if (intervention_enabled() && saved[1] == 4 && v == 0) {
            *(unsigned *)saved[2] = 1;
            wrote = 1;
        }
        p = " val=0x";
        while (*p) *w++ = *p++;
        for (i = 28; i >= 0; i -= 4)
            *w++ = hexdig[(v >> i) & 0xf];
        if (wrote) {
            p = " -> 0x1";
            while (*p) *w++ = *p++;
        }
    }

    /*
     * The SetConfiguration dump. Guards in code: both pointers non-null,
     * size word read through r3 first, buffer read only for a sane size
     * (0 < size <= 64 bytes), words = size/4, printed on one line as
     * comma-separated 32-bit hex. Mode 1 should name 0x40 (16 words of
     * format selectors), mode 2 a single word -- but we print what the
     * size says, not what the analysis predicted.
     */
    if (idx == SHIM_DEREF_IDX_CFG && saved[2] != 0 && saved[3] != 0) {
        unsigned size = *(const unsigned *)saved[3];
        p = " size=";
        while (*p) *w++ = *p++;
        w = shim_put_dec(w, size);
        if (size != 0 && size <= 64) {
            unsigned nwords = size / 4;
            unsigned k;
            p = " cfg=";
            while (*p) *w++ = *p++;
            for (k = 0; k < nwords; k++) {
                unsigned word = ((const unsigned *)saved[2])[k];
                if (k != 0)
                    *w++ = ',';
                for (i = 28; i >= 0; i -= 4)
                    *w++ = hexdig[(word >> i) & 0xf];
            }
        }
    }
    p = " r3=0x";
    while (*p) *w++ = *p++;
    for (i = 28; i >= 0; i -= 4)
        *w++ = hexdig[(saved[3] >> i) & 0xf];
    /* stack candidate: saved[6] = the caller's original sp */
    p = " sp0=0x";
    while (*p) *w++ = *p++;
    for (i = 28; i >= 0; i -= 4)
        *w++ = hexdig[(saved[6] >> i) & 0xf];
    /*
     * Caller-stack dump, ProcessFrame only: 13 words from the caller's
     * original sp, each printed with its offset so a numbering shift
     * cannot hide inside the list. Nothing dereferenced: the stack is
     * ours and valid, we stop after reading the words themselves.
     */
    if (idx == SHIM_DEREF_IDX_PF) {
        int k;
        p = " stack=";
        while (*p) *w++ = *p++;
        for (k = 0; k < 13; k++) {
            unsigned word = saved[6 + k];
            unsigned off = (unsigned)k * 4;
            if (k != 0)
                *w++ = ',';
            *w++ = '+';
            *w++ = hexdig[(off >> 4) & 0xf];
            *w++ = hexdig[off & 0xf];
            *w++ = ':';
            for (i = 28; i >= 0; i -= 4)
                *w++ = hexdig[(word >> i) & 0xf];
        }

        /*
         * The stage-3 gate, read from the LIVE session. Stage 3 starts
         * by loading ctx+0x1318, dereferencing it, and bailing out
         * when the first word of that object is zero; our own runs
         * never reach it. The library reads exactly these fields right
         * after us on its own path, so the reads are as safe as its
         * own: handle null-checked, pointer null-checked, one word
         * deep, inside the budget. The stock's live values tell us
         * what the field must contain -- and whether it is even
         * nonzero when the sensor path works.
         */
        if (saved[0] != 0) {
            unsigned ctx1318 =
                *(const unsigned *)((unsigned)saved[0] + 0x1318);
            p = " ctx1318=0x";
            while (*p) *w++ = *p++;
            for (i = 28; i >= 0; i -= 4)
                *w++ = hexdig[(ctx1318 >> i) & 0xf];
            if (ctx1318 != 0) {
                unsigned obj0 = *(const unsigned *)ctx1318;
                p = " obj0=0x";
                while (*p) *w++ = *p++;
                for (i = 28; i >= 0; i -= 4)
                    *w++ = hexdig[(obj0 >> i) & 0xf];
            } else {
                p = " obj0=null";
                while (*p) *w++ = *p++;
            }
        }
    }

    /*
     * Buffer content dumps for the settings walk. HwSettingsSetAttribute
     * (16): (hSettings, blockId, index, buf, &size) -- the size POINTER
     * is the caller's first stack word (saved[6]). SetStats (29): same
     * form. We print exactly size/4 words from buf, never past the
     * caller's own size, pointers null-checked first -- this is
     * mediaserver's address space.
     */
    if ((idx == SHIM_IDX_HWSATTR || idx == SHIM_IDX_SETSTATS) &&
        saved[3] != 0 && saved[6] != 0) {
        unsigned size = *(const unsigned *)saved[6];
        unsigned words = size / 4;
        unsigned q;

        if (words > 34)
            words = 34; /* the largest known block is 136 bytes */
        p = (idx == SHIM_IDX_HWSATTR) ? " hwsa id=" : " stats id=";
        while (*p) *w++ = *p++;
        w = shim_put_dec(w, saved[1]);
        p = " idx=";
        while (*p) *w++ = *p++;
        w = shim_put_dec(w, saved[2]);
        p = " size=";
        while (*p) *w++ = *p++;
        w = shim_put_dec(w, size);
        p = " buf=";
        while (*p) *w++ = *p++;
        for (q = 0; q < (int)words; q++) {
            unsigned word = ((const unsigned *)saved[3])[q];
            if (q != 0)
                *w++ = ',';
            for (i = 28; i >= 0; i -= 4)
                *w++ = hexdig[(word >> i) & 0xf];
        }

        /* blocks 8 and 9 additionally carry heap POINTERS (counter
           word, then addresses). They are printed AS-IS below -- the
           shim never dereferences anything in mediaserver; the tables
           behind those pointers are read OUTSIDE the process with
           tools memread against /proc/<pid>/mem. (An in-process deref
           crashed mediaserver: a text fragment "7bcd" passed the
           heap-range heuristic and was followed. Lesson recorded in
           the commit that removes it.) */
    }
    *w = 0;

    shim_log(SHIM_LOG_PRIO_INFO, msg);

    /*
     * Descriptor dump, first two LOGGED ProcessFrame calls only (five
     * pointers x 16 words is 80 numbers per call; four calls would flood
     * logcat). Five pointers live at stack offsets +10..+20
     * (saved[10..14]): the library NULL-checks and dereferences them
     * immediately after us, so reading words through each non-null one
     * is exactly what it is about to read. A zero prints as null,
     * never silently: a null PAIR is legal there ("both or neither"),
     * and we must see which one is empty. Pointers found INSIDE the
     * descriptors are not followed -- we do not know they are pointers
     * yet, and a wrong read would kill mediaserver.
     *
     * +10 and +14 are dumped 44 words deep (0xb0 bytes, impl-2: the
     * caller's record stride, stage reads reach 0x28, ptr14's live data
     * fits inside): the input-frame descriptor with plane layout and
     * crop lives there. The other three stay at 16 -- further out it is
     * empty or uninteresting, and the log is heavy already.
     */
    if (idx == SHIM_DEREF_IDX_PF && pf_dumped < 2) {
        static const char ptr_lbl[5][7] = {
            "ptr10=", "ptr14=", "ptr18=", "ptr1c=", "ptr20="
        };
        int j;

        pf_dumped++;
        for (j = 0; j < 5; j++) {
            unsigned ptr = saved[10 + j];
            int nwords = (j <= 1) ? 44 : 16;
            char dmsg[640];
            char *dw = dmsg;
            const char *q;
            int k;

            q = ptr_lbl[j];
            while (*q) *dw++ = *q++;
            if (ptr == 0) {
                q = "null";
                while (*q) *dw++ = *q++;
            } else {
                const unsigned *dp = (const unsigned *)ptr;
                for (k = 0; k < nwords; k++) {
                    unsigned word = dp[k];
                    unsigned off = (unsigned)k * 4;
                    if (k != 0)
                        *dw++ = ',';
                    *dw++ = '+';
                    *dw++ = hexdig[(off >> 4) & 0xf];
                    *dw++ = hexdig[off & 0xf];
                    *dw++ = ':';
                    for (i = 28; i >= 0; i -= 4)
                        *dw++ = hexdig[(word >> i) & 0xf];
                }
            }
            *dw = 0;
            shim_log(SHIM_LOG_PRIO_INFO, dmsg);
        }
    }

    /* return the real address from the cache */
    return hook_real_cache[idx];
}
