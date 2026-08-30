# Plan

## Strategy

The task is a research question: which global illumination methods suit our engine. The right
deliverable is a grounded survey document (`docs/global-illumination.md`) that maps each candidate
GI technique to what this engine actually has, then gives a ranked recommendation with integration
notes. Grounding matters: the engine is a Vulkan renderer with existing passes for AO, SSR, TAA, HZB,
shadows, contact shadow, LPM, RMLUI, light culling, OIT, FSR upscaling, streamed heightmap terrain,
and per-tile vegetation — a GI method that clashes with TAA/OIT/FSR or cannot handle infinite
streaming terrain is not suitable, so the survey must evaluate against those concrete constraints
rather than generic literature. The approach: first inventory the renderer's buffers, lighting
architecture, and frame cadence (what we can reuse or must perturb), then shortlist candidate methods
(local GI: SSGI/SSGI++, HBAO-based, light propagation volumes; volumetric/probe: Lumen-style
SDF GI, VXGI, dynamic GI probes, radiance caches), score each on cost, memory, artifacts, and
integration effort, and write the recommendation. No code is implemented in this workstream; only
the ledger and the survey document change. Every verifier round runs the pinned command below, which
confirms the deliverable exists and has substance.

Verification: test -s docs/global-illumination.md && wc -l docs/global-illumination.md
