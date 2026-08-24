# Step 3 — Skills & Abilities (One Skill, One Tree)

**Order:** After Step 2 (loot works, stats are modifiable).
**Effort:** ~10-15 days
**Prerequisites (content):** Ability animation clips on character models. VFX/shader assets
for skill effects (can start simple — particles are fine).

---

## Goal

Player can cast abilities via hotkeys. Abilities have cooldowns, consume mana, and trigger
the existing combat system. A basic skill tree lets the player choose between two skill
specializations.

---

## Task 3.0 — Ability Definition System (2 days)

Data-driven ability definitions so new skills don't require code changes.

### CSV Schema: `abilities.csv`

```
id,name,description,icon,manaCost,cooldown,type,targetType,shape,damage,aoeRadius,ranges,projectileSpeed
1,Fire Strike,Cast a fire bolt,icon_firestrike,20,2.0,projectile,single,SPHERE,80,0,50,80
2,Hammer Slam,Slam the ground,icon_hammerslam,35,4.0,melee,aoe,BOX,120,3.0,0,0
3,Lightning Dash,Dash and zap enemies,icon_lightdash,25,5.0,dash,self,SPHERE,40,1.5,0,0
```

### Component: `AbilityDef`

```c
typedef enum { ABILITY_MELEE, ABILITY_PROJECTILE, ABILITY_DASH, ABILITY_CHANNEL } AbilityType;
typedef enum { TARGET_SINGLE, TARGET_AOE, TARGET_SELF } TargetType;

typedef struct {
    u32    id;
    char   name[64];
    char   description[256];
    char   iconFile[64];
    f32    manaCost;
    f32    cooldown;
    AbilityType   type;
    TargetType targetType;
    HitboxShape  hitboxShape;  // reuses combat system shapes
    f32    damage;            // base damage (scales with CharacterStats)
    f32    aoeRadius;
    f32    range;
    f32    projectileSpeed;
    u32    castAnimClip;
    u32    effectId;          // optional VFX
    u32    sfxId;             // optional SFX
} AbilityDef;

AbilityDef *abilityDefsLoad(const char *csvPath, u32 *outCount);
```

### Component: `AbilitySlot`

One per hotbar slot — binds a specific ability to a key.

```c
typedef struct {
    u32   abilityDefId;
    f32   cooldownRemaining;  // seconds
    u8    onCooldown;
} AbilitySlot;
```

### Files

| File | Purpose |
|------|---------|
| `c-game/game/abilities/AbilityDef.h` | `AbilityDef`, enums, loader API |
| `c-game/game/abilities/AbilityDef.c` | CSV parser |
| `c-game/game/abilities/AbilitySlot.h` | `AbilitySlot` struct |
| `c-game/data/pak_1/abilities/abilities.csv` | Ability definitions |

---

## Task 3.1 — Skill System (Core Casting) (2-3 days)

Execute abilities when the player presses hotkeys.

### System: `SkillSystem`

#### Hotbar Setup

```c
#define MAX_ABILITY_SLOTS 10

typedef struct {
    AbilitySlot slots[MAX_ABILITY_SLOTS];
} SkillBar;
```

Player starts with one ability in slot 1. More unlocked via skill tree (3.3).

#### Cast Flow

```c
void skillCast(Entity *player, u32 slotIndex) {
    SkillBar *bar = getPlayerSkillBar(player);
    if (bar->slots[slotIndex].onCooldown) return;

    AbilityDef *def = abilityDefGet(bar->slots[slotIndex].abilityDefId);
    CharacterStats *stats = getComponent(player, CharacterStats);

    // Check mana
    if (stats->mana < def->manaCost) {
        playSfx(SFX_NOT_ENOUGH_MANA);
        return;
    }

    // Deduct mana
    stats->mana -= def->manaCost;

    // Start cooldown
    bar->slots[slotIndex].onCooldown = 1;
    bar->slots[slotIndex].cooldownRemaining = def->cooldown;

    // Play cast animation
    animationPlay(getComponent(player, Animator), def->castAnimClip);

    // Execute ability
    executeAbility(player, def);
}
```

#### `executeAbility()` Dispatch

```c
void executeAbility(Entity *player, AbilityDef *def) {
    switch (def->type) {
        case ABILITY_MELEE:
            meleeAbility(player, def);
            break;
        case ABILITY_PROJECTILE:
            projectileAbility(player, def);
            break;
        case ABILITY_DASH:
            dashAbility(player, def);
            break;
    }
}
```

