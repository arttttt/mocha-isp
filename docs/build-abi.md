# Building for stock Android 4.4: the ABI notes

Verified on 2026-08-30 against a snapshot of the device's `/system`.

## Toolchain

NDK r21e, clang 9:

```
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi19-clang
```

Target `armv7a-unknown-linux-android19`. The kernel's `linaro-4.9.4` will not
do: it is `gnueabihf` against glibc, and both the C library and the
floating-point calling convention are wrong for this device.

## The symbol versioning trap

Building against the NDK sysroot emits `.gnu.version_r`, asking for version
`LIBC` from `libc.so` and `libdl.so`.

The device has no symbol versioning at all. Of the **1029 ELF files** in the
snapshot, **not one** carries `.gnu.version_r`. Our binary would be the only
thing on the system exercising that branch of a 2014 dynamic linker — on the
device that holds the only working camera we have.

Rather than test whether the old linker tolerates it, we removed the
condition.

## Linking without the sysroot

Link against SONAME-only stubs instead of the NDK sysroot:

```
clang -shared -fPIC -nostdlib -Wl,-soname,libdl.so -o libdl_stub.so stub_dl.c
clang -shared -fPIC -nostdlib -O2 -o target.so src.c libdl_stub.so
```

The linker only needs symbol names and a library name. Real resolution
happens on the device through the ordinary dynamic loader.

Result: `DT_NEEDED` is `libdl.so` alone, zero versioning sections, and the
only undefined symbols are `dlopen`/`dlsym`. Both live in the device's
`lib/libdl.so` — note that on 4.4 they are *not* in `libc.so`.

## Constructors cost two symbols

`__attribute__((constructor))` pulls in `__cxa_atexit` and `__cxa_finalize`.
Both exist in the device's `libc.so`, so a constructor is usable — but it
adds `libc.so` to the dependencies. Lazy initialisation avoids that.

Note the tension with blind forwarding: a thunk that resolves its target
lazily must not disturb the stack, or stack-passed arguments are corrupted.
Functions with more than four arguments are where this shows up.

## The gate

Two halves, deliberately split because the snapshot only exists on the Mac:

`build/build.sh` runs on the server. It refuses to emit a binary when the
symbol-versioning section count is non-zero, and prints the dependency list
and undefined symbols.

`build/check-against-device.sh` runs on the Mac. It resolves every undefined
symbol and every `DT_NEEDED` entry against the snapshot and exits non-zero if
anything is absent.

Both must pass before anything reaches the device.
