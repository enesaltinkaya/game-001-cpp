# Anti-Aliasing Implementation Plan

## Goal

Implement the new AA settings that now exist in the renderer/UI API:

- MSAA: `Off / 2x / 4x`
- SMAA: `Off / 1x / T2x`
- Sample Rate Shading: `Off / Half / Full`
- CAS Sharpening: `0–100%`

Current status:

- TAA has been removed.
- Camera jitter has been removed.
- UI + settings + renderer API placeholders now exist.
- CAS is already functional in the final pass.
- MSAA / SMAA / sample shading are not implemented in the Vulkan backend yet.

This plan is written to keep the pipeline stable and avoid reintroducing TAA-era ghosting complexity.

---

## Design principles

### 1) Keep AA features independent

Do not rebuild a monolithic “AA mode” system.

Treat these as separate features:

- raster sample count (MSAA)
- edge post-AA (SMAA)
- per-sample shading rate (sample shading)
- post sharpen (CAS)

That matches the current settings/UI shape and makes future combinations possible.

### 2) Do MSAA first, then SMAA 1x, then optional SMAA T2x

Recommended order:

1. CAS polish
2. real MSAA support
3. sample shading wiring
4. SMAA 1x
5. SMAA T2x

This gives useful wins early without forcing a large temporal refactor.

### 3) Keep temporal behavior opt-in and isolated

If SMAA T2x is added later, its jitter/history path should be local to SMAA T2x.
Do not restore the old global camera jitter/TAA assumptions.

### 4) Preserve current resolved-color contract

Right now downstream passes expect:

- `ResolvedColor` exists
- bloom/final sample it

Keep that contract stable even when the source becomes MSAA or SMAA-processed.

---

## Current relevant files

### Renderer API / settings / UI

- `c-engine/renderer/Renderer.h`
- `c-engine/renderer/Renderer.c`
- `c-utils/settings/Settings.c`
- `c-game/game/settingsGui/video/SettingsVideoGui.c`
- `c-game/data/pak_1/gui/settings/video/video.html`
- `c-engine/renderer/gui/rmlui/guis/debugGui/DebugGui.c`
- `c-engine/data/pak_0_engine/gui/debug/debug.html`

### Vulkan frame resources / passes

- `c-engine/renderer/vulkan2/resources/VulkanFrameResources.h`
- `c-engine/renderer/vulkan2/resources/VulkanFrameResources.c`
- `c-engine/renderer/vulkan2/Vulkan.c`
- `c-engine/renderer/vulkan2/pass/composite/VulkanCompositePass.c`
- `c-engine/renderer/vulkan2/pass/final/VulkanFinalPass.c`
- `c-engine/renderer/vulkan2/pass/depth/VulkanDepthPass.c`
- `c-engine/renderer/vulkan2/pass/meshlet/VulkanMeshletRenderPass.c`
- `c-engine/renderer/vulkan2/pass/triangle/VulkanTriangleRenderPass.c`
- `c-engine/renderer/vulkan2/pass/grid/VulkanGridPass.c`
- `c-engine/renderer/vulkan2/pass/reflection/VulkanReflectionPass.c`

### Shaders

- `c-engine/data/pak_0_engine/shaders/pass/final/final.frag`
- `c-engine/data/pak_0_engine/shaders/pass/composite/composite.comp`
- mesh/triangle/depth/reflection shaders as needed for MSAA or SMAA edge inputs

---

## Phase 0 — CAS polish and AA settings plumbing cleanup

### Goal

Finish the easy part first and make the settings fully coherent.

### Tasks

- Clamp CAS consistently to `0..1` in renderer/final pass.
- Make `VulkanFinalPass.c` default CAS strength match settings defaults.
  - Current mismatch is likely:
    - settings default = `100%`
    - final pass default = `1.5f`
- Consider exposing CAS in debug/stats readouts if useful.
- Decide whether video settings should note which AA options are placeholders.

### Recommended changes

- In `c-engine/renderer/vulkan2/pass/final/VulkanFinalPass.c`
  - default CAS strength should be `1.0f` or lower if preferred visually
- In `c-engine/renderer/Renderer.c`
  - keep renderer as the single source of truth for AA settings
- Optional UI note:
  - append `(planned)` or gray out controls until backend support lands

### Done when

- CAS default matches persisted settings
- no confusing mismatch between UI and actual sharpen intensity

---

## Phase 1 — Real MSAA backend support

## Goal

Implement raster MSAA for world rendering with `Off / 2x / 4x`.

### Expected benefit

- real geometric edge AA
- stable and non-temporal
- no ghosting
- solid base for alpha-tested geometry improvements later

### High-level approach

