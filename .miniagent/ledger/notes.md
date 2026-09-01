# notes

## brainstorm

## Core difficulty

The visible instability is "MangoHud frame time", which is the wall-clock/GPU interval between presented frames — but the engine's ready-made `utils::timer.elapsed` is only the CPU+GPU work inside the `timerBegin→timerEnd` window, and the loop runs 60 fps-capped with a **busy-wait pad** (`settings.json`: `fpsLimit 60`, `fpsLimitChecked true`, `busyLoopLinux true`). Recording only `elapsed` would show the engine as healthy while MangoHud still flickers, and the recorder must add zero measurable per-frame cost (no I/O, no logging) or it pollutes the very signal being measured.

## Reductions / key lemmas

1. **True frame time already exists in the timer.** `Timer.cpp:40` computes `timer.frameTime = now - timer.start` where `timer.start` was set by the *previous* `timerBegin` — i.e. wall time between consecutive frames, exactly what MangoHud displays (modulo GPU pipeline latency, which makes MangoHud slightly larger, and a 250 ms clamp at line 43 that only affects catastrophic frames).
2. **Hitch attribution invariant.** With the busy-wait cap: `frameTime ≈ max(16.67 ms, in-window work)`. Since `vulkanPostUpdate` (submit + present) runs inside `ecsPostUpdate`, the vsync wait is *inside* the timer window, so any real hitch shows in **both** `frameTime` and `elapsed`. Recording both separates: (a) hitches (both spike), (b) baseline work cost (`elapsed` on normal frames — if it is near 16.67 ms, the engine is at the edge of the frame budget and even small work spikes become visible hitches), and (c) vsync-absorbed work (elapsed spikes but frameTime stays 16.67).
3. **Non-interference bound.** Two `double` push_backs into a pre-reserved `std::vector` per frame cost <100 ns on a 16.6 ms frame (<0.001%); a single blocking file write after the loop exits satisfies the "no slow I/O in the loop" requirement exactly. ~1500 frames × 24 bytes ≈ 36 KB.
4. **Correlation anchor.** Storing `timer.timeSinceStartSeconds` per frame lets the analysis join spikes against `game.log` wall-clock timestamps (terrain tile streaming, navmesh, pak loads, `futureTaskRun` completions) in the follow-up round. Also: the existing `ENGINE_HITCH_DEBUG` (Engine.cpp:44-48) already logs frames >20 ms to `game.log` — run the capture with it on as a free cross-check.

## Candidate approaches

**A. Record both `frameTime` and `elapsed` (plus time offset) per frame, single write at exit + summary.** One `std::vector<FrameSample>`, env-gated, zero I/O per frame. Risk: `frameTime` is CPU wall time, not MangoHud's GPU-side latency, so absolute values may read a few ms low; 250 ms clamp. Effort: low (~20 lines in Engine.cpp).
**B. Record only `timer.elapsed` (the current plan/task 1).** Minimal, but answers the wrong metric: baseline looks ~5-10 ms "healthy" even when MangoHud flickers, and cannot show frame-rate drops from anything outside the timer window. Effort: low.
**C. Per-frame ring buffer + writer thread doing async I/O.** Technically fancier, but the single-end-write already gives identical data with zero concurrency risk; extra complexity buys nothing for a 25 s capture. Effort: medium.
**D. GPU-side timing via VkTimestamp in command buffers.** Closest to MangoHud's number, but invasive (timestamp queries in the renderer), high effort, and CPU-side `frameTime` should already reproduce the visible instability pattern. Keep as fallback if approach A shows clean times while MangoHud still flickers. Effort: high.

## Recommended approach

A, with a one-line fix to the plan's task 1: sample `utils::timer.frameTime` **and** `utils::timer.elapsed` after `timerEnd()` each frame (frameTime is final by then), plus `timeSinceStartSeconds`. This is the only cheap metric pair that both reproduces MangoHud's number and supports later attribution (work vs. budget-vs-hitch). Must be true for it to work: (1) the capture run reaches gameplay before the 25 s timeout (use `play log 25000`, ≥5000 ms per AGENTS.md), (2) the parked player/camera in `db.db` is left untouched, (3) the summary line and file write happen after the loop so teardown ordering is irrelevant. Also set `ENGINE_HITCH_DEBUG=1` during the capture so `game.log` independently lists >20 ms frames for cross-checking.

