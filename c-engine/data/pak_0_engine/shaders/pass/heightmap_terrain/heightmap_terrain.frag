#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// Fragment shader for the heightmap terrain pass.
//
// Shading model is a port of the experimental azgaar_terrain pass: the
// engine's DEFAULT GRASS texture (sceneBuffer.terrain.grassAlbedoIndex /
// grassNormalIndex), tiled in world space, slope-based triplanar cliff
// texturing, and the full PBR pipeline
// (directional sun Cook-Torrance, screen AO,
// forward+ lights).
//
// On top of the vertex normal (derived from the height texture's ±1-texel
// neighbours) the shading normal is perturbed with a world-anchored
// micro-band value noise (4-16 m wavelengths — the band no grid resolves).
// Only the shading normal is affected: render/physics/CPU geometry all stay
// on the shared bilinear height surface, so nothing clips or pops.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inWorldPos;

layout(location = 0) out vec4 outColor;     // scene color (HDR, R16G16B16A16)
layout(location = 1) out vec2 outNormal;    // oct-encoded world normal (R16G16)
layout(location = 2) out vec4 outMaterial;  // r=roughness g=metallic b=ao a=ground flag for decal pass (R8G8B8A8)
layout(location = 3) out vec4 outAlbedo;    // base albedo (R16G16B16A16)

// Push constants: identical layout to the vertex shader's HeightmapPC block
// (Vulkan requires compatible push constant declarations across stages; a
// separate smaller block here would read wireFrame/debugHeightRamp from
// offsets 0/4 — i.e. the tile origin — and force the red wireframe color).
layout(push_constant, std430) uniform HeightmapPC {
    vec4 tile;   // x = originX, y = originZ, z = sizeMeters, w = gridSegments
    vec4 flags;  // x = heightScale, y = texDim (TEX), z = wireFrame,
                 // w = debugHeightRamp
} pc;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/shadow.shader"
#include "../../includes/forwardplus.shader"

// World-space tiling frequency for the grass texture (repeats per metre).
// Matches the regular terrain pass's DEFAULT_GRASS_DETAIL_TILE /
// TERRAIN_DETAIL_REFERENCE_METERS ratio and the experimental azgaar pass, so
// the grass reads at the same density on both backends.
#define AZGAAR_GRASS_TILE (2048.0 / 7000.0)

#define AZGAAR_CLIFF_DETAIL_TILE 32.0
#define CLIFF_TRIPLANAR_SCALE (AZGAAR_CLIFF_DETAIL_TILE / 4096.0)
#define SPLAT_NORMAL_STRENGTH 2.0

// Flat fallback albedo when no grass texture is registered (matches the
// engine's default ground green in HeightmapTerrain).
#define AZGAAR_FALLBACK_GRASS_COLOR vec3(0.36, 0.55, 0.28)
// Fallbacks when the snow / sand default albedo assets are unavailable —
// flat colours in the right hue so the climate bands still read.
#define AZGAAR_FALLBACK_SNOW_COLOR vec3(0.90, 0.92, 0.96)
#define AZGAAR_FALLBACK_SAND_COLOR vec3(0.76, 0.70, 0.55)

// Climate texture channel encodings (must match azgaarWorldPackClimateTexture):
//   R = temperature (deg C) + 64, G = precipitation, B = coast cells + 11,
//   A = biome id.  Biased unsigned encodes keep every channel monotonic so
//   bilinear filtering cannot ring across a sign change.
#define AZGAAR_CLIMATE_TEMP_BIAS 64.0
#define AZGAAR_CLIMATE_COAST_BIAS 11.0
// Biome ids (FMG): 11 = Glacier.
#define AZGAAR_BIOME_GLACIER 11.0

// ── Lighting toggles (set to 0 to disable) ────────────────────────────
// Same rationale as the experimental pass: keep the full PBR spec terms on
// (the plates are mostly viewed at grazing angles but the GGX lobe there is
// acceptable with the grass albedo).
#define HEIGHTMAP_TERRAIN_ENABLE_SPECULAR 1

