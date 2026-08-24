#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inTangent;
layout(location = 3) in vec2 inUV;
layout(location = 4) in uint inJoints;
layout(location = 5) in uint inWeights;

layout(push_constant) uniform PushConstants {
    uint64_t transformBufferAddress;
    uint64_t drawInstanceBufferAddress;
    uint64_t culledBufferAddress;
    uint64_t jointMatrixBufferAddress;
    uint64_t entitySkinMapBufferAddress;
    uint64_t prevJointMatrixBufferAddress;
    uint cascadeIndex;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/skinning.shader"

TransformBufferRef transformBuffer = TransformBufferRef(transformBufferAddress);

struct GpuDrawInstance {
    uint firstIndex;
    uint indexCount;
    int vertexOffset;
    uint entity;
    uint materialId;
    uint flags;
    uint _pad0;
    uint _pad1;
    vec4 boundingSphere;
};

layout(buffer_reference, std430) buffer DrawInstanceBuffer {
    GpuDrawInstance instances[];
};

layout(buffer_reference, std430) buffer CulledBuffer {
    uint culledIndices[];
};

layout(location = 0) out vec2 outUV;
layout(location = 1) out flat uint outMaterialId;

void main() {
    CulledBuffer culledBuf = CulledBuffer(culledBufferAddress);
    uint instanceIndex     = culledBuf.culledIndices[gl_DrawID];

    DrawInstanceBuffer instBuf = DrawInstanceBuffer(drawInstanceBufferAddress);
    GpuDrawInstance inst = instBuf.instances[instanceIndex];

    vec3 worldPos;

    bool isSkinned = (inst.flags & 4u) != 0u;
    uint skinOffset = isSkinned ? entitySkinMap.offsets[inst.entity] : 0xFFFFFFFFu;

    if (isSkinned && skinOffset != 0xFFFFFFFFu) {
        mat4 skinMatrix = computeSkinMatrixFromWords(skinOffset, inJoints, inWeights);
        worldPos = (skinMatrix * vec4(inPosition, 1.0)).xyz;
    } else {
        Transform transform = transformBuffer.transforms[inst.entity];
        worldPos = transformVertex(inPosition, transform.pos.xyz, transform.rot, vec3(transform.pos.w));
    }

    gl_Position   = sceneBuffer.shadow.shadowViewProjection[cascadeIndex] * vec4(worldPos, 1.0);
    outUV         = inUV;
    outMaterialId = inst.materialId;
}
