#include "AnimatorComponent.h"
#include "AnimationSystem.h"
#include "ecs/Ecs.h"
#include "ecs/components/Skin.h"
#include "ecs/system/scene/SceneSystem.h"

#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "timer/Timer.h"
#include <string.h>
#include <math.h>

AnimationLibrary animationLibrary = {};

/*
 * =====================================================
 * INTERNAL HELPERS
 * =====================================================
 */

/*
 * Linear interpolation between two keyframes
 */
static void lerpKeyframe(const Keyframe* a, const Keyframe* b, float t, vec4 out) {
    vec4 aCopy, bCopy;
    memcpy(aCopy, a->value, sizeof(vec4));
    memcpy(bCopy, b->value, sizeof(vec4));
    glm_vec4_lerp(aCopy, bCopy, t, out);
}

/* quatSlerp is now provided by quatSlerpShortest() in Transform.h */

static void slerpRotation(const Keyframe* a, const Keyframe* b, float t, vec4 out) {
    versor qa = {a->value[0], a->value[1], a->value[2], a->value[3]};
    versor qb = {b->value[0], b->value[1], b->value[2], b->value[3]};
    versor qr;
    glm_quat_normalize(qa);
    glm_quat_normalize(qb);
    quatSlerpShortest(qa, qb, t, qr);
    glm_quat_normalize(qr);
    glm_vec4_copy(qr, out);
}

/*
 * Cubic Hermite spline interpolation
 * glTF uses normalized Hermite spline: P(t) = (2t³ - 3t² + 1)P0 + (t³ - 2t² + t)m0 + (-2t³ + 3t²)P1
 * + (t³ - t²)m1
 */
static void cubicSplineInterpolate(const Keyframe* a,
                                   const Keyframe* b,
                                   float t,
                                   float dt,
                                   vec4 out) {
    float t2 = t * t;
    float t3 = t2 * t;

    // Hermite basis functions
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;  // (2t³ - 3t² + 1)
    float h10 = t3 - 2.0f * t2 + t;            // (t³ - 2t² + t)
    float h01 = -2.0f * t3 + 3.0f * t2;        // (-2t³ + 3t²)
    float h11 = t3 - t2;                       // (t³ - t²)

    for (int i = 0; i < 4; i++) {
        out[i] = h00 * a->value[i] + h10 * a->outTangent[i] * dt + h01 * b->value[i] +
                 h11 * b->inTangent[i] * dt;
    }
}

/*
 * Cubic spline interpolation for rotations (uses quaternion-specific interpolation)
 */
static void cubicSplineRotation(const Keyframe* a, const Keyframe* b, float t, float dt, vec4 out) {
    // For rotations, we use a simplified approach that still respects the tangents
    // A more accurate approach would use quaternion spline interpolation
    cubicSplineInterpolate(a, b, t, dt, out);

    // Normalize the result to ensure valid quaternion
    versor qr = {out[0], out[1], out[2], out[3]};
    glm_quat_normalize(qr);
    glm_vec4_copy(qr, out);
}

/*
 * Find keyframes for a given time using binary search (O(log n))
 * Returns indices of surrounding keyframes and interpolation factor.
 *
 * When looping is enabled and time falls after the last keyframe,
 * we interpolate between the last keyframe and the first keyframe
 * to produce a smooth wrap-around instead of holding the last pose.
 */
