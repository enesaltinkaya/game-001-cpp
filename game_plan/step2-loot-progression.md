# Step 2 — Loot & Progression (One Area, One Enemy)

**Order:** After Step 1 core loop is functional.
**Effort:** ~8-12 days
**Prerequisites (content):** At least one enemy with drop tables defined. Basic weapon/item models.

---

## Goal

When enemies die, items drop on the ground. Player picks them up. Items have stats that
modify player damage. Player gets stronger as they farm.

---

## Task 2.0 — Item Data Model (1 day)

Flat-file item definitions that drive everything: what exists, what it looks like, what it does.

### CSV Schema: `items.csv`

```
id,name,itemType,rarity,levelReq,icon,baseStat1,baseStatValue1,baseStat2,baseStatValue2,model3d
1,Iron Sledge,weapon_mace,common,1,icon_mace_iron,damage,15,size,large,models/weapons/mace_iron.gltf
2,Steel Mace,weapon_mace,uncommon,5,icon_mace_steel,damage,28,size,large,models/weapons/mace_steel.gltf
3,Health Potion,consumable,common,1,icon_healthpot,heal,50,,,
```

### Component: `ItemInstance`

One per individual dropped/picked-up item (not the template).

```c
typedef enum { ITEM_WEAPON, ITEM_ARMOR, ITEM_CONSUMABLE, ITEM_MATERIAL } ItemType;
typedef enum { RARITY_COMMON, RARITY_UNCOMMON, RARITY_RARE, RARITY_LEGENDARY } ItemRarity;

typedef struct {
    u32    itemDefId;       // index into item definitions
    ItemType   itemType;
    ItemRarity rarity;
    u32    quantity;        // stackable items
    f32    stats[6];        // rolled/modified stats (damage, armor, etc.)
    u32    equippedEntity;  // 0 = not equipped, otherwise entity ID
    char   name[64];
    char   iconFile[64];
    char   model3dFile[64];
} ItemInstance;

REGISTER_COMPONENT(ItemInstance);
```

### Item Definition Loader

```c
typedef struct {
    u32   id;
    char  name[64];
    ItemType   type;
    ItemRarity rarity;
    u32   levelReq;
    char  iconFile[64];
    f32   baseStatValue[6];
    char  model3dFile[64];
} ItemDef;

ItemDef *itemDefsLoad(const char *csvPath, u32 *outCount);
const ItemDef *itemDefGet(u32 id);
```

### Files

| File | Purpose |
|------|---------|
| `c-game/game/items/ItemDef.h` | `ItemDef`, `ItemType`, `ItemRarity` |
| `c-game/game/items/ItemDef.c` | CSV parser, `itemDefsLoad()`, lookup |
| `c-game/game/items/ItemInstance.h` | `ItemInstance` component, REGISTER_COMPONENT |
| `c-game/data/pak_1/items/items.csv` | Item definitions |

---

## Task 2.1 — Drop Tables (1 day)

Define what each enemy drops, with probabilities.

### CSV Schema: `drops.csv`

```
enemyId,itemId,minQty,maxQty,probability
1,1,1,1,0.40    // Iron Sledge, 40% chance
1,3,1,3,0.80    // Health Potion, 1-3, 80% chance
1,2,1,1,0.05    // Steel Mace, 5% chance
2,3,2,5,0.90    // Health Potion, 2-5, 90% chance
```

### Drop Table Loader

```c
typedef struct {
    u32  enemyId;
    u32  itemId;
    u32  minQty;
    u32  maxQty;
    f32  probability;
} DropEntry;

DropEntry *dropTablesLoad(const char *csvPath, u32 *outCount);
void dropsForEnemy(u32 enemyId, ItemInstance **outItems, u32 *outCount);
```

### `dropsForEnemy()` Flow

1. Filter `dropTables` where `enemyId` matches
2. For each entry: roll RNG, if `rng < probability` → generate quantity, create `ItemInstance`
3. Append to `outItems` array
4. Return count

### Files

| File | Purpose |
|------|---------|
| `c-game/game/items/DropTable.h` | `DropEntry`, public API |
| `c-game/game/items/DropTable.c` | CSV parser, `dropsForEnemy()` |
| `c-game/data/pak_1/items/drops.csv` | Drop table definitions |

---

## Task 2.2 — Ground Loot (1-2 days)