// ── Micro-band procedural normal perturbation ──────────────────────────
// Value noise in the 4-32 m band (octaves at 32/16/8/4 m). The geometry
// band baked into the height texture is limited to wavelengths >= 64 m
// (Nyquist-safe on the ring-0 lattice); everything shorter perturbs the
// shading normal only, so render/physics/CPU geometry all stay on the
// shared bilinear height surface. The 32 m / 16 m octaves carry the
// amplitudes they had in the old (aliased) geometry band, so the perceived
// surface roughness is unchanged.
// World-anchored and pure: no state, deterministic across frames/tiles.
// The cell hash is the project's fract-based hash() (see utils.shader) so it
// works with the project's shader front-end (no uintBitsFloat).
#define MICRO_NOISE_STRENGTH 0.45

float microValueNoise(vec2 p) {
    vec2 ip = floor(p);
    vec2 fp = p - ip;
    vec2 u  = fp * fp * (3.0 - 2.0 * fp);
    float a = hash(ip);
    float b = hash(ip + vec2(1.0, 0.0));
    float c = hash(ip + vec2(0.0, 1.0));
    float d = hash(ip + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 2.0 - 1.0;
}

// Synthetic "height" of the micro band (metres), used only for its slope.
float microBump(vec2 p) {
    return 0.5 * microValueNoise(p / 32.0 + 11.17) + 0.25 * microValueNoise(p / 16.0 + 5.93) +
           0.5 * microValueNoise(p / 8.0 + 7.31) + 0.25 * microValueNoise(p / 4.0 + 3.7);
}

// Grazing-aware octave fade, driven by the screen-space derivatives of the
// noise input (world XZ) — the same signal hardware mip-mapping uses.
//
// The old fade keyed off vertical pixels-per-metre at the fragment's
// distance, which ignores the view angle. At grazing angles the depth
// direction is heavily compressed toward the horizon, so a noise octave
// that looks resolvable by distance is actually sub-pixel on screen and the
// GGX lobe aliases it into the dark horizontal smudges. dFdx/dFdy of the
// world XZ position capture the TRUE on-screen sampling rate in both screen
// axes (including that compression), so an octave is only kept while its
// wavelength spans a few pixels in BOTH screen directions.
//
// `dpx`/`dpy` are the per-pixel rates of change of the noise input along the
// screen X / Y axes (world metres per pixel). An octave of wavelength
// `lambda` spans `lambda / dpx` (resp. `lambda / dpy`) pixels in that axis;
// it is resolvable only when both span >= ~2 px. Fade out below 2 px, ramp
// back to full amplitude by 8 px (same range as before).
float octaveFade(float lambda, float dpx, float dpy) {
    float pxX = lambda / max(dpx, 1e-5);
    float pxY = lambda / max(dpy, 1e-5);
    return smoothstep(2.0, 8.0, min(pxX, pxY));
}

// Derivative-faded variant of microBump: each octave is only resolvable on
// screen while its wavelength spans a few pixels in both screen axes.
// Below ~2 px the octave is sub-pixel and pure aliasing — invisible in the
// diffuse term but amplified into smudges by the GGX specular lobe at
// grazing angles. Fade each octave out below 2 px, ramping back to full
// amplitude by 8 px.
float microBumpFaded(vec2 p, float dpx, float dpy) {
    return 0.5 * microValueNoise(p / 32.0 + 11.17) * octaveFade(32.0, dpx, dpy) +
           0.25 * microValueNoise(p / 16.0 + 5.93) * octaveFade(16.0, dpx, dpy) +
           0.5 * microValueNoise(p / 8.0 + 7.31) * octaveFade(8.0, dpx, dpy) +
           0.25 * microValueNoise(p / 4.0 + 3.7) * octaveFade(4.0, dpx, dpy);
}

vec3 safeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    if (len2 <= 1e-8) return fallback;
    return v / sqrt(len2);
}

vec3 triplanarWeights(vec3 worldNormal, float sharpness) {
    vec3 w = abs(worldNormal);
    w      = pow(w, vec3(sharpness));
    return w / (w.x + w.y + w.z + 1e-6);
}

mat3 buildTerrainTBN(vec3 geomNormal) {
    vec3 N = safeNormalize(geomNormal, vec3(0.0, 1.0, 0.0));
    vec3 T = vec3(1.0, 0.0, 0.0) - N * N.x;
    T      = safeNormalize(T, vec3(1.0, 0.0, 0.0));

    vec3 B = vec3(0.0, 0.0, 1.0) - N * N.z - T * dot(T, vec3(0.0, 0.0, 1.0));
    B      = safeNormalize(B, safeNormalize(cross(T, N), vec3(0.0, 0.0, 1.0)));

    return mat3(T, B, N);
}