static void findKeyframes(Array(Keyframe) keys,
                          float time,
                          i32* outPrev,
                          i32* outNext,
                          float* outT,
                          i32* hint,
                          bool looping,
                          float clipDuration) {
    size_t keyCount = arraySize(keys);
    if (keyCount == 0) {
        *outPrev = -1;
        *outNext = -1;
        *outT    = 0.0f;
        return;
    }

    if (keyCount == 1) {
        *outPrev = 0;
        *outNext = 0;
        *outT    = 0.0f;
        return;
    }

    // Time is before first keyframe
    if (time <= keys[0].time) {
        if (looping && keys[0].time > 0.0f) {
            // Wrap: interpolate from last keyframe to first keyframe
            // The wrap region spans from keys[last].time -> clipDuration -> 0 -> keys[0].time
            float lastKeyTime = keys[keyCount - 1].time;
            float wrapRange   = (clipDuration - lastKeyTime) + keys[0].time;
            if (wrapRange > 0.0001f) {
                // time is in the [0 .. keys[0].time] portion of the wrap
                float elapsed = (clipDuration - lastKeyTime) + time;
                *outPrev = static_cast<i32>(keyCount) - 1;
                *outNext = 0;
                *outT    = elapsed / wrapRange;
                if (hint) *hint = 0;
                return;
            }
        }
        *outPrev = 0;
        *outNext = 1;
        *outT    = 0.0f;
        if (hint) *hint = 0;
        return;
    }

    // Time is after last keyframe
    if (time >= keys[keyCount - 1].time) {
        if (looping) {
            // Wrap: interpolate from last keyframe to first keyframe
            float lastKeyTime = keys[keyCount - 1].time;
            float wrapRange   = (clipDuration - lastKeyTime) + keys[0].time;
            if (wrapRange > 0.0001f) {
                float elapsed = time - lastKeyTime;
                *outPrev = static_cast<i32>(keyCount) - 1;
                *outNext = 0;
                *outT    = elapsed / wrapRange;
                if (hint) *hint = static_cast<i32>(keyCount) - 1;
                return;
            }
        }
        *outPrev = static_cast<i32>(keyCount) - 2;
        *outNext = static_cast<i32>(keyCount) - 1;
        *outT    = 1.0f;
        if (hint) *hint = *outPrev;
        return;
    }

    // Try hint first (O(1) for sequential playback)
    i32 h = (hint && *hint >= 0 && *hint < static_cast<i32>(keyCount) - 1) ? *hint : 0;
    if (keys[h].time <= time && keys[h + 1].time > time) {
        // Hint is exact
        *outPrev = h;
        *outNext = h + 1;
    } else if (h + 1 < static_cast<i32>(keyCount) - 1 && keys[h + 1].time <= time && keys[h + 2].time > time) {
        // Next interval (most common case: time advanced one interval)
        *outPrev = h + 1;
        *outNext = h + 2;
    } else {
        // Fallback: binary search
        i32 low  = 0;
        i32 high = static_cast<i32>(keyCount) - 1;
        while (low < high - 1) {
            i32 mid = low + (high - low) / 2;
            if (keys[mid].time <= time) {
                low = mid;
            } else {
                high = mid;
            }
        }
        *outPrev = low;
        *outNext = low + 1;
    }

    if (hint) *hint = *outPrev;

    float range = keys[*outNext].time - keys[*outPrev].time;
    if (range <= 0.0001f) {
        *outT = 0.0f;
    } else {
        *outT = (time - keys[*outPrev].time) / range;
    }
}

/*
 * Sample position at given time with interpolation type support
 */
static void samplePosition(AnimationChannel* channel, float time, bool looping, float clipDuration, vec3 out) {
    if (arraySize(channel->positionKeys) == 0) {
        glm_vec3_zero(out);
        return;
    }

    i32 prev, next;
    float t;
    findKeyframes(channel->positionKeys, time, &prev, &next, &t, &channel->lastPosKeyIndex, looping, clipDuration);

    vec4 result;

    switch (channel->positionInterpolation) {
        case INTERPOLATION_STEP:
            // For step interpolation, always use the previous keyframe value
            glm_vec4_copy(channel->positionKeys[prev].value, result);
            break;

        case INTERPOLATION_CUBIC_SPLINE: {
            // Calculate delta time between keyframes for cubic spline
            float dt = channel->positionKeys[next].time - channel->positionKeys[prev].time;
            cubicSplineInterpolate(&channel->positionKeys[prev],
                                   &channel->positionKeys[next],
                                   t,
                                   dt,
                                   result);
            break;
        }

        case INTERPOLATION_LINEAR:
        default:
            lerpKeyframe(&channel->positionKeys[prev], &channel->positionKeys[next], t, result);
            break;
    }

    glm_vec3_copy(result, out);
}

/*
 * Sample rotation at given time with interpolation type support
 */
static void sampleRotation(AnimationChannel* channel, float time, bool looping, float clipDuration, versor out) {
    if (arraySize(channel->rotationKeys) == 0) {
        glm_quat_identity(out);
        return;
    }

    i32 prev, next;
    float t = 0;
    findKeyframes(channel->rotationKeys, time, &prev, &next, &t, &channel->lastRotKeyIndex, looping, clipDuration);

    vec4 result;

    switch (channel->rotationInterpolation) {
        case INTERPOLATION_STEP:
            // For step interpolation, always use the previous keyframe value
            glm_vec4_copy(channel->rotationKeys[prev].value, result);
            break;

        case INTERPOLATION_CUBIC_SPLINE: {
            // Calculate delta time between keyframes for cubic spline
            float dt = channel->rotationKeys[next].time - channel->rotationKeys[prev].time;
            cubicSplineRotation(&channel->rotationKeys[prev],
                                &channel->rotationKeys[next],
                                t,
                                dt,
                                result);
            break;
        }

        case INTERPOLATION_LINEAR:
        default:
            slerpRotation(&channel->rotationKeys[prev], &channel->rotationKeys[next], t, result);
            break;
    }

    glm_vec4_copy(result, out);
}

/*
 * Sample scale at given time with interpolation type support
 */
static void sampleScale(AnimationChannel* channel, float time, bool looping, float clipDuration, vec3 out) {
    if (arraySize(channel->scaleKeys) == 0) {
        glm_vec3_one(out);
        return;
    }

    i32 prev, next;
    float t;
    findKeyframes(channel->scaleKeys, time, &prev, &next, &t, &channel->lastScaleKeyIndex, looping, clipDuration);

    vec4 result;

    switch (channel->scaleInterpolation) {
        case INTERPOLATION_STEP:
            // For step interpolation, always use the previous keyframe value
            glm_vec4_copy(channel->scaleKeys[prev].value, result);
            break;

        case INTERPOLATION_CUBIC_SPLINE: {
            // Calculate delta time between keyframes for cubic spline
            float dt = channel->scaleKeys[next].time - channel->scaleKeys[prev].time;
            cubicSplineInterpolate(&channel->scaleKeys[prev],
                                   &channel->scaleKeys[next],
                                   t,
                                   dt,
                                   result);
            break;
        }

        case INTERPOLATION_LINEAR:
        default:
            lerpKeyframe(&channel->scaleKeys[prev], &channel->scaleKeys[next], t, result);
            break;
    }

    glm_vec3_copy(result, out);
}

