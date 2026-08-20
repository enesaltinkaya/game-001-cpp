# Step 1 — Core Loop (Playable in One Room)

**Order:** After Step 0 (game state machine).
**Effort:** ~12-17 days
**Prerequisites (content):** At least one enemy character model with idle, walk, attack,
and death animation clips. Without this, Step 1 blocks.

---

## Goal

Walk into a room, kill monsters with basic attacks, see health bars and damage numbers,
watch enemies die. One character, one area, 3-5 enemy types.

---

## Task 1.0 — Enemy Entity Setup (1 day)

Wire up enemy entities so they exist in the scene before AI or combat can work.

### Pattern

Reuse the existing scene-loading pattern from `Player.c` — glTF entities tagged with
`"enemy"` in scene JSON trigger component attachment on scene load. For Step 1, hardcode
5-10 enemy spawn positions if scene tagging is slow.

### Components Attached Per Enemy Entity

- `Enemy` (hostile marker, see 1.4)
- `CharacterStats` (HP, damage, etc., see 1.3)
- `HitboxTarget` (can be hit, see 1.5)
- `Animator` (idle/walk/attack/death clips)
- Jolt `CharacterVirtual` (collision, movement)
- `Transform` (spawn position)

### Component: `EnemySpawn` (Phase 1 only)

```c
typedef struct {
    vec3  position;
    u32   enemyTypeId;  // indexes into hardcoded enemy types
    u32   animIdle;
    u32   animWalk;
    u32   animAttack;
    u32   animDeath;
} EnemySpawnConfig;
```

### System: `EnemySpawnSystem`

- On scene load (called from `stateGameplayEnter()`):
  - Iterate hardcoded spawn configs
  - For each: `createEntity()`, attach all required components, set initial state = IDLE
  - Start idle animation
- API: `enemySpawnAll(EnemySpawnConfig *configs, u32 count, Scene *scene)`

### Files

| File                             | Purpose                                 |
| -------------------------------- | --------------------------------------- |
| `c-game/game/enemy/EnemySpawn.h` | `EnemySpawnConfig`, public API          |
| `c-game/game/enemy/EnemySpawn.c` | `enemySpawnAll()`, component attachment |

---

## Task 1.1 — Isometric Camera (1-2 days)

Replace the third-person orbit camera with a fixed-angle isometric camera that smoothly
follows the player.

### Component: `IsoCamera`

```c
typedef struct {
    Entity    *followTarget;
    f32       fov;
    f32       elevation;     // pitch angle (~30-45°)
    f32       distance;      // height above target
    f32       smoothSpeed;   // lerp factor per frame
    f32       minY;
    f32       maxY;          // clamp vertical follow
} IsoCamera;
```

### System: `IsoCameraSystem` — runs in `preUpdate`

1. Get `followTarget`'s world position
2. Calculate desired camera position: `target + offset` (offset derived from elevation
   and distance, fixed 45° yaw)
3. Lerp current position → desired position (factor = `smoothSpeed * dt`)
4. Clamp Y axis to `[minY, maxY]`
5. Update `CameraComponent` view/projection matrices
6. Provide `isoCameraUnproject(screenX, screenY) → vec3` for click-to-move (stretch goal)

### Integration

- Attach `IsoCamera` to the active camera entity in `stateGameplayEnter()`
- The existing `CameraComponent` and `CameraSystem` handle UBO upload — no changes needed
- DO NOT modify `Player.c` camera code. Follow `FlyingCamera.c` pattern: a separate
  controller that writes to the camera entity's `CameraComponent`

### Files

| File                             | Purpose                                  |
| -------------------------------- | ---------------------------------------- |
| `c-game/game/camera/IsoCamera.h` | `IsoCamera` struct, public API           |
| `c-game/game/camera/IsoCamera.c` | `IsoCameraSystem`, lerp logic, unproject |

---

## Task 1.2 — Top-Down Player Movement (1-2 days)

Adapt the existing player controller from third-person to top-down.

### Strip From Existing `Player.c`

- Jump logic
- Camera orbit code (yaw/pitch/distance from mouse)
- Sky-look
- Auto-run
- Ability system (hurricane kick, bicycle kick, fireball) — will be replaced by combat system

