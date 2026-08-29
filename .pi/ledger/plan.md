# Plan

Re-integrate FidelityFX Brixelizer GI into the engine. The FFX SDK static
archive (`/home/enes/Projects/c/cpp-thirdparty/fsr3.1/build.sh`) already
compiles the `brixelizer` + `brixelizergi` components (volk-backed), and a
previous engine-side integration was removed — the ground truth is
`docs/fsr3.1.md` (especially the "Brixelizer GI" sections and the removal
note at the bottom), with the Wine cross-build sample
(`build-brixgi-sample.sh` + `enable-brixgi-screenshot.sh`) as a reference
pipeline. Approach: follow the existing FFX pass pattern (e.g.
`pass/fsr/VulkanFsrPass.{h,cpp}`, `VulkanFfxUtils.h`) to add a
brixelizer-voxelizer + brixelizer-GI pass (create contexts, feed
depth/normal/velocity/scene inputs each frame, update SDF voxels from
mesh data, dispatch GI), wire its output as an additive GI term into the
composite (same slot the GI plan reserved in `composite`), and add a
test-mode IBL-disable path (env var or config flag) so dark areas show the
full GI contribution. Verify by building with `./scripts/build.sh`
(SKIP_NAVMESH=1 acceptable), then
`ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot ...` to inspect dark
areas with IBL off.

Ledger convention: this file is the plan; workers read `task.md`,
`tasks.json`, and `notes.md`. Build entry: `./scripts/build.sh`. Test
entry: `./scripts/run.sh play screenshot <path>` (IBL disabled via the
flag this integration adds).