/*
 * Sample animation channel directly into a Transform structure
 * Only updates components that have keyframes, preserving existing values for others.
 */
static void sampleChannelToTransform(AnimationChannel* channel, float time, bool looping, float clipDuration, Transform* out) {
    if (arraySize(channel->positionKeys) > 0) {
        vec3 pos;
        samplePosition(channel, time, looping, clipDuration, pos);
        glm_vec3_copy(pos, out->pos);
    }

    if (arraySize(channel->rotationKeys) > 0) {
        versor rot;
        sampleRotation(channel, time, looping, clipDuration, rot);
        glm_vec4_copy(rot, out->rot);
    }

    if (arraySize(channel->scaleKeys) > 0) {
        vec3 scale;
        sampleScale(channel, time, looping, clipDuration, scale);
        out->pos[3] = scale[0];
    }
}

/*
 * Update event callbacks for an animation instance
 */
static void updateEvents(AnimationInstance* instance, float oldTime, float newTime) {
    // Handle looping reset
    if (instance->loop && oldTime > newTime) {
        // Wrapped around, reset event tracking
        for (size_t i = 0; i < arraySize(instance->eventCallbacks); i++) {
            instance->eventCallbacks[i].lastFiredTime = 0.0f;
        }
        oldTime = 0.0f;
    }

    // Check each event
    for (size_t i = 0; i < arraySize(instance->clip->events); i++) {
        AnimationEventDef* event = &instance->clip->events[i];

        for (size_t j = 0; j < arraySize(instance->eventCallbacks); j++) {
            EventCallback* cb = &instance->eventCallbacks[j];

            if (strequals(cb->eventName.data, event->name.data)) {
                // Check if we crossed the event time
                bool shouldFire = false;

                if (oldTime < event->time && newTime >= event->time) {
                    shouldFire = true;
                } else if (cb->lastFiredTime < event->time && oldTime < event->time) {
                    // First time crossing
                    shouldFire = true;
                }

                if (shouldFire) {
                    cb->callback(cb->userData);
                    cb->lastFiredTime = event->time;
                }
            }
        }
    }
}

/*
 * Update blend weights for an animation instance
 */
static void updateBlending(AnimationInstance* instance, float deltaTime) {
    if (instance->blendDuration <= 0.0f) {
        instance->weight = instance->blendWeightTarget;
        return;
    }

    instance->blendElapsed += deltaTime;
    float t = instance->blendElapsed / instance->blendDuration;
    t       = glm_clamp(t, 0.0f, 1.0f);

    // Proper linear interpolation from start weight to target weight
    instance->weight = glm_lerp(instance->blendWeightStart, instance->blendWeightTarget, t);

    if (t >= 1.0f) {
        instance->weight = instance->blendWeightTarget;
    }
}

/*
 * =====================================================
 * ANIMATION SYSTEM IMPLEMENTATION
 * =====================================================
 */

static void addEntitySubtreeToBoneMap(Animator* animator, Entity* entity) {
    if (entity->name) {
        strmapPut(animator->boneMap, entity->name, entity);
    }
    for (size_t i = 0; i < arraySize(entity->children); i++) {
        addEntitySubtreeToBoneMap(animator, entity->children[i]);
    }
}

static void buildBoneMap(Animator* animator, Scene* scene) {
    Skin* skin = getComponent(scene, Skin, animator->entity->id);
    if (skin) {
        // Skinned mesh: map from skin joints
        foreach (u32 jointId, skin->joints) {
            Entity* jointEntity = getEntity(scene, jointId);
            if (jointEntity && jointEntity->name) {
                strmapPut(animator->boneMap, jointEntity->name, jointEntity);
            }
        }
    } else {
        // Non-skinned: map from entity and its subtree
        addEntitySubtreeToBoneMap(animator, animator->entity);
    }
}

static void added(void);
static void update(void);
static void removed(void);

System animationSystem = {
    .name    = "animation",
    .added   = added,
    .update  = update,
    .removed = removed,
};

static void added(void) {
    debug("animationSystem: initialized");
}

