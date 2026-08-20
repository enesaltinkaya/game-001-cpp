#version 450
#extension GL_EXT_buffer_reference : enable
#extension GL_EXT_nonuniform_qualifier : enable
#extension GL_ARB_shading_language_include : enable
#extension GL_ARB_gpu_shader_int64 : enable

#include "../../includes/globalset.shader"


layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) in flat int textureId;

layout(location = 0) out vec4 finalColor;

void main() {
    if (textureId > 0) {
        vec4 texColor = texture(sampler2D(textures[textureId], samplers[SAMPLER_LINEAR]), fragTexCoord);
        finalColor    = fragColor * texColor;
    } else {
        finalColor = vec4(pow(fragColor.xyz, vec3(2.2)), fragColor.w);
        // vec4 texColor = texture(_tex, fragTexCoord);
        // finalColor     = fragColor * texColor;
        // finalColor.xyz = pow(finalColor.xyz, vec3(2.2));
        // finalColor = vec4(1, 0, 0, 1);
    }
}