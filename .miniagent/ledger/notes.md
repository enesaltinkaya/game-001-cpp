# notes

## brainstorm

## Core difficulty

The question "which GI methods suit our engine" is only answerable by mapping each candidate
technique against this renderer's concrete pipeline — not generic literature. The difficulty
is that the engine's constraints interact in non-obvious ways: Forward+ lighting at FSR-scaled
internal resolution with TAA jitter, IBL environment map as the _only_ ambient source, infinite
streaming heightmap terrain with a strict tile-determinism contract (no per-tile state), OIT
transparency, and an AMD-DCC-sensitive OIT/OIT-accumulate path — plus a hard GPU floor
(GTX 1080 Ti / RADV class) that kills heavyweight world-space methods.

## Reductions / key lemmas

- **L1 — "GI" reduces to the ambient term.** In `scene.frag`, everything except the ambient is
  already solved: direct sun (cascaded shadows + contact shadow), Forward+ point/spot (16x16
  screen tiles, max 64 lights/tile, MAX*GPU_LIGHTS=1024), emissive, SSR (specular), volumetric
  light shafts. The only "global" term is `ambientDiffuse`/`ambientSpecular` from the IBL env
  map (prefilter + SH-L1 irradiance + BRDF LUT), attenuated by `shadowDarkFactor`. Any suitable
  GI method therefore only has to credibly produce an \_ambient/irradiance* term to replace or
  extend that; it does not need to resupply direct or specular lighting. This shrinks the survey
  from "radiosity methods" to "techniques that produce per-fragment irradiance from available
  inputs."
- **L2 — available inputs bound the candidate set.** The engine already rasterizes everything
  a screen-space method needs: albedo (R16G16B16A16), oct-encoded normals + view normals,
  roughness/metallic material buffer, depth, velocity, camera jitter state (existing shaders use
  `jitteredUvFromUnjittered`, `prevViewProjection`, `invViewProjection`). What it _lacks_ is any
  persistent world-space representation: no SDFs, no world mesh (terrain is an implicit
  vertex-shader lattice), and the determinism contract forbids per-tile persisted state. Hence:
  screen-space methods are fully viable; SDF-based (Lumen) is infeasible at this world scale;
  world-space volumetric/grid methods are viable only if they stream per-terrain-tile and are
  regenerated deterministically like the heightmap tiles.
- **L3 — there is an established temporal-integration pattern.** CACAO AO deliberately has its
  own temporal accumulation pass (`ao_temporal.comp`) with jitter reprojection because the color
  TAA cannot average spatially-correlated half-res noise; volumetric light shafts also have a
  temporal pass. A noisy GI estimate is expected to ship with a dedicated temporal filter
  (jittered reprojection + velocity + depth clamp), and must remain stable under FSR 3's
  reactive mask (large unstable luminance changes trigger FSR accumulation suppression).
- **L4 — prior art already exists in the tree (unwired).** Orphan compiled debug SPIR-V with
  **no sources and no C++ passes** referencing them, at
  `c-engine/data/pak_0_engine/shaders/pass/ssgi/spv/{ssgi,ssgi_temporal}.comp.spv.debug` and
  `.../pass/gi/spv/{gi_initial,gi_gather,gi_blur,gi_temporal}.comp.spv.debug`. (An earlier note
  had a wrong repo-root "shapters/" path — corrected here.) Strings show the ssgi shader already
  implements the right architecture: hemisphere ray directions, depth ray trace, IBL-sky fallback
  on miss (`sampleIblSky`), depth-edge fade, and jittered reprojection; `gi_gather` does per-texel
  traceRay gather; `gi_blur`/`gi_temporal` are the spatial/temporal filters. A previous session
  prototyped exactly the SSGI class and left the artifacts orphaned (no `.comp` sources, so the
  next pak rebuild drops them).
- **L5 — performance floor.** Minimum supported GPU is GTX 1080 Ti class (Vulkan 1.3, BDA,
  descriptor indexing, sync2) on RADV. Per-pass GPU profiling exists (`ENGINE_LOG_PASS_GPU=1`).
  The budget implies: half-resolution GI estimate, ≤ a few rays per hemisphere sample, no
  per-frame world-space updates over a multi-kilometer streaming area.

## Recommended approach

Deliverable: `docs/global-illumination.md` — scoped survey + ranked recommendation (folded in:
cost/benefit "do we need GI at all" intro section; prior-work note on the orphan ssgi/gi
`.spv.debug` artifacts). The generic literature-first approach was rejected as ungrounded.

