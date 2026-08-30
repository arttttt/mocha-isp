/*
 * Link-time stubs. These never ship to the device.
 *
 * We link against this empty library, which carries the SONAME libdl.so,
 * instead of the NDK sysroot. The linker only needs the symbol names and the
 * library name; real resolution happens on the device through the ordinary
 * dynamic loader.
 *
 * Why the detour: building against the NDK sysroot emits symbol version
 * references (.gnu.version_r asking for version LIBC). Stock Android 4.4 has
 * no symbol versioning — not one of the 1029 ELF files in the device snapshot
 * carries it. Our binary would be the only thing on the system exercising
 * that branch of a 2014 linker, on the device that holds our only working
 * camera.
 *
 * Verified 2026-08-30: with these stubs DT_NEEDED is libdl.so alone, the
 * versioning section count is zero, and the only undefined symbols are
 * dlopen/dlsym — both present in the device's lib/libdl.so.
 */

void dlopen(void) {}
void dlsym(void) {}
void dlerror(void) {}
void dlclose(void) {}
