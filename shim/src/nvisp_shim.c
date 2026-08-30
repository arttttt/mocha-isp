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
static unsigned pf_ctx_logged;

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

/*
 * Bounded message writer: EVERY append in the shim goes through these.
 * Writes past the capacity are silently dropped and the string stays
 * terminated -- an overflow here put saved registers and pointers on
 * the stack behind the buffer, and mediaserver dereferenced our own
 * hex digits (fault addrs 0x3733...., ASCII '73'). Never write to the
 * buffer directly.
 */
static const char hexdig[] = "0123456789abcdef";

typedef struct {
    char *buf;
    unsigned cap;
    unsigned len;
    int truncated;
} shim_writer;

static void w_ch(shim_writer *w, char c)
{
    if (w->len + 1 < w->cap)
        w->buf[w->len++] = c;
    else
        w->truncated = 1;
}

static void w_str(shim_writer *w, const char *s)
{
    while (*s) {
        w_ch(w, *s);
        s++;
    }
}

static void w_hex(shim_writer *w, unsigned v)
{
    static const char dig[] = "0123456789abcdef";
    int i;

    for (i = 28; i >= 0; i -= 4)
        w_ch(w, dig[(v >> i) & 0xf]);
}

static void w_dec(shim_writer *w, unsigned v)
{
    char tmp[12];
    int n = 0;

    if (v == 0) {
        w_ch(w, '0');
        return;
    }
    while (v != 0) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n != 0)
        w_ch(w, tmp[--n]);
}

static void w_end(shim_writer *w)
{
    w->buf[w->len < w->cap ? w->len : w->cap - 1] = 0;
    if (w->truncated)
        shim_log(SHIM_LOG_PRIO_ERROR, "msg truncated -- log line lost "
                                      "characters, buffer too small");
}