##### Melee Ability

Create a hitbox at player position, facing player's forward direction:
```c
vec3 forward = playerGetForward(player);
vec3 center = player->transform.position + forward * def->range;
combatCreateHitbox(player, def->hitboxShape, &center,
                   def->damage * stats->damage, DAMAGE_PHYSICAL, 2);
```

##### Projectile Ability

Spawn a projectile entity (see 3.2):
```c
Entity *proj = projectileSpawn(player, def, direction);
```

##### Dash Ability

Move player toward target, create AoE hitbox at destination:
```c
vec3 target = player->transform.position + forward * 8.0f;
characterMoveToward(player, target, 30.0f);  // burst speed
// At arrival:
combatCreateHitbox(player, HITBOX_SPHERE, &target,
                   def->damage, DAMAGE_LIGHTNING, 3);
```

#### Cooldown Tick

Each frame, in `SkillSystem.update()`:
```c
for (u32 i = 0; i < MAX_ABILITY_SLOTS; i++) {
    if (skillBar->slots[i].onCooldown) {
        skillBar->slots[i].cooldownRemaining -= dt;
        if (skillBar->slots[i].cooldownRemaining <= 0) {
            skillBar->slots[i].onCooldown = 0;
        }
    }
}
```

### Files

| File | Purpose |
|------|---------|
| `c-game/game/abilities/SkillSystem.h` | `skillCast()`, `skillBarInit()` |
| `c-game/game/abilities/SkillSystem.c` | Cast flow, cooldown tick, ability dispatch |

---

## Task 3.2 — Projectile System (1-2 days)

Independent entities that fly through the world and detonate on impact.

### Component: `Projectile`

```c
typedef struct {
    Entity   *owner;
    Entity   *meshEntity;     // visual mesh
    vec3     velocity;
    f32      lifetime;        // seconds
    f32      lifetimeRemaining;
    f32      damage;
    u32      damageType;
    HitboxShape shape;
    f32      hitboxRadius;
    u32      effectId;        // detonation VFX
} Projectile;

REGISTER_COMPONENT(Projectile);
```

### System: `ProjectileSystem`

Each frame:
1. Iterate all `Projectile` entities
2. `position += velocity * dt`
3. Raycast forward to check for `HitboxTarget` entities
4. On hit:
   - `combatCreateHitbox(owner, shape, &hitPos, damage, damageType, 1)`
   - Spawn detonation VFX
   - `entityDestroy(projectile)`
5. Decrement `lifetimeRemaining`
6. If `lifetimeRemaining <= 0` → destroy (timeout)

### Files

| File | Purpose |
|------|---------|
| `c-game/game/abilities/Projectile.h` | `Projectile` component, REGISTER_COMPONENT |
| `c-game/game/abilities/ProjectileSystem.h` | `projectileSpawn()` |
| `c-game/game/abilities/ProjectileSystem.c` | Movement, collision, detonation |

---

## Task 3.3 — Skill Tree UI (2-3 days)

Basic choice tree — two specialization branches.

### Data Structure: `skills_tree.csv`

```
nodeId,parentId,name,description,abilityId,unlocks,icon,xPos,yPos
ROOT,,Base,Starting node,,,icon_root,500,800
FIRE_BRANCH,ROOT,Fire Path,Unlocks fire abilities,,LIGHTNING_BRANCH,icon_fire,300,600
LIGHTNING_BRANCH,ROOT,Lightning Path,Unlocks lightning abilities,,FIRE_BRANCH,icon_lightning,700,600
FIREBALL,FIRE_BRANCH,Fireball,Learn Fire Strike ability,1,,icon_fireball,300,400
HAMMER,FIRE_BRANCH,Hammer Slam,Learn Hammer Slam ability,2,,icon_hammer,200,200
DASH,LIGHTNING_BRANCH,Lightning Dash,Learn Lightning Dash ability,3,,icon_dash,700,400
```

Mutually exclusive branches: picking FIRE_BRANCH blocks LIGHTNING_BRANCH and vice versa.

### Component: `SkillTree`

```c
typedef struct {
    u32   nodesUnlocked[32];  // node IDs that are unlocked
    u32   nodeCount;
    u32   totalSkillPoints;
    u32   spentSkillPoints;
} SkillTree;

REGISTER_COMPONENT(SkillTree);
```

