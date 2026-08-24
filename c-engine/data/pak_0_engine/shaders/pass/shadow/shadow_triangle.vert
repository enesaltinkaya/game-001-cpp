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
    uint cascadeIndex;
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

layout(location = 0) out vec2 outUV;
layout(location = 1) out flat uint outMaterialId;

void main() {
    uint visibleIndex = gl_DrawID;

    CulledBuffer culledBuf = CulledBuffer(culledBufferAddress);
    uint instanceIndex     = culledBuf.culledIndices[visibleIndex];

    TriangleInstanceBuffer instBuf =
        TriangleInstanceBuffer(triangleInstanceBufferAddress);
    GpuTriangleInstance inst = instBuf.instances[instanceIndex];

    // Per-cascade bounding sphere culling: reject instances whose bounding
    // sphere is entirely outside the cascade's clip volume.  Without this,
    // geometry at the wrong depth range gets depth-clamped to 0, blocking
    // all subsequent geometry in the shadow map.
    {
        bool isSkinned_cull = (inst.attributeMask & (1 << 6)) != 0;
        uint skinOff_cull = isSkinned_cull ? entitySkinMap.offsets[inst.entity] : 0xFFFFFFFF;
        vec3 wc;
        float wr;
        if (isSkinned_cull && skinOff_cull != 0xFFFFFFFF) {
            mat4 rootJoint = jointMatrixBuffer.matrices[skinOff_cull];
            wc = (rootJoint * vec4(inst.boundingSphere.xyz, 1.0)).xyz;
            float sx = length(rootJoint[0].xyz);
            float sy = length(rootJoint[1].xyz);
            float sz = length(rootJoint[2].xyz);
            wr = inst.boundingSphere.w * max(sx, max(sy, sz));
        } else {
            Transform t = transformBuffer.transforms[inst.entity];
            wc = transformVertex(inst.boundingSphere.xyz, t.pos.xyz, t.rot, vec3(t.pos.w));
            wr = inst.boundingSphere.w * abs(t.pos.w);
        }

        mat4 svp = sceneBuffer.shadow.shadowViewProjection[cascadeIndex];
        vec4 clip = svp * vec4(wc, 1.0);

        float rx = wr * length(vec3(svp[0][0], svp[1][0], svp[2][0]));
        float ry = wr * length(vec3(svp[0][1], svp[1][1], svp[2][1]));
        float rz = wr * length(vec3(svp[0][2], svp[1][2], svp[2][2]));

        bool outside = (clip.x - rx >  1.5) || (clip.x + rx < -1.5)
                    || (clip.y - ry >  1.5) || (clip.y + ry < -1.5)
                    || (clip.z - rz >  1.5) || (clip.z + rz < -0.5);
        if (outside) {
            gl_Position = vec4(3.0, 3.0, 3.0, 1.0);
            outUV = vec2(0.0);
            outMaterialId = 0u;
            return;
        }
    }

    uint vertexIndex = gl_VertexIndex;

    PositionBuffer posBuf = PositionBuffer(positionBufferAddress);
    vec3 position         = vec3(posBuf.data[vertexIndex * 3 + 0],
                         posBuf.data[vertexIndex * 3 + 1],
                         posBuf.data[vertexIndex * 3 + 2]);

    // Skinning
    AttributeBufferWords attrBuf = AttributeBufferWords(attributeBufferAddress);
    uint vertexStrideWords       = inst.attributeStride;
    bool isSkinned = (inst.attributeMask & (1 << 6)) != 0;
    uint skinOffset = isSkinned ? entitySkinMap.offsets[inst.entity] : 0xFFFFFFFF;

    vec3 worldPos;
    if (isSkinned && skinOffset != 0xFFFFFFFF) {
        uint bIdx        = vertexIndex * vertexStrideWords;
        uint jointsWord  = attrBuf.data[bIdx + 4];
        uint weightsWord = attrBuf.data[bIdx + 5];
        mat4 skinMatrix  = computeSkinMatrixFromWords(skinOffset, jointsWord, weightsWord);
        worldPos = (skinMatrix * vec4(position, 1.0)).xyz;
    } else {
        Transform transform = transformBuffer.transforms[inst.entity];
        worldPos = transformVertex(position, transform.pos.xyz, transform.rot, vec3(transform.pos.w));
    }

    gl_Position = sceneBuffer.shadow.shadowViewProjection[cascadeIndex] * vec4(worldPos, 1.0);

    uint baseIndex = vertexIndex * vertexStrideWords;
    outUV         = unpackUnorm2x16(attrBuf.data[baseIndex + 3]);
    outMaterialId = inst.materialId;
}
