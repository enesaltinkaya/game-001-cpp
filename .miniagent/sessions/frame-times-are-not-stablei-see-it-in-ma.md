# Scaffold session

- Task: frame times are not stable.
i see it in mangohud's display.
but our screenshot feature does not capture mangohud overlay.

so maybe you can, create a temporary file, write individual frame times in that file, for maybe 20-30 seconds, then analyze.
but that writing file logic should not interfere with engine loop. i mean io operations can be slow.
maybe store every frame time in memory, and do a single write at the end.
- Date: Tue Sep  1 09:26:48 2026
- Outcome: no clean sign-off
- Last verifier verdict: PASS

## Final report

Scaffold finished without a clean sign-off (last verifier verdict PASS). Final worker report:

## Task result

done — implemented the env-gated frame-time recorder's single end-write (task 2), ran the 25 s capture (task 3), analyzed it (task 4), and recorded the deliverable findings (task 5).

## Files changed

- `c-engine/Engine.cpp` — after the main loop in `engineStart()`: one `std::ofstream` write of all recorded samples (header + `time_offset_s frame_time_ms cpu_work_ms` per line + `# count`), plus `FRAME_TIMES` summary lines (n/mean/stdev/p50/p95/p99/max for both series) via `utils::info`; graceful `utils::error` on open failure. Added `<algorithm>/<cmath>/<fstream>/<vector>` includes.
- `.miniagent/ledger/notes.md` — appended `## final` with the full analysis and suspect ranking.

## Findings

- **Verified end-to-end:** clean strict build; 25 s run (`ENGINE_FRAME_TIMES_LOG=/tmp/frame_times.txt ENGINE_HITCH_DEBUG=1 ./scripts/run.sh play log 25000`, needs `TERM` set) reached gameplay, exited cleanly, wrote 1442 samples; summaries appear in both the file and `game.log`.
- **The CPU main loop is rock-stable in steady state:** from t≈2 s to t=25 s, all 1370 frames are 16.667–16.703 ms (zero frames above 17.5 ms); CPU work p50 3.3 ms / p95 4.4 ms / p99 10.3 ms. Every spike (>20 ms, up to one 389 ms CPU frame) occurs only in the t=0–2 s asset-loading window and joins cleanly to game.log events (pak/tex load, terrain tile gen, navmesh, settlement upload); no periodicity.
- **Why MangoHud still flickers: `"vsync": false` → swapchain is `VK_PRESENT_MODE_IMMEDIATE` (VulkanSwapchain.cpp:188–193).** The 60 fps cap is a CPU busy-wait pad, and present returns before the GPU finishes (elapsed p95 4.4 ms proves the engine window sees ~no GPU work). MangoHud measures the *presented* interval = busy-wait rhythm ± GPU completion latency, so any GPU frame >16.67 ms shows as a MangoHud spike while all engine metrics read perfect. This is the top suspect; GPU-side timing (plan approach D, VkTimestamp) or a zero-code test with `vsync: true` in `settings.json` is the discriminating next experiment.
- Caveat: this capture's steady state was at the parked location with no terrain streaming; streaming hitches (worker-thread scatter 100–210 ms, 0.3–0.8 ms GPU uploads) exist but are confined to tile swaps.
- Artifacts kept at `/tmp/frame_times.txt`, `/tmp/ft_analyze.py`, `/tmp/ft_run.log`; parked player/camera untouched.

## Remaining steps

- Ask the user to flip `"vsync": true` in `build/c-game/data/settings.json` and re-watch MangoHud (zero-code discriminator), or add a per-frame `gpuWorkNs` field via VkTimestamp queries to the existing `ENGINE_FRAME_TIMES_LOG` recorder and re-capture.
[usage: ↑368046 ↓11885 tokens]

Verifier:

## Verdict

PASS

## Commands run

- `ENGINE_FRAME_TIMES_LOG=/tmp/frame_times.txt ./scripts/run.sh play log 25000 && wc -l /tmp/frame_times.txt` — exit 0
- `head -5 /tmp/frame_times.txt; tail -3 /tmp/frame_times.txt` — exit 0 (read-only sanity check)

## First failure

None.

## Notes

- The game ran ~25 s of gameplay, hit `ENGINE_LOG_TIMEOUT` cleanly, and the new recorder logged its summary (`n=1442 mean=17.234 ms stdev=9.636 ms p50=16.667 ms p95=16.668 ms p99=16.670 ms max=250.000 ms`, cpuWork max=384.229 ms) and wrote the file.
- `/tmp/frame_times.txt` has 1444 lines = 1 header + 1442 samples + 1 `# count 1442` footer; format is `time_offset_s frame_time_ms cpu_work_ms`, consistent with the expected per-frame recording.
- The first frame is a 193 ms spike (asset-load tail), and the max of 250 ms / cpuWork 384 ms suggests a few large spikes — the data is in place for the planned spike/periodicity analysis.
- A tool-call error warning appeared after execution, but it was spurious: the command itself completed with exit 0 and all output present.
[usage: ↑56564 ↓709 tokens]
