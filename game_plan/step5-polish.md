# Step 5 — Polish & Content Pipeline

**Order:** After Steps 0-4 are functional.
**Effort:** Ongoing (this is where the game gets *good*)
**Prerequisites:** All previous steps working.

---

## Goal

Make the game feel polished and build tools that let content people add new stuff without
touching code. This is the difference between "it works" and "it's fun."

---

## Task 5.0 — VFX System (2-3 days)

Particle effects for ability impacts, loot spawns, level ups, area transitions.

### Component: `VFXEmitter`

```c
typedef struct {
    u32   prefabId;         // references VFX prefab
    vec3  position;
    vec3  scale;
    f32   lifetime;
    f32   lifetimeRemaining;
    u8    worldSpace;       // vs attached to entity bone
} VFXEmitter;

REGISTER_COMPONENT(VFXEmitter);
```

### VFX Prefab Format: `vfx_prefabs.json`

```json
{
  "id": 1,
  "name": "fire_impact",
  "particles": [
    {
      "emitterType": "sphere",
      "emissionRate": 50,
      "lifetime": 0.5,
      "speed": {"min": 2.0, "max": 5.0},
      "size": {"start": 0.3, "end": 0.0},
      "color": {"start": [1.0, 0.5, 0.0], "end": [1.0, 0.0, 0.0]},
      "gravity": -5.0,
      "texture": "textures/particles/fire.png"
    }
  ]
}
```

### Integration

- `executeAbility()` spawns VFX at hit location
- `groundLootSpawn()` spawns sparkle VFX
- `levelUp()` spawns celebration VFX around player

### Files

| File | Purpose |
|------|---------|
| `c-game/game/vfx/VFXEmitter.h` | Component, REGISTER_COMPONENT |
| `c-game/game/vfx/VFXSystem.h/.c` | Prefab loader, emitter lifecycle |
| `c-game/data/pak_1/vfx/vfx_prefabs.json` | VFX definitions |

---

## Task 5.1 — Screen Shake & Hit Stop (1 day)

Juice — make combat feel impactful.

### Screen Shake

In `CameraSystem` (or `IsoCameraSystem`):
```c
typedef struct {
    f32  intensity;
    f32  duration;
    f32  elapsed;
    vec3 offset;
} ScreenShake;

void cameraAddShake(f32 intensity, f32 duration);
// Each frame:
// if (shake.elapsed < shake.duration) {
//     cameraOffset = randomInSphere(shake.intensity * (1 - elapsed/duration));
//     shake.elapsed += dt;
// }
```

Triggered by:
- Player takes heavy damage → shake
- Boss slam ability → strong shake
- Player casts AoE → moderate shake

### Hit Stop (Frame Freeze)

In `Engine.h` or game loop:
```c
static f32 hitStopRemaining = 0.0f;

void triggerHitStop(f32 duration) {
    hitStopRemaining = duration;
}

// In game loop update:
if (hitStopRemaining > 0) {
    hitStopRemaining -= dt;
    // Skip game logic this frame (visuals still render)
    return;
}
```

Triggered by:
- Critical hit → 50-100ms freeze
- Boss phase transition → 200ms freeze
- Boss death → 300ms freeze

### Files

| File | Purpose |
|------|---------|
| `c-game/game/juice/ScreenShake.h/.c` | Shake accumulator, camera integration |
| `c-game/game/juice/HitStop.h/.c` | Frame freeze, trigger API |

---

## Task 5.2 — Minimap (1-2 days)

Top-down radar showing enemies, portals, and player position.

### RMLUI Component

Top-right corner, circular or square overlay:
- Player dot (white, center or positioned by area-relative location)
- Enemy dots (red, directional from player, clamped to minimap bounds)
- Portal dots (blue)
- Loot dots (yellow)

### Data Source

In `HudSystem` or dedicated `MinimapSystem`:
```c
typedef struct {
    RmlUi::Core::ElementDocument *document;
    Element *playerDot;
    Element *enemyDots[64];
    Element *portalDots[8];
    Element *lootDots[32];
} MinimapComponent;
```

Each frame, update dot positions based on world position → minimap projection.

### Files

| File | Purpose |
|------|---------|
| `c-game/game/hud/Minimap.h` | `MinimapComponent` |
| `c-game/game/hud/Minimap.c` | Dot positioning, entity tracking |
| `c-game/data/pak_1/gui/minimap/minimap.html` | RMLUI layout |

---

## Task 5.3 — Content Pipeline Tools (3-5 days)

Scripts and tools to let non-programmers add content.

### Asset Pipeline

