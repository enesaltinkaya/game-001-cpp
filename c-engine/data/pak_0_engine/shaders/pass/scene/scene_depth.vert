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
    uint64_t prevTransformBufferAddress;
    uint64_t drawInstanceBufferAddress;
    uint64_t culledBufferAddress;
    uint64_t jointMatrixBufferAddress;
    uint64_t entitySkinMapBufferAddress;
    uint64_t prevJointMatrixBufferAddress;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/skinning.shader"

TransformBufferRef transformBuffer     = TransformBufferRef(transformBufferAddress);
TransformBufferRef prevTransformBuffer = TransformBufferRef(prevTransformBufferAddress);

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

layout(location = 0) out vec4 outClipCurrent;
layout(location = 1) out vec2 outUV;
layout(location = 2) out flat uint outMaterialId;
layout(location = 3) out vec3 outViewNormal;
layout(location = 4) out vec4 outClipPrev;
layout(location = 5) out flat uint outEntity;

void main() {
    CulledBuffer culledBuf = CulledBuffer(culledBufferAddress);
    uint instanceIndex     = culledBuf.culledIndices[gl_DrawID];

    DrawInstanceBuffer instBuf = DrawInstanceBuffer(drawInstanceBufferAddress);
    GpuDrawInstance inst = instBuf.instances[instanceIndex];

    vec3 worldPos;
    vec3 prevWorldPos;
    vec3 worldNormal;

    bool isSkinned = (inst.flags & 4u) != 0u;
    uint skinOffset = isSkinned ? entitySkinMap.offsets[inst.entity] : 0xFFFFFFFFu;

    if (isSkinned && skinOffset != 0xFFFFFFFFu) {
        mat4 skinMatrix = computeSkinMatrixFromWords(skinOffset, inJoints, inWeights);
        mat4 prevSkinMatrix = computePrevSkinMatrixFromWords(skinOffset, inJoints, inWeights);
        worldPos     = (skinMatrix * vec4(inPosition, 1.0)).xyz;
        prevWorldPos = (prevSkinMatrix * vec4(inPosition, 1.0)).xyz;
        worldNormal  = normalize((skinMatrix * vec4(inNormal, 0.0)).xyz);
    } else {
        Transform transform     = transformBuffer.transforms[inst.entity];
        Transform prevTransform = prevTransformBuffer.transforms[inst.entity];
        worldPos     = transformVertex(inPosition, transform.pos.xyz, transform.rot, vec3(transform.pos.w));
        prevWorldPos = transformVertex(inPosition, prevTransform.pos.xyz, prevTransform.rot, vec3(prevTransform.pos.w));
        worldNormal  = transformDirection(inNormal, transform.rot, vec3(1.0));
    }

    gl_Position = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);

    outClipCurrent = sceneBuffer.cameras[0].viewProjectionNoJitter * vec4(worldPos, 1.0);
    outClipPrev    = sceneBuffer.cameras[0].prevViewProjectionNoJitter * vec4(prevWorldPos, 1.0);

    outUV         = inUV;
    outMaterialId = inst.materialId;
    outViewNormal = normalize((sceneBuffer.cameras[0].view * vec4(worldNormal, 0.0)).xyz);
    outEntity     = inst.entity;
}
