/* ibl_common.shader — shared IBL utility functions
 *
 * Include AFTER utils.shader (needs PI).
 * Used by IBL generation shaders and scene rendering shaders.
 */

#ifndef IBL_COMMON_SHADER
#define IBL_COMMON_SHADER

// ---------------------------------------------------------------------------
// Equirectangular mapping
// ---------------------------------------------------------------------------
vec2 directionToEquirectUv(vec3 dir) {
    dir         = normalize(dir);
    float phi   = atan(dir.z, dir.x);
    float theta = asin(clamp(dir.y, -1.0, 1.0));
    return vec2(phi * (0.5 / PI) + 0.5, 0.5 - theta * (1.0 / PI));
}

// ---------------------------------------------------------------------------
// Cubemap face direction from UV
// ---------------------------------------------------------------------------
vec3 cubemapDirection(uint face, vec2 uv) {
    if (face == 0u) return normalize(vec3( 1.0, uv.y, -uv.x));
    if (face == 1u) return normalize(vec3(-1.0, uv.y,  uv.x));
    if (face == 2u) return normalize(vec3(uv.x,  1.0, -uv.y));
    if (face == 3u) return normalize(vec3(uv.x, -1.0,  uv.y));
    if (face == 4u) return normalize(vec3(uv.x, uv.y,  1.0));
    return                  normalize(vec3(-uv.x, uv.y, -1.0));
}

// ---------------------------------------------------------------------------
// Low-discrepancy sequence (Hammersley)
// ---------------------------------------------------------------------------
float RadicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint n) {
    return vec2(float(i) / float(n), RadicalInverse_VdC(i));
}

// ---------------------------------------------------------------------------
// GGX importance sampling
// ---------------------------------------------------------------------------
vec3 ImportanceSampleGGX(vec2 xi, vec3 N, float roughness) {
    float a  = roughness * roughness;
    float a2 = a * a;

    float phi      = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a2 - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));

    vec3 H = vec3(cos(phi) * sinTheta,
                  sin(phi) * sinTheta,
                  cosTheta);

    vec3 up        = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent   = normalize(cross(up, N));
    vec3 bitangent = cross(N, tangent);

    return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

#endif /* IBL_COMMON_SHADER */