1. **Item CSV Validator** — Python script that validates `items.csv`:
   - Required columns present
   - IDs unique
   - References exist (icons, models)
   - Stats are valid numbers

2. **Drop Table Validator** — validates `drops.csv`:
   - Enemy IDs exist in `enemy_types.csv`
   - Item IDs exist in `items.csv`
   - Probabilities in [0, 1]

3. **Encounter Validator** — validates `encounters.csv`:
   - Area IDs exist
   - Enemy type IDs exist
   - Counts, distances are valid

4. **Batch CSV → JSON** — Convert CSVs to game-loadable binary/JSON format at build time

### Blender Export Scripts

Extend existing `scripts/blender_export_*.py` to:
- Export enemy animations with standardized clip names (idle, walk, attack, death)
- Export with proper bone naming for Jolt ragdoll
- Generate animation clip metadata (start frame, end frame)

### Asset Packing

Extend the existing `.pak` packing to include new asset types:
- CSVs (items, enemies, abilities, areas, encounters)
- JSON (VFX prefabs)
- New audio categories (combat SFX)

### Files

| File | Purpose |
|------|---------|
| `scripts/validate_csv.py` | CSV validation for all data files |
| `scripts/csv_to_binary.py` | Convert CSVs to game format |
| `scripts/blender_export_enemy.py` | Enemy export with animation metadata |

---

## Task 5.4 — Enemy Behavior Variants (2-3 days)

Extend `EnemyAISystem` with more behavior types.

### Charger Behavior

```c
// Periodic charge: burst of speed toward player, then slow recovery
void behaviorCharger(Entity *enemy, f32 dt) {
    if (chargeCooldown <= 0) {
        // Charge!
        characterMoveToward(enemy, playerPos, stats->moveSpeed * 4.0f);
        chargeCooldown = 5.0f;
        playSfx(SFX_CHARGE_START);
    } else {
        // Slow recovery
        characterMoveToward(enemy, playerPos, stats->moveSpeed * 0.5f);
        chargeCooldown -= dt;
    }
}
```

### Ranged Behavior

```c
// Maintain distance, fire projectiles when in range
void behaviorRanged(Entity *enemy, f32 dt) {
    f32 dist = distance(enemy->pos, playerPos);
    f32 idealRange = 15.0f;

    if (dist < idealRange * 0.7f) {
        // Too close — back away
        vec3 away = normalize(enemy->pos - playerPos);
        characterMoveToward(enemy, enemy->pos + away * 10.0f, stats->moveSpeed);
    } else if (dist > idealRange * 1.3f) {
        // Too far — approach
        characterMoveToward(enemy, playerPos, stats->moveSpeed);
    }
    // Else: hold position, attack

    if (inAttackRange && cooldown <= 0) {
        // Fire projectile (reuse projectile system from 3.2)
        projectileSpawn(enemy, rangedAbilityDef, towardPlayer);
        cooldown = attackCooldown;
    }
}
```

### Patrol Behavior (non-hostile until provoked)

```c
// Wander between patrol points, aggro only when attacked
void behaviorPatrol(Entity *enemy, f32 dt) {
    if (wasAttacked) {
        // Switch to chase
        state = CHASING;
        targetEntity = attacker;
        return;
    }

    // Move toward next patrol point
    if (distanceToNextPatrolPoint < 1.0f) {
        nextPatrolPoint = patrolPoints[nextIndex];
        nextIndex = (nextIndex + 1) % patrolPointCount;
    }
    characterMoveToward(enemy, nextPatrolPoint, stats->moveSpeed * 0.5f);
}
```

### Files

| File | Purpose |
|------|---------|
| `c-game/game/enemy/EnemyAISystem.c` | Add charger, ranged, patrol behaviors |

---

## Task 5.5 — Enemy Death VFX & Physics (1-2 days)

When enemies die, make it satisfying.

### Ragdoll

On death (in `CharacterSystem` death flow):
1. Disable `CharacterVirtual` (already done)
2. Enable Jolt rigid body ragdoll (pre-configured from glTF)
3. Apply random impulse for dramatic fall
4. After 3 seconds → freeze ragdoll (static)
5. After 10 seconds → fade out + destroy

### Death VFX

- Spawn particle effect at death location (blood/dust/sparkle based on enemy type)
- Spawn floating "-HP" or "DEAD" text
- Brief screen shake if boss

### Corpse Despawn

