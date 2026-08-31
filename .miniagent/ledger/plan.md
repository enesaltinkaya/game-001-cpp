# Plan

Task: produce a detailed SSGI / SSGI++ implementation plan document and save it in `plans/`, grounded in the existing survey at `docs/global-illumination.md`.

Strategy: the deliverable is a planning document, not code, so the work is research-then-write.
First read the full survey (`docs/global-illumination.md`) and pull out every verified engine
fact it already establishes (pass order, G-buffer slots/formats, Forward+ config, the AO
temporal-pass pattern, FSR 3.1 history interaction, GTX 1080 Ti budget) since the plan must
build on those instead of re-deriving them. Then work through the SSGI++ algorithm's stages
(hemisphere ray tracing, visibility-weighted irradiance, temporal filter with edge/depth
continuity, spatial blur, ambient-term injection in `scene.frag`) and map each stage to a
concrete engine change: new pass name + insertion point after `oit_composite`/before `ao`,
new RTs and their formats at FSR-scaled internal resolution, ping-pong history, and the
one-frame-latency cadence the survey already proposes. Write the document to `plans/ssgi.md`
following the structure of existing plans (status header, scope, phased breakdown, risks),
with phases ordered so each phase is independently shippable and verifiable (e.g. P1
single-bounce hemisphere estimate visible in a debug pass, P2 temporal, P3 spatial +
injection into ambient, P4 integration with vegetation props and FSR reactive-mask
validation). Finally cross-check every file path, pass name, and G-buffer slot the plan
cites against the actual tree so the plan contains no stale facts.

Approach: worker reads `docs/global-illumination.md` end to end plus the files it cites
(pass list in `c-engine/renderer/vulkan/Vulkan.cpp`, G-buffer in
`resources/VulkanFrameResources.cpp`, `scene.frag` ambient block, AO pass for the temporal
pattern), writes `plans/ssgi.md`, then a verifier pass validates citations and document
quality against the conventions of neighboring plan files.

Verification: test -s plans/ssgi.md && wc -l plans/ssgi.md