static void update(void) {
    float deltaTime = timer.dt;

    // Iterate through all scenes (for future multi-scene system)
    for (size_t s = 0; s < arraySize(ecs.scenes); s++) {
        Scene* scene = ecs.scenes[s];
        if (!scene) continue;

        // 1. Update Animations and apply to local Transform components
        SparseSet* animatorSet = getComponents(scene, Animator);
        if (animatorSet) {
            for (u32 i = 0; i < animatorSet->size; i++) {
                Animator* animator  = static_cast<Animator*>(ssGetDataByIndex(animatorSet, i));
                if (!animator || arraySize(animator->activeInstances) == 0) continue;

                // Build bone map lazily from Skin joints
                if (!animator->boneMap && animator->entity) {
                    buildBoneMap(animator, scene);
                }

                // Save last transforms for the entire skeleton hierarchy.
                // The animator entity (skinned mesh) may be a sibling of the
                // bone entities, not their parent.  Walking only the mesh
                // entity's subtree would miss the bones, leaving their
                // LastTransform stale and causing jittery joint interpolation
                // at frame-rates above the fixed timestep.
                //
                // Strategy: find the skeleton root (common ancestor of the
                // animator entity and all joint entities) and activate that
                // entire subtree so every bone gets its LastTransform saved.
                if (animator->entity) {
                    if (!animator->skeletonRoot) {
                        Entity* subtreeRoot = animator->entity;
                        Skin* skin          = getComponent(scene, Skin, animator->entity->id);
                        if (skin && arraySize(skin->joints) > 0) {
                            Entity* firstJoint = getEntity(scene, skin->joints[0]);
                            if (firstJoint) {
                                // Walk up from the first joint to find the
                                // deepest ancestor shared with the animator
                                // entity (usually the armature node).
                                Entity* candidate = firstJoint;
                                while (candidate->parent) {
                                    candidate = candidate->parent;
                                    Entity* p = animator->entity;
                                    while (p) {
                                        if (p == candidate) goto found;
                                        p = p->parent;
                                    }
                                }
                            found:
                                subtreeRoot = candidate;
                            }
                        }
                        animator->skeletonRoot = subtreeRoot;
                    }
                    transformActivateAndSaveLastSubtree(scene, animator->skeletonRoot->id);
                }

                // Process each active animation instance
                for (size_t j = 0; j < arraySize(animator->activeInstances); j++) {
                    AnimationInstance* instance = animator->activeInstances[j];
                    if (!instance || instance->markedForRemoval) continue;

                    // Update blend weights
                    updateBlending(instance, deltaTime);

                    // Skip if weight is negligible
                    if (instance->weight < 0.001f) {
                        instance->markedForRemoval = true;
                        continue;
                    }

                    // Update animation time and events
                    float oldTime = instance->currentTime;
                    instance->currentTime += deltaTime * instance->speed;

                    // Handle looping.  Negative speed is supported for reversed
                    // playback, so loop both ends of the clip range.
                    if (instance->loop && instance->clip->duration > 0.0f &&
                        (instance->currentTime > instance->clip->duration ||
                         instance->currentTime < 0.0f)) {
                        instance->currentTime =
                            fmodf(instance->currentTime, instance->clip->duration);
                        if (instance->currentTime < 0.0f) {
                            instance->currentTime += instance->clip->duration;
                        }
                        // Reset keyframe hints so the hint-based fast path
                        // doesn't stall after time wraps back to near zero.
                        for (size_t k = 0; k < arraySize(instance->clip->channels); k++) {
                            AnimationChannel* ch = &instance->clip->channels[k];
                            ch->lastPosKeyIndex   = 0;
                            ch->lastRotKeyIndex   = 0;
                            ch->lastScaleKeyIndex = 0;
                        }
                    } else if (!instance->loop) {
                        if (instance->currentTime > instance->clip->duration) {
                            instance->currentTime = instance->clip->duration;
                        } else if (instance->currentTime < 0.0f) {
                            instance->currentTime = 0.0f;
                        }
                    }

                    // Update events
                    updateEvents(instance, oldTime, instance->currentTime);
                }

                // Blend all active clips from the same saved local-pose base.
                // Applying clips sequentially on top of already-modified local transforms
                // makes the result depend on update order and causes visible shakiness during
                // crossfades / partial blends.
                struct ChannelAccum {
                    u32 entityId;
                    Transform baseTransform;
                    Transform accumPosition;
                    Transform accumRotation;
                    Transform accumScale;
                    float positionWeight;
                    float rotationWeight;
                    float scaleWeight;
                    bool initialized;
                };

                Array(ChannelAccum) accumulators = {};

                for (size_t j = 0; j < arraySize(animator->activeInstances); j++) {
                    AnimationInstance* instance = animator->activeInstances[j];
                    if (!instance || instance->markedForRemoval || instance->weight < 0.001f) {
                        continue;
                    }

                    // Build/update channel entity resolution cache
                    if (animator->cachedClip != instance->clip) {
                        arrayClear(animator->cachedChannelEntities);
                        for (size_t k = 0; k < arraySize(instance->clip->channels); k++) {
                            AnimationChannel* ch = &instance->clip->channels[k];
                            Entity* ent          = strmapGet(animator->boneMap, ch->jointName.data);
                            arrayPut(animator->cachedChannelEntities, ent ? ent->id : 0);
                        }
                        animator->cachedClip = instance->clip;
                    }

                    for (size_t k = 0; k < arraySize(instance->clip->channels); k++) {
                        AnimationChannel* channel = &instance->clip->channels[k];
                        u32 targetJointEntity     = animator->cachedChannelEntities[k];
                        if (targetJointEntity == 0) {
                            continue;
                        }

                        Transform* localTransform =
                            getComponent(scene, Transform, targetJointEntity);
                        if (!localTransform) {
                            continue;
                        }

                        ChannelAccum* accum = nullptr;
                        for (size_t a = 0; a < arraySize(accumulators); a++) {
                            if (accumulators[a].entityId == targetJointEntity) {
                                accum = &accumulators[a];
                                break;
                            }
                        }
                        if (!accum) {
                            ChannelAccum init = {};
                            init.entityId     = targetJointEntity;
                            transformCopy(&init.baseTransform, localTransform);
                            transformCopy(&init.accumPosition, localTransform);
                            transformCopy(&init.accumRotation, localTransform);
                            transformCopy(&init.accumScale, localTransform);
                            arrayPut(accumulators, init);
                            accum = &accumulators[arraySize(accumulators) - 1];
                        }

                        Transform sampledTransform;
                        transformCopy(&sampledTransform, &accum->baseTransform);
                        sampleChannelToTransform(channel, instance->currentTime, instance->loop, instance->clip->duration, &sampledTransform);

                        if (arraySize(channel->positionKeys) > 0) {
                            float totalWeight = accum->positionWeight + instance->weight;
                            float mixWeight = totalWeight > 0.0f ? (instance->weight / totalWeight)
                                                                 : 0.0f;
                            glm_vec3_lerp(accum->accumPosition.pos,
                                          sampledTransform.pos,
                                          mixWeight,
                                          accum->accumPosition.pos);
                            accum->positionWeight = totalWeight;
                            accum->initialized    = true;
                        }

                        if (arraySize(channel->rotationKeys) > 0) {
                            float totalWeight = accum->rotationWeight + instance->weight;
                            float mixWeight = totalWeight > 0.0f ? (instance->weight / totalWeight)
                                                                 : 0.0f;
                            quatSlerpShortest(accum->accumRotation.rot,
                                              sampledTransform.rot,
                                              mixWeight,
                                              accum->accumRotation.rot);
                            glm_quat_normalize(accum->accumRotation.rot);
                            accum->rotationWeight = totalWeight;
                            accum->initialized    = true;
                        }

                        if (arraySize(channel->scaleKeys) > 0) {
                            float totalWeight = accum->scaleWeight + instance->weight;
                            float mixWeight = totalWeight > 0.0f ? (instance->weight / totalWeight)
                                                                 : 0.0f;
                            accum->accumScale.pos[3] = glm_lerp(accum->accumScale.pos[3],
                                                                sampledTransform.pos[3],
                                                                mixWeight);
                            accum->scaleWeight       = totalWeight;
                            accum->initialized       = true;
                        }
                    }
                }

                for (size_t a = 0; a < arraySize(accumulators); a++) {
                    ChannelAccum* accum = &accumulators[a];
                    if (!accum->initialized) {
                        continue;
                    }

                    Transform finalTransform;
                    transformCopy(&finalTransform, &accum->baseTransform);

                    if (accum->positionWeight > 0.0f) {
                        glm_vec3_copy(accum->accumPosition.pos, finalTransform.pos);
                    }
                    if (accum->rotationWeight > 0.0f) {
                        glm_vec4_copy(accum->accumRotation.rot, finalTransform.rot);
                    }
                    if (accum->scaleWeight > 0.0f) {
                        finalTransform.pos[3] = accum->accumScale.pos[3];
                    }

                    Transform* localTransform = getComponent(scene, Transform, accum->entityId);
                    if (localTransform) {
                        transformCopy(localTransform, &finalTransform);
                    }
                }

                arrayFree(accumulators);

                // Remove marked instances
                for (i32 j = arraySize(animator->activeInstances) - 1; j >= 0; j--) {
                    AnimationInstance* instance = animator->activeInstances[j];
                    if (instance && instance->markedForRemoval) {
                        arrayDeleteSlow(animator->activeInstances, j);
                        memoryFree(instance);
                    }
                }
            }
        }

        // Note: Skin joint transforms are now computed directly in vulkanSceneFlushJoints()
        // after the TransformSystem has propagated world transforms down the hierarchy.
    }
}