When enemies die, item meshes spawn on the ground as collectible entities.

### Component: `GroundLoot`

```c
typedef struct {
    ItemInstance  item;        // embedded item instance
    vec3          position;    // world position (where it dropped)
    f32           despawnTime; // seconds until auto-despawn (default: 60s)
    f32           despawnRemaining;
    Entity       *meshEntity;  // reference to the 3D mesh entity
} GroundLoot;

REGISTER_COMPONENT(GroundLoot);
```

### System: `GroundLootSystem`

On spawn (called from `CharacterSystem` death flow):
1. `dropsForEnemy(enemyTypeId, &items, &count)`
2. For each item:
   a. Create entity with `GroundLoot` component
   b. Set position = enemy's death position + random offset
   c. If `item.model3dFile` exists → spawn 3D mesh at position
   d. Else → spawn simple cube/coin mesh
   e. Play loot spawn SFX

Each frame:
1. Iterate all `GroundLoot` entities
2. Check distance to player (pickup range, default: 2 units)
3. If in range → auto-pickup (or show pickup prompt + keybind)
4. Decrement `despawnRemaining`, destroy at 0 (play despawn SFX)
5. Bob animation: `mesh->position.y = baseY + sin(time * frequency) * amplitude`

### Pickup Flow

```c
void groundLootPickup(Entity *lootEntity, Entity *player) {
    GroundLoot *loot = getComponent(lootEntity, GroundLoot);
    // Add item to player inventory
    inventoryAdd(player, &loot->item);
    // Play pickup SFX
    combatAudioPlayPickup();
    // Destroy ground entity + mesh
    entityDestroy(lootEntity->meshEntity);
    entityDestroy(lootEntity);
}
```

### Files

| File | Purpose |
|------|---------|
| `c-game/game/items/GroundLoot.h` | `GroundLoot` component, REGISTER_COMPONENT |
| `c-game/game/items/GroundLootSystem.h` | `groundLootSpawn()`, `groundLootPickup()` |
| `c-game/game/items/GroundLootSystem.c` | Spawn, bob, despawn, auto-pickup |

---

## Task 2.3 — Inventory System (2 days)

Client-side only storage. No networking, no persistence yet.

### Component: `Inventory`

```c
#define INVENTORY_SLOTS 40

typedef struct {
    ItemInstance *slots[INVENTORY_SLOTS];
    u32           slotCount;
    u32           capacity;
} Inventory;

REGISTER_COMPONENT(Inventory);
```

### System: `InventorySystem`

```c
void inventoryAdd(Entity *owner, ItemInstance *item);
ItemInstance *inventoryRemove(Entity *owner, u32 slotIndex);
void inventoryEquip(Entity *owner, u32 slotIndex);
void inventoryUse(Entity *owner, u32 slotIndex);  // consumables
```

#### `inventoryAdd()` Flow

