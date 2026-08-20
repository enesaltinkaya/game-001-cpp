#pragma once

#include "ecs/system/scene/SceneSystem.h"
#include "cglm/git/include/cglm/types.h"
#include "renderer/Renderer.h"

#define DEFAULT_ANIMATION_FRAME_RATE 30.0f
#define MAX_ANIMATION_NAME_LENGTH 64
#define MAX_EVENT_NAME_LENGTH 64

// Forward declarations
struct AnimationClip;
struct AnimationChannel;
struct AnimationInstance;
struct Animator;
struct JointMapping;

/*
 * Interpolation type for animation keyframes
 */
enum InterpolationType {
    INTERPOLATION_LINEAR,
    INTERPOLATION_STEP,
    INTERPOLATION_CUBIC_SPLINE
};

/*
 * Keyframe for position, rotation, or scale
 * Supports linear, step, and cubic spline interpolation
 */
struct Keyframe {
    float time;
    vec4 value;       // Keyframe value
    vec4 inTangent;   // For cubic spline interpolation
    vec4 outTangent;  // For cubic spline interpolation
};

/*
 * Event definition in an animation clip
 */
struct AnimationEventDef {
    String name;
    float time;  // Time in seconds when event triggers
};

/*
 * Animation event callback data
 */
struct EventCallback {
    String eventName;
    void (*callback)(void* userData);
    void* userData;
    float lastFiredTime;  // Track last fire time to avoid duplicates
};

/*
 * Single animation channel for one joint
 * Each channel can have different interpolation types per property
 */
struct AnimationChannel {
    String jointName;  // Bone/node name (resolved to entity ID at playback time)
    InterpolationType positionInterpolation;
    InterpolationType rotationInterpolation;
    InterpolationType scaleInterpolation;
    Array(Keyframe) positionKeys;
    Array(Keyframe) rotationKeys;
    Array(Keyframe) scaleKeys;
    // Cached last keyframe indices for sequential playback optimization
    i32 lastPosKeyIndex;
    i32 lastRotKeyIndex;
    i32 lastScaleKeyIndex;
};

/*
 * Complete animation clip loaded from glTF
 */
struct AnimationClip {
    String name;
    float duration;
    Array(AnimationChannel) channels;
    Array(AnimationEventDef) events;
};

/*
 * Animation instance (per-entity playback state)
 */
struct AnimationInstance {
    AnimationClip* clip;
    float currentTime;
    float speed;
    bool loop;
    float weight;             // Current blend weight (0.0 to 1.0)
    float blendWeightStart;   // Weight when blend began
    float blendWeightTarget;  // Target weight for smooth transitions
    float blendDuration;      // Time to reach target weight
    float blendElapsed;       // Time elapsed since blend started
    Array(EventCallback) eventCallbacks;
    bool markedForRemoval;
};

/*
 * Joint mapping for animation remapping
 * Maps joint names from source skeleton to target entity IDs
 */
struct JointMapping {
    u32 sourceJointCount;
    u32 targetJointCount;
    u32 jointMap[MAX_JOINTS];       // sourceJointIndex -> targetJointIndex
    vec3 scaleFactors[MAX_JOINTS];  // Scale adjustment per joint
};

/*
 * Animator component (attached to animated entities)
 */
struct Animator {
    Entity* entity;  // Owning entity
    Array(AnimationInstance*) activeInstances;
    JointMapping* mapping;  // NULL if no remapping needed
    StrMap(Entity*) boneMap; // bone name -> Entity* cache (built from entity subtree)

    // Per-clip channel resolution cache: clip pointer -> array of resolved entity IDs
    // Avoids strmapGet per channel per frame
    AnimationClip* cachedClip;
    Array(u32) cachedChannelEntities;  // parallel to cachedClip->channels

    // Cached skeleton root entity (common ancestor of the animator entity
    // and all joint entities).  Computed once, used every tick to activate
    // and save LastTransform for the entire skeleton subtree.
    Entity* skeletonRoot;
};

REGISTER_COMPONENT(Animator);

/*
 * Animation Library - Global storage for all loaded animations
 */
struct AnimationLibrary {
    Array(AnimationClip*) clips;
};

extern AnimationLibrary animationLibrary;

/*
 * =====================================================
 * ANIMATION SYSTEM PUBLIC API
 * =====================================================
 */

/*
 * Animation Management
 */

// Get or create animation clip by name
AnimationClip* animationGetOrCreate(const char* name);

// Get animation clip by name (returns NULL if not found)
AnimationClip* animationGet(const char* name);

// Get the first animation clip in the library (returns NULL if empty)
AnimationClip* animationGetFirst(void);

/*
 * Entity Animation Control
 */

// Play animation on entity (replaces all current animations)
void animationPlay(Entity* entity, const char* clipName, float speed, bool loop);

// Play animation with smooth blend transition
void animationPlayBlended(Entity* entity, const char* clipName, float speed, bool loop, float blendDuration);

// Crossfade between two animations
void animationCrossFade(Entity* entity, const char* fromClip, const char* toClip, float duration);

// Stop specific animation on entity
void animationStop(Entity* entity, const char* clipName);

// Stop all animations on entity
void animationStopAll(Entity* entity);

// Set animation playback speed
void animationSetSpeed(Entity* entity, const char* clipName, float speed);

// Check if animation is playing
bool animationIsPlaying(Entity* entity, const char* clipName);

// Check if a non-looping animation has finished (reached its duration)
bool animationIsFinished(Entity* entity, const char* clipName);

// Restart an animation from the beginning (no blend, instant reset)
void animationRestart(Entity* entity, const char* clipName, float speed);

// Check if a non-looping animation is about to finish (within threshold seconds)
bool animationIsNearEnd(Entity* entity, const char* clipName, float thresholdSeconds);

// Get the Entity* for a named bone/joint in the entity's skeleton.
// Returns NULL if not found or no animator.
Entity* animationGetBoneEntity(Entity* entity, const char* boneName);

/*
 * Event Management
 */

// Add event callback for a specific animation clip
void animationAddEventCallback(const char* clipName, const char* eventName, void (*callback)(void*), void* userData);

// Remove event callback
void animationRemoveEventCallback(const char* clipName, const char* eventName);

/*
 * Name-only convenience API (looks up entity by name in default scene)
 */

// Play animation on a named entity (replaces all current animations)
void animationPlayByName(const char* entityName, const char* clipName, float speed, bool loop);

// Play animation on a named entity with smooth blend transition
void animationPlayBlendedByName(const char* entityName, const char* clipName, float speed, bool loop, float blendDuration);

// Crossfade between two animations on a named entity
void animationCrossFadeByName(const char* entityName, const char* fromClip, const char* toClip, float duration);

// Stop specific animation on a named entity
void animationStopByName(const char* entityName, const char* clipName);

// Stop all animations on a named entity
void animationStopAllByName(const char* entityName);

// Check if animation is playing on a named entity
bool animationIsPlayingByName(const char* entityName, const char* clipName);

// Check if a non-looping animation has finished on a named entity
bool animationIsFinishedByName(const char* entityName, const char* clipName);

/*
 * Remapping
 */

// Set up joint remapping for an entity
void animationSetRemapping(Entity* entity, const JointMapping* mapping);

// Create joint mapping from source to target skeleton
JointMapping* animationCreateJointMapping(const char* sourceSkeleton, const char* targetSkeleton);

// Free joint mapping
void animationFreeJointMapping(JointMapping* mapping);