static void removed(void) {
    // Free all active animation instances in all scenes
    foreach (auto scene, ecs.scenes) {
        SparseSet* animators = getComponents(scene, Animator);
        if (!animators) continue;
        for (u32 i = 0, si = animators->size; i < si; i++) {
            Animator* animator  = static_cast<Animator*>(ssGetDataByIndex(animators, i));
            foreach (auto instance, animator->activeInstances) {
                arrayFree(instance->eventCallbacks);
                memoryFree(instance);
            }
            arrayFree(animator->activeInstances);
            strmapFree(animator->boneMap);
            arrayFree(animator->cachedChannelEntities);
        }
    }

    // Free animation library
    foreach (auto clip, animationLibrary.clips) {
        stringDestroy(&clip->name);
        foreach (auto channel, clip->channels) {
            stringDestroy(&channel.jointName);
            arrayFree(channel.positionKeys);
            arrayFree(channel.rotationKeys);
            arrayFree(channel.scaleKeys);
        }
        arrayFree(clip->channels);
        foreach (auto event, clip->events) {
            stringDestroy(&event.name);
        }
        arrayFree(clip->events);
        memoryFree(clip);
    }
    arrayFree(animationLibrary.clips);

    info("animationSystem: shutdown");
}

/*
 * =====================================================
 * PUBLIC API IMPLEMENTATION
 * =====================================================
 */