When MSAA is enabled:

- render scene/depth/G-buffer attachments as multisampled images
- resolve or composite them into single-sample `ResolvedColor`
- keep post stack reading single-sample textures unless a pass truly needs multisample access

### Important design decision

Prefer **multisampled scene-color + depth path** with explicit resolve to a single-sample post chain.

That means:

- world/depth passes write multisampled targets
- post-processing remains mostly unchanged
- `ResolvedColor` stays single-sample

### Resource changes

Update `c-engine/renderer/vulkan2/resources/VulkanFrameResources.c/.h` to support:

- single-sample images always:
  - `ResolvedColor`
  - AO/reflection/post resources
- optional multisample images when MSAA > Off:
  - `SceneColorMsaa`
  - `NormalsMsaa` if needed
  - `MaterialMsaa` if needed
  - depth MSAA image if depth prepass and later passes share it

Possible getter additions:

- `vulkanFrameResourcesGetSceneColorTarget()`
- `vulkanFrameResourcesGetNormalsTarget()`
- `vulkanFrameResourcesGetMaterialTarget()`
- `vulkanFrameResourcesGetMsaaSampleCount()`
- optional specific getters for multisample resources

### Pipeline/pass changes

#### 1) Depth pass

Files:

- `c-engine/renderer/vulkan2/pass/depth/VulkanDepthPass.c`
- related pipeline setup

Tasks:

- create multisampled depth image when MSAA is enabled
- ensure later passes consuming depth know whether they need:
  - resolved single-sample depth
  - original MSAA depth

Risk:

- Hi-Z and AO currently likely expect single-sample sampled depth.

Recommended solution:

- keep a single-sample sampled depth path for post passes
- if Vulkan depth resolve is supported/clean in current abstraction, resolve depth after prepass
- otherwise continue using a single-sample depth prepass until the depth side is redesigned

#### 2) Meshlet / triangle / grid render passes

Files:

- `c-engine/renderer/vulkan2/pass/meshlet/VulkanMeshletRenderPass.c`
- `c-engine/renderer/vulkan2/pass/triangle/VulkanTriangleRenderPass.c`
- `c-engine/renderer/vulkan2/pass/grid/VulkanGridPass.c`

Tasks:

- use the chosen sample count from renderer settings
- bind multisampled color attachments when MSAA is enabled
- render into multisampled scene targets

#### 3) Composite pass

File:

- `c-engine/renderer/vulkan2/pass/composite/VulkanCompositePass.c`

Tasks:

- read from single-sample scene inputs
- if upstream render is multisampled, add a resolve step before composite or make composite consume resolved scene textures

Recommended implementation order:

- simplest route: resolve scene color to a single-sample image before composite
- only keep G-buffer multisampled if there is a real benefit and the passes need it

### Vulkan plumbing needed

Likely updates in:

- `VulkanImage` creation helpers
- `VulkanPipe` renderpass/pipeline sample-count setup
- possibly attachment descriptions and resolve attachments in graphics passes

Need support for:

- `VkSampleCountFlagBits` selection from renderer settings
- color/depth attachments with sample count > 1
- optional resolve attachment for color
- sample shading enable/min sample shading later

### Recommended sub-steps for MSAA

#### Commit A — sample-count plumbing only

- add helper mapping:
  - Off -> `VK_SAMPLE_COUNT_1_BIT`
  - 2x -> `VK_SAMPLE_COUNT_2_BIT`
  - 4x -> `VK_SAMPLE_COUNT_4_BIT`
- store active sample count in frame resources or renderer vulkan state
- no render changes yet

#### Commit B — scene color MSAA path

- add `SceneColorMsaa`
- render mesh/grid/triangle into multisampled color
- resolve to single-sample `SceneColor` or directly into `ResolvedColor`

#### Commit C — depth compatibility

- make depth path compatible with MSAA mode
- verify depth prepass, Hi-Z, AO, and reflections still behave

#### Commit D — optional G-buffer MSAA

- only if normals/material need matching sample count for quality
- otherwise keep them single-sample to reduce cost/complexity

### Open questions for MSAA

- Do alpha-tested foliage/materials need alpha-to-coverage later?
- Does the engine/device capability set guarantee 4x MSAA for all required formats?
- Should reflections use their own sample count or stay single-sample initially?

### Done when

- `MSAA Off/2x/4x` changes actual Vulkan raster sample count
- world edges visually improve
- bloom/final continue to read valid single-sample `ResolvedColor`
- build + screenshot verification pass

---

## Phase 2 — Real sample rate shading support

## Goal

Make `Sample Rate Shading: Off / Half / Full` affect Vulkan pipelines when MSAA is on.

