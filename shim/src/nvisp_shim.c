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

static void *real_handle;
static void *hook_real_cache[41];
static unsigned call_counter;

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
__attribute__((used, visibility("hidden")))
void *shim_log_call(unsigned idx, unsigned *saved)
{
    /* hex encoding table (no libc) */
    static const char hexdig[] = "0123456789abcdef";
    char msg[160];
    char *w = msg;
    const char *p;
    int i;
    unsigned long v;

    /* build: "hook [Name] #N r0=0xXXXXXXXX r1=0xXXXXXXXX r2=0xXXXXXXXX r3=0xXXXXXXXX" */
    p = "hook [";
    while (*p) *w++ = *p++;
    p = shim_bindings[idx].name;
    while (*p) *w++ = *p++;
    *w++ = ']';
    *w++ = ' ';
    /* counter (decimal) */
    {
        char buf[12];
        int n = 0;
        unsigned c = call_counter;
        if (c == 0) { *w++ = '0'; }
        else {
            while (c > 0) { buf[n++] = '0' + (c % 10); c /= 10; }
            while (n > 0) { *w++ = buf[--n]; }
        }
    }
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
    p = " r3=0x";
    while (*p) *w++ = *p++;
    for (i = 28; i >= 0; i -= 4)
        *w++ = hexdig[(saved[3] >> i) & 0xf];
    *w = 0;

    shim_log(SHIM_LOG_PRIO_INFO, msg);

    /* ensure the real library is loaded */
    if (real_handle == 0) {
        real_handle = dlopen(real_path, SHIM_DLOPEN_FLAGS);
    }

    /* resolve: dlsym the real address for this binding, cache it */
    {
        void *t = dlsym(real_handle, shim_bindings[idx].name);
        if (t != 0) {
            hook_real_cache[idx] = t;
        }
    }
    return hook_real_cache[idx];
}
