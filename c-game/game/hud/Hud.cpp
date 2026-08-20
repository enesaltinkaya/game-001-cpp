#include "Utils.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/camera/CameraComponent.h"
#include "ecs/system/transform/TransformComponent.h"
#include "ecs/system/transform/TransformSystem.h"
#include "ecs/system/window/WindowSystem.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "character/CharacterStats.h"
#include "enemy/Enemy.h"
#include "player/Player.h"
#include "hud/Hud.h"

#define DMG_NUM_POOL_SIZE 8
#define DMG_NUM_LIFETIME 1200.0f  // ms
#define ENEMY_HP_POOL_SIZE 16

static void added(void);
static void update(void);
static void removed(void);

System hud = {
    .name                = "hud",
    .added               = added,
    .removed             = removed,
    .preUpdate           = nullptr,
    .update              = update,
    .postUpdate          = nullptr,
    .cpuElapsedLastFrame = 0.0,
    .cpuElapsed          = 0.0,
    .gpuElapsed          = 0.0,
    .priority            = 0,
};

// ── RMLUI ──────────────────────────────────────────

static void* document;
static void* model;

// Bound values
static float hudHp, hudMaxHp, hudHpPercent;
static float hudMana, hudMaxMana, hudManaPercent;
static float hudXpPercent;
static int   hudLevel;

// Damage number pool bindings (8 slots)
static float hudDmgX[DMG_NUM_POOL_SIZE];
static float hudDmgY[DMG_NUM_POOL_SIZE];
static float hudDmgAlpha[DMG_NUM_POOL_SIZE];
static char* hudDmgText[DMG_NUM_POOL_SIZE];
static char* hudDmgColor[DMG_NUM_POOL_SIZE];

static char hudDmgTextBuf[DMG_NUM_POOL_SIZE][32];
static char hudDmgColorBuf[DMG_NUM_POOL_SIZE][32];

// ── Enemy health bar pool ──────────────────────────

struct EnemyHpBar {
    Scene* scene;
    u32    entityId;
    float  screenX, screenY;
    float  hpPercent;
    bool   active;
};

static EnemyHpBar enemyHpPool[ENEMY_HP_POOL_SIZE];

static float  hudEnemyHpX[ENEMY_HP_POOL_SIZE];
static float  hudEnemyHpY[ENEMY_HP_POOL_SIZE];
static float  hudEnemyHpPercent[ENEMY_HP_POOL_SIZE];
static float  hudEnemyHpAlpha[ENEMY_HP_POOL_SIZE];

// Smoothed screen positions to avoid jitter


// ── Damage number pool ─────────────────────────────

struct DamageNumber {
    float   worldX, worldY, worldZ;
    float   screenX, screenY;
    float   value;
    float   vy;           // screen rise speed (px/s)
    double  spawnTime;    // ms timestamp
    bool    active;
};

static DamageNumber dmgPool[DMG_NUM_POOL_SIZE];

static DamageNumber* dmgPoolPush(float wx, float wy, float wz, float value) {
    // Find oldest inactive slot, or evict oldest active
    int slot = -1;
    double oldest = millies();
    for (int i = 0; i < DMG_NUM_POOL_SIZE; i++) {
        if (!dmgPool[i].active) {
            slot = i;
            break;
        }
        if (dmgPool[i].spawnTime < oldest) {
            oldest = dmgPool[i].spawnTime;
            slot = i;
        }
    }
    if (slot < 0) return nullptr;

    DamageNumber* dn = &dmgPool[slot];
    *dn = DamageNumber{
        .worldX  = wx,
        .worldY  = wy,
        .worldZ  = wz,
        .value   = value,
        .vy      = 80.0f + static_cast<float>(randomU32() % 40),  // 80-120 px/s rise
        .spawnTime = millies(),
        .active  = 1,
        .screenX = 0.0f,
        .screenY = 0.0f,
    };
    return dn;
}

// ── World to screen projection ─────────────────────

static bool worldToScreen(float wx, float wy, float wz, float* outX, float* outY) {
    Entity* cam = cameraGetEntity();
    if (!cam || !cam->scene) return false;

    Camera* camera = getComponent(cam->scene, Camera, cam->id);
    if (!camera) return false;

    vec4 pos = {wx, wy, wz, 1.0f};
    vec4 ndc;
    glm_mat4_mulv(camera->cameraUbo.viewProjectionNoJitter, pos, ndc);

    if (ndc[3] <= 0.0f) return false;

    ndc[0] /= ndc[3];
    ndc[1] /= ndc[3];
    ndc[2] /= ndc[3];

    float sx = (ndc[0] * 0.5f + 0.5f) * static_cast<float>(window.width);
    float sy = (1.0f - (ndc[1] * 0.5f + 0.5f)) * static_cast<float>(window.height);

    if (sx < -50 || sx > static_cast<float>(window.width) + 50 ||
        sy < -50 || sy > static_cast<float>(window.height) + 50) {
        return false;
    }

    *outX = sx;
    *outY = sy;
    return true;
}

