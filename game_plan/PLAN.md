# Game Design — Index

> This file is a high-level roadmap. Each phase has a detailed step file.

| Phase                    | Step File                                              | Effort | Status         |
| ------------------------ | ------------------------------------------------------ | ------ | -------------- |
| 0 — Game State Machine   | [step0-game-state.md](step0-game-state.md)             | 1-2d   | ⬜ Not Started |
| 1 — Core Loop (One Room) | [step1-core-loop.md](step1-core-loop.md)               | 12-17d | ⬜ Not Started |
| 2 — Loot & Progression   | [step2-loot-progression.md](step2-loot-progression.md) | 8-12d  | ⬜ Not Started |
| 3 — Skills & Abilities   | [step3-skills.md](step3-skills.md)                     | 10-15d | ⬜ Not Started |
| 4 — Content Scale        | [step4-content.md](step4-content.md)                   | 15-25d | ⬜ Not Started |
| 5 — Polish & Pipeline    | [step5-polish.md](step5-polish.md)                     | 12-18d | ⬜ Not Started |

**Total estimated:** ~58-89 days (solo dev, part-time)

---

## Dependency Graph

```
Step 0 (state machine)
  └─→ Step 1 (core loop)
       ├─ 1.0 enemy entity setup
       ├─ 1.1 isometric camera
       ├─ 1.2 top-down movement
       ├─ 1.3 character stats
       ├─ 1.5 combat (hitboxes+damage) ← before 1.4!
       ├─ 1.4 enemy AI (chase+attack)
       ├─ 1.6 basic HUD
       ├─ 1.7 floating damage numbers
       └─ 1.8 combat audio
            └─→ Step 2 (loot & progression)
                 ├─ 2.0 item data model
                 ├─ 2.1 drop tables
                 ├─ 2.2 ground loot
                 ├─ 2.3 inventory
                 ├─ 2.4 equipment + stats
                 └─ 2.5 XP & level up
                      └─→ Step 3 (skills & abilities)
                           ├─ 3.0 ability definitions
                           ├─ 3.1 skill system (casting)
                           ├─ 3.2 projectile system
                           ├─ 3.3 skill tree UI
                           └─ 3.4 skill bar HUD
                                └─→ Step 4 (content scale)
                                     ├─ 4.0 area system + encounters
                                     ├─ 4.1 enemy type registry
                                     ├─ 4.2 boss behavior
                                     ├─ 4.3 character selection
                                     └─ 4.4 world map
                                      └─→ Step 5 (polish)
                                           ├─ 5.0 VFX system
                                           ├─ 5.1 screen shake + hit stop
                                           ├─ 5.2 minimap
                                           ├─ 5.3 content pipeline tools
                                           ├─ 5.4 enemy behavior variants
                                           ├─ 5.5 death VFX + ragdoll
                                           ├─ 5.6 save/load
                                           └─ 5.7 performance
```

---

## Architecture Principles

### ECS Pattern

Components are pure data structs. Systems iterate components each frame.
No inheritance. No OO. Component registration via `REGISTER_COMPONENT(Type)`.

### Data-Driven Design

All game content (items, enemies, abilities, areas, encounters, skill trees)
defined in CSV/JSON files. Zero code changes needed to add new content.

### Combat Flow

```
Player presses hotkey
  → SkillSystem.cast()
    → executeAbility()
      → combatCreateHitbox() [melee/projectile/dash]
        → CombatSystem checks overlap against HitboxTarget entities
          → CharacterSystem.applyDamage()
            → floating damage number (1.7)
            → hit SFX (1.8)
            → if HP <= 0: death animation → ragdoll → loot drop
```

### Camera

Camera-relative rendering: camera position stays near origin, `worldOrigin` in
`CameraUbo` stores actual world position. Shaders subtract `worldOrigin` for
final positions. See AGENTS.md → "Camera-relative rendering".

### Audio Pattern

`soundLoad()` at init (cache handles), `soundPlay()` at runtime. Follow
existing `Player.c` pattern.

---

## Technical Constraints

- **Engine:** Custom C23 engine (c-engine), CMake + Ninja, strict warnings
- **Renderer:** Vulkan, SPIR-V shaders
- **UI:** RMLUI (HTML/CSS-like, Lua bindings)
- **Physics:** Jolt Physics, `CharacterVirtual` for characters
- **Animation:** Custom animation system with glTF import, blended transitions
- **Assets:** `.pak` archives, glTF models, CSV/JSON data files
- **Audio:** OpenAL (or whatever `SoundSystem.h` uses)

---

## Risk Register

| Risk                                            | Impact                 | Mitigation                                               |
| ----------------------------------------------- | ---------------------- | -------------------------------------------------------- |
| No enemy models/animations                      | Blocks Step 1 entirely | Create placeholder capsules with animations first        |
| RMLUI can't render damage numbers               | Blocks Step 1.7        | Use 3D billboard text as fallback                        |
| Jolt character movement conflicts with top-down | Blocks Step 1.2/1.4    | Extract `characterMoveToward()` early, test in isolation |
| CSV parsing is slow at runtime                  | Blocks Step 4 scale    | Pre-convert CSVs to binary at build time (5.3)           |
| Too many areas → memory pressure                | Blocks Step 4 scale    | Aggressive asset unloading on area transition            |
| Skill tree becomes unwieldy                     | Scope creep            | Phase 1: 2 branches, 3 abilities max. Expand later.      |

---

## Game Design Pillars

### 1. "I Killed That" — Combat Clarity

Every hit must communicate unambiguously: floating damage numbers, enemy flinch
animation, hit SFX, screen shake on heavy hits, brief hit-stop on criticals.
No ambiguity about whether an attack landed.

### 2. "I'm Getting Stronger" — Visible Progression

Every kill matters. XP bar fills. Level up triggers full heal + stat boost +
skill point. Equipment from kills directly increases damage. The gap between
"new player vs tough enemy" closes visibly through farming.

### 3. "I Built My Character" — Build Diversity

Skills come from trees, not loot. Players choose their identity (fire specialist
vs lightning striker). Skills scale with character level. Equipment modifies
existing abilities (bigger fire, faster cooldown) rather than replacing them.

### 4. "One More Room" — Hook Loop

Short, repeatable combat encounters (30-60 seconds). Clear progression through
areas. Visible rewards on every kill. Low friction to retry. The loop is:
**enter room → kill enemies → collect loot → get stronger → enter next room.**

---

## What We're NOT Doing (Phase 1)

- No networking/multiplayer
- No procedural generation (all hand-authored areas)
- No quest system (progression = area completion)
- No NPC dialogue
- No crafting
- No trading/economy
- No persistent world state (clear everything on exit)
- No advanced pathfinding (direct movement toward targets)
- No camera occlusion handling (isometric angle minimizes this)