1. If item is stackable and identical item exists → increment quantity
2. Else → find empty slot → place item
3. If no empty slot → fail (or stack overflow, Phase 2 doesn't need bank)
4. Play pickup confirmation SFX
5. Notify HUD to update

### Inventory UI (RMLUI)

Simple grid overlay, toggle with 'I' key:
- Grid of slots (5x8)
- Each slot shows item icon
- Hover shows item name + stats tooltip
- Click to equip/use

### Files

| File | Purpose |
|------|---------|
| `c-game/game/items/Inventory.h` | `Inventory` component, public API |
| `c-game/game/items/Inventory.c` | Add, remove, equip, use logic |
| `c-game/game/items/InventoryGui.h` | `inventoryGuiShow()`, `inventoryGuiHide()` |
| `c-game/game/items/InventoryGui.c` | RMLUI grid, tooltips, click handlers |
| `c-game/data/pak_1/gui/inventory/inventory.html` | RMLUI layout |

---

## Task 2.4 — Equipment Slots & Stat Application (1-2 days)

Items modify the player's `CharacterStats` when equipped.

### Equipment Slots

```c
typedef enum {
    SLOT_WEAPON,
    SLOT_HELM,
    SLOT_CHEST,
   _SLOT_GLOVES,
    SLOT_BOOTS,
    SLOT_ACCESSORY,
    SLOT_COUNT
} EquipmentSlot;
```

### Component: `Equipment`

```c
typedef struct {
    u32   slotIds[SLOT_COUNT];  // slot index in inventory, 0xFFFFFFFF = empty
} Equipment;

REGISTER_COMPONENT(Equipment);
```

### Stat Application

When `inventoryEquip()` is called:
1. Get item from inventory slot
2. If something is already in that equipment slot → `unapplyStats(oldItem)`
3. `equipment->slotIds[slotType] = slotIndex`
4. `applyStats(item)` → add item stats to `CharacterStats`
5. If weapon → replace player's weapon mesh with item's `model3d`

```c
void applyStats(ItemInstance *item, CharacterStats *stats) {
    switch (item->itemType) {
        case ITEM_WEAPON:
            stats->damage += item->stats[0];  // damage stat
            break;
        case ITEM_ARMOR:
            stats->armor += item->stats[0];   // armor stat
            break;
        // ...
    }
}
```

### Weapon Mesh Swap

When equipping a weapon:
1. Destroy current weapon mesh entity (child of player entity)
2. Load `item->model3dFile` as glTF
3. Attach to player's "hand" bone or equipment socket
4. Update attack hitbox size based on weapon size

### Files

| File | Purpose |
|------|---------|
| `c-game/game/items/Equipment.h` | `Equipment` component, `EquipmentSlot` |
| `c-game/game/items/Equipment.c` | `applyStats()`, `unapplyStats()`, weapon mesh swap |

---

## Task 2.5 — XP & Level Up (1-2 days)

Enemies award XP on death. Level up increases base stats.

### XP Awards

On enemy death in `CharacterSystem`:
```c
f32 xpAward = enemyStats->level * 10.0f;  // Simple formula
gainXP(playerEntity, xpAward);
```

### Level Up Flow

In `CharacterSystem.gainXP()`:
```c
void gainXP(Entity *target, f32 amount) {
    CharacterStats *stats = getComponent(target, CharacterStats);
    stats->xp += amount;

    while (stats->xp >= stats->xpToNext) {
        stats->xp -= stats->xpToNext;
        stats->level++;
        stats->xpToNext = levelXPCurve(stats->level);

        // Stat increases on level up
        stats->maxHp += 10.0f;
        stats->hp = stats->maxHp;  // full heal on level up
        stats->maxMana += 5.0f;
        stats->mana = stats->maxMana;
        stats->damage += 2.0f;

        // Show level up notification
        hudShowLevelUp(stats->level);
        combatAudioPlayLevelUp();
    }
}
```

### XP Curve

```c
f32 levelXPCurve(u32 level) {
    return 100.0f * powf(1.15f, level - 1);
}
```

### HUD Integration

In `HudSystem`:
- Add XP bar below health/mana
- Add level display
- Level up notification: floating text, brief animation

### Files

| File | Purpose |
|------|---------|
| `c-game/game/character/CharacterSystem.c` | Add `gainXP()`, `levelXPCurve()` |
| `c-game/game/hud/Hud.h/.c` | XP bar, level display, level up notification |

---

## Implementation Order

| #    | Task | Depends On | Effort |
|------|------|-----------|--------|
| 2.0  | Item data model | — | 1d |
| 2.1  | Drop tables | 2.0 | 1d |
| 2.2  | Ground loot | 2.0, 2.1 | 1-2d |
| 2.3  | Inventory system | 2.0 | 2d |
| 2.4  | Equipment + stats | 2.3, 1.3 | 1-2d |
| 2.5  | XP & level up | 1.3 | 1-2d |

**Step 2 total:** ~8-12 days

---

## Verification Checklist

1. [ ] Enemy dies → items drop at death location with 3D mesh
2. [ ] Loot bobs up and down, auto-despawns after 60s
3. [ ] Player walks near loot → auto-picks up (or presses pickup key)
4. [ ] Pickup SFX plays, item appears in inventory
5. [ ] Press I → inventory grid shows items
6. [ ] Click item in inventory → tooltip shows name + stats
7. [ ] Click weapon → equips to weapon slot, weapon mesh appears on player
8. [ ] Equipped weapon modifies player's damage stat
9. [ ] Unequip → stats revert
10. [ ] Enemy death → XP awarded to player
11. [ ] XP bar fills, level up at threshold → stats increase, full heal
12. [ ] Level up notification appears briefly
