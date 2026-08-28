// Shared procedural sky (skybox.frag + the brixelizer env-cube bake).
// Requires globalset.shader (sceneBuffer for the directional light).
// worldDir must be a normalized world-space direction.
vec3 skyEvaluate(vec3 worldDir) {
    // Elevation: 1 at zenith, -1 at nadir
    float elevation = worldDir.y;

    // ── Procedural blue-sky gradient ──────────────────────────────
    // Blend between a deep zenith blue and a lighter horizon color.
    // Below the horizon we fade into a darker ground colour.

    // Sky colours (linear)
    vec3 zenithColor  = vec3(0.15, 0.35, 0.75);   // deep blue
    vec3 horizonColor = vec3(0.55, 0.75, 0.95);    // light blue-white
    vec3 groundColor  = vec3(0.25, 0.28, 0.32);    // muted dark

    float t = clamp(elevation, 0.0, 1.0);
    // Use a pow curve so the transition spends more range near the horizon
    float skyMix = pow(t, 0.5);

    vec3 skyColor = mix(horizonColor, zenithColor, skyMix);

    // Below-horizon fade
    float groundFade = smoothstep(0.0, -0.15, elevation);
    skyColor = mix(skyColor, groundColor, groundFade);

    // ── Sun disc ────────────────────────────────────────────────────────────
    vec3 sunDir = normalize(-sceneBuffer.directionalLight.direction.xyz);
    float sunDot = max(dot(worldDir, sunDir), 0.0);

    // Small, crisp disc: solid core ~0.5deg, tight 0.5-1.0deg AA edge.
    float sunDisc = smoothstep(0.99985, 0.99995, sunDot);
    // Narrow halo hugging the disc (gone by ~8deg), not a wide soft halo.
    float sunGlow = pow(sunDot, 256.0) * 0.2;

    vec3 sunColor = sceneBuffer.directionalLight.color.rgb
                  * sceneBuffer.directionalLight.direction.w;  // intensity
    skyColor += sunDisc * sunColor * 8.0;
    skyColor += sunGlow  * sunColor;

    return skyColor;
}