AnimationClip* animationGetOrCreate(const char* name) {
    // Check if already exists
    for (size_t i = 0; i < arraySize(animationLibrary.clips); i++) {
        if (strequals(animationLibrary.clips[i]->name.data, name)) {
            return animationLibrary.clips[i];
        }
    }

    // Create new clip
    AnimationClip* clip = static_cast<AnimationClip*>(memoryAlloc(sizeof(AnimationClip)));
    memset(clip, 0, sizeof(AnimationClip));
    stringPrintf(&clip->name, name);
    clip->duration = 0.0f;

    arrayPut(animationLibrary.clips, clip);

    return clip;
}

AnimationClip* animationGet(const char* name) {
    for (size_t i = 0; i < arraySize(animationLibrary.clips); i++) {
        if (strequals(animationLibrary.clips[i]->name.data, name)) {
            return animationLibrary.clips[i];
        }
    }
    return nullptr;
}

AnimationClip* animationGetFirst(void) {
    if (arraySize(animationLibrary.clips) == 0) {
        return nullptr;
    }
    return animationLibrary.clips[0];
}

void animationPlay(Entity* entity, const char* clipName, float speed, bool loop) {
    Scene* scene = entity->scene;

    // Get or create animator component
    Animator* animator = getComponent(scene, Animator, entity->id);
    if (!animator) {
        animator          = createComponent(scene, Animator, entity->id);
        animator->entity  = entity;
        animator->mapping = nullptr;
    }

    // Get animation clip
    AnimationClip* clip = animationGet(clipName);
    if (!clip) {
        warn("animationPlay: clip '%s' not found", clipName);
        return;
    }

    // Stop all current animations
    animationStopAll(entity);

    // Create new instance
    AnimationInstance* instance = static_cast<AnimationInstance*>(memoryAlloc(sizeof(AnimationInstance)));
    memset(instance, 0, sizeof(AnimationInstance));
    instance->clip              = clip;
    instance->currentTime       = 0.0f;
    instance->speed             = speed;
    instance->loop              = loop;
    instance->weight            = 1.0f;
    instance->blendWeightStart  = 1.0f;
    instance->blendWeightTarget = 1.0f;
    instance->blendDuration     = 0.0f;
    instance->blendElapsed      = 0.0f;
    instance->markedForRemoval  = false;

    arrayPut(animator->activeInstances, instance);
}

void animationPlayBlended(Entity* entity,
                          const char* clipName,
                          float speed,
                          bool loop,
                          float blendDuration) {
    Scene* scene = entity->scene;

    Animator* animator = getComponent(scene, Animator, entity->id);
    if (!animator) {
        animator          = createComponent(scene, Animator, entity->id);
        animator->entity  = entity;
        animator->mapping = nullptr;
    }

    AnimationClip* clip = animationGet(clipName);
    if (!clip) {
        warn("animationPlayBlended: clip '%s' not found", clipName);
        return;
    }

    // Fade out existing animations
    for (size_t i = 0; i < arraySize(animator->activeInstances); i++) {
        AnimationInstance* instance = animator->activeInstances[i];
        if (instance) {
            instance->blendWeightStart  = instance->weight;
            instance->blendWeightTarget = 0.0f;
            instance->blendDuration     = blendDuration;
            instance->blendElapsed      = 0.0f;
        }
    }

    // Create and fade in new animation
    AnimationInstance* instance = static_cast<AnimationInstance*>(memoryAlloc(sizeof(AnimationInstance)));
    memset(instance, 0, sizeof(AnimationInstance));
    instance->clip              = clip;
    instance->currentTime       = 0.0f;
    instance->speed             = speed;
    instance->loop              = loop;
    instance->weight            = 0.0f;
    instance->blendWeightStart  = 0.0f;
    instance->blendWeightTarget = 1.0f;
    instance->blendDuration     = blendDuration;
    instance->blendElapsed      = 0.0f;
    instance->markedForRemoval  = false;

    arrayPut(animator->activeInstances, instance);
}

