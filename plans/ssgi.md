# SSGI — Screen-Space Global Illumination

Single-bounce screen-space global illumination: for each pixel, raymarch a
jittered hemisphere of rays through the depth buffer, gather the lit scene
color at hit points, and integrate a Lambertian diffuse bounce term that
replaces the (currently zeroed) ambient term in the scene shader.

Raymarching method: **h3r3tic's "Depth buffer raymarching"** (gist
"heretic-raymarch-hlsl", method 1) with **normal-threshold edge detection**
(method 3) on hit. This is distinct from the contact shadow, which uses
Bend Studio's wave-offset dispatch-list raymarch. The existing SSR pass is
the in-repo template for the projection / jitter bookkeeping.

## Frame position & inputs

The pass runs **after the depth pre-pass / contact shadow and before the
scene pass** (added right after `vulkanContactShadowPass` in
`Vulkan.cpp`), so the scene shader can consume the result **in the same
frame** — exactly like the contact shadow.

| Input | Freshness | Notes |
|---|---|---|
| Depth | current frame | written by depth pre-pass, jittered, reverse-Z |
| World normal (RGBA16F, `GetWorldNormal`) | current frame | written by the depth pre-pass, cleared to 0 for sky; used for origin direction, hit edge detection, and both filter kernels |
| Velocity | current frame | written by the depth pre-pass |
| Scene color | **previous frame** | written by the scene pass last frame; SSGI is a temporally-accumulated soft quantity, so the one-frame delay is invisible (delayed-color pattern) |
| Material (roughness/metallic G-buffer) | previous frame | same as scene color |
| Camera UBO | current frame | `invViewProjectionNoJitter`, jitter / prevJitter |

Because scene color and material are one frame old, gather lookups at hit
points use `uv - jitter - prevJitter` (the previous frame's jittered grid),
while depth / world-normal lookups use the jittered UV as-is (current-frame
grid). Origin reconstruction is jitter-stable per the established SSR
pattern (unjittered UV + `invViewProjectionNoJitter`).

## Pass pipeline (`c-engine/renderer/vulkan/pass/ssgi/`)

Mirrors the contact shadow's three-stage structure
(`VulkanContactShadowPass`), all at full render resolution, 8x8 groups:

1. **`ssgi.comp`** — raw raymarch.
   - Skip sky pixels and pure-metallic pixels (material G-buffer `.g`).
   - 8 rays / pixel over the hemisphere, per-frame rotated by
     `interleavedGradientNoise(coord, frameIndex)` + camera TAA jitter.
   - March in **view space** (constant `maxDist / 64` steps; view Z is
     linear with the reverse-Z depth buffer, so the hit test is a plain
     linear-distance comparison — h3r3tic method 1).
   - Hit test: buffer surface closer than the ray sample, with a distance-
     scaled thickness tolerance (`max(0.05, t * 0.01)` m) so grazing
     intersections still register.
   - Edge detection on hit: `dot(N_hit, -D) < 0.1` → reject (h3r3tic
     method 3), which also kills grazing self-intersections.
   - Gather: scene color at hit × `(1 - metallic_hit)` (kills the specular
     part; the Lambertian assumption covers the view-direction mismatch).
   - Integrate: `Li = (2 / RAYS) Σ color · max(dot(N_hit,−D)) · max(dot(N,D))`
     (uniform-hemisphere sampling, `2π/π` factor). Distance fade
     `1 - smoothstep(0.5, 1.0, t/maxDist)` on the contribution.
   - Output RGBA16F: `rgb` = bounce irradiance, `a` = confidence
     (origin depth-edge fade × offscreen-ray fraction).
2. **`ssgi_spatial.comp`** — depth/normal-weighted bilateral 5×5 blur of the
   raw output (port of `contact_shadow_spatial.comp`, generalized to vec3).
3. **`ssgi_temporal.comp`** — velocity reprojection + conservative blend
   (≥10% current) + variance clipping (port of
   `contact_shadow_temporal.comp`, per-channel over rgb; alpha rides the
   same blend factor). Ping-pong `R16_SFLOAT` history, same
   `historyValid` / first-frame guard as the contact shadow.

Public API (contact-shadow style): `vulkanSsgiPassSetDisabled/IsDisabled`,
`vulkanSsgiPassSetDistance/GetDistance` (default 10 m), env var
`ENGINE_SSGI_DISABLED`, settings key `ssgiDisabled`, graphics-settings GUI
toggle (same wiring as the SSR / AO / contact-shadow entries).

## Integration

- `ShadowUbo` (LightComponent.h) / `ShadowData` (globalset.shader):
  `shadow.pad2` → `ssgiImageIndex` (same offset; 0 = off).
- `vulkanResourceSetSsgiImageIndex(u32)` mirrors the contact-shadow setter.
- `sampleSSGI()` in `includes/shadow.shader` mirrors `sampleContactShadow()`.
- `scene.frag`: the ambient term is the application point (albedo and kD
  are only known here — this is why SSGI cannot be applied in the composite
  pass like SSR/AO):

  ```glsl
  vec4  ssgi    = sampleSSGI();
  vec3  ambient = mix(dirLight.ambient.rgb, ssgi.rgb, ssgi.a);
  vec3  color   = Lo + ambient * kD * baseColor.rgb;   // existing line
  ```

## Tuning constants (initial)

- 8 rays, 64 steps, maxDist 10 m, origin bias 0.01 m along the normal,
  normal threshold 0.1, thickness `max(0.05, t*0.01)`.
- Temporal: base blend 0.10, hard cap 0.2 (slightly looser than contact
  shadow's 0.15 since GI color shifts are larger).

## Debugging

- `ENGINE_SSGI_DISABLED=1` turns the pass off (output index 0 → the scene
  shader falls back to the UBO ambient, i.e. black).
- `ENGINE_DEBUG_DUMP_IMAGES=ssgi,ssgiRaw` dumps the temporally-filtered
  and raw raymarch buffers alongside a screenshot (each channel is
  min-max normalised; the 4th channel is confidence, dropped by the
  JPG writer).  All SSGI images carry `TRANSFER_SRC` usage for the
  readback.

## Not in scope

- Multiple bounces, specular GI, volumetric / soft-light SSGI variants
  (h3r3tic's method 2 uses dFdx-based edges; not used — we have a normal
  buffer).
- FSR reactive-mask hookup (the temporal filter's own accumulation is the
  stabilizer; verify in screenshots).