### Expected benefit

- better quality for specular highlights / subpixel material detail under MSAA
- especially useful for foliage, thin geometry, and noisy speculars

### Important note

Sample shading does little or nothing when MSAA is `Off`.
So the UI behavior should be:

- still configurable always, or
- visibly marked as only relevant when MSAA > Off

### Implementation

Files likely involved:

- `c-engine/renderer/vulkan2/pipeline/VulkanPipe.h`
- `c-engine/renderer/vulkan2/pipeline/VulkanPipe.c`
- graphics passes creating pipelines

Tasks:

- add per-pipeline controls:
  - `sampleShadingEnable`
  - `minSampleShading`
  - sample count
- map settings:
  - Off -> disabled / `0.0`
  - Half -> enabled / `0.5`
  - Full -> enabled / `1.0`
- enable this only on pipelines that render multisampled targets

### Recommended scope

Start with:

- meshlet pass
- triangle pass
- maybe grid pass

Do not bother with:

- fullscreen passes
- compute passes
- single-sample post passes

### Done when

- changing sample shading updates pipeline behavior in MSAA modes
- no validation errors
- no measurable breakage when toggling at runtime

---

## Phase 3 — SMAA 1x implementation

## Goal

Implement non-temporal SMAA as a post-process AA option.

### Expected benefit

- good edge cleanup for non-MSAA paths
- much cheaper and simpler than temporal AA
- no ghosting

### Recommended architecture

Use a standard 3-pass SMAA pipeline:

1. edge detection
2. blend weight calculation
3. neighborhood blending

Output:

- final single-sample anti-aliased `ResolvedColor`

### Suggested resources

Add to `VulkanFrameResources` as needed:

- `SmaaEdges`
- `SmaaBlendWeights`
- maybe support textures/lookup textures:
  - SMAA area texture
  - SMAA search texture

### Suggested files

New pass module(s):

- `c-engine/renderer/vulkan2/pass/smaa/VulkanSmaaPass.h`
- `c-engine/renderer/vulkan2/pass/smaa/VulkanSmaaPass.c`

Shaders:

- `c-engine/data/pak_0_engine/shaders/pass/smaa/...`

Potential pass structure:

- one module with 3 internal pipelines
- or 3 explicit pass modules if that fits current architecture better

### Pipeline integration

Recommended order:

- world render
- composite -> produces base single-sample color
- SMAA 1x -> writes `ResolvedColor`
- bloom -> reads `ResolvedColor`
- final -> reads `ResolvedColor`

If SMAA is off:

- composite writes `ResolvedColor` directly, as today

If SMAA 1x is on:

- composite writes `SceneColor` or `CompositeColor`
- SMAA consumes it and writes `ResolvedColor`

### Inputs for edge detection

Use one of:

- luma-based edge detection from color
- color + depth/normals if desired for quality

Recommended MVP:

- luma color edge detection first
- add depth-based assistance only if needed

### Runtime interaction with MSAA

Decide policy early.
Recommended policy:

- keep `MSAA` and `SMAA` fully independent
- allow all combinations, including:
  - `MSAA + SMAA 1x`
  - `MSAA + SMAA T2x`
- keep sample shading independent as a user setting too
  - it only has a runtime effect when `MSAA > Off`
  - but the selected value should be preserved even when `MSAA Off`

Implementation rule:

- do not force `MSAA = Off` when `SMAA != Off`
- do not force `SMAA = Off` when `MSAA != Off`
- do not reset sample shading just because `MSAA` is currently off
- instead, show sample shading as inactive/ignored in UI/debug until `MSAA` is enabled again

### Done when

- SMAA 1x visibly reduces jaggies
- no temporal history is involved
- pipeline still produces valid `ResolvedColor`

---

## Phase 4 — SMAA T2x implementation

## Goal

Add optional temporal SMAA T2x without bringing back old global TAA behavior.

### Important requirement

This should be treated as **a separate feature**, not “TAA returns”.

### Expected behavior

SMAA T2x generally needs:

- 2-frame subpixel jitter pattern
- previous frame color/history
- neighborhood-aware temporal resolve
- history rejection tuned only for the T2x path

### Key design rule

Keep the temporal data local to SMAA T2x.

That means:

- do not reintroduce generic always-on camera jitter
- only apply jitter when `SMAA == T2x`
- only allocate/use history resources when `SMAA == T2x`

### Required data to restore selectively

Likely needed again:

- 2-frame jitter sequence in camera system
- previous color history buffer
- maybe previous depth / velocity depending on implementation quality target

But this should be guarded by SMAA T2x mode only.

### Suggested implementation path

#### Step 1

Get SMAA 1x fully stable first.

