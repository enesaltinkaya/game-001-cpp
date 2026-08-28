#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    uint environmentMapIndex;
    uint faceIndex;
    float roughness;
    float pad;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/ibl_common.shader"

// Sun-threshold clamping (inspired by EEVEE):
// Instead of a flat clamp, we separate radiance above the threshold.
// The excess energy is handled by the extracted sun light, so clamping
// here doesn't lose energy — it just redirects it.
const float SUN_THRESHOLD = 10.0;

vec3 sampleEnvironment(vec3 dir) {
    vec3 radiance = textureLod(sampler2D(textures[nonuniformEXT(environmentMapIndex)],
                                         samplers[SAMPLER_LINEAR]),
                               directionToEquirectUv(dir),
                               0.0)
                        .rgb;
    // Clamp per-channel: keep color ratios but limit magnitude
    float lum = dot(radiance, vec3(0.2126, 0.7152, 0.0722));
    if (lum > SUN_THRESHOLD) {
        radiance *= SUN_THRESHOLD / lum;
    }
    return radiance;
}

void main() {
    vec2 uv = inUV * 2.0 - 1.0;
    vec3 N  = cubemapDirection(faceIndex, uv);

    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up         = normalize(cross(N, right));

    const uint SAMPLE_COUNT = 2048u;
    vec3 irradiance         = vec3(0.0);

    for (uint i = 0u; i < SAMPLE_COUNT; i++) {
        vec2 xi = Hammersley(i, SAMPLE_COUNT);

        float phi      = 2.0 * PI * xi.x;
        float cosTheta = sqrt(1.0 - xi.y);
        float sinTheta = sqrt(xi.y);

        vec3 tangentSample = vec3(cos(phi) * sinTheta,
                                  sin(phi) * sinTheta,
                                  cosTheta);
        vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
        irradiance += sampleEnvironment(sampleVec);
    }

    irradiance = PI * irradiance / float(SAMPLE_COUNT);
    outColor   = vec4(irradiance, 1.0);
}
