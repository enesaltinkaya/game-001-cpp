#include "ecs/system/transform/TransformSystem.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/mesh/MeshComponent.h"
#include "ecs/system/transform/TransformDb.h"
#include <unordered_map>
#include "renderer/Renderer.h"
#include "thread/Thread.h"
#include "timer/Timer.h"

// entities that had their transforms modified
// interpolated and uploaded to gpu
// considered active for 2 seconds
namespace engine {
static struct utils::ThreadPool* threadPool;


static void transformSetWorldWithChildren(Scene* scene, u32 entity);
static void transformSetWorld(Scene* scene, u32 entity, Entity* entityObj);

TransformSystem transformSystem;

TransformSystem::TransformSystem() : System("transform") {}

void TransformSystem::added() {
    threadPool = utils::threadPoolInit(10);
    transformDbInit();
}

void TransformSystem::removed() {
    utils::threadPoolDestroy(threadPool);
}

void TransformSystem::update() {
    {
        for (auto scene : ecs.scenes) {
            for (const auto& entry : scene->activeEntities) {
                transformSetWorldWithChildren(scene, entry.first);
            }
        }
    }
}

void TransformSystem::postUpdate() {
for (auto scene : ecs.scenes) {
            for (const auto& entry : scene->activeEntities) {
                u32 entity  = entry.first;
                double time = entry.second;

            if (utils::millies() < time + 2000) {
                LastTransform* lastTransform   = getComponent(scene, LastTransform, entity);
                WorldTransform* worldTransform = transformGetWorld(scene, entity);

                if (lastTransform && worldTransform) {
                    Transform lerp = {};
                    quatSlerpShortest(lastTransform->rot,
                                      worldTransform->rot,
                                      utils::timer.alpha,
                                      lerp.rot);
                    glm_vec3_lerp(lastTransform->pos, worldTransform->pos, utils::timer.alpha, lerp.pos);
                    lerp.pos[3] =
                        glm_lerp(lastTransform->pos[3], worldTransform->pos[3], utils::timer.alpha);
                    rendererUploadTransform(scene, entity, &lerp);
                } else if (worldTransform) {
                    rendererUploadTransform(scene, entity, reinterpret_cast<Transform*>(worldTransform));
                }
            } else {
                scene->activeEntityRemoveList.push_back(entity);
            }
        }

        for (auto entity : scene->activeEntityRemoveList) {
            scene->activeEntities.erase(entity);
        }
        scene->activeEntityRemoveList.clear();
    }
}

static void transformActivateChildren(Scene* scene, u32 entity) {
    Entity* e = getEntity(scene, entity);
    if (e && !e->children.empty()) {
        for (Entity* child : e->children) {
            scene->activeEntities[child->id] = utils::millies();
            transformActivateChildren(scene, child->id);
        }
    }
}

void transformActivate(Scene* scene, u32 entity) {
    {
        Entity* e = getEntity(scene, entity);
        scene->activeEntities[entity] = utils::millies();
        if (e) {
            if (e->parent) {
                transformActivate(scene, e->parent->id);
            }
            // Also activate children so their transforms get uploaded to the GPU
            if (!e->children.empty()) {
                for (Entity* child : e->children) {
                    scene->activeEntities[child->id] = utils::millies();
                    transformActivateChildren(scene, child->id);
                }
            }
        }
    }
}

void transformSetWorldWithChildren(Scene* scene, u32 entity) {
    Entity* e = getEntity(scene, entity);
    transformSetWorld(scene, entity, e);

    if (e && !e->children.empty()) {
        for (Entity* child : e->children) {
            transformSetWorldWithChildren(scene, child->id);
        }
    }
}

static void transformAABB(vec3* local, WorldTransform* worldTransform, vec3* world) {
    for (int i = 0; i < 3; ++i) {
        float scale      = worldTransform->pos[3];
        float scaled_min = local[0][i] * scale;
        float scaled_max = local[1][i] * scale;
        if (scaled_min < scaled_max) {
            world[0][i] = scaled_min + worldTransform->pos[i];
            world[1][i] = scaled_max + worldTransform->pos[i];
        } else {
            world[0][i] = scaled_max + worldTransform->pos[i];
            world[1][i] = scaled_min + worldTransform->pos[i];
        }
    }
}

void transformSetWorld(Scene* scene, u32 entity, Entity* entityObj) {
    Transform* transform           = getComponent(scene, Transform, entity);
    WorldTransform* worldTransform = transformGetWorld(scene, entity);

    if (entityObj && entityObj->parent) {
        assert(getComponent(scene, WorldTransform, entity) == worldTransform && "unpossible!");

        WorldTransform* parentWorldTransform = transformGetWorld(scene, entityObj->parent->id);
        glm_quat_mul(parentWorldTransform->rot, transform->rot, worldTransform->rot);
        glm_quat_normalize(worldTransform->rot);

        float parentScale = parentWorldTransform->pos[3];

        vec3 scaled_child_local_pos;
        // Child translation is affected by the parent's scale only.
        // Applying the child's own scale here makes parented animated nodes
        // drift/move too far when their local scale changes.
        glm_vec3_scale(transform->pos, parentScale, scaled_child_local_pos);
        vec3 rotated_scaled_child_local_pos;
        // Rotate this scaled local position by parent's rotation
        glm_quat_rotatev(parentWorldTransform->rot,
                         scaled_child_local_pos,
                         rotated_scaled_child_local_pos);
        glm_vec3_add(parentWorldTransform->pos,
                     rotated_scaled_child_local_pos,
                     worldTransform->pos);
        worldTransform->pos[3] = parentWorldTransform->pos[3] * transform->pos[3];
    }

    Mesh* mesh = getComponent(scene, Mesh, entity);
    if (mesh) {
        transformAABB(mesh->aabbLocal, worldTransform, mesh->aabbWorld);
    }
}

WorldTransform* transformGetWorld(Scene* scene, u32 entity) {
    WorldTransform* worldTransform = getComponent(scene, WorldTransform, entity);
    if (worldTransform) {
        return worldTransform;
    }
    Transform* transform = getComponent(scene, Transform, entity);
    return reinterpret_cast<WorldTransform*>(transform);
}

void transformSaveLast(Scene* scene, u32 entity) {
    transformActivate(scene, entity);
    Entity* e = getEntity(scene, entity);
    if (e && !e->children.empty()) {
        for (Entity* child : e->children) {
            transformSaveLast(scene, child->id);
        }
    }

    LastTransform* pt = getComponent(scene, LastTransform, entity);
    if (!pt) {
        pt = createComponent(scene, LastTransform, entity);
    }

    WorldTransform* wt = transformGetWorld(scene, entity);
    glm_vec4_copy(wt->rot, pt->rot);
    glm_vec4_copy(wt->pos, pt->pos);
}

/*
 * Optimized subtree activation + save-last in a single recursive pass.
 * Unlike transformSaveLast which calls transformActivate (walks up to parents
 * and down to all children) for EVERY node, this does:
 *   1. One upward walk to activate parents (only for the root)
 *   2. One downward pass that activates + saves for each node
 */
static void activateAndSaveLastRecurse(Scene* scene, u32 entity) {
    scene->activeEntities[entity] = utils::millies();

    LastTransform* pt = getComponent(scene, LastTransform, entity);
    if (!pt) {
        pt = createComponent(scene, LastTransform, entity);
    }
    WorldTransform* wt = transformGetWorld(scene, entity);
    glm_vec4_copy(wt->rot, pt->rot);
    glm_vec4_copy(wt->pos, pt->pos);

    Entity* e = getEntity(scene, entity);
    if (e && !e->children.empty()) {
        for (Entity* child : e->children) {
            activateAndSaveLastRecurse(scene, child->id);
        }
    }
}

void transformActivateAndSaveLastSubtree(Scene* scene, u32 entity) {
    // Activate parents once (walk up)
    Entity* e = getEntity(scene, entity);
    if (e && e->parent) {
        // Walk up and activate parents only (no child expansion)
        Entity* p = e->parent;
        while (p) {
            scene->activeEntities[p->id] = utils::millies();
            p = p->parent;
        }
    }
    // Single downward pass: activate + save last for entire subtree
    activateAndSaveLastRecurse(scene, entity);
}

void transformGetDirection(Scene* scene, u32 entity, vec3 out) {
    static vec3 FORWARD = {0.0f, 0.0f, -1.0f};  // GLM_FORWARD
    WorldTransform* worldTransform = transformGetWorld(scene, entity);
    glm_quat_rotatev(worldTransform->rot, FORWARD, out);
    glm_vec3_normalize(out);
}

void transformQuatToPitchYaw(versor q, float* pitch, float* yaw) {
    versor q_norm;
    glm_quat_copy(q, q_norm);
    glm_quat_normalize(q_norm);
    vec3 local_forward_model_space = {0.0F, 0.0F, -1.0F};
    vec3 world_forward;

    glm_quat_rotatev(q_norm, local_forward_model_space, world_forward);
    glm_vec3_normalize(world_forward);
    // if i negate yaw, it looks correct
    *yaw   = -atan2f(world_forward[0], -world_forward[2]);
    *pitch = asinf(world_forward[1]);
}

}  // namespace engine
