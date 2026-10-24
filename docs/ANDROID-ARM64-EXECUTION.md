# Proposal: an optional direct ARM64 execution backend for Android

Status: design proposal, not an implemented backend or a performance claim.
The generated-C backend remains the default and the correctness reference.

## Purpose and limits

Evaluate whether executing the original ARM64 instructions, with verified platform
rewrites, reduces CPU execution cost enough to justify another backend. This is
inspired by MacNativeGames' direct-execution approach; it is not a proposal to copy
its Apple runtime into the Android app or replace this project's Vulkan renderer
with Metal code.

Sharing an instruction set does not make a Switch executable an Android executable.
The loader, system calls, thread state, memory map, SDK imports and graphics still
need adaptation. Conversely, generated C is compiled before execution, not
interpreted during play. Either backend can be efficient; measure the difference.

Direct execution does **not** remove NVN-to-Vulkan translation, Maxwell shader
translation, GPU readbacks, buffer copies, synchronization, or driver constraints.
The existing [renderer measurements](DESEMPENHO-60FPS-2026-10-23.md) describe substantial
render-thread/GPU costs and a slower safe-copy path. Those are independent workstreams.
Do not promise higher FPS from changing only the CPU backend.

## Source baseline

This proposal was checked against BOTWRecompAndroid commit
[`fae7ba6`](https://github.com/mamcjr/BOTWRecompAndroid/tree/fae7ba6fe4f300f12b2cd0fd824e25d40b19373a).
Relevant existing boundaries are:

| Area | Existing implementation | Proposed integration |
| --- | --- | --- |
| NSO loading, relocation, version identification | `runtime/loader.c`, `runtime/rt.h` | Share validated module metadata; add a separate executable-image path. |
| Generated-function and HLE lookup | `runtime/dispatch.c`, `runtime/cpu.h` | Keep generated dispatch; add explicit native entry and host-call gates. |
| SVC, threads, waits, reboot | `runtime/kernel.c`, `runtime/rt.h` | Reuse service semantics through a defined register-context adapter. |
| Android address-window reservation | `runtime/platform/android.c`, `android/common/guest_window.h` | Reuse reservation policy, but independently validate executable mapping and guest pointers. |
| Graphics, audio, input, files, mods | `runtime/nvn/`, `runtime/services/`, `runtime/hle/` | Keep host implementations; adapt their entry boundaries. |
| Game-library compatibility | `android/gamelib_id.cmake` | Keep generated-library IDs; define distinct native artifact metadata. |

MacNativeGames reference snapshot:
[`5281186`](https://github.com/ly05010419/MacNativeGames/tree/5281186af56bdcce6ae9b3bb9daa9791bc02994e),
especially its [overview](https://github.com/ly05010419/MacNativeGames/blob/5281186af56bdcce6ae9b3bb9daa9791bc02994e/README.md),
[loader](https://github.com/ly05010419/MacNativeGames/blob/5281186af56bdcce6ae9b3bb9daa9791bc02994e/games/totk/runtime/loader.c),
[register gate](https://github.com/ly05010419/MacNativeGames/blob/5281186af56bdcce6ae9b3bb9daa9791bc02994e/games/totk/runtime/gate.S),
and [thread entry](https://github.com/ly05010419/MacNativeGames/blob/5281186af56bdcce6ae9b3bb9daa9791bc02994e/games/totk/runtime/thread_enter.S).
These are architectural references, not Android-ready drop-in components. Check
their transitive dependencies and retain required notices before reusing code;
both projects' own top-level code uses MIT licenses, but third-party code retains
its own terms.

## Stage 0: establish a baseline before writing the backend

1. Build the existing backend with documented release compiler flags. Record any
   files compiled at lower optimization; do not compare a development build with
   an optimized candidate and attribute the difference to execution architecture.
2. Profile game threads, render-thread CPU work, GPU work, waits, copy volume,
   submission count, and shader compilation separately. CPU/GPU work can overlap;
   adding their durations does not necessarily give the frame's critical path.
3. Use the same game build, driver, scene, camera, actual internal/output pixel
   dimensions, frame-rate policy and mods for A/B tests. Warm both caches equally.
   Disable interpolation and adaptive resolution for the comparison. Account for
   the game's own dynamic-resolution behavior too; report actual dimensions.
4. Measure simulation speed against wall time. A displayed frame rate or an
   increased cap is not proof of correct game speed. Do not unlock a version's FPS
   cap without independently validating its timing patches.
5. Repeat thermally comparable runs, alternate backend order, and report median
   and p95 frame times, memory use and sustained behavior. A capped scene needs
   CPU-time measurements to reveal headroom, not just an FPS counter.

Gate: if CPU execution is not a meaningful part of the critical path, prioritize
the renderer instead. A synthetic CPU improvement alone is insufficient evidence
to replace the backend.

## Stage 1: define a backend boundary and a synthetic harness

Introduce an experimental backend selector at boot, before loading modules or
creating guest threads. The name and configuration format are implementation
decisions, not existing settings documented by this proposal. Default to generated C.

The boundary must cover image preparation, main/worker thread entry, guest-to-host
calls, host-to-guest callbacks, time, faults and shutdown. Avoid converting
`GuestFn` into a native code pointer: the generated ABI takes a `Cpu *`, whereas
the native ABI carries register state and a guest stack.

Start with handwritten ARM64 fixtures containing no game code. Make a gate round
trip through an existing host service and test every register and status field.
Add a synthetic module with relocations, imports, an indirect callback, TLS and
multiple threads. Verify the release APK on unmodified Android before attempting
game startup. Keep these tests independently runnable without game files.

Gate: deterministic round trips, correct fault reports, and no changes to the
default backend's behavior.

## Stage 2: map and rewrite code safely

Use each locally supplied module's complete build identity and content hash to
select a rewrite recipe. Validate segment bounds, relocation types, imports,
instruction words, patch lengths and overlapping edits. Reject unknown modules
or instructions rather than guessing. Generate patched images and address tables
locally; commit the tools and recipes, never game bytes or generated game code.

The current Android window moves at startup. Reconcile that with actual machine
code: relocate all supported address-bearing data and code references; handle
PC-relative instructions and branch ranges; provide in-range veneers where needed.
Never assume that replacing a base constant in the C generator relocates original
instructions, literal pools, function pointers or callback targets. A failed
reservation must not overwrite an existing process mapping.

Inventory at least these rewrite classes:

- Horizon `svc` instructions: route to host service gates, not Android syscalls.
- Guest TLS/system-register access: redirect through per-guest-thread state.
- Platform-register use: Android reserves `x18`; inspect and rewrite guest uses
  without damaging host state, including any shadow-call-stack configuration.
- Timer/frequency reads: preserve the guest clock contract and conversions instead
  of assuming the host counter has the Switch's frequency or pause behavior.
- Imported SDK/NVN functions and registered callbacks: map both call directions.
- Relocated instruction windows: preserve control flow, flags and any embedded
  address calculations; distinguish code from data during discovery.

Respect Android executable-memory and storage restrictions on the actual target
API levels. Stage writable code without execute permission, complete patches and
relocations, flush the instruction cache, then establish RX code and RW data.
Do not leave RWX mappings. Query the host page size: support 4 KiB and 16 KiB
hosts, with guest permission bookkeeping at the guest granularity. Reject layouts
that cannot preserve the intended protections rather than broadening permissions.
Do not copy Darwin `MAP_JIT`, Mach APIs, Apple TLS offsets, or 16 KiB assumptions.

Gate: synthetic relocation, permission, branch-range, cache-coherency and invalid
input tests pass on both host page sizes, including denied executable mappings.

## Stage 3: implement the Android ABI bridge

Write an Android/AArch64 assembly gate with a documented register-context layout.
At each rewritten boundary preserve the complete required guest state: x0-x30,
guest SP/PC, q0-q31, NZCV, FPCR and FPSR. Define separately which fields each host
service may return or modify. A normal C call only preserves the host ABI's subset;
that is insufficient for an arbitrary rewritten instruction boundary.

Switch to a suitably aligned host stack before entering Bionic or host C code.
Keep Android's TLS usable on every host call; never leave guest TLS in the host
thread-pointer register across Bionic calls. Store guest TLS in an explicit
per-thread record accessible from the gate without first corrupting host state.
Ensure host callbacks back into guest code can nest without overwriting the
outer frame. Audit linker/PLT scratch-register use, vector arguments, structure
returns and callback signatures, not just integer-only entry functions.

Route SVC and HLE through adapters to the existing service implementations. Define
native implementations of host-to-guest calls; generated `rt_dispatch` and
`guest_call` cannot execute untouched instruction addresses by themselves. Preserve
memory ordering and exclusive/atomic instruction behavior. Validate floating-point
state with arithmetic fixtures, and retain any required instruction rewrites.

Test compatibility with host branch protection and shadow-call-stack settings.
Inspect the actual release binary's ABI requirements; do not assume every Android
device or build has identical PAC/BTI behavior. Use valid call/return and indirect
branch landing conventions where enforcement requires them.

Gate: synthetic integer, SIMD, flags, TLS, atomic, callback and nested-call tests
pass concurrently on several host threads, including error returns and reentry.

## Stage 4: integrate guest scheduling and reboot

Map each supported guest thread to an explicit host-thread record, with correct
entry arguments, guest stack, TLS, priority policy and completion behavior. Reuse
the existing handle, wait, event and mutex semantics. Validate address arbitration,
timeouts, cancellation and producer/consumer ordering before testing physics/jobs.

The current reboot path depends on cooperative unwind points. Direct guest code
can run for long periods without reaching one. Design a bounded stop protocol,
prove all guest threads have stopped, then reset mappings and host resources.
Do not unmap live code or `longjmp` out of a signal handler across arbitrary host
frames. Use signals only through a separately reviewed, async-signal-safe protocol;
distinguish deliberate write-tracking faults from actual guest or host crashes.

Integrate with `RT_ON_RESET` resource lifetime rules. Test repeated start/stop,
return to title, background/resume, audio interruption, controller reconnect and
fault recovery without stale handles or callbacks into unmapped code.

Gate: repeated synthetic boots and concurrent shutdown tests pass; no guest thread
or GPU operation can access an image after it is released.

## Stage 5: validate one game build through the existing host services

Pick one supported build as a pilot, identified by full module hashes, rather than
claiming support for all versions. Reproduce its constructor/import initialization,
then reach title, save selection, load, movement, pause, dialogue and save/reload.
Use scratch save profiles as described in `CONTRIBUTING.md`; restarting with the
other backend must not alter the original profile.

Reuse the current Vulkan, shader, audio and input layers first. Otherwise a renderer
change and a CPU-backend change cannot be evaluated independently. In particular,
retain terrain readbacks, GPU-to-CPU visibility, completion fences and correct
render-target pitch: removing them for speed can silently break physics or gameplay.

Keep the existing version-specific timing and game hooks. Convert their call
boundaries and verify patch sites; do not discard their semantics just because
the generated-C backend is no longer executing them. Validate swimming, terrain
contact, projectiles, ragdolls, animation, menus and movies against wall time at
the pilot build's supported frame rates. Add another version only after this gate.

Mods and DLC require their own compatibility pass. Preserve file overlays and
added-file enumeration, implement/validate ownership and mounting APIs where
missing, and verify executable patches against the selected build before rewriting.
Do not advertise a mod as working merely because its title screen loads. Use an
independent save copy for each mod/backend configuration.

Gate: repeatable gameplay and save/reload pass on both backends with the same
Vulkan settings, with failures and untested areas recorded explicitly.

## Stage 6: evaluate performance and keep a rollback path

Repeat Stage 0 on multiple GPU/driver families, including both the safe-copy path
and a validated faster-memory path. Record backend, game/module hashes, compiler
flags, runtime commit, OS/API level, host page size, driver, pixel dimensions,
simulation speed, median/p95 frame time, CPU/GPU timing and memory use.

Treat direct execution as a CPU experiment. Investigate renderer bottlenecks
separately: buffer-copy volume and lifetime, descriptor churn, pipeline reuse,
render-pass boundaries, submission overlap and GPU readback cost. Validate each
memory/feedback optimization on its supported devices; faster CPU execution does
not make an unsupported Vulkan feedback loop or missing synchronization valid.

Publish only sanitized measurements and reproduction steps. Exclude game files,
keys, save contents, personal filesystem paths and device identifiers. Sanitized
test metadata should be sufficient for another contributor to reproduce locally.

Choose an acceptance threshold before testing, above measured run-to-run variance:
for example, a reproducible CPU-time improvement without correctness, memory or
frame-time-tail regressions. A throughput claim additionally requires improved
wall-clock gameplay performance, not interpolated frames or a menu counter.
If rendering still dominates, document the result instead of claiming a faster port.

Backend fallback means a clean restart through the known working loader. Do not
attempt live, per-function switching between generated and original code without
a separately specified state-transfer ABI. Keep artifact hashes/version metadata
distinct so one backend cannot accidentally load the other's executable image.

## Suggested pull-request sequence

1. Profiling protocol and baseline report; no backend behavior changes.
2. Backend interface and synthetic harness; generated C still the only game backend.
3. Android gates, loader/rewriter and ABI tests behind an experimental build option.
4. Thread/reboot integration and one locally validated game build.
5. Independent renderer optimizations, each with visual and timing evidence.
6. Mod/DLC compatibility and additional game versions after the pilot passes.

Each implementation PR should state what ran on real hardware and what remains
untested. This document itself supplies no implementation, test result, generated
game artifact or guarantee that direct execution will outperform generated C.
