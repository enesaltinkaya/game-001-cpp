#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) in vec2 inUV;
layout(location = 1) in flat uint inMaterialId;

void main() {
    Material material = materialBuffer.materials[inMaterialId];

    // Alpha masking: discard transparent fragments to avoid phantom shadows
    if ((material.featureMask & (1u << MAT_ALPHA_MASK)) != 0u) {
        vec4 baseColor = material.baseColor;
        if ((material.featureMask & (1u << MAT_HAS_TEXTURE_COLOR)) != 0u) {
            vec2 uv = inUV * material.baseColorOffsetScale.zw +
                      material.baseColorOffsetScale.xy;
            baseColor *= textureLod(
                sampler2D(textures[nonuniformEXT(material.colorTexture)],
                          samplers[nonuniformEXT(material.colorTextureSampler)]),
                uv, 0.0);
        }
        if (baseColor.a < material.rmas.z) {
            discard;
        }
    }
}