#### Step 2

Add optional camera jitter provider:

- local helper in camera system
- enabled only for `RENDERER_SMAA_T2X`
- use a simple 2-sample pattern, not the old wider TAA sequence

#### Step 3

Add T2x history buffers back in frame resources, but only for this path:

- `SmaaHistoryColor[2]`
- validity flag
- end-of-frame swap

#### Step 4

Add T2x resolve shader/pass after SMAA weights/neighborhood stage.

### Risks

- this is where ghosting can return if history rejection is poor
- velocity/depth handling must be correct
- transparent objects may need special treatment

### Recommendation

Do not start SMAA T2x until:

- MSAA is done
- sample shading is done
- SMAA 1x is done and liked visually

### Done when

- T2x is optional and isolated
- disabling T2x fully removes jitter/history path again

---

## Phase 5 — Runtime sanitization and feature policy

## Goal

Define legal AA combinations and enforce them in one place.

### Recommended place

- `c-engine/renderer/Renderer.c`
- `sanitizeAASettings(...)`

### Suggested first policy

Independence rules:

- `MSAA` and `SMAA` are separate settings and must not disable each other
- all combinations are legal, including `SMAA T2x + MSAA`
- sample shading is also a separate setting
- sample shading only has a runtime effect when `msaa != OFF`
- when `msaa == OFF`, preserve the selected sample shading value and treat it as inactive/ignored
- CAS always allowed

### Why

This matches the current settings model better, avoids surprising user-facing coercion, and still lets UI/debug communicate which settings are currently effective.

### Done when

- UI settings always resolve to a supported backend state
- debug and video UI show the same effective state

---

## Phase 6 — UI/UX follow-up

## Goal

Make the settings understandable once backends become real.

### Tasks

- indicate which options are active vs ignored
- if policy disables combinations, reflect that in labels/UI
- optionally show “restart required” only if pipeline recreation cannot be made live

### Recommended behavior

If Vulkan pipelines/resources are recreated live on setting changes:

- no restart prompt needed

If not:

- show pending/apply/restart messaging

### Nice-to-have

Add a short tooltip or subtitle in the video settings page:

- MSAA: geometric AA
- SMAA: post-process AA
- Sample Shading: improves MSAA shading quality
- CAS: sharpening only

---

## Technical notes by feature

## CAS

Already implemented.

Need only:

- default cleanup
- maybe expose more tasteful default than 100% if image looks too crispy

## MSAA

Main technical needs:

- multisample attachments
- graphics pipeline sample count
- optional resolves
- depth compatibility

## Sample shading

Main technical needs:

- pipeline multisample state wiring
- only meaningful with MSAA

## SMAA 1x

Main technical needs:

- 3 post passes
- area/search textures
- edge/blend intermediate images

## SMAA T2x

Main technical needs:

- selective jitter reintroduction
- history resources
- resolve logic
- strict mode scoping

---

## Validation plan

For each phase:

1. `./scripts/build.sh`
2. `./scripts/run.sh screenshot`
3. compare edge quality / shimmer / blur / performance

### Specific checks

#### CAS

- sharpen slider visibly changes output
- no oversharpen halos on UI/world edges

#### MSAA

- thin geometry edges improve
- no broken depth interactions
- no flickering in reflections/composite/final

#### Sample shading

- specular aliasing improves under MSAA
- no pipeline creation errors

#### SMAA 1x

- edge cleanup visible on geometry silhouettes
- no blurrier-than-expected image
- no broken bloom/final path

#### SMAA T2x

- no ghosting trails on moving characters/camera pans
- jitter disappears when T2x is off
- history invalidates correctly on resize / camera mode changes / swapchain recreation

---

## Suggested implementation order

### Milestone 1

- CAS cleanup
- finalize sanitize policy

### Milestone 2

- MSAA backend support
- live sample-count switching if feasible

### Milestone 3

- sample shading backend support

### Milestone 4

- SMAA 1x

### Milestone 5

- SMAA T2x only if still wanted after evaluating MSAA + SMAA 1x

---

## Suggested first concrete tasks

1. Fix CAS default consistency
2. Add AA sanitize policy in `Renderer.c`
3. Add Vulkan sample-count plumbing helpers
4. Audit which render targets really need multisampled variants
5. Implement `MSAA 2x/4x` on the main world color path
6. Add screenshot-based visual verification for edge quality

---

## Definition of success

The AA system is successful when:

- TAA remains gone
- no always-on jitter exists outside SMAA T2x
- users can choose between stable non-temporal options
- `ResolvedColor` remains the stable handoff for post/final
- each AA feature is modular and can be enabled/disabled without pipeline confusion
