# plan

Strategy: profile the current frame to find where the ~9.5 ms of GPU time goes, then target the highest-cost passes with optimizations that keep visuals identical. Approach: (1) take a headless RenderDoc capture per `docs/renderdoc-capture.md` and use the Python replay API to extract per-pass / per-drawcall GPU timings and rank the hot spots; (2) read the relevant pass source in `c-engine/renderer/vulkan/pass/` and the pass docs; (3) apply targeted, low-risk wins first (resolution/attachment scale factors, redundant passes or loads/stores, overdraw-heavy fullscreen passes, shader ALU simplifications, redundant texture loads / mip bias, culling) and re-capture after each significant change to confirm the saving and no visual regression via screenshots; (4) only ship changes that measurably reduce GPU time and pass `./scripts/build.sh` cleanly. Visual-safety rule: changes must not alter output pixels beyond floating point noise (screenshots before/after compared).

Verification: ./scripts/build.sh
Baseline commit: 3fe3d313678528fdee73e209ce193ee24f571775 (clean)
