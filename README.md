# mocha-isp

Userspace for the hardware ISP on the Xiaomi Mi Pad (mocha, Tegra K1).

This repository holds a wrapper around the stock `libnvisp_v3.so` and the
tooling around it. Research notes, reports and the system snapshot live in a
separate `isp-lab` workspace — different lifecycle, not published.

---

## Why

The tablet runs stock firmware and its camera works. That is our only
reference. The goal is to learn to drive the ISP ourselves.

Method: **from working to ours**, not from nothing to working.

1. The wrapper impersonates `libnvisp_v3.so`, loads the real one and forwards
   every call through itself. The camera keeps working.
2. The wrapper starts logging: which calls, with which arguments, in which
   order. This is what neither static analysis nor a memory dump could give.
3. The wrapper starts intervening: change one value, look at the frame.
4. The wrapper starts **serving calls itself**, one at a time, with a live
   reference right next to it for comparison.

By the end nothing of the stock library remains, and the camera has been
working the whole way.

## Why not a memory dump

We tried. The settings live on the heap, are computed at runtime, and the
handler addresses do not exist in the file at all — they are assembled in
registers during execution. Watching the call boundary is far cheaper than
digging through internals.

---

## Rules

**Builds happen on the build server only.** Commits are made on the Mac,
never on the server: what was built must correspond to a commit
unambiguously.

**Nothing reaches the device without passing the gate.** The gate has two
halves, because the device snapshot only exists on the Mac while the compiler
only exists on the server. Both must pass:

- `build/build.sh` (server) — refuses to emit a binary when the count of
  symbol-versioning sections is non-zero. Also prints dependencies and
  undefined symbols for the human to read.
- `build/check-against-device.sh` (Mac) — resolves every undefined symbol and
  every `DT_NEEDED` entry against the snapshot, and exits non-zero if
  anything is absent.

Neither half enforces what the other checks. Running only one is not a gate.

**Rehearse the retreat before the advance.** Before the first replacement:
back up the original, confirm that restoring works, and only then touch
anything. The working camera is the only reference we have.

**One owner per source, reviewed by someone other than the author.**

---

## Building

```
build/build.sh shim/src/<file>.c <name>.so
```

Output goes to `build/out/`, which is not tracked.

Toolchain: NDK r21e, `armv7a-linux-androideabi19-clang`. The kernel's
`linaro-4.9.4` will not do — different libc, different calling convention for
floating point.

Details on linking without the NDK sysroot, and on the symbol-versioning
trap, are in `docs/build-abi.md`.

---

## Layout

- `shim/src/` — wrapper sources
- `shim/stubs/` — link-time stubs (never shipped to the device)
- `tools/` — utilities
- `build/` — build script and the gate
- `docs/` — decisions and ABI notes
