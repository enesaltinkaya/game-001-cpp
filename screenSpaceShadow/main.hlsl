// Vulkan-compatible contact shadows shader using raymarch.hlsl
// Compile with: glslc -fshader-stage=compute main.hlsl -o main.spv

#version 450
#extension GL_EXT_shader_explicit_arithmetic_types : enable
#extension GL_GOOGLE_include_directive : enable

// Define HLSL-like constructs for Vulkan/GLSL compatibility
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define float3x3 mat3
#define float4x4 mat4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define lerp mix
#define frac fract
#define mul(m, v) (m * v)

// Samplers and textures
layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D output_image;

// Uniform buffer for frame constants
layout(set = 0, binding = 2) uniform FrameConstants {
    mat4 view_to_clip;
    mat4 clip_to_view;
    mat4 world_to_view;
    mat4 view_to_world;
    vec3 camera_position;
    float near_plane;
    vec2 screen_size;
    vec2 inv_screen_size;
    vec3 sun_direction;
    float time;
} frame;

// Helper functions that raymarch.hlsl depends on

float rcp_near_plane_distance() {
    return 1.0 / frame.near_plane;
}

vec2 cs_to_uv(vec2 cs) {
    // Vulkan: Y is flipped compared to OpenGL
    return cs * vec2(0.5, -0.5) + vec2(0.5, 0.5);
}

vec2 uv_to_cs(vec2 uv) {
    return (uv - vec2(0.5)) * vec2(2.0, -2.0);
}

vec3 position_clip_to_world(vec4 clip_pos) {
    vec4 view_pos = frame.clip_to_view * clip_pos;
    view_pos /= view_pos.w;
    vec4 world_pos = frame.view_to_world * view_pos;
    return world_pos.xyz;
}

vec3 position_world_to_sample(vec3 world_pos) {
    vec4 view_pos = frame.world_to_view * vec4(world_pos, 1.0);
    vec4 clip_pos = frame.view_to_clip * view_pos;
    return clip_pos.xyz / clip_pos.w;
}

vec4 direction_world_to_clip(vec3 dir) {
    vec4 view_dir = frame.world_to_view * vec4(dir, 0.0);
    return frame.view_to_clip * view_dir;
}

vec3 direction_to_sun() {
    return normalize(frame.sun_direction);
}

// Simple interleaved gradient noise for jittering
float interleaved_gradient_noise(vec2 px) {
    return fract(52.9829189 * fract(0.06711056 * px.x + 0.00583715 * px.y));
}

// Include the modified raymarch implementation
// We'll inline the necessary parts here instead of using #include

struct RootFinderInput {
    float distance;
    bool valid;
};

struct DistanceWithPenetration {
    float distance;
    bool valid;
    float penetration;
};

struct DepthRayMarchResult {
    bool hit;
    float hit_t;
    vec2 hit_uv;
    float hit_penetration;
    float hit_penetration_frac;
};

// Simplified depth raymarch distance function for Vulkan
DistanceWithPenetration depth_raymarch_distance(
    vec3 ray_point_cs,
    bool march_behind_surfaces,
    float depth_thickness,
    bool use_sloppy_march
) {
    vec2 interp_uv = cs_to_uv(ray_point_cs.xy);
    float ray_depth = 1.0 / ray_point_cs.z;
    
    float linear_depth, unfiltered_depth;
    
    if (use_sloppy_march) {
        unfiltered_depth = 1.0 / texture(depth_tex, interp_uv).r;
        linear_depth = unfiltered_depth;
    } else {
        linear_depth = 1.0 / texture(depth_tex, interp_uv).r;
        unfiltered_depth = 1.0 / texelFetch(depth_tex, ivec2(interp_uv * frame.screen_size), 0).r;
    }
    
    float max_depth = max(linear_depth, unfiltered_depth);
    float min_depth = min(linear_depth, unfiltered_depth);
    
    const float bias = 0.000002;
    
    DistanceWithPenetration res;
    res.distance = max_depth * (1.0 + bias) - ray_depth;
    res.penetration = ray_depth - min_depth;
    res.valid = march_behind_surfaces ? (res.penetration < depth_thickness) : true;
    
    return res;
}

