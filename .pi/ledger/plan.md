# Plan

The user expects to see color bleeding between a red house wall and green ground
(i.e. a visual artifact where one surface's albedo bleeds into the other's), but
is not seeing it — so this is an *analysis* task: figure out the rendering path
that would cause (or fail to cause) that cross-surface color bleed, and explain
what is actually happening.

Strategy:
1. Identify the render passes and shaders involved in drawing the house (walls,
   red material) and the ground (green) — likely the forward/deferred lit pass,
   the G-buffer, and any AO/SSAO or ambient-occlusion / soft-shadow /
   screen-space effect that samples neighboring pixels.
2. Determine whether there is any mechanism that would bleed albedo across
   surfaces (e.g. a screen-space filter, a shared texture, a missing clear, a
   depth test issue, or an implicit lattice/vertex-shader sampling of a shared
   height/normal texture).
3. If a real artifact exists or is expected, capture a screenshot around the
   parked player (the object under test) to confirm what the frame actually shows.
4. Produce a written analysis of the root cause and whether the expected bleed
   is a bug or the correct behavior.

Approach: this is primarily a code-reading + screenshot-verification task.
Workers should read the relevant shader/pass source, trace the data flow, and use
`./scripts/run.sh play screenshot` (with `ENGINE_HIDE_GUI=1` for a clean frame)
to confirm the actual on-screen result. No code changes are expected unless a
concrete bug is found.

## Verification entry points

- Build: `./scripts/build.sh`
- Screenshot: `ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/screenshot.jpg`
- Log timeout: `./scripts/run.sh play log 5000`