## Proposed tasks

1. **Recorder (revised task 1):** in `c-engine/Engine.cpp` `engineStart()`, if `ENGINE_FRAME_TIMES_LOG` is set, each frame after `timerEnd()` push `{timeSinceStartSeconds, timer.frameTime, timer.elapsed}` into a static `std::vector` (reserve 8192); nothing else in the loop — no logging, no I/O.
2. **Single end-write (task 2, unchanged in spirit):** after the loop, one write to the env path: header line, then `time_offset_s frame_time_ms cpu_work_ms` per line, then a summary (count, mean, stdev, p50, p95, p99, max of **both** series); log the path and summary via `utils::info`; handle write failure without crashing.
3. **Capture (task 3, with HITCH_DEBUG):** `ENGINE_FRAME_TIMES_LOG=/tmp/frame_times.txt ENGINE_HITCH_DEBUG=1 ./scripts/run.sh play log 25000`; verify sane line count (~1500 ±) and that `game.log`'s HITCH lines exist at all (if zero HITCH lines but MangoHud still flickers, that is itself a finding).
4. **Analysis (task 4, extended):** Python one-off over `/tmp/frame_times.txt`: percentiles/stdev for both series; histogram; spike frames (>2× p50) with time offsets; periodicity check (spike every Nth frame / fixed period); join spike offsets against `game.log` wall-clock events (terrain streaming, futureTask, loading) and write the findings to notes.md.

## round 1