// Simplified hybrid root finder for contact shadows
bool find_root_linear(
    vec3 start_cs,
    vec3 end_cs,
    uint linear_steps,
    float jitter,
    bool march_behind_surfaces,
    float depth_thickness,
    bool use_sloppy_march,
    out float hit_t,
    out DistanceWithPenetration hit_d
) {
    vec3 dir = end_cs - start_cs;
    float min_t = 0.0;
    float max_t = 1.0;
    
    bool intersected = false;
    DistanceWithPenetration min_d, max_d;
    
    // Linear march
    for (uint step = 0; step < linear_steps; step++) {
        float candidate_t = mix(0.0, 1.0, (float(step) + jitter) / float(linear_steps));
        vec3 candidate = start_cs + dir * candidate_t;
        DistanceWithPenetration candidate_d = depth_raymarch_distance(
            candidate, march_behind_surfaces, depth_thickness, use_sloppy_march
        );
        
        if (candidate_d.distance < 0.0 && candidate_d.valid) {
            max_t = candidate_t;
            max_d = candidate_d;
            intersected = true;
            break;
        } else {
            min_t = candidate_t;
            min_d = candidate_d;
        }
    }
    
    hit_t = intersected ? max_t : min_t;
    hit_d = intersected ? max_d : min_d;
    
    return intersected;
}

// Main depth raymarch function for contact shadows
DepthRayMarchResult depth_ray_march(
    vec3 ray_start_cs,
    vec3 ray_end_cs,
    uint linear_steps,
    float jitter,
    float depth_thickness_linear_z,
    bool march_behind_surfaces,
    bool use_sloppy_march
) {
    DepthRayMarchResult res;
    res.hit = false;
    res.hit_t = 0.0;
    res.hit_uv = vec2(0.0);
    res.hit_penetration = 0.0;
    res.hit_penetration_frac = 0.0;
    
    vec2 ray_start_uv = cs_to_uv(ray_start_cs.xy);
    vec2 ray_end_uv = cs_to_uv(ray_end_cs.xy);
    
    float linear_z_to_scaled = rcp_near_plane_distance();
    float depth_thickness = depth_thickness_linear_z * linear_z_to_scaled;
    
    DistanceWithPenetration hit;
    bool intersected = find_root_linear(
        ray_start_cs, ray_end_cs, linear_steps, jitter,
        march_behind_surfaces, depth_thickness, use_sloppy_march,
        res.hit_t, hit
    );
    
    if (intersected && hit.penetration < depth_thickness && hit.distance < depth_thickness) {
        res.hit = true;
        res.hit_uv = mix(ray_start_uv, ray_end_uv, res.hit_t);
        res.hit_penetration = hit.penetration / linear_z_to_scaled;
        res.hit_penetration_frac = hit.penetration / depth_thickness;
    } else {
        res.hit_uv = mix(ray_start_uv, ray_end_uv, res.hit_t);
    }
    
    return res;
}

// Compute shader entry point
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    
    if (any(greaterThanEqual(px, ivec2(frame.screen_size)))) {
        return;
    }
    
    vec2 uv = (vec2(px) + 0.5) * frame.inv_screen_size;
    
    // Get depth and reconstruct world position
    float depth = texture(depth_tex, uv).r;
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y; // Vulkan NDC
    vec3 ray_start_cs = vec3(ndc, depth);
    
    // Reconstruct world position
    vec3 ray_hit_ws = position_clip_to_world(vec4(ray_start_cs, 1.0));
    
    // Set up contact shadow ray toward sun
    vec3 sun_dir = direction_to_sun();
    float shadow_distance = 0.3; // Adjust based on scene scale
    
    vec3 ray_end_ws = ray_hit_ws + sun_dir * shadow_distance;
    vec3 ray_end_cs = position_world_to_sample(ray_end_ws);
    
    // Clamp ray to screen bounds
    ray_end_cs = clamp(ray_end_cs, vec3(-1.0), vec3(1.0));
    
    // Generate jitter for temporal stability
    float jitter = interleaved_gradient_noise(vec2(px));
    
    // Perform raymarch for contact shadows
    DepthRayMarchResult raymarch_result = depth_ray_march(
        ray_start_cs,
        ray_end_cs,
        4,      // linear_steps
        jitter,
        0.5,    // depth_thickness
        true,   // march_behind_surfaces
        false   // use_sloppy_march
    );
    
    // Calculate shadow value
    float shadow = 1.0;
    if (raymarch_result.hit) {
        shadow = smoothstep(1.0, 0.5, raymarch_result.hit_penetration_frac);
    }
    
    // Output shadow value (you can modulate scene color here)
    vec4 output_color = vec4(shadow, shadow, shadow, 1.0);
    imageStore(output_image, px, output_color);
}