### Keep

- WASD → movement direction
- Jolt `CharacterVirtual` setup and velocity application
- Animation state machine (idle/run/walk)
- Footstep sounds (bone raycast pattern)

### Add

- Movement speed read from `CharacterStats.moveSpeed` (see 1.3)
- Ground-plane constrained movement (input is XY only, no Z)
- **Extract** shared utility: `characterMoveToward(entity, targetPos, speed)` —
  takes a world position and speed, sets Jolt velocity. Enemy AI (1.4) will reuse this.

### Component Changes

`Player` component — remove `jumping`, `autoRun`, camera yaw/pitch fields.
Add `moveSpeed` override (falls back to `CharacterStats.moveSpeed`).

### Stretch Goal

Mouse-click-to-move: `IsoCamera.unproject()` → terrain raycast → set move target.
WASD-only is sufficient for the vertical slice.

### Files

| File                          | Purpose                                                |
| ----------------------------- | ------------------------------------------------------ |
| `c-game/game/player/Player.h` | Modified `Player` component                            |
| `c-game/game/player/Player.c` | Refactored controller, `characterMoveToward()` utility |

---

## Task 1.3 — Character Stats & Resources (1 day)

Minimal RPG stats to drive combat feel.

### Component: `CharacterStats`

Attachable to player AND enemies.

```c
typedef struct {
    f32  hp;
    f32  maxHp;
    f32  mana;
    f32  maxMana;
    f32  moveSpeed;
    f32  attackSpeed;
    f32  damage;            // base damage
    f32  armor;
    f32  elementalResist[3]; // FIRE, COLD, LIGHTNING
    f32  xp;
    f32  xpToNext;
    u32  level;
    u8   isDead;
} CharacterStats;

REGISTER_COMPONENT(CharacterStats);
```

### System: `CharacterSystem`

```c
void applyDamage(Entity *target, f32 amount, u32 damageType);
void applyHeal(Entity *target, f32 amount);
void gainXP(Entity *target, f32 amount);
```

#### `applyDamage()` Flow

1. Apply elemental resistance reduction
2. Subtract from `hp`
3. Spawn floating damage number (1.7) at target position
4. Play hit SFX (1.8)
5. If `hp <= 0`: trigger death

#### Death Flow

1. Set `isDead = 1`
2. Get entity's `AnimatorComponent` → `animationPlayBlended(entity, deathAnimClip)`
3. Disable Jolt `CharacterVirtual` (stop physics)
4. If entity has `Enemy` component → set `Enemy.state = DEAD`
5. Play death SFX
6. (Phase 2) Spawn ground loot

### Files

| File                                      | Purpose                              |
| ----------------------------------------- | ------------------------------------ |
| `c-game/game/character/CharacterStats.h`  | Component struct, REGISTER_COMPONENT |
| `c-game/game/character/CharacterSystem.h` | Public API                           |
| `c-game/game/character/CharacterSystem.c` | Damage, healing, death, XP logic     |

---

## Task 1.5 — Combat System (Hitboxes + Damage) (2 days)

⚠️ Must come BEFORE 1.4 — enemies call `combatCreateHitbox()`.

Generalize the existing ability hit detection in `Player.c` into a reusable combat system.

### Component: `AttackHitbox`

Ephemeral — lives for 1-3 frames then self-destructs.

```c
typedef enum { HITBOX_SPHERE, HITBOX_BOX, HITBOX_RAY } HitboxShape;

typedef struct {
    Entity      *ownerEntity;
    HitboxShape  shape;
    union {
        struct { vec3 center; f32 radius; };
        struct { vec3 center; vec3 halfExtents; };
        struct { vec3 origin; vec3 direction; f32 length; };
    } data;
    f32  damage;
    u32  damageType;
    u32  lifetime;          // frames remaining
    Entity  *hitTargets[32]; // prevent double-hit
    u32  hitCount;
} AttackHitbox;

REGISTER_COMPONENT(AttackHitbox);
```

### Component: `HitboxTarget`

Lightweight marker — any entity that can be hit.

```c
typedef struct {
    // Currently just a marker. Collision shape could be added later.
} HitboxTarget;

REGISTER_COMPONENT(HitboxTarget);
```