// ── Public API ──────────────────────────────────────

void hudDamageNumber(float x, float y, float z, float value) {
    static_cast<void>(dmgPoolPush(x, y + 1.0f, z, value));
}

// ── System lifecycle ───────────────────────────────

static void added(void) {
    document = rmlNewDocument("gui/hud/hud.html");
    model    = rmlCreateModel("hud");

    // Bind player stats
    rmlBindFloat(model, "hp", &hudHp);
    rmlBindFloat(model, "maxHp", &hudMaxHp);
    rmlBindFloat(model, "hpPercent", &hudHpPercent);
    rmlBindFloat(model, "mana", &hudMana);
    rmlBindFloat(model, "maxMana", &hudMaxMana);
    rmlBindFloat(model, "manaPercent", &hudManaPercent);
    rmlBindFloat(model, "xpPercent", &hudXpPercent);
    rmlBindInt(model, "level", &hudLevel);

    // Bind damage number pool
    for (int i = 0; i < DMG_NUM_POOL_SIZE; i++) {
        char idxStr[4];
        snprintf(idxStr, sizeof(idxStr), "%d", i);

        char fmtX[8], fmtY[8], fmtAlpha[12], fmtText[10], fmtColor[12];
        snprintf(fmtX, sizeof(fmtX), "dmg%sX", idxStr);
        snprintf(fmtY, sizeof(fmtY), "dmg%sY", idxStr);
        snprintf(fmtAlpha, sizeof(fmtAlpha), "dmg%sAlpha", idxStr);
        snprintf(fmtText, sizeof(fmtText), "dmg%sText", idxStr);
        snprintf(fmtColor, sizeof(fmtColor), "dmg%sColor", idxStr);

        hudDmgX[i] = 0.0f;
        hudDmgY[i] = 0.0f;
        hudDmgAlpha[i] = 0.0f;
        hudDmgText[i] = hudDmgTextBuf[i];
        hudDmgColor[i] = hudDmgColorBuf[i];
        hudDmgTextBuf[i][0] = '\0';
        snprintf(hudDmgColorBuf[i], sizeof(hudDmgColorBuf[i]), "rgba(255,255,255,0)");

        rmlBindFloat(model, fmtX, &hudDmgX[i]);
        rmlBindFloat(model, fmtY, &hudDmgY[i]);
        rmlBindFloat(model, fmtAlpha, &hudDmgAlpha[i]);
        rmlBindCharPointer(model, fmtText, &hudDmgText[i]);
        rmlBindCharPointer(model, fmtColor, &hudDmgColor[i]);
    }

    // Initialize damage number pool
    for (int i = 0; i < DMG_NUM_POOL_SIZE; i++) {
        dmgPool[i].active = 0;
    }

    // Bind enemy health bar pool
    for (int i = 0; i < ENEMY_HP_POOL_SIZE; i++) {
        char idxStr[4];
        snprintf(idxStr, sizeof(idxStr), "%d", i);

        char fmtX[10], fmtY[10], fmtPercent[14], fmtAlpha[14];
        snprintf(fmtX, sizeof(fmtX), "ehp%sX", idxStr);
        snprintf(fmtY, sizeof(fmtY), "ehp%sY", idxStr);
        snprintf(fmtPercent, sizeof(fmtPercent), "ehp%sPercent", idxStr);
        snprintf(fmtAlpha, sizeof(fmtAlpha), "ehp%sAlpha", idxStr);

        hudEnemyHpX[i]        = 0.0f;
        hudEnemyHpY[i]        = 0.0f;
        hudEnemyHpPercent[i]  = 0.0f;
        hudEnemyHpAlpha[i]    = 0.0f;

        rmlBindFloat(model, fmtX, &hudEnemyHpX[i]);
        rmlBindFloat(model, fmtY, &hudEnemyHpY[i]);
        rmlBindFloat(model, fmtPercent, &hudEnemyHpPercent[i]);
        rmlBindFloat(model, fmtAlpha, &hudEnemyHpAlpha[i]);

        enemyHpPool[i].active = 0;
        enemyHpPool[i].entityId = 0;
    }

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);
}

static void removed(void) {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = nullptr;
    model = nullptr;
}

