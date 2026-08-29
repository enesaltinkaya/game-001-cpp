# plan

**Goal:** determine whether Brixelizer (FFX GPU SDF GI) diffuse color bleeding should be visible in this project, and if it is broken, find and fix the defect so GI color reflects on the player character, house walls, and terrain ground.

**Strategy:** Brixelizer is the FFX GPU-SDF global illumination system: it bakes scene geometry into SDF bricks, marches them, and writes a per-pixel diffuse/specular GI result that the composite pass multiplies into the rendered image (knob: `ENGINE_BRIX_GI_DIFFUSE_FACTOR`, default 1.0). The investigation should first establish *whether the GI pipeline is alive at all* (context created, bricks populated, SDF atlas built, GI outputs present and resolution-matched in the composite) by reading logs and the debug GUI, then trace the full data path — SDF brick generation from meshes (player, house, terrain), the GI dispatch, the composite consumption (albedo GBuffer, image layout transitions, factor scaling) — to find where the diffuse color is lost. Verification is via `./scripts/build.sh` + screenshots (`./scripts/run.sh play screenshot /tmp/x.jpg`, `ENGINE_HIDE_GUI=1` for clean frames) and log inspection; a visible colored bleed from a colored surface onto a neighbor is the acceptance test.

**Approach:** read-only diagnosis first (logs, DebugGui brixelizer panel, code path audit), then a minimal targeted fix, then screenshot-based visual verification. Do not move the parked player/camera in the transform db.

**Build/test entry points:**
- `./scripts/build.sh` (compile + asset pipeline; `SKIP_NAVMESH=1` to skip the bake when iterating)
- `./scripts/run.sh play log 5000` (runtime log; read `build/c-game/data/game.log`)
- `./scripts/run.sh play screenshot /tmp/shot.jpg` (frame capture; `ENGINE_HIDE_GUI=1` to hide HUD)
- GDB for crashes: `ENGINE_DEBUG=1` + `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json`

**Key files:** `c-engine/renderer/vulkan/pass/brixelizer/VulkanBrixelizerPass.{h,cpp}`, `c-engine/renderer/vulkan/pass/composite/VulkanCompositePass.cpp`, `c-engine/gui/rmlui/guis/debugGui/DebugGui.cpp`, `c-engine/renderer/vulkan/resources/VulkanIbl.h`, FFX headers under thirdparty `cpp-thirdparty` (FidelityFX/gpu/brixelizer).