```c
// In Enemy death flow:
enemy->corpseDespawnTime = 10.0f;
enemy->corpseFadeStart = 7.0f;

// Each frame:
if (enemy->isDead) {
    enemy->corpseDespawnTime -= dt;
    if (enemy->corpseDespawnTime < 3.0f) {
        // Fade out mesh
        f32 alpha = enemy->corpseDespawnTime / 3.0f;
        meshSetAlpha(enemy->meshEntity, alpha);
    }
    if (enemy->corpseDespawnTime <= 0) {
        entityDestroy(enemy);
    }
}
```

### Files

| File | Purpose |
|------|---------|
| `c-game/game/enemy/EnemyDeathVFX.h/.c` | Ragdoll enable, death particles, fade, despawn |

---

## Task 5.6 — Save/Load System (2-3 days)

Persist player state so they don't lose progress on exit.

### Save Data Structure

```c
typedef struct {
    u32  selectedCharacterId;
    u32  level;
    f32  xp;
    u32  currentAreaId;
    vec3 lastPosition;
    u32  skillTreeNodesUnlocked[32];
    u32  skillTreeNodeCount;
    ItemInstance inventory[INVENTORY_SLOTS];
    u32  inventoryCount;
    u32  equipmentSlots[SLOT_COUNT];
    u64  playTimeSeconds;
    u32  checksum;
} SaveData;
```

### Save Format

JSON for human readability (can switch to binary later for performance):
```json
{
    "version": 1,
    "character": {"id": 1, "level": 5, "xp": 450},
    "area": {"id": 2, "position": [10.5, 2.0, -5.3]},
    "inventory": [...],
    "equipment": {"weapon": 1, "helm": 0, ...},
    "skillTree": [1, 3, 5],
    "playTime": 3600
}
```

### Save Triggers

- Manual: ESC → "Save & Quit"
- Auto: on area transition, on level up
- Load: on game start, from main menu "Continue"

### Integration with State Machine

```c
// In main menu:
// If save file exists → show "Continue" button
// "Continue" → gameStateTransition(STATE_GAMEPLAY) → load save data

// In gameplay ESC:
// "Save & Quit" → saveGame() → gameStateTransition(STATE_MAIN_MENU)
// "Quit" → gameStateTransition(STATE_MAIN_MENU) (no save)
```

### Files

| File | Purpose |
|------|---------|
| `c-game/game/save/SaveSystem.h` | `saveGame()`, `loadGame()`, `saveExists()` |
| `c-game/game/save/SaveSystem.c` | JSON serialization/deserialization, file I/O |
| `c-game/game/gameState/GameState.c` | Save/load integration |

---

## Task 5.7 — Performance Optimization (1-2 days)

Profile and optimize the hot paths.

### Hitbox Optimization

Current: O(hitboxes × targets) brute force. Fine for <200 entities.
Future: Spatial grid for 500+ entities (not urgent for Phase 1).

### Entity Component System

Review component iteration patterns — ensure hot loops use the existing
component iteration utilities without unnecessary lookups.

### Asset Streaming

For multi-area gameplay:
- Unload assets from previous area on transition
- Preload next area assets during transition fade

### LOD for Enemies

- Close (<20m): full model, animations, physics
- Medium (20-50m): simplified animations, no physics updates
- Far (>50m): billboard or disabled

### Files

| File | Purpose |
|------|---------|
| Various | Targeted optimizations based on profiling |

---

## Implementation Order

| #    | Task | Depends On | Effort |
|------|------|-----------|--------|
| 5.0  | VFX system | 3.1 (skills) | 2-3d |
| 5.1  | Screen shake + hit stop | 1.5 (combat) | 1d |
| 5.2  | Minimap | 4.0 (areas) | 1-2d |
| 5.3  | Content pipeline tools | Any | 3-5d |
| 5.4  | Enemy behavior variants | 1.4 (AI) | 2-3d |
| 5.5  | Death VFX + ragdoll | 1.3 (death) | 1-2d |
| 5.6  | Save/load | Steps 1-4 complete | 2-3d |
| 5.7  | Performance | After profiling | 1-2d |

**Step 5 total:** ~12-18 days

---

## Verification Checklist

1. [ ] Abilities spawn VFX on impact
2. [ ] Screen shake on heavy hits
3. [ ] Hit stop on critical hits (brief freeze)
4. [ ] Minimap shows player, enemies, portals, loot
5. [ ] CSV validators catch invalid data files
6. [ ] Enemy chargers dash, ranged enemies maintain distance
7. [ ] Enemies ragdoll on death, fade out, despawn
8. [ ] Game saves on "Save & Quit", loads on "Continue"
9. [ ] Inventory, equipment, skill tree persist across sessions
10. [ ] Area transitions don't cause memory leaks over 10+ cycles
