# UI Screens — How They Work

## Overview

UI screens use **RmlUi** (an HTML/CSS-like UI library) managed by the `GuiManagerRmlUi` system. Each screen is implemented as a **`System` struct** with `added`/`update`/`removed` lifecycle callbacks, paired with an **HTML template** and optional **CSS**.

## Architecture

```
GuiManagerRmlUi (master system, registered in ECS)
  └── manages a dynamic array of GUI sub-systems (rmluiGuis)
       ├── StatsGui
       ├── ShowFpsGui
       ├── MainMenuGui
       ├── SettingsGui
       └── ...
```

The GuiManager is itself an ECS `System`. In its `postUpdate`, it:
1. Processes SDL input events → forwards to RmlUi
2. Calls `rmlUpdate()` (RmlUi frame tick)
3. Iterates all registered GUI sub-systems and calls their `update()`
4. Calls `rmlRenderVulkan()` to produce render geometry

## Creating a UI Screen — Step by Step

### 1. Define the System struct (`.h` / `.c`)

```c
// MyGui.h
#pragma once
extern struct System myGui;
```

```c
// MyGui.c
#include "ecs/system/System.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "rmlui/wrapper/src/crmlui.h"

static void added(void);
static void update(void);   // optional — only if you need periodic refresh
static void removed(void);

struct System myGui = {
    .name    = "myGui",
    .added   = added,
    .update  = update,
    .removed = removed,
};

static void* document;
static void* model;
```

### 2. `added()` — Load document & bind data

```c
void added(void) {
    // 1. Create document from HTML template (path relative to data paks)
    document = rmlNewDocument("gui/myGui/myGui.html");

    // 2. Create a data model (name must match data-model="..." in HTML)
    model = rmlCreateModel("myGui");

    // 3. Bind C variables to template expressions
    rmlBind(model, "score", &score);              // generic bind (auto type)
    rmlBindBool(model, "visible", &isVisible);    // typed variants exist
    rmlBindDouble(model, "health", &health);

    // 4. Bind arrays (for data-for loops in HTML)
    rmlBindArray(model, "items", &itemCount);      // itemCount = size_t

    // 5. Register transform functions (for formatting in templates)
    rmlRegisterTransformFunc(model, "itemInfo", itemInfoCallback);

    // 6. Load and show
    rmlLoadDocument(document);
    rmlShowDocument(document);              // takes focus (for menus)
    // OR
    rmlShowDocumentWithoutFocus(document);  // overlay (HUD, stats)
}
```

### 3. `update()` — Refresh bound data

Called every frame by GuiManager. Typical pattern: throttle updates.

```c
void update(void) {
    double now = nanos();
    if (now > lastUpdate + BILLION / 2.) {   // 2 Hz refresh
        lastUpdate = now;
        // update C-side variables, then:
        rmlUpdateDirtyAll(model);            // push all bindings to RmlUi
    }
}
```

### 4. `removed()` — Cleanup

```c
void removed(void) {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);    // if model was created
    document = NULL;          // useful as a toggle sentinel
}
```

### 5. HTML Template

Place in the appropriate data pak directory (engine: `c-engine/data/pak_0_engine/gui/`, game: `c-game/data/pak_1_game/gui/`).

```html
<html>
<head>
  <link type="text/rcss" href="../milligram.css" />
  <link type="text/rcss" href="myGui.css" />
</head>
<body id="myGui">
  <div class="container" data-model="myGui">
    <!-- Simple binding -->
    <div>Score: {{score}}</div>

    <!-- Formatted binding -->
    <div>Health: {{format(health, 1)}}</div>

    <!-- Conditional -->
    <div>{{visible ? 'YES' : 'NO'}}</div>

    <!-- Array loop with transform function -->
    <div data-for="it,it_index:items">
      <span>{{itemInfo(it_index, 0, it)}}</span>
      <span>{{itemInfo(it_index, 1, it)}}</span>
    </div>
  </div>
</body>
</html>
```

### 6. Show / Hide / Toggle

```c
// Show
guiManagerAddGuiNextFrame(&myGui);

// Hide
guiManagerRemoveGuiNextFrame(&myGui);

// Toggle pattern (uses document as sentinel)
void myGuiToggle(void) {
    if (document) {
        guiManagerRemoveGuiNextFrame(&myGui);
    } else {
        guiManagerAddGuiNextFrame(&myGui);
    }
}
```

`guiManagerAddGuiNextFrame` / `guiManagerRemoveGuiNextFrame` defer the add/remove to the next frame via `futureTaskAdd(0, ...)` to avoid mutating the GUI list during iteration.

## Key RmlUi C Wrapper Functions

| Function | Purpose |
|---|---|
| `rmlNewDocument(path)` | Create document handle from HTML file |
| `rmlCreateModel(name)` | Create a data model for bindings |
| `rmlBind(model, name, ptr)` | Bind a variable (generic) |
| `rmlBindBool/Double(model, name, ptr)` | Typed binding variants |
| `rmlBindArray(model, name, &size)` | Bind an array (size_t* for length) |
| `rmlRegisterTransformFunc(model, name, fn)` | Register `void fn(int index, int type, char* out)` |
| `rmlLoadDocument(doc)` | Load into RmlUi |
| `rmlShowDocument(doc)` | Show with focus |
| `rmlShowDocumentWithoutFocus(doc)` | Show as overlay |
| `rmlUnloadDocument(doc)` | Remove from RmlUi |
| `rmlUnloadModel(model)` | Destroy data model |
| `rmlUpdateDirtyAll(model)` | Push all bound values to UI |

## Transform Function Signature

Used for arrays where each element needs multiple display fields:

```c
void myTransform(int index, int type, char* out) {
    // index = array index, type = which field (0, 1, 2...), out = output buffer
    if (type == 0) sprintf(out, "%s", items[index].name);
    if (type == 1) sprintf(out, "%d", items[index].value);
}
```

In HTML: `{{myTransform(it_index, 0, it)}}` — the `type` argument selects which field.

## Lua Integration

GUIs can register Lua-callable functions for button handlers:

```c
luaRegisterFunction("onButtonClick", onButtonClick);
```

Then in HTML: `<button onclick="onButtonClick()">Click</button>`

(See `MainMenuGui.c` for examples with `settingsOpen` / `creditsOpen`.)

## File Locations

| Component | Engine GUI | Game GUI |
|---|---|---|
| C code | `c-engine/renderer/gui/rmlui/guis/<name>/` | `c-game/game/<name>/` |
| HTML/CSS | `c-engine/data/pak_0_engine/gui/<name>/` | `c-game/data/pak_1_game/gui/<name>/` |
| Shared CSS | `c-engine/data/pak_0_engine/gui/milligram.css` | — |
