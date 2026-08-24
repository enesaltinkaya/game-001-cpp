#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_ARB_shading_language_include : enable

layout(push_constant) uniform PushConstants {
    uint64_t positionBufferAddress;
    uint64_t attributeBufferAddress;
    uint64_t indexBufferAddress;
    uint64_t triangleInstanceBufferAddress;
    uint64_t culledBufferAddress;
    uint64_t drawCountBufferAddress;
    uint64_t transformBufferAddress;
    uint64_t jointMatrixBufferAddress;
    uint64_t entitySkinMapBufferAddress;
    uint64_t prevJointMatrixBufferAddress;
    uint maxTriangleInstances;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/skinning.shader"

TransformBufferRef transformBuffer = TransformBufferRef(transformBufferAddress);

layout(buffer_reference, std430) buffer PositionBuffer {
    float data[];
};

layout(buffer_reference, std430) buffer AttributeBufferWords {
    uint data[];
};

struct GpuTriangleInstance {
    uint indexOffset;
    uint indexCount;
    uint entity;
    uint materialId;
    uint attributeMask;
    uint attributeStride;
    uint _pad0;
    uint _pad1;
    vec4 boundingSphere;
};

layout(buffer_reference, std430) buffer TriangleInstanceBuffer {
    GpuTriangleInstance instances[];
};

layout(buffer_reference, std430) buffer CulledBuffer {
    uint culledIndices[];
};

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec2 outUV;
layout(location = 2) out flat uint outMaterialId;
layout(location = 3) out vec4 outTangent;
layout(location = 4) out vec3 outWorldPos;

void main() {
    uint visibleIndex = gl_DrawID;

    CulledBuffer culledBuf = CulledBuffer(culledBufferAddress);
    uint instanceIndex     = culledBuf.culledIndices[visibleIndex];

    TriangleInstanceBuffer instBuf =
        TriangleInstanceBuffer(triangleInstanceBufferAddress);
    GpuTriangleInstance inst = instBuf.instances[instanceIndex];

    // gl_VertexIndex is the actual index value from the index buffer
    uint vertexIndex = gl_VertexIndex;

    // Position (packed as 3 floats per vertex)
    PositionBuffer posBuf = PositionBuffer(positionBufferAddress);
    vec3 position         = vec3(posBuf.data[vertexIndex * 3 + 0],
                         posBuf.data[vertexIndex * 3 + 1],
                         posBuf.data[vertexIndex * 3 + 2]);

    // Attributes
    AttributeBufferWords attrBuf = AttributeBufferWords(attributeBufferAddress);
    uint vertexStrideWords       = inst.attributeStride;
    uint baseIndex               = vertexIndex * vertexStrideWords;

    vec2 nxy     = unpackSnorm2x16(attrBuf.data[baseIndex + 0]);
    vec2 nzw     = unpackSnorm2x16(attrBuf.data[baseIndex + 1]);
    vec3 normal  = vec3(nxy, nzw.x);
    vec4 tangent = unpackSnorm4x8(attrBuf.data[baseIndex + 2]);
    vec2 uv      = unpackUnorm2x16(attrBuf.data[baseIndex + 3]);

    // Check for skinning
    bool isSkinned = (inst.attributeMask & (1 << 6)) != 0;
    uint skinOffset = isSkinned ? entitySkinMap.offsets[inst.entity] : 0xFFFFFFFF;

    vec3 worldPos;
    vec3 worldNormal;
    vec3 worldTangent;

    if (isSkinned && skinOffset != 0xFFFFFFFF) {
        uint jointsWord  = attrBuf.data[baseIndex + 4];
        uint weightsWord = attrBuf.data[baseIndex + 5];
        mat4 skinMatrix  = computeSkinMatrixFromWords(skinOffset, jointsWord, weightsWord);
        worldPos     = (skinMatrix * vec4(position, 1.0)).xyz;
        worldNormal  = normalize((skinMatrix * vec4(normal, 0.0)).xyz);
        worldTangent = normalize((skinMatrix * vec4(tangent.xyz, 0.0)).xyz);
    } else {
        Transform transform = transformBuffer.transforms[inst.entity];
        worldPos     = transformVertex(position, transform.pos.xyz, transform.rot, vec3(transform.pos.w));
        worldNormal  = transformDirection(normal, transform.rot, vec3(1.0));
        worldTangent = transformDirection(tangent.xyz, transform.rot, vec3(1.0));
    }

    gl_Position = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);

    outNormal     = worldNormal;
    outUV         = uv;
    outMaterialId = inst.materialId;
    outTangent    = vec4(worldTangent, tangent.w);
    outWorldPos   = worldPos;
}