Task 1 done. `c-engine/Engine.cpp engineStart()`: `FrameTimeSample{u64 timeSinceStartSeconds; double frameTime; double elapsed;}` (24-byte POD, no padding), one `std::vector` reserved at 8192, gated by `ENGINE_FRAME_TIMES_LOG`. Per-frame work: one env-independent `bool` branch + one `push_back` after `timerEnd()` — no I/O, no logging. Sampled values confirmed: `frameTime` is final after `timerEnd()` (set in the current frame's `timerBegin`); `elapsed` is the in-window work.
- Verified: full `ninja` build clean (no warnings), and `ENGINE_FRAME_TIMES_LOG=... ./scripts/run.sh play log 6000` ran into gameplay and exited cleanly with recorder active. (Note: `run.sh` needs `TERM` set — ran with `TERM=xterm-256color`.)
- `timeSinceStartSeconds` is integer seconds (u64), which matches game.log's timestamp precision (`%H:%M:%S` in `c-utils/logger/Logger.cpp:77`), so the task-4 log join will work at 1 s granularity. If sub-second spike positioning turns out to matter, swapping to `timer.timeSinceStart / BILLION` (double, same 24 bytes) is a trivial one-line change — not done yet since task 1 specified `timeSinceStartSeconds` literally.
- The recorder vector is local to `engineStart()`; task 2 must keep the write inside `engineStart()` (after the loop, before/around the existing `windowSystemHide()/ecsDestroy()` teardown block) or hoist the storage.

## round 2 (manager curation)

- Verifier verdict FAIL after round 1: consistent with code state — task 1's recorder is in `engineStart()` but the task 2 end-write does not exist yet (no file is ever produced), so there is nothing to verify as a feature.
- tasks.json: task 1 marked done; task 2 kept next; task 4 extended with the fallback of switching `timeSinceStartSeconds` (u64) to `timer.timeSinceStart / BILLION` if the log join needs sub-second precision.

## final

Tasks 2–5 completed. Deliverable: `ENGINE_FRAME_TIMES_LOG` end-write implemented, 25 s capture recorded and analyzed.

**Task 2 (end-write):** `c-engine/Engine.cpp engineStart()`, after the loop: one `std::ofstream` write — header `# time_offset_s frame_time_ms cpu_work_ms`, one line per sample, `# count N`; then a `FRAME_TIMES <series>: n/mean/stdev/p50/p95/p99/max` summary via `utils::info` for both series; `utils::error` on open failure, no crash. Build clean, no warnings (strict flags).

**Task 3 (capture):** `TERM=xterm-256color ENGINE_FRAME_TIMES_LOG=/tmp/frame_times.txt ENGINE_HITCH_DEBUG=1 ./scripts/run.sh play log 25000` — reached gameplay, exited cleanly at timeout, **1442 samples** (~58 s avg over 25 s incl. ~1.7 s of loading frames). 7 `HITCH: frame cpu` lines in game.log, all in the 09:22:53–55 window.

**Task 4 (analysis) — the answer to task.md:**
- **Steady state (frames ~72 → 1441, i.e. ~2 s → 25 s, 1370 frames): the CPU loop is rock-solid.** frameTime: all 1370 frames in **16.667–16.703 ms** (0 frames above 17.5 ms); cpuWork: p50 3.3 ms, p95 4.4 ms, p99 10.3 ms — ~75% headroom under the 16.67 ms budget. No periodicity, no slow drift, no GC-like sawtooth on the CPU side.
- **All 8 spike frames (>20 ms) and all cpuWork >15 ms frames fall in the t=0–2 s asset-loading window** (pak/tex load, `.map` parse, 25 terrain tiles × ~60 ms, 21k settlement instances, scene models, navmesh). Largest: one 389 ms CPU frame at idx 29, its 250 ms (clamped) frameTime at idx 30. Gaps: [1, 29, 5, 10, 1, 1] — a loading burst, then single 16.35 ms tail at t=2 s (vegetation scatter results applying; the 100–210 ms scatter jobs themselves ran on worker threads, off the main frame).
- **The MangoHud flicker is NOT visible from the CPU side in steady state — and there's a concrete reason why it still flickers there: `settings.json` has `"vsync": false`, so the swapchain is created with `VK_PRESENT_MODE_IMMEDIATE_KHR` (VulkanSwapchain.cpp:188-193).** The 60 fps cap is enforced purely by the CPU busy-wait pad (`busyLoopLinux true`), and `vkQueuePresent` in IMMEDIATE mode returns before the GPU finishes the frame (evidenced by elapsed p95 = 4.4 ms — the engine window measures essentially no GPU work). MangoHud measures the *presented* interval: busy-wait rhythm ± GPU completion latency. Whenever the GPU (25 passes @ 2880×1627 + FSR3/GI/OIT) needs >16.67 ms, the present lands late (25–33 ms interval) and MangoHud flickers while every engine-side metric still reads a perfect 16.67 ms. Secondary effect of IMMEDIATE: tearing.

**Task 5 / suspect ranking:**
1. **GPU-side frame time >16.67 ms on busy frames** (most likely, by elimination) — not measurable with current recorder.
2. **Busy-wait cap jitter** (CPU timer drift / scheduler) — unlikely given the 16.667–16.703 ms spread seen.
3. Terrain/vegetation streaming hitches — real but confined to load-in and tile swaps (worker-thread scatter + `HITCH:` lines show 0.3–0.8 ms GPU uploads; only matters when the player moves the window, which the parked-capture did not).

**Recommended next steps:**
- Zero-code experiment for the user: set `"vsync": true` in `build/c-game/data/settings.json` and watch MangoHud. If it locks to ~16.7 ms flat, the flicker was the no-vsync GPU-late-present (and the real fix is FIFO + `renderScale`<1 or pass culling if the GPU can't hit 60). If it still flickers with vsync on, the GPU is missing every frame — profile GPU time per pass.
- Code experiment (plan's approach D): VkTimestamp queries around the frame's submit/present (or per pass) to measure true GPU frame time with the same `ENGINE_FRAME_TIMES_LOG` recorder — the recorder in `engineStart()` is in place and can absorb a per-frame `gpuWorkNs` field.
- Data left at `/tmp/frame_times.txt`, `/tmp/ft_analyze.py`, `/tmp/ft_run.log` for follow-up.
- The recorder (env-gated, one push_back/frame, single end-write) is now a permanent debug facility in `engineStart()`; safe to leave in.
