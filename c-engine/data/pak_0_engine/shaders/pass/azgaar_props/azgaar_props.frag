#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

// ── Azgaar props fragment shader (workstream B) ───────────────────────────
// Cheap analytic lighting (Lambert) mirroring the old Azgaar
// pass, plus a Lambert-only Forward+ point/spot accumulation
// (full PBR is overkill at vegetation scale).
// Receives the scene's cascaded sun shadows, screen-space contact shadows
// and screen-space AO (same sources as the terrain/scene passes) so grass
// under tree canopies darkens with the ground instead of staying lit.
// Per-instance tint (biome colour x jitter) is multiplied into the result.
// Species 12 (flowers) gets a radial alpha test (dot look) via the mesh UV.

layout(location = 0) in vec3 inWorldPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inUV;
layout(location = 4) flat in uint inSpecies;
layout(location = 5) flat in uint inTexId;   // texture-array index (0xFFFFFFFF = none)
layout(location = 6) in vec3 inVertColor;    // per-part colour (white = tintable)

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outNormal;   // oct-encoded world normal (R16G16)
layout(location = 2) out vec4 outMaterial; // roughness, metallic, alphaMask, ao (R8G8B8A8)

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/shadow.shader"
#include "../../includes/forwardplus.shader"

void main() {
    /* Jitter compensation for alpha-cutout: undo the UV shift caused by
     * the jittered projection so the discard boundary is pinned to the
     * same WORLD point every frame, regardless of jitter phase.  Without
     * this the hard alpha test below would shift ±0.5px per phase and the
     * TAA would see the edge flicker.  Must match azgaar_props_depth.frag.
     * (Same technique as scene.frag / scene_depth.frag.) */
    vec2 cutUV = inUV;
    if (inTexId != 0xFFFFFFFFu) {
        vec2 jitterPx = -vec2(sceneBuffer.cameras[0].jitterX, sceneBuffer.cameras[0].jitterY)
                        * sceneBuffer.cameras[0].viewport;
        cutUV += dFdx(inUV) * jitterPx.x + dFdy(inUV) * jitterPx.y;
    }

    // Flower alpha test (species 12): keep only a small central disc of the
    // unit-UV quad so it reads as a sparse flower dot rather than a card.
    if (inSpecies == 12u) {
        float d = length(cutUV - 0.5);
        if (d > 0.30) discard;
    }

    vec3 N = normalize(inNormal);
    // The props pipe renders double-sided (noCull), so back faces must store
    // the OUTWARD normal of the visible surface (matches the scene pass).
    // Without this the G-buffer keeps the front-face normal on back faces,
    // which breaks normal-based consumers (decal GROUND_ONLY check, SSR, ...).
    if (!gl_FrontFacing) N = -N;
    vec3 V = normalize(sceneBuffer.cameras[0].position.xyz - inWorldPos);

    vec3 sunDir   = normalize(-sceneBuffer.directionalLight.direction.xyz);
    vec3 sunColor = sceneBuffer.directionalLight.color.rgb
                  * sceneBuffer.directionalLight.direction.w;

    float NdotL = max(dot(N, sunDir), 0.0);

    // Shadows: cascaded directional (trees / props cast onto the vegetation)
    // multiplied by screen-space contact shadows.  Same sources as the
    // terrain / scene passes, so grass under a canopy darkens with the
    // ground instead of staying fully lit.
    vec4 shadowFull     = sampleShadowFull(inWorldPos, N);
    float contactShadow = sampleContactShadow();
    vec3 shadow         = shadowFull.rgb * contactShadow;

    // Base colour: per-instance tint, multiplied by the per-vertex base-color
    // texture when one is assigned (authored .glb species).  The texture lives
    // in the global set 0 `textures` array (globalset.shader), indexed by the
    // per-vertex texId.  Procedural species carry NO_PROPS_TEX (0xFFFFFFFF)
    // and keep the plain tint.
    // Per-part tint mask: a white vertex color marks a part as tintable
    // (leaves / canopy) and receives the per-instance biome tint; a
    // non-white vertex color (trunk) is NOT tinted.  For textured parts the
    // trunk keeps its own base-color texture (brown bark) as-is; for
    // untextured (procedural) parts the vertex color is the actual part
    // colour.
    float tintable = step(0.99, min(inVertColor.r, min(inVertColor.g, inVertColor.b)));
    vec3 albedo;
    if (inTexId != 0xFFFFFFFFu) {
        /* Sample with the jitter-compensated UV so the colour and the
         * alpha discard below agree on the world point. */
        vec4 tex = texture(sampler2D(textures[nonuniformEXT(inTexId)],
                                    samplers[SAMPLER_LINEAR]), cutUV);
        // Tint only the tintable (white) parts; non-tintable parts (trunk)
        // keep their own base-color texture (brown bark) unchanged.
        vec3 tint = mix(vec3(1.0), inColor, tintable);
        albedo = tint * tex.rgb;
        // Alpha-masked textures (leaf / flower) discard transparent pixels.
        // HARD test (frameIndex 0) + the UV jitter compensation above:
        // the cutout boundary is pinned to the world point, so in a static
        // scene it is frozen and TAA accumulates fully (a per-frame
        // stochastic dither would instead make every boundary pixel flip
        // between leaf and background, which the TAA's color rejection
        // reads as a disocclusion and resets — full-canopy shimmer).
        // Under motion the velocity buffer (which captures the wind sway)
        // reprojects the edge to where it actually was, so the TAA only
        // reacts at the genuinely moving edge.
        if (stochasticAlphaDiscard(tex.a, 0.5, gl_FragCoord.xy, 0u))
            discard;
    } else {
        // Procedural (no texture): tintable parts get the biome tint,
        // non-tintable parts keep their baked vertex color.
        albedo = mix(inVertColor, inColor, tintable);
    }

    // No ambient / IBL: shadowed vegetation is pure black; only direct
    // light (sun + Forward+ point/spot) reaches the surface.
    //
    // Energy-consistent with the PBR passes: the Lambert diffuse integrates
    // to 1/PI, so the direct sun term carries the same /PI that scene.frag
    // and heightmap_terrain.frag apply.
    vec3 color = albedo * (sunColor / PI) * NdotL * shadow;

    // Forward+ point/spot lights: Lambert-only accumulation (vegetation is
    // matte, so the specular GGX term is skipped).  Same light-grid source
    // as the terrain / scene passes, so a torch near some trees reads as
    // lit canopies instead of sun-lit green.
    color += evaluateForwardPlusLightsDiffuse(inWorldPos, N, albedo);

    if (any(isnan(color)) || any(isinf(color))) color = vec3(0.0);
    outColor    = vec4(color, 1.0); // opaque: the LOD hard switch is resolved in the vertex shader
    outNormal   = OctEncode(N);
    // Vegetation is matte: high roughness, no metal.  alphaMask = 1 because
    // leaf/flower textures are alpha-masked (FSR reactive mask uses this).
    outMaterial = vec4(0.9, 0.0, 1.0, 0.0);  // .a = ground flag: props (grass/trees) are not ground, so GROUND_ONLY decals don't paint onto canopies
}