void main() {
    vec3 geometryNormal = normalize(inNormal);
    if (!gl_FrontFacing) geometryNormal = -geometryNormal;

    vec3 V = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);

    // World-space tiled UV for the grass texture (stable across tiles).
    vec2 grassUV = inWorldPos.xz * AZGAAR_GRASS_TILE;

    uint grassAlbedo = sceneBuffer.terrain.grassAlbedoIndex;
    uint grassNormal = sceneBuffer.terrain.grassNormalIndex;

    vec3 baseColor;
    float roughness = 0.9;
    vec3 N          = geometryNormal;

    if (grassAlbedo != 0u) {
        // Default grass albedo (rgb) + roughness (a).
        vec4 a = texture(sampler2D(textures[nonuniformEXT(grassAlbedo)], samplers[SAMPLER_LINEAR]),
                         grassUV);
        baseColor = a.rgb;
        roughness = clamp(a.a, 0.04, 1.0);

        // Tangent-space normal mapping (trivial TBN, matches the regular
        // terrain pass for near-horizontal geometry).
        if (grassNormal != 0u) {
            vec4 n =
                texture(sampler2D(textures[nonuniformEXT(grassNormal)], samplers[SAMPLER_LINEAR]),
                        grassUV);
            vec2 nxy = (n.rg * 2.0 - 1.0) * SPLAT_NORMAL_STRENGTH;
            float nz = sqrt(max(1.0 - dot(nxy, nxy), 0.0));
            mat3 TBN = buildTerrainTBN(geometryNormal);
            N        = safeNormalize(TBN * vec3(nxy, nz), geometryNormal);
            if (!gl_FrontFacing) N = -N;
        }
    } else {
        // No grass texture registered — fall back to a flat ground green.
        baseColor = AZGAAR_FALLBACK_GRASS_COLOR;
    }

    // ── Azgaar climate fields (workstream A) ─────────────────────────────
    // Static per-world textures (biome tint + temperature/precipitation/
    // coast/biome) sampled through the map-space UV derived from the terrain
    // bounds the game sets at world load.  Map +X is world -X and map +Y is
    // world -Z (azgaarMapToWorld), hence the mirrored formulation.
    uint biomeColorIndex = sceneBuffer.terrain.biomeColorIndex;
    uint climateIndex    = sceneBuffer.terrain.climateIndex;
    vec2 mapExtent  = sceneBuffer.terrain.worldMax.xz - sceneBuffer.terrain.worldMin.xz;
    bool climateOn  = sceneBuffer.terrain.climateParams.w > 0.5 && biomeColorIndex != 0u &&
                     climateIndex != 0u && mapExtent.x > 1.0 && mapExtent.y > 1.0;
    vec4 climate    = vec4(0.0);
    vec2 mapUV      = vec2(0.0);
    float landMask  = smoothstep(0.0, 0.2, inWorldPos.y);  // sea level = 0 m
    float snowT     = 0.0;
    float beachT    = 0.0;
    uint sandAlbedo = sceneBuffer.terrain.sandAlbedoIndex;
    uint snowAlbedo = sceneBuffer.terrain.snowAlbedoIndex;

    if (climateOn) {
        mapUV = clamp(
            vec2((sceneBuffer.terrain.worldMax.x - inWorldPos.x) / mapExtent.x,
                 (sceneBuffer.terrain.worldMax.z - inWorldPos.z) / mapExtent.y),
            0.0, 1.0);

        // 1) Biome tint over the grass base — soft multiply keeps the grass
        // texture detail while the world reads as FMG's authored biome map.
        vec3 biomeT = texture(
            sampler2D(textures[nonuniformEXT(biomeColorIndex)], samplers[SAMPLER_LINEAR]),
            mapUV).rgb;
        baseColor = mix(baseColor, baseColor * (biomeT * 2.0), 0.55);

        climate = texture(
            sampler2D(textures[nonuniformEXT(climateIndex)], samplers[SAMPLER_LINEAR]),
            mapUV);
    }

    // ── Turf colour variation (dry-grass patches) ───────────────
    // Coarse world-anchored value noise (12 m / 48 m) splits the ground into
    // patches of dry, sun-bleached straw and surviving green turf, like a
    // parched summer field (todo/terrain-texture-example.jpeg).  The dry
    // state dominates; green survives where the noise runs high.  A fine 4 m
    // octave wobbles brightness so neither state reads as a flat fill.
    // World-anchored + stateless → stable across tiles/frames.  Inserted
    // before the beach/cliff/snow swaps below so only the grass base is
    // affected.
    {
        float n = 0.6 * microValueNoise(inWorldPos.xz / 12.0 + 31.7) +
                  0.4 * microValueNoise(inWorldPos.xz / 48.0 + 71.3);  // [-1,1]
        // Mean noise ≈ 0 → mean mask ≈ 0.65: the ground is mostly parched
        // straw with a healthy scatter of green patches where the noise runs
        // high (matches the dry-dominant look of todo/terrain-texture-example.jpeg).
        float dryMask = smoothstep(0.32, 0.62, 0.5 + 0.5 * n);
        // Sun-bleached straw (linear space; the grass albedo is sRGB-decoded).
        vec3 dryColor = vec3(0.58, 0.50, 0.32);
        float wobble  = 0.82 + 0.36 * (0.5 + 0.5 * microValueNoise(inWorldPos.xz / 4.0 + 13.9));
        baseColor    = mix(baseColor, dryColor, dryMask) * wobble;
    }

    // 2) Beach band: low land near sea level becomes sand, with a darker
    // wet-sand strip right at the waterline.  Driven by the fragment's own
    // height (metres, 4 m grid), not the coarse climate grid.
    {
        float beachH = sceneBuffer.terrain.climateParams.z;
        if (beachH > 0.0) {
            // The sand band extends ~1.5 m below the waterline: the shallow
            // submerged shelf around beaches/ponds would otherwise keep the
            // grass tint (landMask cuts off underwater) and reads as a green
            // ring seen through the translucent water.
            float beachMask = smoothstep(-1.5, 0.2, inWorldPos.y);
            beachT = (1.0 - smoothstep(beachH * 0.24, beachH, inWorldPos.y)) * beachMask;
            float wetT = (1.0 - smoothstep(0.1, 1.2, inWorldPos.y)) * landMask;
            vec3 sandColor = sandAlbedo != 0u
                                 ? texture(sampler2D(textures[nonuniformEXT(sandAlbedo)],
                                                     samplers[SAMPLER_LINEAR]),
                                           grassUV)
                                       .rgb
                                 : AZGAAR_FALLBACK_SAND_COLOR;
            baseColor = mix(baseColor, sandColor, beachT);
            baseColor = mix(baseColor, sandColor * 0.55, wetT);
        }
    }

    // Slope-based cliff texturing + altitude rock band (mirrors terrain.frag /
    // azgaar_terrain.frag).  The rock weight is the max of the slope blend and
    // a highland band (55%..85% of the world's max land height), so exposed
    // rock appears both on steep faces and on high plateaus above the snowless
    // tree line.
    float rockT = 0.0;
    {
        float slope      = 1.0 - max(dot(geometryNormal, vec3(0.0, 1.0, 0.0)), 0.0);
        float cliffBlend = smoothstep(0.1, 0.4, slope);
        float maxLandY   = sceneBuffer.terrain.worldMax.y;
        float rockAlt    = maxLandY > 1.0
                               ? smoothstep(0.55, 0.85, inWorldPos.y / maxLandY) * landMask
                               : 0.0;
        rockT            = max(cliffBlend, rockAlt);

        uint cliffAlbedoIdx = sceneBuffer.terrain.cliffAlbedoIndex;
        uint cliffNormalIdx = sceneBuffer.terrain.cliffNormalIndex;

        if (rockT > 0.01 && cliffAlbedoIdx != 0u && grassAlbedo != 0u) {
            // Triplanar sampling for cliff
            vec3 w   = triplanarWeights(geometryNormal, 4.0);
            vec2 uvX = inWorldPos.zy * CLIFF_TRIPLANAR_SCALE;
            vec2 uvY = inWorldPos.xz * CLIFF_TRIPLANAR_SCALE;
            vec2 uvZ = inWorldPos.xy * CLIFF_TRIPLANAR_SCALE;

            vec4 aX = texture(
                sampler2D(textures[nonuniformEXT(cliffAlbedoIdx)], samplers[SAMPLER_LINEAR]),
                uvX);
            vec4 aY = texture(
                sampler2D(textures[nonuniformEXT(cliffAlbedoIdx)], samplers[SAMPLER_LINEAR]),
                uvY);
            vec4 aZ = texture(
                sampler2D(textures[nonuniformEXT(cliffAlbedoIdx)], samplers[SAMPLER_LINEAR]),
                uvZ);
            vec4 cliffAlbedoSample = aX * w.x + aY * w.y + aZ * w.z;

            vec3 cliffBase             = cliffAlbedoSample.rgb;
            float cliffRoughnessSample = clamp(cliffAlbedoSample.a, 0.04, 1.0);

            // Blend albedo and roughness
            baseColor = mix(baseColor, cliffBase, rockT);
            roughness = mix(roughness, cliffRoughnessSample, rockT);

            // Optional cliff normal blending
            if (cliffNormalIdx != 0u) {
                vec4 nX = texture(
                    sampler2D(textures[nonuniformEXT(cliffNormalIdx)], samplers[SAMPLER_LINEAR]),
                    uvX);
                vec4 nY = texture(
                    sampler2D(textures[nonuniformEXT(cliffNormalIdx)], samplers[SAMPLER_LINEAR]),
                    uvY);
                vec4 nZ = texture(
                    sampler2D(textures[nonuniformEXT(cliffNormalIdx)], samplers[SAMPLER_LINEAR]),
                    uvZ);
                vec4 cliffNormalSample = nX * w.x + nY * w.y + nZ * w.z;

                vec2 nxy = cliffNormalSample.rg * 2.0 - 1.0;
                nxy *= SPLAT_NORMAL_STRENGTH;
                float nz                = sqrt(max(1.0 - dot(nxy, nxy), 0.0));
                vec3 cliffTangentNormal = normalize(vec3(nxy, nz));

                mat3 TBN              = buildTerrainTBN(geometryNormal);
                vec3 cliffWorldNormal = normalize(TBN * cliffTangentNormal);

                N = normalize(mix(N, cliffWorldNormal, rockT));
            }
        }
    }

    // 3) Snow line — last so the peaks stay white over rock.  FMG's cell
    // temperature already falls with altitude, so the isotherm band gives a
    // natural altitude snow line; the Glacier biome is snow unconditionally.
    // A value-noise breakup keeps the line from reading as a contour.
    if (climateOn) {
        // The A channel carries discrete biome ids (nearest-cell values), so
        // it must be read through a NEAREST sampler: bilinear-filtering ids
        // invents bogus intermediate biomes, and Wetland (id 12) numerically
        // outranks Glacier (id 11), which made whole wetlands render as snow
        // behind the old >= 10.5 threshold.  Exact-id match on the snapped
        // value instead.
        vec4 climateNearest = texture(
            sampler2D(textures[nonuniformEXT(climateIndex)], samplers[SAMPLER_NEAREST]),
            mapUV);
        float tempC   = climate.r * 255.0 - AZGAAR_CLIMATE_TEMP_BIAS;
        float biomeId = floor(climateNearest.a * 255.0 + 0.5);
        float snowLo  = sceneBuffer.terrain.climateParams.x;
        float snowHi  = sceneBuffer.terrain.climateParams.y;

        snowT = 1.0 - smoothstep(snowLo, snowHi, tempC);
        snowT = max(snowT,
                    (biomeId > AZGAAR_BIOME_GLACIER - 0.5 &&
                     biomeId < AZGAAR_BIOME_GLACIER + 0.5) ? 1.0 : 0.0);
        snowT *= landMask;
        snowT *= 0.75 + 0.25 * (0.5 + 0.5 * microValueNoise(inWorldPos.xz * 0.02));

        if (snowT > 0.004) {
            vec3 snowColor = snowAlbedo != 0u
                                 ? texture(sampler2D(textures[nonuniformEXT(snowAlbedo)],
                                                     samplers[SAMPLER_LINEAR]),
                                           grassUV)
                                       .rgb
                                 : AZGAAR_FALLBACK_SNOW_COLOR;
            baseColor = mix(baseColor, snowColor, snowT);
            // Snow normal stays flat in v1 (micro-band noise still applies
            // below, which reads well on snow).
            N = normalize(mix(N, geometryNormal, snowT));
        }
    }

    // Roughness follows the same material chain (snow smooth, sand rough).
    roughness = mix(roughness, 0.6, snowT);
    roughness = mix(roughness, 0.85, beachT);

    // Micro-band roughness: perturb the shading normal with the 4-16 m value
    // noise (finite differences, 0.5 m step).  Does not move geometry.
    //
    // Two guards keep the GGX lobe from turning this into visible smudges:
    //   1) octaveFade (screen-space derivatives): sub-pixel octaves are
    //      faded out so they don't alias into shimmer (grazing-aware).
    //   2) grazing gain: even a *resolvable* octave tilts the normal by
    //      several degrees, and at grazing angles the (Fresnel-dominant)
    //      specular lobe is hypersensitive to that tilt — it modulates the
    //      sky/sun reflection into the dark horizontal smudges. The tilt is
    //      only useful where the surface detail actually reads (steep view),
    //      so attenuate it to zero as the view goes grazing. Keyed on the
    //      geometry normal (stable, pre-perturbation) so it isn't circular.
    vec3 microN = vec3(0.0);
    {
        const float e = 0.5;
        vec2 p        = inWorldPos.xz;
        // True on-screen sampling rate of the noise input (metres/pixel),
        // in both screen axes — captures grazing-angle depth compression.
        float dpx = length(dFdx(p));
        float dpy = length(dFdy(p));
        float hC  = microBumpFaded(p, dpx, dpy);
        float hX  = microBumpFaded(p + vec2(e, 0.0), dpx, dpy);
        float hZ  = microBumpFaded(p + vec2(0.0, e), dpx, dpy);
        microN    = vec3((hC - hX) / e, 0.0, (hC - hZ) / e);

        float geomNdotV   = max(dot(geometryNormal, V), 0.0);
        float grazingGain = smoothstep(0.05, 0.30, geomNdotV);
        N                 = safeNormalize(N + microN * (MICRO_NOISE_STRENGTH * grazingGain), N);
        if (!gl_FrontFacing) N = -N;
    }

    // ── Full PBR lighting (parity with terrain.frag / azgaar_terrain.frag) ──
    float NdotV = max(dot(N, V), 0.001);

    const vec3 F0        = vec3(0.04);
    const float metallic = 0.0;

    // Directional sun (Cook-Torrance GGX).
    DirectionalLight dirLight = sceneBuffer.directionalLight;
    vec3 lightDir             = normalize(dirLight.direction.xyz);
    vec3 lightColor           = dirLight.color.rgb * dirLight.direction.w;
    vec3 L                    = -lightDir;
    float NdotL               = max(dot(N, L), 0.0);

    vec3 specular = vec3(0.0);
    vec3 kD;