### System: `CombatSystem`

Each frame:

1. Iterate all `AttackHitbox` components via `getComponents(scene, AttackHitbox)`
2. For each hitbox, iterate all `HitboxTarget` entities
3. Check overlap (sphere-sphere, AABB-AABB, ray-AABB)
4. On hit (and target not in `hitTargets[]`):
   - `CharacterSystem.applyDamage(target, hitbox.damage, hitbox.damageType)`
   - Add target to `hitbox.hitTargets[]`
5. Decrement `hitbox.lifetime`
6. If `lifetime == 0` → `removeComponent(scene, AttackHitbox, entity->id)`

### Public API

```c
void combatCreateHitbox(Entity *owner, HitboxShape shape, void *shapeData,
                        f32 damage, u32 damageType, u32 lifetime);
```

### Player Integration

Replace existing inline sphere overlap in `Player.c` abilities with
`combatCreateHitbox()`. Delete the old hit detection code.

### Files

| File                                | Purpose                                       |
| ----------------------------------- | --------------------------------------------- |
| `c-game/game/combat/AttackHitbox.h` | `AttackHitbox`, `HitboxTarget`, `HitboxShape` |
| `c-game/game/combat/Combat.h`       | `combatCreateHitbox()` API                    |
| `c-game/game/combat/Combat.c`       | `CombatSystem`, overlap checks, lifecycle     |

---

## Task 1.4 — Enemy AI (Chase + Attack) (2-3 days)

Depends on: 1.0 (spawn), 1.2 (movement), 1.3 (stats), 1.5 (combat).

### Component: `Enemy`

```c
typedef enum { ENEMY_IDLE, ENEMY_CHASING, ENEMY_ATTACKING, ENEMY_DEAD } EnemyState;

typedef struct {
    f32    aggroRange;
    f32    attackRange;
    f32    attackCooldown;
    f32    attackCooldownRemaining;
    EnemyState state;
    Entity  *targetEntity;
    vec3    spawnPosition;   // return point when disengaging
    u32     deathAnimClip;
} Enemy;

REGISTER_COMPONENT(Enemy);
```

### System: `EnemyAISystem`

Per enemy, per frame:

1. If `state == DEAD` → skip
2. If player within `aggroRange` and `state != ATTACKING`:
   → `state = CHASING`, `targetEntity = player`
3. If `CHASING` and player within `attackRange`:
   → `state = ATTACKING`, `attackCooldownRemaining = attackCooldown`
4. If `ATTACKING`:
   - If `attackCooldownRemaining <= 0`:
     - Trigger attack animation
     - `combatCreateHitbox(entity, ...)` — melee hitbox in front of enemy
     - `attackCooldownRemaining = attackCooldown`
   - Else: `attackCooldownRemaining -= dt`
   - If player outside `attackRange * 1.5`: `state = CHASING`
5. If `CHASING`:
   - `characterMoveToward(entity, playerPosition, stats->moveSpeed)` (from 1.2)
   - Play walk animation
6. If player outside `aggroRange * 1.5`:
   → `state = IDLE`, walk back to `spawnPosition`

### Files

| File                                | Purpose                                         |
| ----------------------------------- | ----------------------------------------------- |
| `c-game/game/enemy/Enemy.h`         | `Enemy`, `EnemyState`, REGISTER_COMPONENT       |
| `c-game/game/enemy/EnemyAISystem.h` | Public API                                      |
| `c-game/game/enemy/EnemyAISystem.c` | Finite state machine, movement, attack triggers |

---

## Task 1.6 — Basic HUD (1-2 days)

Health/mana bars only. Skill bar is Step 3.

### RMLUI Layout

`c-game/data/pak_1/gui/hud/hud.html`:

- Health bar (red, top-left or near-cursor PoE-style)
- Mana bar (blue, below health)
- Text labels for numeric HP/mana

### Component: `HudComponent`

```c
typedef struct {
    RmlUi::Core::ElementDocument *document;
    RmlUi::Core::Element         *healthBar;
    RmlUi::Core::Element         *manaBar;
    RmlUi::Core::Element         *healthText;
    RmlUi::Core::Element         *manaText;
    Entity                       *playerEntity;
} HudComponent;
```