void animationCrossFade(Entity* entity, const char* fromClip, const char* toClip, float duration) {
    Scene* scene = entity->scene;

    Animator* animator = getComponent(scene, Animator, entity->id);
    if (!animator) {
        warn("animationCrossFade: entity has no animator");
        return;
    }

    // Find and fade out "from" animation
    for (size_t i = 0; i < arraySize(animator->activeInstances); i++) {
        AnimationInstance* instance = animator->activeInstances[i];
        if (instance && strequals(instance->clip->name.data, fromClip)) {
            instance->blendWeightStart  = instance->weight;
            instance->blendWeightTarget = 0.0f;
            instance->blendDuration     = duration;
            instance->blendElapsed      = 0.0f;
            break;
        }
    }

    // Find or create "to" animation
    AnimationInstance* toInstance = nullptr;
    for (size_t i = 0; i < arraySize(animator->activeInstances); i++) {
        AnimationInstance* instance = animator->activeInstances[i];
        if (instance && strequals(instance->clip->name.data, toClip)) {
            toInstance = instance;
            break;
        }
    }

    if (!toInstance) {
        AnimationClip* clip = animationGet(toClip);
        if (!clip) {
            warn("animationCrossFade: toClip '%s' not found", toClip);
            return;
        }

        toInstance  = static_cast<AnimationInstance*>(memoryAlloc(sizeof(AnimationInstance)));
        memset(toInstance, 0, sizeof(AnimationInstance));
        toInstance->clip             = clip;
        toInstance->currentTime      = 0.0f;
        toInstance->speed            = 1.0f;
        toInstance->loop             = true;
        toInstance->weight           = 0.0f;
        toInstance->blendDuration    = duration;
        toInstance->blendElapsed     = 0.0f;
        toInstance->markedForRemoval = false;

        arrayPut(animator->activeInstances, toInstance);
    }

    // Fade in "to" animation
    toInstance->blendWeightStart  = toInstance->weight;
    toInstance->blendWeightTarget = 1.0f;
    toInstance->blendDuration     = duration;
    toInstance->blendElapsed      = 0.0f;
}

void animationStop(Entity* entity, const char* clipName) {
    Animator* animator = getComponent(entity->scene, Animator, entity->id);
    if (!animator) return;

    for (i32 i = arraySize(animator->activeInstances) - 1; i >= 0; i--) {
        AnimationInstance* instance = animator->activeInstances[i];
        if (instance && strequals(instance->clip->name.data, clipName)) {
            arrayDeleteSlow(animator->activeInstances, i);
            memoryFree(instance);
            break;
        }
    }
}

void animationStopAll(Entity* entity) {
    Animator* animator = getComponent(entity->scene, Animator, entity->id);
    if (!animator) return;

    // Free all animation instances before clearing the array
    for (size_t i = 0; i < arraySize(animator->activeInstances); i++) {
        AnimationInstance* instance = animator->activeInstances[i];
        if (instance) {
            arrayFree(instance->eventCallbacks);
            memoryFree(instance);
        }
    }
    arrayClear(animator->activeInstances);
}

void animationSetSpeed(Entity* entity, const char* clipName, float speed) {
    Animator* animator = getComponent(entity->scene, Animator, entity->id);
    if (!animator) return;

    for (size_t i = 0; i < arraySize(animator->activeInstances); i++) {
        AnimationInstance* instance = animator->activeInstances[i];
        if (instance && strequals(instance->clip->name.data, clipName)) {
            instance->speed = speed;
            break;
        }
    }
}

bool animationIsPlaying(Entity* entity, const char* clipName) {
    Animator* animator = getComponent(entity->scene, Animator, entity->id);
    if (!animator) return false;

    for (size_t i = 0; i < arraySize(animator->activeInstances); i++) {
        AnimationInstance* instance = animator->activeInstances[i];
        if (instance && !instance->markedForRemoval &&
            strequals(instance->clip->name.data, clipName)) {
            return true;
        }
    }
    return false;
}

bool animationIsFinished(Entity* entity, const char* clipName) {
    Animator* animator = getComponent(entity->scene, Animator, entity->id);
    if (!animator) return false;

    // Search backwards — the most recently added instance is last in the array.
    // Skip instances that are fading out (blendWeightTarget == 0) since those
    // are leftovers from a previous play call.
    for (i32 i = static_cast<i32>(arraySize(animator->activeInstances)) - 1; i >= 0; i--) {
        AnimationInstance* instance = animator->activeInstances[i];
        if (instance && !instance->markedForRemoval && instance->blendWeightTarget > 0.0f &&
            strequals(instance->clip->name.data, clipName)) {
            return !instance->loop && instance->currentTime >= instance->clip->duration;
        }
    }
    return false;
}

void animationRestart(Entity* entity, const char* clipName, float speed) {
    Animator* animator = getComponent(entity->scene, Animator, entity->id);
    if (!animator) return;

    for (i32 i = static_cast<i32>(arraySize(animator->activeInstances)) - 1; i >= 0; i--) {
        AnimationInstance* instance = animator->activeInstances[i];
        if (instance && !instance->markedForRemoval &&
            strequals(instance->clip->name.data, clipName)) {
            instance->currentTime = 0.0f;
            instance->speed       = speed;
            // Reset any active blend so the instance stays at full weight
            instance->weight             = 1.0f;
            instance->blendWeightStart   = 1.0f;
            instance->blendWeightTarget  = 1.0f;
            instance->blendDuration      = 0.0f;
            instance->blendElapsed       = 0.0f;
            return;
        }
    }
}

