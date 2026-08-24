# Step 0 — Game State Machine

**Order:** First. Must exist before Phase 1 so gameplay has a container.
**Effort:** 1-2 days

---

## Goal

A state machine that gates what systems run, what renders, and what input is consumed.
Phase 1 scope: Main Menu → "Play" → Gameplay → ESC → Main Menu. No char select, no
save/load, no game over screen.

---

## Component: `GameState`

Singleton, attached to a dedicated entity.

```c
typedef enum {
    STATE_MAIN_MENU,
    STATE_GAMEPLAY,
    // future: STATE_CHAR_SELECT, STATE_LOADING, STATE_PAUSE, STATE_GAME_OVER
} GameState;

typedef struct {
    GameState   currentState;
    GameState   prevState;
    f32         transitionProgress;  // 0.0→1.0 for fade animations
} GameStateComponent;

REGISTER_COMPONENT(GameStateComponent);
```

---

## System: `GameStateMachine`

### Core API

```c
void gameStateInit(void);           // create state entity, enter STATE_MAIN_MENU
void gameStateTransition(GameState target);  // exit old → enter new
void gameStateUpdate(void);         // dispatch to current state's update
GameState gameStateCurrent(void);
```

### State Registration

Each state registers enter/exit/update callbacks:

```c
typedef struct {
    void (*enter)(void);
    void (*exit)(void);
    void (*update)(void);
} StateCallbacks;

static void gameStateRegister(GameState state, StateCallbacks callbacks);
```

### Transition Flow

1. `gameStateTransition(target)` called
2. Call `oldState->exit()` — unload assets, hide UI, disable systems
3. Set `currentState = target`, `transitionProgress = 0.0`
4. Call `targetState->enter()` — load assets, show UI, enable systems
5. On each `gameStateUpdate()`, increment `transitionProgress` until 1.0

---

## Phase 1 States

### STATE_MAIN_MENU

**Enter:**

- `soundPlay(soundLoad(MUSIC))` — background music (existing pattern)
- `rmluiRegisterDocument("gui/mainMenu/mainMenu.html")` — existing main menu
- Register Lua binding: `playGame()` → `gameStateTransition(STATE_GAMEPLAY)`

**Update:**

- Run existing `mainMenu` system (settings, credits)

**Exit:**

- `soundStop(MUSIC)`
- `rmluiUnloadDocument("gui/mainMenu/mainMenu.html")`

### STATE_GAMEPLAY

**Enter:**

- `terrainLoad("oghuzlands.dat")`
- `sceneLoad("test2.dat")`
- Spawn player entity (existing pattern from `Player.c`)
- Attach `IsoCamera` to camera entity
- Spawn enemies (Step 1, task 1.0)
- `hudShow()` — health/mana bars

**Update:**

- Run all Phase 1 systems: player, enemy AI, combat, HUD, damage numbers

**Exit:**

- Destroy player entity
- Destroy enemy entities
- `hudHide()`
- `sceneUnload()`
- `terrainUnload()`
- ESC from gameplay → `gameStateTransition(STATE_MAIN_MENU)`

---

## Changes to Existing Code

### `c-game/game/Game.h`

```c
// Add:
#include "gameState/GameState.h"
void gameStateInit(void);
void gameStateUpdate(void);
GameState gameStateCurrent(void);
```

### `c-game/game/Game.c`

```c
// BEFORE:
static void gameSystemAdded(void) {
    soundPlay(soundLoad(MUSIC));
    rmluiRegisterDocument(...);
    terrainLoad("oghuzlands.dat");
    sceneLoad("test2.dat");
    animationDatLoad("eve_animator");
    // ... everything loads immediately
}

// AFTER:
static void gameSystemAdded(void) {
    gameStateInit();  // enters STATE_MAIN_MENU, loads only menu
}

static void gameSystemUpdate(void) {
    gameStateUpdate();  // dispatches to current state
    // ... existing per-frame game logic (if any)
}
```

---

## Main Menu Lua Binding

In `MainMenuGui.c`, add:

```c
static int luaPlayGame(State *L) {
    (void)L;
    gameStateTransition(STATE_GAMEPLAY);
    return 0;
}

// Register in mainMenuGuiAdded():
luaRegisterFunction(model, "playGame", luaPlayGame);
```

In `mainMenu.html`, wire the "Continue" or "Play" button:

```xml
<button onclick="playGame()">Play</button>
```

---

## Files to Create

| File                                | Purpose                                   |
| ----------------------------------- | ----------------------------------------- |
| `c-game/game/gameState/GameState.h` | Enum, component, public API               |
| `c-game/game/gameState/GameState.c` | State machine, transition logic, dispatch |

## Files to Modify

| File                                           | Change                                           |
| ---------------------------------------------- | ------------------------------------------------ |
| `c-game/game/Game.c`                           | Replace `gameSystemAdded` with `gameStateInit()` |
| `c-game/game/mainMenu/MainMenuGui.c`           | Add `playGame` Lua binding                       |
| `c-game/data/pak_1/gui/mainMenu/mainMenu.html` | Wire Play button                                 |

---

## Verification Checklist

1. Game launches → main menu appears (existing behavior preserved)
2. Click "Play" → main menu disappears, terrain + scene + player load
3. Player is visible, camera follows from isometric angle
4. Press ESC → gameplay unloads, main menu reappears
5. Click "Play" again → fresh gameplay session (no state leak from previous)
6. No crashes, no memory leaks (Valgrind or ASan)
