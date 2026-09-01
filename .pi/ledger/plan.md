# Strategy

Implement `plans/ssgi.md` phase by phase, mirroring the proven
`VulkanAOPass` structure: a new `VulkanGiPass` (multi-pipe System, registered
in `vulkanInit()` between `vulkanSsrPass` and `vulkanAOPass`) with a
half-res `gi_estimate.comp` ray march (D3) and a full-res ping-pong
`gi_temporal.comp` history filter (D4), then inject the filtered result into
the ambient term of `scene.frag` / `azgaar_props.frag` with the
`0xFFFFFFFFu` NULL-sentinel + one-frame-latency contract (D5/D6), plus
`vulkanAOPassSetStrength()` AO attenuation to avoid double darkening.
Phases land in order (P1 estimate+debug visibility, P2 temporal, P3 injection,
P4 validation+docs), each independently shippable; consult the plan's exact
line references and conventions (ssr.comp header block, ao_temporal push
constants, AO getter sentinel contract) before writing code. Do not move the
parked player/camera; all screenshot A/Bs use the parked vantage with
settle-time (`ENGINE_SCREENSHOT_DELAY_MS`). Budget: gi_estimate ≤0.2 ms,
gi_temporal ≤0.3 ms, total ≤0.5 ms hard / 1.5 ms fail (measured with
`ENGINE_LOG_PASS_GPU=1`); GI ships off-by-default in settings until P4
validation passes.

Key references (read before coding):
- `plans/ssgi.md` — the spec (D1–D7 decisions, phases, verification steps)
- `docs/global-illumination.md` — method survey
- `c-engine/renderer/vulkan/pass/ao/VulkanAOPass.{h,cpp}` — structural template
- `c-engine/renderer/vulkan/pass/ssr/ssr.comp` — ray-march conventions
- `c-engine/data/pak_0_engine/shaders/pass/ao/ao_temporal.comp` — temporal template
- `docs/screenshot.md` — multi-shot screenshot testing

Verification: ./scripts/build.sh
Baseline commit: 727c863d746cc80a76cce4e67bbbecc4f8fcabcc (clean)