The natural conclusion — which must be stated — is **SSGI/SSGI++-style screen-space
irradiance as phase 1** (only method fully compatible with all constraints: half-res, temporal
filter per the AO precedent, replaces `ambientDiffuse` in `scene.frag`, IBL-sky fallback on ray
miss, no world-space state so the terrain determinism contract is untouched), **streamed
per-tile GI probes as an optional phase 2** for long-range soft bounce (regenerated with
terrain tiles; excludes vegetation), and **SDF-based GI rejected** (no SDF representation,
unbounded world, vegetation density, GPU floor). Must-be-true conditions for the
recommendation to hold: (1) the GI pass can run at internal (FSR-scaled) resolution before the
scene pass and feed `scene.frag` as an irradiance texture, with the same jitter/velocity
machinery as the AO temporal pass; (2) ambient replacement stays energy-consistent with the
existing `shadowDarkFactor` ambient suppression and CACAO AO (GI already encodes diffuse
occlusion, so AO strength must be reduced when GI is on, to avoid double darkening);
(3) vegetation (azgaar_props Lambert-only Forward+ path) gets the same GI ambient;
(4) OIT-transparent objects simply not receiving GI is an accepted artifact; (5) the FSR
reactive mask does not fire on GI shimmer (temporal filter must clamp luminance changes).

## rounds

Round 1 (task 4): wrote `docs/global-illumination.md` (5 sections: cost/benefit intro, engine
inventory, 5-method comparison table, SSGI-primary + per-tile-probes-fallback recommendation with
pass/buffer sketch and the 5 must-be-true conditions, orphan prior-work note). Verifier PASS.

Round 2 (task 5, done — **tasks.json still shows it pending; manager should mark done**):
independently re-verified every existing-code claim in docs/global-illumination.md against
c-engine sources. Fixed 3 factual errors (below); everything else verified correct, incl. pass
order (Vulkan.cpp `vulkanInit()` lines 293-323, exact match with doc §2.1), scene.frag ambient
block (scene.frag:227, IBL chain lines 213/217/219/221; shadowDarkFactor grazing recovery
lines 187-188), Forward+ config (16 px tiles VulkanLightCullingPass.cpp:79-80; 64/tile line 22;
1024 lights LightComponent.h:20), AO (CACAO no internal temporal + `ao_temporal.comp`; applied in
composite.comp:115-120 as plain `composite *= aoFactor`, no strength uniform), volumetric
temporal (light_shafts_temporal.comp, VulkanVolumetricPass.cpp:196-198), TAA input
(VulkanTaaPass.cpp:189-191), internal res (Renderer.cpp rendererUpdateRenderDimensions),
`ENGINE_LOG_PASS_GPU`/`ENGINE_DEBUG_DUMP_IMAGES`, jitter machinery (globalset.shader), terrain
512²/2048 m (HeightmapTerrain.h), azgaar_props diffuse-only ambient (azgaar_props.frag:168-174),
and orphan .spv.debug file list (grep: no C++ references).

FOUND & FIXED (3 errors, all in §2.2 G-buffer table + one clarification; corrections already
applied to the doc):

1. material row: doc said "R16G16_SFLOAT / R8G8B8A8_UNORM" — actual: R8G8B8A8_UNORM only
   (VulkanFrameResources.cpp:165); content is vec4(roughness, metallic, alphaMask, 0)
   (scene.frag:265). No 16F material buffer exists.
2. view normal row: doc said R16G16B16A16_SFLOAT full view-space normal — actual:
   R16G16_SNORM storing only normalize(inViewNormal).xy (triangle_depth.frag:59), z
   reconstructed at use.
3. velocity row: doc said R16G16B16A16_SFLOAT — actual: R16G16_SFLOAT (xy pixel velocity
   from clip-space positions, triangle_depth.frag:58, clamped ±32767).
   → GI design impact: SSGI estimate sampling velocity/view-normal must decode these
   2-component encodings (viewNormal z = sqrt(1-xy·xy); velocity xy only).
4. §4.2 "single aoStrength uniform" read like it exists — clarified it would be a NEW
   uniform multiplied into the plain `composite *= aoFactor` in composite.comp.

Residual uncertainty: the doc's GI pass placement ("after oit*composite, before ao") and the
one-frame-latency history cadence are \_design choices*, not existing-code facts — the doc labels
them as design proposals (§2.1 "can slot in", §4.1 "registered after oit_composite"), confirmed
in round 2; everything else in the doc is verified against code.