#if HEIGHTMAP_TERRAIN_ENABLE_SPECULAR
    vec3 H      = normalize(V + L);
    float HdotV = max(dot(H, V), 0.0);

    float D  = DistributionGGX(N, H, roughness);
    float G  = GeometrySmith(N, V, L, roughness);
    vec3 F   = fresnelSchlick(HdotV, F0);
    specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    kD       = vec3(1.0) - F;
#else
    kD = vec3(1.0);
#endif

    // Shadows: cascaded directional (objects/characters cast onto the terrain)
    // multiplied by screen-space contact shadows.
    vec4 shadowFull     = sampleShadowFull(inWorldPos, N);
    float contactShadow = sampleContactShadow();
    vec3 shadow         = shadowFull.rgb * contactShadow;
    float cascadeShadow = shadowFull.a;

    vec3 Lo = (kD * baseColor / PI + specular) * lightColor * NdotL * shadow;

    // Ambient / IBL — ambient comes exclusively from the IBL resource;
    // with IBL disabled there is no fill at all.
    vec3 ambientDiffuse  = vec3(0.0);
    vec3 ambientSpecular = vec3(0.0);
    if (sceneBuffer.ibl.enabled != 0u) {
        vec3 R                 = reflect(-V, N);
        float iblIntensity     = sceneBuffer.ibl.intensity;
        float iblSpecIntensity = sceneBuffer.ibl.specularIntensity;
        float maxLod           = sceneBuffer.ibl.prefilterMapMaxLod;
        float specLod          = sqrt(roughness) * maxLod;

        if (sceneBuffer.ibl.prefilterMapIndex != 0u && sceneBuffer.ibl.brdfLutIndex != 0u) {
            vec3 irradiance;
            if (sceneBuffer.ibl.hasSH != 0u) {
                vec3 rN         = mat3(sceneBuffer.ibl.envRotation) * N;
                const float A0  = PI, A1 = 2.0 * PI / 3.0;
                const float Y00 = 0.282095, Y1x = 0.488603;
                irradiance = sceneBuffer.ibl.shL0_M0.rgb * A0 * Y00;
                irradiance += sceneBuffer.ibl.shL1_Mp1.rgb * A1 * Y1x * rN.x;
                irradiance += sceneBuffer.ibl.shL1_Mn1.rgb * A1 * Y1x * rN.y;
                irradiance += sceneBuffer.ibl.shL1_M0.rgb * A1 * Y1x * rN.z;
                irradiance = max(irradiance, vec3(0.0));
            } else {
                irradiance = vec3(0.5);
            }

            vec3 prefilteredColor =
                textureLod(
                    samplerCube(cubeTextures[nonuniformEXT(sceneBuffer.ibl.prefilterMapIndex)],
                                samplers[SAMPLER_LINEAR]),
                    normalize(mat3(sceneBuffer.ibl.envRotation) * R),
                    clamp(specLod, 0.0, maxLod))
                    .rgb;

            const float BLEND_START = 0.7;
            const float BLEND_END   = 0.9;
            float specBlend =
                clamp((roughness - BLEND_START) / (BLEND_END - BLEND_START), 0.0, 1.0);
            specBlend *= specBlend;
            vec3 specColor = mix(prefilteredColor, irradiance / PI, specBlend);

            vec2 brdf = texture(sampler2D(textures[nonuniformEXT(sceneBuffer.ibl.brdfLutIndex)],
                                          samplers[SAMPLER_CLAMP_LINEAR]),
                                vec2(NdotV, roughness))
                            .rg;
            vec3 specFactor = F0 * brdf.x + brdf.y;
#if HEIGHTMAP_TERRAIN_ENABLE_SPECULAR
            ambientDiffuse  = (vec3(1.0) - specFactor) * irradiance * baseColor / PI * iblIntensity;
            ambientSpecular = specColor * specFactor * iblIntensity * iblSpecIntensity;
#else
            ambientDiffuse  = irradiance * baseColor / PI * iblIntensity;
            ambientSpecular = vec3(0.0);
#endif
        }
    }

    // Attenuate ambient in shadow so shadowed terrain isn't lit only by sky.
    float shadowAmbientFade = mix(1.0 - SHADOW_DARKNESS, 1.0, mix(1.0, cascadeShadow, NdotL));
    vec3 color = (ambientDiffuse + ambientSpecular) * shadowAmbientFade + Lo;

    // Forward+ point/spot lights (includes per-light specular BRDF).
#if HEIGHTMAP_TERRAIN_ENABLE_SPECULAR
    {
        vec3 T_aniso = vec3(0.0);
        vec3 B_aniso = vec3(0.0);
        color += evaluateForwardPlusLights(inWorldPos,
                                           N,
                                           V,
                                           NdotV,
                                           F0,
                                           roughness,
                                           metallic,
                                           baseColor,
                                           T_aniso,
                                           B_aniso,
                                           0.0);
    }
#endif

    // Debug: height ramp (periodic hue cycle, one full cycle per 256 m).
    if (pc.flags.w != 0.0) {
        float h = inWorldPos.y;
        color   = 0.5 + 0.5 * cos(6.2831853 * (h * (1.0 / 256.0) + vec3(0.0, 0.33, 0.67)));
    }

    if (pc.flags.z != 0.0) color = vec3(1.0, 0.0, 0.0);

    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    color = clamp(color, vec3(0.0), vec3(65504.0));

    outColor    = vec4(color, 1.0);
    outNormal   = OctEncode(normalize(N));
    outMaterial = vec4(roughness, 0.0, 1.0, 1.0);
    outAlbedo   = vec4(baseColor, 0.0);
}