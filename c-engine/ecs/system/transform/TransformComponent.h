#pragma once

#include "ecs/system/scene/SceneSystem.h"

namespace engine {
typedef struct Transform {
    vec4 rot;
    vec4 pos; // last element is scale
} Transform;

typedef struct WorldTransform {
    vec4 rot;
    vec4 pos;
} WorldTransform;

typedef struct LastTransform {
    vec4 rot;
    vec4 pos;
} LastTransform;

REGISTER_COMPONENT(Transform);
REGISTER_COMPONENT(WorldTransform);
REGISTER_COMPONENT(LastTransform);

// if an entity is not animated and has no parent, single component Transform
// * its world and local transforms are both Transform
// * its render transform is also Transform
// * it has no need for PreviousTransform

// if an entity is not animated but has a parent, two components Transform for local, WorldTransform for global
// * it has a WorldTransform component, calculated by multiplying/adding with parent
// * its render transform is also WorldTransform
// * it has no need for PreviousTransform

// if an entity is animated without a parent, 3 components, Transform, LastTransform and WorldTransform
// * it stores previous state in LastTransform, takes previous state from Transform
// * interpolated results go into WorldTransform, from LastTransform to Transform
// * no parent so its world transform is Transform

// if an entity is animated and has a parent
// * same as above, WorldTransform is multiplied with parent at the end

/*
 * =====================================================
 * TRANSFORM HELPER FUNCTIONS
 * =====================================================
 */

/*
 * Correct quaternion slerp that always takes the shortest path.
 * cglm's glm_quat_slerp has a bug: when the dot product is negative it
 * negates a local copy but the LERP fallback still uses the original
 * (un-negated) quaternion, causing flips/glitches for near-antipodal quats.
 */
static inline void quatSlerpShortest(const versor from, const versor to, float t, versor out) {
    /* Local copies so from/to may alias out safely. */
    versor f = {from[0], from[1], from[2], from[3]};
    float dot = f[0]*to[0] + f[1]*to[1] + f[2]*to[2] + f[3]*to[3];

    versor to2;
    if (dot < 0.0f) {
        dot = -dot;
        to2[0] = -to[0]; to2[1] = -to[1]; to2[2] = -to[2]; to2[3] = -to[3];
    } else {
        to2[0] = to[0]; to2[1] = to[1]; to2[2] = to[2]; to2[3] = to[3];
    }

    float s0, s1;
    if (dot < 0.9999f) {
        float angle    = acosf(dot);
        float sinAngle = sinf(angle);
        s0 = sinf((1.0f - t) * angle) / sinAngle;
        s1 = sinf(t * angle) / sinAngle;
    } else {
        s0 = 1.0f - t;
        s1 = t;
    }

    out[0] = s0*f[0] + s1*to2[0];
    out[1] = s0*f[1] + s1*to2[1];
    out[2] = s0*f[2] + s1*to2[2];
    out[3] = s0*f[3] + s1*to2[3];
}

/*
 * Create a Transform from position, rotation, and scale
 */
static inline void transformFromPRS(Transform* out, const vec3 pos, const versor rot, float scale) {
    memcpy(out->rot, rot, sizeof(vec4));
    memcpy(out->pos, pos, sizeof(vec3));
    out->pos[3] = scale;
}

/*
 * Create a Transform from position, rotation quaternion, and 3-component scale
 * 
 * IMPORTANT LIMITATION: This struct stores uniform scale in a single float (pos[3]).
 * 
 * This is an intentional optimization for memory efficiency:
 * - Using uniform scale: 32 bytes per Transform (2 vec4s)
 * - Using full 4x4 matrix: 64 bytes per Transform (16 floats)
 */
static inline void transformFromPRS3(Transform* out, const vec3 pos, const versor rot, const vec3 scale) {
    memcpy(out->rot, rot, sizeof(vec4));
    memcpy(out->pos, pos, sizeof(vec3));
    out->pos[3] = scale[0];
}

/*
 * Extract position, rotation, and scale from a Transform
 */
static inline void transformToPRS(const Transform* in, vec3 pos, versor rot, float* scale) {
    memcpy(rot, in->rot, sizeof(vec4));
    memcpy(pos, in->pos, sizeof(vec3));
    *scale = in->pos[3];
}

/*
 * Linear interpolation between two Transforms
 * Uses slerp for rotation and lerp for position and scale
 */
static inline void transformLerp(Transform* out, const Transform* a, const Transform* b, float t) {
    // Slerp rotation (shortest path, alias-safe)
    quatSlerpShortest((float*)a->rot, (float*)b->rot, t, out->rot);
    glm_quat_normalize(out->rot);
    
    // Lerp position + scale (alias-safe: read before write)
    vec4 aPos; glm_vec4_copy((float*)a->pos, aPos);
    glm_vec3_lerp(aPos, (float*)b->pos, t, out->pos);
    out->pos[3] = glm_lerp(aPos[3], b->pos[3], t);
}

/*
 * Blend two Transforms with proper component-wise interpolation
 * This is more expensive than simple lerp but handles transformations correctly
 */
static inline void transformBlend(Transform* out, const Transform* a, const Transform* b, float weight) {
    // Weighted slerp for rotation (shortest path, alias-safe)
    quatSlerpShortest((float*)a->rot, (float*)b->rot, weight, out->rot);
    glm_quat_normalize(out->rot);
    
    // Weighted lerp for position + scale (alias-safe: read before write)
    vec4 aPos; glm_vec4_copy((float*)a->pos, aPos);
    glm_vec3_lerp(aPos, (float*)b->pos, weight, out->pos);
    out->pos[3] = glm_lerp(aPos[3], b->pos[3], weight);
}

/*
 * Convert Transform to a 4x4 matrix
 */
static inline void transformToMat4(const Transform* in, mat4 out) {
    // Convert quaternion to rotation matrix
    glm_quat_mat4((float*)in->rot, out);
    
    // Apply scale (stored in pos[3])
    float scale = in->pos[3];
    glm_vec3_scale(out[0], scale, out[0]);
    glm_vec3_scale(out[1], scale, out[1]);
    glm_vec3_scale(out[2], scale, out[2]);
    
    // Set translation
    memcpy(out[3], in->pos, sizeof(vec3));
}

/*
 * Copy one Transform to another
 */
static inline void transformCopy(Transform* out, const Transform* in) {
    memcpy(out->rot, in->rot, sizeof(vec4));
    memcpy(out->pos, in->pos, sizeof(vec4));
}

/*
 * Set Transform to identity (no rotation, zero position, scale 1)
 */
static inline void transformIdentity(Transform* out) {
    glm_quat_identity(out->rot);
    glm_vec4_zero(out->pos);
    out->pos[3] = 1.0f;
}
}  // namespace engine