static void update(void) {
    // ── Update player stats ─────────────────────────
    Scene* playerScene = getPlayerScene();
    if (playerScene) {
        SparseSet* players = getComponents(playerScene, Player);
        if (players->size > 0) {
            u32 playerId = ssGetValueByIndex(players, 0);
            CharacterStats* stats = getComponent(playerScene, CharacterStats, playerId);
            if (stats) {
                hudHp     = stats->hp;
                hudMaxHp  = stats->maxHp;
                hudHpPercent = stats->maxHp > 0.0f ? stats->hp / stats->maxHp : 0.0f;
                hudMana     = stats->mana;
                hudMaxMana  = stats->maxMana;
                hudManaPercent = stats->maxMana > 0.0f ? stats->mana / stats->maxMana : 0.0f;
                hudXpPercent = stats->xpToNext > 0.0f ? stats->xp / stats->xpToNext : 0.0f;
                hudLevel    = stats->level;
            }
        }
    }

    // ── Update damage numbers ───────────────────────
    double now = millies();
    for (int i = 0; i < DMG_NUM_POOL_SIZE; i++) {
        DamageNumber* dn = &dmgPool[i];
        if (!dn->active) {
            hudDmgAlpha[i] = 0.0f;
            hudDmgTextBuf[i][0] = '\0';
            continue;
        }

        float elapsed = (now - dn->spawnTime) / 1000.0f;  // seconds
        float lifetime = DMG_NUM_LIFETIME / 1000.0f;
        float t = elapsed / lifetime;

        if (t >= 1.0f) {
            dn->active = 0;
            hudDmgAlpha[i] = 0.0f;
            hudDmgTextBuf[i][0] = '\0';
            continue;
        }

        // Project world -> screen each frame
        // NOTE: damage numbers use fixed world coords (spawned at hit time),
        // so no interpolation needed — they stay at the impact point.
        if (!worldToScreen(dn->worldX, dn->worldY, dn->worldZ, &dn->screenX, &dn->screenY)) {
            dn->active = 0;
            hudDmgAlpha[i] = 0.0f;
            hudDmgTextBuf[i][0] = '\0';
            continue;
        }

        // Apply rise offset
        float rise = dn->vy * elapsed;
        hudDmgX[i] = dn->screenX - 20.0f;  // center offset
        hudDmgY[i] = dn->screenY - rise;

        // Fade: quick in, slow out
        hudDmgAlpha[i] = t < 0.1f ? t / 0.1f : 1.0f - ((t - 0.1f) / 0.9f);
        if (hudDmgAlpha[i] < 0.0f) hudDmgAlpha[i] = 0.0f;

        // Text
        snprintf(hudDmgTextBuf[i], sizeof(hudDmgTextBuf[i]), "%d", (int)(dn->value));

        // Color: positive = damage dealt (white/yellow), negative = damage received (red)
        if (dn->value > 0) {
            snprintf(hudDmgColorBuf[i], sizeof(hudDmgColorBuf[i]),
                     "rgba(255,255,200,%d)", (int)(hudDmgAlpha[i] * 255));
        } else {
            snprintf(hudDmgColorBuf[i], sizeof(hudDmgColorBuf[i]),
                     "rgba(255,80,80,%d)", (int)(hudDmgAlpha[i] * 255));
        }
    }

    // ── Update enemy health bars ────────────────────
    int hpSlot = 0;
    u32 numScenes = arraySize(ecs.scenes);
    for (u32 si = 0; si < numScenes && hpSlot < ENEMY_HP_POOL_SIZE; si++) {
        Scene* scene = ecs.scenes[si];
        if (!scene || !scene->ready) continue;
        SparseSet* enemies = getComponents(scene, Enemy);
        if (!enemies || enemies->size == 0) continue;
        for (u32 ei = 0; ei < enemies->size && hpSlot < ENEMY_HP_POOL_SIZE; ei++) {
            u32 eId = ssGetValueByIndex(enemies, ei);
            Enemy* enemy = (Enemy*)ssGetDataByIndex(enemies, ei);
            if (!enemy || enemy->state == ENEMY_STATE_DEAD) continue;
            CharacterStats* stats = getComponent(scene, CharacterStats, eId);
            if (!stats || stats->isDead) continue;
            Transform* eT = getComponent(scene, Transform, eId);
            if (!eT) continue;

            // Place bar above the enemy's head based on capsule height
            float height = enemy->capsuleHalfHeight * 2.0f + enemy->capsuleRadius * 2.0f;
            float headY = eT->pos[1] + height + 0.3f;
            float sx, sy;
            if (!worldToScreen(eT->pos[0], headY, eT->pos[2], &sx, &sy)) {
                continue;  // behind camera or out of bounds
            }

            EnemyHpBar* hb = &enemyHpPool[hpSlot];
            hb->scene     = scene;
            hb->entityId  = eId;
            hb->screenX   = sx;
            hb->screenY   = sy;
            hb->hpPercent = stats->maxHp > 0.0f ? stats->hp / stats->maxHp : 0.0f;
            hb->active    = 1;

            hudEnemyHpX[hpSlot] = sx - 30.0f;  // center (bar is 60px wide)
            hudEnemyHpY[hpSlot] = sy;
            hudEnemyHpPercent[hpSlot] = hb->hpPercent;
            hudEnemyHpAlpha[hpSlot]   = 1.0f;
            hpSlot++;
        }
    }

    // Hide unused slots
    for (int i = hpSlot; i < ENEMY_HP_POOL_SIZE; i++) {
        enemyHpPool[i].active   = 0;
        hudEnemyHpAlpha[i] = 0.0f;
    }

    rmlUpdateDirtyAll(model);
}