### System: `HudSystem`

Each frame:

1. Get `CharacterStats` from `hud.playerEntity`
2. Update bar widths: `hp / maxHp`, `mana / maxMana`
3. Update text labels
4. Flash bars on damage (brief color change)

### Files

| File                                 | Purpose                                           |
| ------------------------------------ | ------------------------------------------------- |
| `c-game/game/hud/Hud.h`              | `HudComponent`, public API (`hudShow`, `hudHide`) |
| `c-game/game/hud/Hud.c`              | `HudSystem`, RMLUI element updates                |
| `c-game/data/pak_1/gui/hud/hud.html` | RMLUI layout                                      |

---

## Task 1.7 — Floating Damage Numbers (1 day)

### Component: `DamageNumber`

```c
typedef struct {
    vec3  position;
    vec3  velocity;      // upward drift
    char  text[32];       // formatted damage value
    vec4  color;          // color-coded by damage type
    f32   alpha;          // fade out
    u32   lifetime;       // frames remaining
} DamageNumber;

REGISTER_COMPONENT(DamageNumber);
```

### System: `DamageNumberSystem`

Each frame:

1. Iterate all `DamageNumber` components
2. `position += velocity * dt` (drift upward)
3. `alpha = lifetime / maxLifetime` (fade)
4. Update RMLUI element position (world → screen via camera matrix)
5. Decrement `lifetime`, destroy at 0

### Spawn

`CharacterSystem.applyDamage()` calls:

```c
damageNumberSpawn(targetEntity, damageAmount, damageType, hitPosition);
```

### Files

| File                                      | Purpose                                 |
| ----------------------------------------- | --------------------------------------- |
| `c-game/game/combat/DamageNumber.h`       | Component, REGISTER_COMPONENT           |
| `c-game/game/combat/DamageNumberSystem.h` | `damageNumberSpawn()` API               |
| `c-game/game/combat/DamageNumberSystem.c` | Drift, fade, screen projection, destroy |

---

## Task 1.8 — Combat Audio (0.5 day)

### Sounds

- Attack whoosh (on cast) — played by whoever casts
- Hit impact (on damage applied) — played by `CharacterSystem.applyDamage()`
- Enemy death — played by `CharacterSystem` on death
- (Phase 2) Loot pickup

### Integration

```c
// In CharacterSystem.applyDamage():
combatAudioPlayHit(damageType);

// In CharacterSystem death flow:
combatAudioPlayDeath();

// In CombatSystem when player casts:
combatAudioPlayAttack();
```

### Pattern

Follow existing `Player.c` pattern: `soundLoad()` at init (cache handles),
`soundPlay()` at runtime.

### Files

| File                              | Purpose                                           |
| --------------------------------- | ------------------------------------------------- |
| `c-game/game/audio/CombatAudio.h` | `combatAudioInit()`, `combatAudioPlayHit()`, etc. |
| `c-game/game/audio/CombatAudio.c` | Load sound handles, play wrappers                 |

---

## Verification Checklist

1. [ ] Game starts → main menu → click Play → gameplay loads
2. [ ] Camera follows player from isometric angle (~30-45° elevation, 45° yaw)
3. [ ] Player moves with WASD on ground plane (no jump, no orbit)
4. [ ] 5-10 enemies visible in the scene at spawn positions
5. [ ] Enemies are idle until player enters aggro range
6. [ ] Enemies chase player when in range
7. [ ] Player presses attack key → animation plays → hitbox activates
8. [ ] Hitbox overlaps enemy → `applyDamage()` fires
9. [ ] Enemy HP decreases, floating damage number appears at hit location
10. [ ] Damage number drifts upward, fades, and disappears
11. [ ] Enemy reaches 0 HP → plays death animation → stops moving → physics disabled
12. [ ] Enemy attack hits player → player HP decreases
13. [ ] Player health/mana bars visible, update on damage, flash on hit
14. [ ] Attack whoosh, hit impact, and death SFX play
15. [ ] ESC → returns to main menu, no crashes
16. [ ] Re-enter gameplay → fresh state (no leaks from previous session)
