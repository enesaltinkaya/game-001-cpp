# plan

## Known environment (verified in code)

- IBL off: `ENGINE_IBL_DISABLED=1` (c-engine/renderer/vulkan/resources/VulkanIbl.cpp:248 — ambient disabled, env image stays valid so GI still samples it).
- Weather: `ENGINE_AZGAAR_WEATHER_COUNT=N` (VulkanAzgaarWeatherPass.cpp:258) — note the clamp: values < 1 are raised to 1, so weather can only be reduced to 1 particle, not fully disabled. Use `ENGINE_AZGAAR_WEATHER_COUNT=1`.
- T-pose: `ENGINE_TPOSE=1` (c-game/game/player/Player.cpp:51).
- Consecutive screenshots: `ENGINE_SCREENSHOT=<base>` + `ENGINE_SCREENSHOT_COUNT=N` captures one swapchain image per frame (Vulkan.cpp:492-511), naming `base_1.jpg`…`base_N.jpg`, then stops the engine. Initial settle delay after load: `ENGINE_SCREENSHOT_DELAY_MS` (default 3000; use a larger value, e.g. 12000, so TAA converges and the camera settles before the capture window).
- Run via `./scripts/run.sh play screenshot /tmp/brix <count>` with `TERM=xterm` (run.sh uses `clear` under `set -e` and fails if TERM is unset). run.sh re-runs build.sh each time; build is incremental and should be fast if nothing changed.
- The parked player/camera in `build/c-game/data/db/db.db` must NOT be moved — it frames the object under test.

## Strategy

Run the game headless with the exact env combination (IBL off, weather reduced to 1, T-pose on, skip main menu) and capture a consecutive burst of ~12-20 frames so the temporal artifact is visible across frames. Then analyze the frames visually (wrist, leg, neck regions the user flagged), zoom/crop with image tools to quantify the per-frame color changes, and correlate with the Brixelizer GI pass code (c-engine/renderer/vulkan/pass/brixelizer/ — `ENGINE_BRIX_GI_DEBUG` / `ENGINE_BRIX_GI_SAVE` dumps exist as extra evidence) to identify the source of the glitchy color animation (suspects: GI accumulation/temporal filtering on skin, TAA/GI interaction, emissive/skin material response, IBL-off interaction). Record concrete findings per round in notes.md.

## Build/test entry points

- Build: `./scripts/build.sh` (code + shaders + assets; `SKIP_NAVMESH=1` to skip navmesh bake).
- Run: `./scripts/run.sh play screenshot /tmp/brix 16` with the env vars above.
- No unit tests for rendering; verification is screenshot comparison + code inspection.
