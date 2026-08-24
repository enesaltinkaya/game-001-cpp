#version 460

layout(location = 0) in vec3 inPosition;
layout(location = 0) out vec3 outColor;

struct GpuTransform {
    vec4 rot;
    vec4 pos;
};

struct CullInstance {
    uint entity;
    uint primitiveIndex;
    uint indexCount;
    uint firstIndex;
    int vertexOffset;
    uint materialId;
    vec4 aabbMin;
    vec4 aabbMax;
};

layout(set = 1, binding = 0, std430) readonly buffer TransformBuffer {
    GpuTransform transforms[];
};

layout(set = 1, binding = 1, std430) readonly buffer CullInstanceBuffer {
    CullInstance cullInstances[];
};

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    vec4 color;
} pc;

mat4 quatToMat4(vec4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;

    return mat4(
        vec4(1.0 - 2.0 * (yy + zz), 2.0 * (xy + wz), 2.0 * (xz - wy), 0.0),
        vec4(2.0 * (xy - wz), 1.0 - 2.0 * (xx + zz), 2.0 * (yz + wx), 0.0),
        vec4(2.0 * (xz + wy), 2.0 * (yz - wx), 1.0 - 2.0 * (xx + yy), 0.0),
        vec4(0.0, 0.0, 0.0, 1.0)
    );
}

void main() {
    CullInstance ci = cullInstances[gl_BaseInstance];
    GpuTransform t = transforms[ci.entity];

    mat4 model = quatToMat4(t.rot);
    model[0].xyz *= t.pos.w;
    model[1].xyz *= t.pos.w;
    model[2].xyz *= t.pos.w;
    model[3] = vec4(t.pos.xyz, 1.0);

    vec4 worldPos = model * vec4(inPosition, 1.0);

    outColor = pc.color.rgb;
    gl_Position = pc.viewProjection * worldPos;
}
