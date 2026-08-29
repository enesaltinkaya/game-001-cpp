# Plan

Strategy: The user reports that Brixelizer GI output makes the scene (player + house in front of her) look washed out when IBL is off and weather is disabled. The player/camera is parked in the SQLite transform db, so `./scripts/run.sh play screenshot /tmp/...` frames exactly the object under test — do not move it.

Approach:
1. Reproduce: build with `./scripts/build.sh`, then run `ENGINE_IBL_DISABLED=1 ENGINE_AZGAAR_WEATHER=0 ENGINE_TPOSE=1 ./scripts/run.sh play screenshot /tmp/baseline.jpg` (plus `ENGINE_HIDE_GUI=1` for a clean frame) and inspect the screenshot for the washed-out look.
2. Inspect the Brixelizer GI path (`c-engine/renderer/vulkan/pass/brixelizer/VulkanBrixelizerPass.{h,cpp}`, `ENGINE_BRIX_GI_*` debug envs, `ENGINE_BRIX_GI_SAVE`/`_MASK_SAVE` to dump buffers) and the composite pass to find where GI is added/tonemapped when IBL is off (likely over-additive GI, wrong scale, or missing IBL-dependent normalization).
3. Fix the root cause in the pass/shader (not the parked camera), rebuild, re-screenshot, and compare to the baseline to confirm colors are no longer washed out.

Verification entry points: `./scripts/build.sh` (code + shaders), `./scripts/run.sh play screenshot <path>` with the env vars above; visual comparison of before/after JPGs. Verifier: build must be warning-free and the after-screenshot must show corrected (non-washed-out) colors in the player/house region.
