# Step 4 — Content Scale (Multi-Area, Multi-Enemy, Multi-Character)

**Order:** After Step 3 (skills work, combat loop is deep).
**Effort:** ~15-25 days (content-heavy)
**Prerequisites (content):** Multiple area terrains, 20+ enemy models, 2 character models
with full animation sets.

---

## Goal

Expand from one room to multiple interconnected areas. Each area has different enemies,
different difficulty. Player can choose between 2-3 characters at the start.

---

## Task 4.0 — Area System & Data-Driven Encounters (2-3 days)

Replace hardcoded enemy spawns with area-based encounter definitions.

### Area Definition: `areas.csv`

```
id,name,sceneFile,terrainFile,ambientMusic,skybox,spawnX,spawnY,spawnZ,connections
1,Dark Forest,darkforest.scene,oghuzlands.dat,music_forest,sky_forest,0,5,0,2;3
2,Crypt,darkcrypt.scene,crypt_terrain.dat,music_crypt,sky_void,-10,2,0,1;4
3,Ancient Ruins,ruins.scene,ruins_terrain.dat,music_ruins,sky_ruins,20,8,0,1
4,Boss Chamber,bosschamber.scene,crypt_terrain.dat,music_boss,sky_void,0,0,0,2
```

### Encounter Definition: `encounters.csv`

```
areaId,enemyTypeId,count,minDistance,maxDistance,respawnTime
1,1,5,5,20,0       // 5x Enemy Type 1, scattered 5-20 units from spawn
1,2,3,15,30,0
2,3,8,3,15,0
2,4,2,20,35,120   // 2x Elite Type 4, respawn after 120s
4,5,1,0,5,0       // 1x Boss Type 5, no respawn
```

### Component: `Area`

```c
typedef struct {
    u32   id;
    char  name[64];
    char  sceneFile[128];
    char  terrainFile[128];
    char  ambientMusic[128];
    char  skybox[64];
    vec3  spawnPoint;
    u32   connectionIds[8];
    u32   connectionCount;
} AreaDef;

typedef struct {
    u32    enemyTypeId;
    u32    count;
    f32    minDistance;
    f32    maxDistance;
    f32    respawnTime;     // 0 = no respawn
    f32    respawnRemaining;
    u8     respawnActive;
} EncounterDef;
```

### Component: `AreaPortal`

Placed at area boundaries. Player walks through → loads new area.

```c
typedef struct {
    u32   targetAreaId;
    vec3  targetSpawn;
    f32   triggerRadius;
    u8    isTriggered;      // prevent double-trigger
} AreaPortal;

REGISTER_COMPONENT(AreaPortal);
```

### System: `AreaSystem`

```c
void areaLoad(u32 areaId);
void areaUnload(void);
void areaTransition(u32 targetAreaId);
```

#### `areaLoad()` Flow

1. `areaUnload()` if another area is loaded
2. `terrainLoad(areaDef->terrainFile)`
3. `sceneLoad(areaDef->sceneFile)`
4. `soundPlay(soundLoad(areaDef->ambientMusic))`
5. Set skybox
6. Spawn player at `areaDef->spawnPoint`
7. Spawn all encounters for this area
8. Spawn area portals

#### `areaTransition()` Flow

1. Find portal → get target area ID + spawn point
2. `areaUnload()` — destroy enemies, unload scene/terrain
3. Play transition fade (reuse state transition fade from Step 0)
4. `areaLoad(targetAreaId)`
5. Player appears at portal's `targetSpawn`

### Encounter Spawning

```c
void encounterSpawnAll(u32 areaId) {
    EncounterDef *encounters = encountersForArea(areaId, &count);
    for (u32 i = 0; i < count; i++) {
        EncounterDef *enc = &encounters[i];
        if (enc->respawnActive) continue;

        for (u32 j = 0; j < enc->count; j++) {
            vec3 pos = randomInCircle(spawnPoint, enc->minDistance, enc->maxDistance);
            enemySpawn(enc->enemyTypeId, pos, currentScene);
        }
    }
}
```

### Respawn System

