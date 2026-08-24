# VB-AO Review — Bugs Found

_Last reviewed: March 9, 2026_

---

## Bug 1 — Critical: `radius` is used as two completely different units simultaneously

**Files:** `ao.comp`, `VulkanAOPass.c`

In the main loop `radius` serves as a **pixel-count radius** for UV sampling:

```glsl
// radius = 1.2  →  1.2 * 0.9125 / 1920 ≈ 0.00057 UV ≈ 1.1 full-res pixels
vec2 sampleUv = uv + dir * (pc.radius * stepT) / fullSize;
```

But *the same value* is then used as a **world-space distance threshold** for `rangeWeight`:

```glsl
float rangeWeight = 1.0 - smoothstep(pc.radius * 0.25, pc.radius, distance);
//                                    ← view-space metres  ↑
```

With `radius = 1.2f` the UV search reaches at most **~1.1 full-resolution pixels**. GTAO/VB-AO
needs at least 24–64 pixels of search radius to detect any meaningful occluders. In practice
`weightSum` stays at `0.0` for all but the closest geometry, hitting the early-out and returning
`ao = 1.0` (no occlusion) everywhere.

### Fix

Split the single `radius` into two fields in `VulkanAOPass.c`:

```c
typedef struct AoPushConstants {
    u32   depthTextureIndex;
    u32   outputImageIndex;
    u32   fullWidth;
    u32   fullHeight;
    u32   halfWidth;
    u32   halfHeight;
    float screenRadius;   // half-res pixel search radius  (new)
    float radius;         // view-space world-unit falloff  (was radius)
    float bias;
    float intensity;
    float power;
} AoPushConstants;
```

```c
AoPushConstants pc = {
    ...
    .screenRadius = 24.0f,  // half-res pixels (~48 full-res pixels)
    .radius       = 1.5f,   // world-space metres for range falloff
    ...
};
```

Update the push-constant block in `ao.comp` and change the sampling step to use `halfSize`:

```glsl
layout(push_constant) uniform PushConstants {
    uint  depthTextureIndex;
    uint  outputImageIndex;
    uint  fullWidth;
    uint  fullHeight;
    uint  halfWidth;
    uint  halfHeight;
    float screenRadius;   // half-res pixel radius  (new)
    float radius;         // world-space falloff radius
    float bias;
    float intensity;
    float power;
} pc;
```

```glsl
// uv lives in half-res space, so divide by halfSize
vec2 sampleUv = uv + dir * (pc.screenRadius * stepT) / halfSize;
```

The `rangeWeight` line stays unchanged — it now correctly uses `pc.radius` in world units.

---

## Bug 2 — Major: view-space normal is inverted — wrong flip condition

**File:** `ao.comp` — `estimateViewNormal()`

`reconstructViewPos` uses `1.0 - uv.y * 2.0` — the OpenGL / Y-up NDC convention used by this
renderer (cglm `glm_perspective` + Y-up view). In that view space **Y points up and +Z points
toward the camera**. A camera-facing flat surface should therefore have a normal of `+Z`.

The naming of `upPos` / `downPos` is backwards:

```glsl
// "upPos" adds +invFullSize.y → larger UV.y → screen-downward → lower view-space Y
vec3 upPos = reconstructViewPos(uv + vec2(0.0, invFullSize.y),
                                fetchDepth(baseCoord + ivec2(0, 1)));   // row+1 = down

// "downPos" subtracts invFullSize.y → screen-upward → higher view-space Y
vec3 downPos = reconstructViewPos(uv - vec2(0.0, invFullSize.y),
                                  fetchDepth(baseCoord + ivec2(0, -1)));
```

`upPos` is actually *below* center in view space, so `dy = upPos – center` points in **−Y**.
Combined with `dx` in `+X`:

```
cross(+X, −Y) = −Z   ← points into the scene, away from the camera
```

The flip condition `if (normal.z > 0.0)` never fires (z = −1 < 0), so the normal stays pointing
**away** from the camera. The horizon test then rejects all valid occluders and accepts geometry
behind the surface, producing zero or inverted AO at every corner and crevice.

### Fix — two parts

**Part A:** Swap `upPos` / `downPos` so they match their names in both screen-space and view-space
(`ao.comp`):

```glsl
// upPos: screen-upward (smaller row) → higher view-space Y
vec3 upPos = reconstructViewPos(uv - vec2(0.0, invFullSize.y),
                                fetchDepth(baseCoord + ivec2(0, -1)));

// downPos: screen-downward (larger row) → lower view-space Y
vec3 downPos = reconstructViewPos(uv + vec2(0.0, invFullSize.y),
                                  fetchDepth(baseCoord + ivec2(0, 1)));
```

With this fix `dy = upPos – center` points in `+Y`, and `cross(+X, +Y) = +Z` — correct.

**Part B:** Update the flip guard to match the new convention (`ao.comp`):

```glsl
// was: if (normal.z > 0.0)
if (normal.z < 0.0) {
    normal *= -1.0;
}
```

---

## Bug 3 — Minor: centre-tap depth reference in the bilateral upsample is offset by one pixel

**Files:** `meshlet.frag`, `triangle.frag` — `sampleBilateralAO()`

The AO compute shader samples the full-res depth at the **odd** pixel within each 2×2 block:

```
halfRes pixel (cx, cy)  →  fullRes depth at (2·cx + 1,  2·cy + 1)
```

The bilateral upsample correctly derives the neighbour depth references with `aoCoord * 2 + ivec2(1)`.
However, for the **centre tap** (`x == 0 && y == 0`) — which carries 4× the spatial weight — the
same formula maps to `baseAo * 2 + 1`, which is one pixel away from the current fragment for
even-column / even-row pixels. This causes a slight depth-weight under-estimate at the centre tap
and a small amount of AO bleed across depth edges.

### Fix

Use the exact fragment coordinate for the centre tap (`meshlet.frag` and `triangle.frag`):

```glsl
ivec2 depthCoord;
if (x == 0 && y == 0) {
    depthCoord = ivec2(gl_FragCoord.xy);          // exact match for this fragment
} else {
    depthCoord = clamp(aoCoord * 2 + ivec2(1),
                       ivec2(0),
                       ivec2(fullSize) - 1);
}
```

---

## Summary

| # | Severity | File(s) | Root Cause | Visible Effect |
|---|---|---|---|---|
| 1 | **Critical** | `ao.comp`, `VulkanAOPass.c` | `radius = 1.2` used as both pixel radius (~1.1 px search) and world-unit range threshold | `weightSum` stays 0 everywhere → `ao = 1.0` → no occlusion visible at all |
| 2 | **Major** | `ao.comp` — `estimateViewNormal` | `upPos`/`downPos` UV offsets are swapped; flip condition `> 0` should be `< 0` | All view-space normals point into the scene; hemisphere test inverted → zero / wrong AO at corners |
| 3 | **Minor** | `meshlet.frag`, `triangle.frag` | Centre-tap depth fetched at `baseAo*2+1` instead of exact fragment coord | Slight depth-weight inaccuracy; small AO bleed at depth discontinuities |

Bugs 1 and 2 together fully explain the "not working properly" report: the shader produces
`ao = 1.0` everywhere (Bug 1) and would apply inverted horizon tests at any geometry that
survived (Bug 2).
