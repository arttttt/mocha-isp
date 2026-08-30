/*
 * nvisp_shim.c -- stage 1 passthrough wrapper for libnvisp_v3.
 *
 * This library impersonates libnvisp_v3.so: it is placed on the device under
 * the original path, loads the real library (renamed to
 * /system/vendor/lib/libnvisp_v3.real.so) and forwards every call to it.
 *
 * Stage 1 discipline: forward only. No logging, no argument touch-up, no
 * intervention. The gate for stage 1 is that the camera works exactly as
 * before AND that the deployment control proves both libraries are mapped
 * (see impl-1-shim-step1.md).
 *
 * How the blind forwarding works (ARM32 / AAPCS):
 *   - the first four arguments travel in r0-r3, the rest on the stack;
 *   - each exported symbol is a tiny stub that loads the real address from
 *     its slot and does a tail jump (`bx r12`). It clobbers nothing but r12
 *     (scratch under AAPCS) and leaves lr pointing at our caller, so the
 *     real function returns straight to it. Argument stack is untouched.
 *   - dlsym returns Thumb addresses with bit0 set; `bx` performs the
 *     mode switch, so the real function (Thumb) is entered correctly.
 *
 * No libc is linked (-nostdlib). The only external symbols are dlopen and
 * dlsym, satisfied by libdl.so on the device (see shim/stubs/stub_dl.c).
 *
 * The slots are filled by the constructor, which runs when the wrapper is
 * loaded as a DT_NEEDED dependency -- before the framework ever calls into
 * the camera stack.
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
 * We want NOW (everything resolved at load, no late failures) and LOCAL
 * (the real library exports the same names as this wrapper; keeping them
 * private makes it impossible for anyone to bind past the wrapper).
 * Both are zero, so the flags argument is 0.
 */
#define SHIM_DLOPEN_FLAGS 0

/* The real library, renamed once at deployment (see the step 1 note). */
static const char real_path[] = "/system/vendor/lib/libnvisp_v3.real.so";

static void *real_handle;

/*
 * Any call that the constructor could not resolve lands here: a deliberate
 * SIGILL. A crash with a tombstone is diagnosable; a silent wrong return is
 * not. Unresolved slots must never be silently ignored.
 */
void shim_trap(void)
{
    __builtin_trap();
}

__attribute__((constructor))
static void shim_init(void)
{
    const struct shim_binding *b;

    real_handle = dlopen(real_path, SHIM_DLOPEN_FLAGS);

    for (b = shim_bindings; b->name != 0; b++) {
        void *target = real_handle
            ? dlsym(real_handle, b->name)
            : (void *)shim_trap;
        if (target == 0) {
            target = (void *)shim_trap;
        }
        *b->slot = target;
    }
}
