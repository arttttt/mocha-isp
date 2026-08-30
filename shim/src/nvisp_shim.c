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

static void shim_trap(void)
{
    __builtin_trap();
}

/*
 * Resolve binding idx: load the real library once, look the name up,
 * rewrite the slot with the real address (or the trap on failure).
 * Returns the address the slot now holds.
 */
static void *shim_resolve(int idx)
{
    const struct shim_binding *b = &shim_bindings[idx];
    void *t;

    if (real_handle == 0) {
        real_handle = dlopen(real_path, SHIM_DLOPEN_FLAGS);
    }
    if (real_handle == 0) {
        return (void *)shim_trap;
    }
    t = dlsym(real_handle, b->name);
    if (t == 0) {
        t = (void *)shim_trap;
    }
    *b->slot = t;
    return t;
}