Each frame, in `AreaSystem.update()`:
```c
for (each encounter) {
    if (enc->respawnTime > 0 && allEnemiesDead(enc->enemyTypeId)) {
        enc->respawnActive = 1;
        enc->respawnRemaining = enc->respawnTime;
    }
    if (enc->respawnActive) {
        enc->respawnRemaining -= dt;
        if (enc->respawnRemaining <= 0) {
            enc->respawnActive = 0;
            encounterSpawn(enc);
        }
    }
}
```

### Files

| File | Purpose |
|------|---------|
| `c-game/game/areas/Area.h` | `AreaDef`, `EncounterDef`, public API |
| `c-game/game/areas/Area.c` | CSV loaders, `areaLoad()`, `areaUnload()`, encounters |
| `c-game/game/areas/AreaPortal.h` | `AreaPortal` component, REGISTER_COMPONENT |
| `c-game/game/areas/AreaPortalSystem.c` | Portal trigger detection, transition |
| `c-game/data/pak_1/areas/areas.csv` | Area definitions |
| `c-game/data/pak_1/areas/encounters.csv` | Encounter definitions |

---

## Task 4.1 — Enemy Type Registry (1-2 days)

Replace hardcoded enemy types with a registry that maps type IDs to configurations.

### CSV Schema: `enemy_types.csv`

```
id,name,health,damage,armor,moveSpeed,aggroRange,attackRange,
attackCooldown,expReward,behaviorType,modelFile,animIdle,animWalk,animAttack,animDeath
1,Forest Imp,80,12,0,3.0,15,2.0,1.5,25,basic,models/enemies/imp.gltf,0,1,2,3
2,Crypt Skeleton,150,25,5,2.5,12,2.5,2.0,50,basic,models/enemies/skeleton.gltf,0,1,2,3
3,Ruin Golem,400,45,20,1.5,10,3.0,3.0,100,charger,models/enemies/golem.gltf,0,1,2,3
4,Crypt Lich,200,60,3,3.5,20,25,1.0,80,ranged,models/enemies/lich.gltf,0,1,2,3
5,Demon Lord,2000,120,50,2.0,30,3.0,2.5,500,boss,models/enemies/demonlord.gltf,0,1,2,3
```

### Enemy Behavior Types

- `basic` — chase + melee (existing EnemyAI)
- `charger` — periodic charge attack (dash toward player)
- `ranged` — maintain distance, fire projectiles
- `boss` — phase-based, enrage at 50% HP, summon minions

### Component: `EnemyType`

```c
typedef enum { BEHAVIOR_BASIC, BEHAVIOR_CHARGER, BEHAVIOR_RANGED, BEHAVIOR_BOSS } BehaviorType;

typedef struct {
    u32       id;
    char      name[64];
    f32       health;
    f32       damage;
    f32       armor;
    f32       moveSpeed;
    f32       aggroRange;
    f32       attackRange;
    f32       attackCooldown;
    f32       expReward;
    BehaviorType behavior;
    char      modelFile[128];
    u32       animIdle;
    u32       animWalk;
    u32       animAttack;
    u32       animDeath;
} EnemyType;

EnemyType *enemyTypeRegistryLoad(const char *csvPath, u32 *outCount);
EnemyType *enemyTypeGet(u32 id);
```

### Integration

When `enemySpawn()` is called with a type ID:
1. Look up `EnemyType` from registry
2. Attach `Enemy` component with values from registry
3. Attach `CharacterStats` with HP, damage, armor from registry
4. Load model, set animations from registry

### Files

| File | Purpose |
|------|---------|
| `c-game/game/enemy/EnemyType.h` | `EnemyType`, `BehaviorType`, registry API |
| `c-game/game/enemy/EnemyType.c` | CSV loader, `enemyTypeGet()` |
| `c-game/game/enemy/EnemySpawn.c` | Updated to use registry |
| `c-game/data/pak_1/enemies/enemy_types.csv` | Enemy type definitions |

---

## Task 4.2 — Boss Behavior (1-2 days)

Extend `EnemyAISystem` to handle boss-specific behaviors.

### Boss Phases

```c
typedef struct {
    f32  hpThreshold;     // percentage (e.g., 0.5 = 50% HP)
    u32  animPhase;       // animation state for this phase
    f32  damageMultiplier;
    u32  summonEnemyId;   // 0 = no summon
    u32  summonCount;
} BossPhase;
```