__attribute__((used, visibility("hidden")))
void *shim_log_call(unsigned idx, unsigned *saved)
{
    shim_writer W;
    char msg[384];
    char line2[384];

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
     * Budget: four logged calls per binding; HwSettingsSetAttribute
     * gets 14 (the whole block set) and SetStats 8 (probe phase).
     * The ProcessFrame exception: r1 != 2 logs regardless of the
     * budget, at most 20 times per process.
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

    /*
     * Main line, worst case ~370 of 384: "hook [" + name (<=44) + "] " +
     * counter (10) + four " rN=0x........" (4*14 = 56) + the SetAttribute
     * val field (20) + " r3=0x........ sp0=0x........" (28). The
     * SetConfiguration cfg dump and the ProcessFrame stack dump never
     * appear on a hook line together with all of that -- but even stacked
     * the writer just truncates instead of corrupting the stack.
     */
    W.buf = msg;
    W.cap = sizeof(msg);
    W.len = 0;
    W.truncated = 0;
    w_str(&W, "hook [");
    w_str(&W, shim_bindings[idx].name);
    w_ch(&W, ']');
    w_ch(&W, ' ');
    call_counter++;
    w_dec(&W, call_counter);
    w_str(&W, " r0=0x");
    w_hex(&W, saved[0]);
    w_str(&W, " r1=0x");
    w_hex(&W, saved[1]);
    w_str(&W, " r2=0x");
    w_hex(&W, saved[2]);

    /*
     * The one permitted dereference: r2 of NvIspSetAttribute, known
     * arity, library reads through it right after us. Guards in code:
     * id 4 only, only over zero, pointer non-null, flag file present.
     */
    if (idx == SHIM_DEREF_IDX && saved[2] != 0) {
        unsigned v = *(const unsigned *)saved[2];
        int wrote = 0;

        if (intervention_enabled() && saved[1] == 4 && v == 0) {
            *(unsigned *)saved[2] = 1;
            wrote = 1;
        }
        w_str(&W, " val=0x");
        w_hex(&W, v);
        if (wrote != 0)
            w_str(&W, " -> 0x1");
    }

    /*
     * SetConfiguration dump: size word through r3, buffer only for a
     * sane size (0 < size <= 64), cfg words are at most 16 -- one line.
     */
    if (idx == SHIM_DEREF_IDX_CFG && saved[2] != 0 && saved[3] != 0) {
        unsigned size = *(const unsigned *)saved[3];
        w_str(&W, " size=");
        w_dec(&W, size);
        if (size != 0 && size <= 64) {
            unsigned nwords = size / 4;
            unsigned k;
            w_str(&W, " cfg=");
            for (k = 0; k < nwords; k++) {
                if (k != 0)
                    w_ch(&W, ',');
                w_hex(&W, ((const unsigned *)saved[2])[k]);
            }
        }
    }
    w_str(&W, " r3=0x");
    w_hex(&W, saved[3]);
    w_str(&W, " sp0=0x");
    w_hex(&W, saved[6]);
    w_end(&W);
    shim_log(SHIM_LOG_PRIO_INFO, msg);

    /*
     * Caller-stack dump, ProcessFrame only: 13 words with offsets.
     * 13 x 13 + base ~110 fits 384; the writer truncates safely if a
     * future edit grows it.
     */
    if (idx == SHIM_DEREF_IDX_PF) {
        int k;
        W.len = 0;
        W.truncated = 0;
        w_str(&W, " stack=");
        for (k = 0; k < 13; k++) {
            unsigned off = (unsigned)k * 4;
            if (k != 0)
                w_ch(&W, ',');
            w_ch(&W, '+');
            w_ch(&W, hexdig[(off >> 4) & 0xf]);
            w_ch(&W, hexdig[off & 0xf]);
            w_ch(&W, ':');
            w_hex(&W, saved[6 + k]);
        }
        w_end(&W);
        shim_log(SHIM_LOG_PRIO_INFO, msg);

        /*
         * The stage-3 gate, read from the LIVE session (library reads
         * exactly these right after us): ctx+0x1318 and the first word
         * of the object behind it.
         */
        if (saved[0] != 0) {
            unsigned ctx1318 =
                *(const unsigned *)((unsigned)saved[0] + 0x1318);
            W.len = 0;
            W.truncated = 0;
            w_str(&W, " ctx1318=0x");
            w_hex(&W, ctx1318);
            if (ctx1318 != 0) {
                w_str(&W, " obj0=0x");
                w_hex(&W, *(const unsigned *)ctx1318);
            } else {
                w_str(&W, " obj0=null");
            }
            w_end(&W);
            shim_log(SHIM_LOG_PRIO_INFO, msg);
        }
    }

    /*
     * Settings-content dumps, split 16 words per line: the first line
     * carries id/index/size, continuation lines carry the rest of the
     * buffer. Sizes: HwSettingsSetAttribute -- the size POINTER in the
     * caller's first stack word (saved[6]); SetStats -- where the size
     * lives is unestablished, the known per-type sizes are used and
     * marked. Reads never go past the caller's own size; pointers
     * null-checked. This is mediaserver's address space.
     */
    if ((idx == SHIM_IDX_HWSATTR || idx == SHIM_IDX_SETSTATS) &&
        saved[3] != 0) {
        unsigned size;
        unsigned words;
        unsigned q, part;
        const char *tag =
            (idx == SHIM_IDX_HWSATTR) ? "hwsa" : "stats";

        if (idx == SHIM_IDX_HWSATTR) {
            size = (saved[6] != 0) ? *(const unsigned *)saved[6] : 0;
        } else {
            unsigned t = saved[1];
            size = (t == 1) ? 0x20 : (t == 2) ? 0x68
                 : (t == 3) ? 0x24 : (t == 4) ? 0x48 : 0;
        }
        if (size == 0) {
            W.len = 0;
            W.truncated = 0;
            w_str(&W, " stats: size unavailable -- content skipped");
            w_end(&W);
            shim_log(SHIM_LOG_PRIO_INFO, msg);
            return hook_real_cache[idx];
        }
        words = size / 4;
        if (words > 34)
            words = 34; /* the largest known block is 136 bytes */

        for (part = 0; part * 16 < words; part++) {
            unsigned from = part * 16;
            unsigned to = from + 16 <= words ? from + 16 : words;
            W.len = 0;
            W.truncated = 0;
            if (part == 0) {
                w_str(&W, " [");
                w_str(&W, tag);
                w_str(&W, "] id=");
                w_dec(&W, saved[1]);
                w_str(&W, " idx=");
                w_dec(&W, saved[2]);
                w_str(&W, " size=");
                w_dec(&W, size);
                w_str(&W, " buf=");
            } else {
                w_str(&W, " [");
                w_str(&W, tag);
                w_str(&W, " cont]");
            }
            for (q = from; q < to; q++) {
                if (q != from)
                    w_ch(&W, ',');
                w_hex(&W, ((const unsigned *)saved[3])[q]);
            }
            w_end(&W);
            shim_log(SHIM_LOG_PRIO_INFO, msg);
        }

        /* blocks 8 and 9 additionally carry heap POINTERS (counter
           word, then addresses). They are printed AS-IS -- the shim
           never dereferences anything in mediaserver; the tables
           behind those pointers are read OUTSIDE the process with
           tools memread against /proc/<pid>/mem. (An in-process deref
           crashed mediaserver: a text fragment "7bcd" passed the
           heap-range heuristic and was followed. The real lesson came
           later: the crash was the ctxst line overflowing its buffer.
           Both reasons point the same way -- nothing extra runs in
           mediaserver.) */
    }

    /*
     * Descriptor dump, first two LOGGED ProcessFrame calls. Five
     * pointers, 44 words for +10/+14 and 16 for the rest, split into
     * lines of 16 words each -- every line has its own writer and its
     * own bounded buffer.
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
            int nlines = (nwords + 15) / 16;
            int part;

            if (ptr == 0) {
                W.len = 0;
                W.truncated = 0;
                w_str(&W, " ");
                w_str(&W, ptr_lbl[j]);
                w_str(&W, "null");
                w_end(&W);
                shim_log(SHIM_LOG_PRIO_INFO, msg);
                continue;
            }
            for (part = 0; part * 16 < nwords; part++) {
                unsigned from = part * 16;
                unsigned to = from + 16 <= (unsigned)nwords
                                  ? from + 16
                                  : (unsigned)nwords;
                unsigned k;
                W.len = 0;
                W.truncated = 0;
                w_str(&W, " ");
                w_str(&W, ptr_lbl[j]);
                if (part != 0) {
                    w_str(&W, " cont");
                    w_dec(&W, part + 1);
                }
                w_str(&W, " @0x");
                w_hex(&W, ptr);
                w_str(&W, " ");
                for (k = from; k < to; k++) {
                    if (k != from)
                        w_ch(&W, ',');
                    w_ch(&W, '+');
                    w_ch(&W, hexdig[((k * 4) >> 4) & 0xf]);
                    w_ch(&W, hexdig[(k * 4) & 0xf]);
                    w_ch(&W, ':');
                    w_hex(&W, ((const unsigned *)ptr)[k]);
                }
                w_end(&W);
                shim_log(SHIM_LOG_PRIO_INFO, msg);
            }
        }

        /*
         * Shared-stage context state, first two logged calls. Stages 3
         * and 4 do NOT branch by mode, so the stock's live values here
         * are lawful samples for our memory-mode run. Fixed offsets,
         * null checks at every level, window split 3 x 16 words.
         */
        if (pf_ctx_logged < 2 && saved[0] != 0) {
            unsigned c123c =
                *(const unsigned *)((unsigned)saved[0] + 0x123c);
            unsigned c1254 =
                *(const unsigned *)((unsigned)saved[0] + 0x1254);
            unsigned c1258 =
                *(const unsigned *)((unsigned)saved[0] + 0x1258);
            unsigned c125c =
                *(const unsigned *)((unsigned)saved[0] + 0x125c);
            unsigned objp =
                *(const unsigned *)((unsigned)saved[0] + 0x1318);
            unsigned obj0 = objp != 0 ? *(const unsigned *)objp : 0;
            unsigned q3;

            pf_ctx_logged++;

            W.len = 0;
            W.truncated = 0;
            w_str(&W, " [ctxst] ctx+0x123c=0x");
            w_hex(&W, c123c);
            w_str(&W, " ctx+0x1254=");
            w_hex(&W, c1254);
            w_str(&W, " +0x1258=");
            w_hex(&W, c1258);
            w_str(&W, " +0x125c=");
            w_hex(&W, c125c);
            w_end(&W);
            shim_log(SHIM_LOG_PRIO_INFO, msg);

            W.len = 0;
            W.truncated = 0;
            w_str(&W, " [ctxst] obj@0x");
            w_hex(&W, objp);
            w_str(&W, " obj0=0x");
            w_hex(&W, obj0);
            w_end(&W);
            shim_log(SHIM_LOG_PRIO_INFO, msg);

            if (objp != 0) {
                for (q3 = 0; q3 < 3; q3++) {
                    unsigned k4;
                    W.len = 0;
                    W.truncated = 0;
                    w_str(&W, " [ctxst] obj+0x1660:");
                    for (k4 = 0; k4 < 16; k4++) {
                        unsigned w =
                            *(const unsigned *)(objp + 0x1660 +
                                                (q3 * 16 + k4) * 4);
                        if (k4 != 0)
                            w_ch(&W, ',');
                        w_ch(&W, '+');
                        w_ch(&W, hexdig[(((q3 * 16 + k4) * 4) >> 4) &
                                        0xf]);
                        w_ch(&W, hexdig[((q3 * 16 + k4) * 4) & 0xf]);
                        w_ch(&W, ':');
                        w_hex(&W, w);
                    }
                    w_end(&W);
                    shim_log(SHIM_LOG_PRIO_INFO, msg);
                }
            }
        }
    }

    /* return the real address from the cache */
    return hook_real_cache[idx];
}
