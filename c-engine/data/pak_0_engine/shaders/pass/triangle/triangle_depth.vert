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
    uint64_t prevTransformBufferAddress;
    uint64_t jointMatrixBufferAddress;
    uint64_t entitySkinMapBufferAddress;
    uint64_t prevJointMatrixBufferAddress;
    uint maxTriangleInstances;
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"
#include "../../includes/skinning.shader"

TransformBufferRef transformBuffer     = TransformBufferRef(transformBufferAddress);
TransformBufferRef prevTransformBuffer = TransformBufferRef(prevTransformBufferAddress);

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

layout(location = 0) out vec4 outClipCurrent;
layout(location = 1) out vec2 outUV;
layout(location = 2) out flat uint outMaterialId;
layout(location = 3) out vec3 outViewNormal;
layout(location = 4) out vec4 outClipPrev;

void main() {
    uint visibleIndex = gl_DrawID;

    CulledBuffer culledBuf = CulledBuffer(culledBufferAddress);
    uint instanceIndex     = culledBuf.culledIndices[visibleIndex];

    TriangleInstanceBuffer instBuf =
        TriangleInstanceBuffer(triangleInstanceBufferAddress);
    GpuTriangleInstance inst = instBuf.instances[instanceIndex];

    uint vertexIndex = gl_VertexIndex;

    PositionBuffer posBuf = PositionBuffer(positionBufferAddress);
    vec3 position         = vec3(posBuf.data[vertexIndex * 3 + 0],
                         posBuf.data[vertexIndex * 3 + 1],
                         posBuf.data[vertexIndex * 3 + 2]);

    // Skinning check
    AttributeBufferWords attrBuf = AttributeBufferWords(attributeBufferAddress);
    uint vertexStrideWords       = inst.attributeStride;
    bool isSkinned = (inst.attributeMask & (1 << 6)) != 0;
    uint skinOffset = isSkinned ? entitySkinMap.offsets[inst.entity] : 0xFFFFFFFF;

    // Read normal from attribute buffer
    vec2 nxy    = unpackSnorm2x16(attrBuf.data[vertexIndex * vertexStrideWords + 0]);
    vec2 nzw    = unpackSnorm2x16(attrBuf.data[vertexIndex * vertexStrideWords + 1]);
    vec3 normal = vec3(nxy, nzw.x);

    vec3 worldPos;
    vec3 prevWorldPos;
    vec3 worldNormal;
    if (isSkinned && skinOffset != 0xFFFFFFFF) {
        uint bIdx        = vertexIndex * vertexStrideWords;
        uint jointsWord  = attrBuf.data[bIdx + 4];
        uint weightsWord = attrBuf.data[bIdx + 5];
        mat4 skinMatrix      = computeSkinMatrixFromWords(skinOffset, jointsWord, weightsWord);
        mat4 prevSkinMatrix  = computePrevSkinMatrixFromWords(skinOffset, jointsWord, weightsWord);
        worldPos     = (skinMatrix * vec4(position, 1.0)).xyz;
        prevWorldPos = (prevSkinMatrix * vec4(position, 1.0)).xyz;
        worldNormal  = normalize((skinMatrix * vec4(normal, 0.0)).xyz);
    } else {
        Transform transform     = transformBuffer.transforms[inst.entity];
        Transform prevTransform = prevTransformBuffer.transforms[inst.entity];
        worldPos = transformVertex(position, transform.pos.xyz, transform.rot, vec3(transform.pos.w));
        prevWorldPos = transformVertex(position,
                                       prevTransform.pos.xyz,
                                       prevTransform.rot,
                                       vec3(prevTransform.pos.w));
        worldNormal = transformDirection(normal, transform.rot, vec3(1.0));
    }

    // Depth uses the main camera VP (must match the color pass for depth-equal test)
    gl_Position = sceneBuffer.cameras[0].viewProjection * vec4(worldPos, 1.0);

    // Velocity uses the stable VP matrices.
    outClipCurrent = sceneBuffer.cameras[0].viewProjectionNoJitter * vec4(worldPos, 1.0);
    outClipPrev    = sceneBuffer.cameras[0].prevViewProjectionNoJitter * vec4(prevWorldPos, 1.0);

    uint baseIndex = vertexIndex * vertexStrideWords;
    outUV         = unpackUnorm2x16(attrBuf.data[baseIndex + 3]);
    outMaterialId = inst.materialId;

    // View-space normal for the view-normal attachment
    vec3 viewNormal = normalize((sceneBuffer.cameras[0].view * vec4(worldNormal, 0.0)).xyz);
    outViewNormal = viewNormal;
}