### Boss AI Modifications

In `EnemyAISystem`, when `behavior == BEHAVIOR_BOSS`:

1. Track current phase based on HP percentage
2. On phase transition:
   - Play phase transition animation
   - Apply `damageMultiplier`
   - If `summonEnemyId > 0` → spawn minions
3. Add special abilities:
   - AoE slam (ground-targeted hitbox)
   - Teleport (move to random position near player)
   - Enrage (increase speed/damage at low HP)

### Files

| File | Purpose |
|------|---------|
| `c-game/game/enemy/EnemyAISystem.c` | Extended with boss logic |

---

## Task 4.3 — Character Selection (2-3 days)

Multiple characters with different base stats and abilities.

### CSV Schema: `characters.csv`

```
id,name,modelFile,baseHp,baseMana,baseDamage,baseArmor,
moveSpeed,startAbility1,startAbility2,portraitFile
1,Oghuz Oglu,models/characters/oghuz.gltf,100,50,20,5,4.5,1,2,portrait_oghuz.png
2,Eve,models/characters/eve.gltf,80,80,15,3,5.0,3,4,portrait_eve.png
```

### Character Selection UI

Simple screen (Phase 2 of state machine):
- Character portraits in a row
- Click portrait → show stats + abilities tooltip
- "Play" button → start game with selected character

### State Machine Extension

Add `STATE_CHAR_SELECT`:
- Main Menu → "New Game" → `STATE_CHAR_SELECT`
- Select character → `STATE_GAMEPLAY` with character data
- Store `selectedCharacterId` in game state

### Files

| File | Purpose |
|------|---------|
| `c-game/game/characters/CharacterDef.h` | `CharacterDef` struct, CSV loader |
| `c-game/game/characters/CharacterDef.c` | Parse `characters.csv` |
| `c-game/game/characters/CharacterSelectGui.h/.c` | RMLUI character select screen |
| `c-game/data/pak_1/characters/characters.csv` | Character definitions |
| `c-game/game/gameState/GameState.c` | Add `STATE_CHAR_SELECT` |

---

## Task 4.4 — World Map (1-2 days)

Visual area transition map showing connected areas.

### RMLUI Layout

Simple node-based map:
- Area nodes displayed as circles/icons
- Lines between connected areas
- Current area highlighted
- Locked areas grayed out (based on progress)
- Click adjacent area → `areaTransition()`

### Progression Gate

Optional: areas can require minimum level or completed prerequisite area.

```c
// In areas.csv, add:
// requiredLevel,prerequisiteAreaId
4,Boss Chamber,...,10,2    // requires level 10, area 2 completed
```

### Files

| File | Purpose |
|------|---------|
| `c-game/game/areas/WorldMapGui.h` | `worldMapShow()`, `worldMapHide()` |
| `c-game/game/areas/WorldMapGui.c` | RMLUI map rendering, click handlers |
| `c-game/data/pak_1/gui/worldMap/worldMap.html` | RMLUI layout |

---

## Implementation Order

| #    | Task | Depends On | Effort |
|------|------|-----------|--------|
| 4.0  | Area system + encounters | Step 1 complete | 2-3d |
| 4.1  | Enemy type registry | 4.0 | 1-2d |
| 4.2  | Boss behavior | 4.1 | 1-2d |
| 4.3  | Character selection | 4.1 | 2-3d |
| 4.4  | World map | 4.0 | 1-2d |

**Step 4 total:** ~15-25 days (heavy on content/data, light on code)

---

## Verification Checklist

1. [ ] Player can walk through area portals to transition between areas
2. [ ] Each area loads its own terrain, scene, music, and skybox
3. [ ] Enemies spawn according to encounter definitions (correct count, spread)
4. [ ] Different enemy types behave correctly (basic, charger, ranged)
5. [ ] Boss has multiple phases (enrage at 50%, summon minions)
6. [ ] Enemies respawn in areas (if respawnTime > 0)
7. [ ] Main menu → "New Game" → character select screen appears
8. [ ] Select character → game starts with that character's stats
9. [ ] World map shows connected areas, clicking transitions
10. [ ] No state leaks between area transitions (old enemies destroyed, assets freed)
