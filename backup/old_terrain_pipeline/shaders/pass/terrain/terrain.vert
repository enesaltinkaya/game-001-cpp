#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outUV;
layout(location = 2) out vec4 outTangent;
layout(location = 3) out vec3 outWorldPos;
layout(location = 4) flat out uint outAzgaarCellId;

#define AZGAAR_CELL_ID_TANGENT_BIAS 1024.0
#define AZGAAR_INVALID_CELL_ID 0xffffffffu

void main() {
    vec3 worldPos = inPosition;
    gl_Position   = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);
    outNormal     = inNormal;
    outUV         = inUV;
    outTangent    = inTangent;
    outWorldPos   = worldPos;

    float encodedCellId = inTangent.w - AZGAAR_CELL_ID_TANGENT_BIAS;
    outAzgaarCellId    = (encodedCellId >= 0.0) ? uint(round(encodedCellId)) : AZGAAR_INVALID_CELL_ID;
}
