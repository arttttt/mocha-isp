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

### Where we are

Stage 1 is done. The wrapper is on the device, the camera works through it,
and the streams are the same ones the stock library produces. Fourteen of
the forty-one entry points are actually used; the other twenty-seven were
never called, which the live slot table shows directly -- they still point
at their own trampolines.

The fourteen: `NvIspOpen`, `NvIspClose`, `NvIspProcessFrame`,
`NvIspSetAttribute`, `NvIspGetStatus`, `NvIspUpdateEmcClock`,
`NvIspSetIspClockRate`, `NvIspHwSettingsCreate`, `NvIspHwSettingsDestroy`,
`NvIspHwSettingsSetAttribute`, `NvIspHwSettingsApply`,
`NvIspSetConfiguration`, `NvIspGetStats`, `NvIspSetStats`.

Stage 2 is done. Every slot enters a hook that logs the arguments, with a
budget of four calls per binding so the per-frame traffic cannot drown the
log. The init sequence, read off a running device:

```
NvIspOpen              r1 = 1, then 2      two instances
NvIspSetConfiguration  r1 = 1, then 2      always as a pair, per instance
NvIspGetStatus         r1 = 6              the only status id the library serves
NvIspSetAttribute      r1 = 4              the only attribute id it serves
```

Two things this measurement settled that reading alone had not. The whole
group of fourteen per-frame settings is re-applied every 33 ms -- a full
rebuild per frame, not a one-time configuration. And the registers beyond a
function's real arity hold the caller's leftovers, not arguments: in one
line a value sat four bytes from a neighbouring call's leftover, close
enough to read as data. Arity first, then the registers.

### Raising the ISP by hand

`tools/ispinit` is the first thing here that acts instead of watching. It
loads the stock library, brings the ISP up, asks it a question, and gives it
back:

```
[5] NvIspOpen(dev=0x1, instance=1, &hIsp) -> rc=0x0 hIsp=0xb837aef8
[6] NvIspSetIspClockRate(rate=0x003fffff) -> rc=0x0
[7] NvIspGetStatus(id=6, size=4)          -> rc=0x0 value=0x927c0
[8] NvIspClose                            -> rc=0x0
```

Five runs in a row, a silent kernel log, and the stock camera comes up
afterwards every time. Giving the hardware back is the part that had to be
right: `NvIspClose` releases the host1x channel itself, and a leaked channel
is what would have left the camera dead.

The device handle is worth a note. `NvRmOpen` is a four-instruction stub
that writes 1 into its out-parameter, and `NvRmChannelOpen` -- which lives
in `libnvrm_graphics.so`, not `libnvrm.so` -- never reads that argument at
all. So the handle is the literal value 1. Reading the disassembly as
"pass the pointer" would have produced a silent failure; the live hook log
said `r0=1` and settled it.

The clock read-back is the proof that this reaches hardware, and it took a
control to earn that word. Asking for different rates gives different
answers:

| requested (kHz) | read back |
|---|---|
| 4194303 | 600000 |
| 600000 | 600000 |
| 300000 | 420000 |
| 100000 | 420000 |

Two distinct values, so it is not a constant, and they behave like the steps
of a clock tree: a ceiling at 600000 and a floor at 420000. The kernel's own
table for this SoC carries `{ "isp", "pll_c4", 600000000 }` -- 600 MHz, the
same ceiling, in Hz where we read kHz.

So the number crosses from our process, through the stock library, into the
clock tree, and comes back changed. That is what tells us the ISP is really
there.

### Configuring it the way the stock stack does

`ispinit` now brings the ISP up *configured*, not merely open. The stock
stack sends `NvIspSetConfiguration` twice per instance -- mode 1 with a
sixteen-word payload, then mode 2 with one word -- and the hook read both
off a running device:

```
mode 1 (64 bytes): 1 7 9 a 3 0 6 8 11 f c e b 0 10 d
mode 2 (4 bytes):  2
```

Replaying those by hand returns `rc=0` from both calls with the sizes
unchanged, three runs in a row, and the stock camera comes up afterwards.

These sixteen words are safe to hold as a constant because they are format
selectors -- values 0 to 0x11, none of them an address. Nothing that is an
address, a handle, an offset, or anything else belonging to one session may
ever be written down here. Those live exactly one run: a buffer handle comes
from the allocation that made it, and the address the ISP uses does not
exist until the kernel patches it in at submit time.

### The mode switch

`NvIspProcessFrame` takes the mode as its second argument: **1 is a frame
from memory, 2 is the stream from the sensor.** The disassembly says so from
both sides -- the caller writes it, the library branches on it -- and the
hook confirms it on a live preview, where every call arrives with `r1=2`.

The frame buffer is not passed as an address but as a memory handle and an
offset, which the library feeds to `NvRmStreamPushReloc`; the kernel
resolves the real address, in the ISP's own SMMU domain, when the command is
submitted. That is why hardcoding one would be meaningless as well as wrong.

### Getting data in and out

`NvRmMemWrite(handle, offset, ptr, size)` and `NvRmMemRead` with the same
shape: a buffer written from the CPU and read back byte for byte, 64, 512
and 2048 bytes, with no mapping step at all.

The argument order was found by enumeration, not by reading. Six
permutations, one run each, one of them passed -- against a disassembly that
had the middle pair the other way round. The same enumeration settled the
question of whether the stock's `MMAP` step is needed for us: it is not.

That is the rule this cost us a day to learn. Where the space of
possibilities is small and closed, enumerate it. Reading the disassembly
produces one candidate and no way to tell whether it is right; five such
candidates in a row were wrong, and each was refuted by hardware in a single
run.

The stock stack never uses it. The hook was changed to log any call whose
mode is not 2 outside the budget, and the camera was driven through preview,
three full-resolution stills and a video: three JPEGs written, and four
`ProcessFrame` calls, all of them mode 2. Not one call in the memory mode.
ZSL appears in the logs but is evidently implemented without a second pass
through the ISP.

So there is nothing to watch. The memory mode exists in the library and has
to be assembled by hand -- which changes what a gap in the analysis costs:
it used to mean "read it off the device", and now it means "guess".

### What a dead process leaves behind

Worth knowing, and established by accident. `ispinit` crashed between
`NvIspOpen` and `NvIspClose`, so the host1x channel was never returned --
the one leak we had been carefully avoiding, because nobody had established
whether the kernel reclaims it. It does: the stock camera came up
afterwards with all four streams and wrote a photo.

So the teardown discipline stays, but the fear behind it can go.


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