bool animationIsNearEnd(Entity* entity, const char* clipName, float thresholdSeconds) {
    Animator* animator = getComponent(entity->scene, Animator, entity->id);
    if (!animator) return false;

    for (i32 i = static_cast<i32>(arraySize(animator->activeInstances)) - 1; i >= 0; i--) {
        AnimationInstance* instance = animator->activeInstances[i];
        if (instance && !instance->markedForRemoval && instance->blendWeightTarget > 0.0f &&
            strequals(instance->clip->name.data, clipName)) {
            float remaining = instance->clip->duration - instance->currentTime;
            return remaining >= 0.0f && remaining < thresholdSeconds;
        }
    }
    return false;
}

Entity* animationGetBoneEntity(Entity* entity, const char* boneName) {
    if (!entity || !boneName) return nullptr;
    Animator* animator = getComponent(entity->scene, Animator, entity->id);
    if (!animator || !animator->boneMap) return nullptr;
    return strmapGet(animator->boneMap, boneName);
}

void animationAddEventCallback(const char* clipName,
                               const char* eventName,
                               void (*callback)(void*),
                               void* userData) {
    (void)eventName;
    (void)callback;
    (void)userData;
    AnimationClip* clip = animationGet(clipName);
    if (!clip) {
        warn("animationAddEventCallback: clip '%s' not found", clipName);
        return;
    }

    // Add callback to all instances of this clip
    // Note: This is a simplified approach. A better implementation would
    // store callbacks in the clip and apply them when instances are created
}

void animationRemoveEventCallback(const char* clipName, const char* eventName) {
    (void)clipName;
    (void)eventName;
    // Implementation similar to add
}

void animationSetRemapping(Entity* entity, const JointMapping* mapping) {
    Animator* animator = getComponent(entity->scene, Animator, entity->id);
    if (!animator) {
        animator         = createComponent(entity->scene, Animator, entity->id);
        animator->entity = entity;
    }

    animator->mapping = const_cast<JointMapping*>(mapping);
}

JointMapping* animationCreateJointMapping(const char* sourceSkeleton, const char* targetSkeleton) {
    (void)sourceSkeleton;
    (void)targetSkeleton;
    // TODO: Implement joint name-based mapping
    // This would need access to skeleton joint names
    warn("animationCreateJointMapping: not yet implemented");
    return nullptr;
}

void animationFreeJointMapping(JointMapping* mapping) {
    if (mapping) {
        memoryFree(mapping);
    }
}

void animationSystemInit(void) {
    arraySetSize(animationLibrary.clips, 0);
}

void animationSystemDestroy(void) {
    // Free all animation clips
    for (size_t i = 0; i < arraySize(animationLibrary.clips); i++) {
        AnimationClip* clip = animationLibrary.clips[i];
        if (clip) {
            // Free channels
            for (size_t j = 0; j < arraySize(clip->channels); j++) {
                AnimationChannel* channel = &clip->channels[j];
                stringDestroy(&channel->jointName);
                arrayFree(channel->positionKeys);
                arrayFree(channel->rotationKeys);
                arrayFree(channel->scaleKeys);
            }
            arrayFree(clip->channels);

            // Free events
            for (size_t j = 0; j < arraySize(clip->events); j++) {
                stringDestroy(&clip->events[j].name);
            }
            arrayFree(clip->events);

            stringDestroy(&clip->name);
            memoryFree(clip);
        }
    }
    arrayFree(animationLibrary.clips);
}

/*
 * Name-only convenience API
 */

void animationPlayByName(const char* entityName, const char* clipName, float speed, bool loop) {
    Entity* entity = searchEntity(entityName);
    if (entity) animationPlay(entity, clipName, speed, loop);
}

void animationPlayBlendedByName(const char* entityName,
                                const char* clipName,
                                float speed,
                                bool loop,
                                float blendDuration) {
    Entity* entity = searchEntity(entityName);
    if (entity) animationPlayBlended(entity, clipName, speed, loop, blendDuration);
}

void animationCrossFadeByName(const char* entityName,
                              const char* fromClip,
                              const char* toClip,
                              float duration) {
    Entity* entity = searchEntity(entityName);
    if (entity) animationCrossFade(entity, fromClip, toClip, duration);
}

void animationStopByName(const char* entityName, const char* clipName) {
    Entity* entity = searchEntity(entityName);
    if (entity) animationStop(entity, clipName);
}

void animationStopAllByName(const char* entityName) {
    Entity* entity = searchEntity(entityName);
    if (entity) animationStopAll(entity);
}

bool animationIsPlayingByName(const char* entityName, const char* clipName) {
    Entity* entity = searchEntity(entityName);
    return entity ? animationIsPlaying(entity, clipName) : false;
}

bool animationIsFinishedByName(const char* entityName, const char* clipName) {
    Entity* entity = searchEntity(entityName);
    return entity ? animationIsFinished(entity, clipName) : false;
}