### Skill Point Awards

On level up (in `CharacterSystem.gainXP()`):
```c
skillTree->totalSkillPoints++;
```

### Skill Tree UI (RMLUI)

Full-screen overlay, toggle with 'K' key:
- Nodes displayed at `(xPos, yPos)` from CSV
- Lines drawn between parent and child nodes
- Node states: locked (gray), available (yellow), unlocked (green), blocked (red)
- Click available node → spend 1 point → unlock node + any associated ability
- Hover → show name, description, prerequisites

### Ability Unlock Flow

When a node with an `abilityId` is unlocked:
1. Add ability to next empty `AbilitySlot`
2. Show "Ability Learned!" notification
3. Update skill bar HUD

### Files

| File | Purpose |
|------|---------|
| `c-game/game/abilities/SkillTreeData.h` | `SkillTreeNode` struct, CSV loader |
| `c-game/game/abilities/SkillTreeData.c` | Parse `skills_tree.csv` |
| `c-game/game/abilities/SkillTree.h` | `SkillTree` component, REGISTER_COMPONENT |
| `c-game/game/abilities/SkillTreeGui.h` | `skillTreeGuiShow()`, `skillTreeGuiHide()` |
| `c-game/game/abilities/SkillTreeGui.c` | RMLUI rendering, click handlers |
| `c-game/data/pak_1/gui/skillTree/skillTree.html` | RMLUI layout |
| `c-game/data/pak_1/abilities/skills_tree.csv` | Tree definition |

---

## Task 3.4 — Skill Bar HUD (1-2 days)

Hotbar overlay at bottom of screen showing equipped abilities.

### RMLUI Layout

Horizontal bar at screen bottom:
- 10 slots (or however many abilities are unlocked)
- Each slot shows:
  - Ability icon
  - Cooldown overlay (dark rectangle, percentage filled)
  - Keybind number (1-0)
- Hover → tooltip with ability name + description + mana cost

### Integration

In `HudSystem`, each frame:
```c
for (u32 i = 0; i < MAX_ABILITY_SLOTS; i++) {
    AbilitySlot *slot = &skillBar->slots[i];
    if (slot->abilityDefId == 0) continue;

    Element *icon = hud->abilityIcons[i];
    Element *cdOverlay = hud->cooldownOverlays[i];

    if (slot->onCooldown) {
        f32 pct = slot->cooldownRemaining / abilityDef->cooldown;
        cdOverlay->SetStyleHeight(pct * 100 + "%");
        cdOverlay->SetAttribute("visible", "true");
    } else {
        cdOverlay->SetAttribute("visible", "false");
    }
}
```

### Files

| File | Purpose |
|------|---------|
| `c-game/game/hud/SkillBarHud.h` | `skillBarHudInit()`, `skillBarHudUpdate()` |
| `c-game/game/hud/SkillBarHud.c` | RMLUI hotbar rendering, cooldown overlays |
| `c-game/data/pak_1/gui/hud/skillbar.html` | RMLUI layout (added to existing HUD) |

---

## Implementation Order

| #    | Task | Depends On | Effort |
|------|------|-----------|--------|
| 3.0  | Ability definitions | — | 2d |
| 3.1  | Skill system (casting) | 3.0, 1.5 (combat) | 2-3d |
| 3.2  | Projectile system | 3.0, 1.5 (combat) | 1-2d |
| 3.3  | Skill tree UI | 3.0, 2.5 (XP) | 2-3d |
| 3.4  | Skill bar HUD | 3.1 | 1-2d |

**Step 3 total:** ~10-15 days

---

## Verification Checklist

1. [ ] Player has at least one ability in the hotbar
2. [ ] Press ability hotkey → cast animation plays, mana is deducted
3. [ ] Melee ability → hitbox activates at correct range, damages enemies
4. [ ] Projectile ability → projectile spawns, flies, explodes on impact
5. [ ] Cooldown appears on ability icon, counts down, ability re-activates
6. [ ] Press hotkey during cooldown → nothing happens
7. [ ] Not enough mana → sound plays, nothing happens
8. [ ] Level up → gain skill point
9. [ ] Press K → skill tree opens, shows nodes
10. [ ] Click available node → point spent, node unlocks
11. [ ] Node with ability → ability added to hotbar
12. [ ] Mutually exclusive branches work (pick one, other is blocked)
13. [ ] Skill bar shows icons, cooldowns, keybinds at screen bottom
