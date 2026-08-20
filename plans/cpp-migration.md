# C++ Idiom Migration Plan

**Date:** 2026-08-20
**Scope:** Port the C-idiom codebase (already `.cpp`/C++23) to idiomatic modern C++.
**Decisions (agreed):**

- **Depth:** Full idiomatic C++ (classes w/ access control, smart pointers, RAII, namespaces, virtual-method systems, replace global singletons with owned instances).
- **Containers:** Migrate hot-path containers to `std::vector` (accept perf change for portability/idiom).
- **Order:** Bottom-up by dependency: `c-utils` → `c-engine` → `c-game`.
- **Verification:** Build after each module (`./scripts/build.sh`); run the game (`./scripts/run.sh`) only at milestones.

## Scale

| Module     | Lines | Files | Notes                                      |
| ---------- | ----- | ----- | ------------------------------------------ |
| `c-utils`  | ~4.5k | 45    | Foundational. Convert first.               |
| `c-engine` | ~32k  | 164   | ECS core + Vulkan renderer + GUI. Largest. |
| `c-game`   | ~17k  | 80    | Gameplay, azgaar world, player/enemy, GUI. |
| `tools`    | ~3k   | 4     | Offline builders; convert last / optional. |

Current C-idiom footprint: `typedef struct X{…}X` ×190, stb_ds `Array(T)=T*` + `foreach` macros, function-pointer `System` structs, global singletons (`extern Ecs ecs`), `malloc/free/memcpy/memset/NULL` (×931 NULLs), designated initializers. Almost no C++ today (3 namespaces, ~189 `std::` uses).

## Progress log (as of 2026-08-20)

| Item | Status |
| ---- | ------ |
| Phase 0 — green baseline | ✅ done (links clean; pre-existing warning debt catalogued) |
| Phase 1 — `c-utils` idiom | ✅ done, **0 c-utils warnings** |
| Phase 2 — ECS core (SparseSet→`std::vector`, `Ecs` modernized) | ✅ done, runtime-validated |
| Phase 2b — all 11 `c-engine` ECS systems (safe idiom subset) | ✅ done, build green + smoke test passed |
| Phase 3 — `c-engine` renderer/vulkan + GUI | ⬜ not started (largest remaining piece) |
| Phase 4 — `c-game` | ⬜ not started |
| Phase 5a/5b/5c/5e/5d — cross-cutting flips | ⬜ not started |

**Next up:** Phase 3 (Vulkan renderer + GUI). Then Phase 4, then the cross-cutting flips. Each phase ends with a green build; milestones add a `./scripts/run.sh` smoke/screenshot run.

---

## Step 0 — Green baseline (do first, before any edits)

1. `./scripts/build.sh` → must **compile + link with 0 errors**.
2. Record the warning baseline. **Reality:** the repo currently links clean (0 errors) but carries _pre-existing_ C-extension warning debt (C99 compound literals in `Settings.cpp`/`Sqlite.cpp`, missing-designated-field-initializer in `FutureTask.cpp`/`DataManager.cpp`, unused vars). It is **not** zero-warning today.
3. Optional smoke run: `./scripts/run.sh play log 5000` to confirm the game boots before we touch anything.

### Per-phase gate (revised)

Because of the existing debt, the gate for Phases 1–4 is: **no new errors, no new warnings, and actively reduce C-extension debt in the modules being touched.** "Zero warnings across the whole tree" is the end-state goal reached in **Phase 5d**, not an immediate per-module requirement.

---

## Target C++ idiom (the consistency anchor)

Apply these rules uniformly. This section is the reference every module is converted against.

### Types & declarations

- `typedef struct X { … } X;` → `struct X { … };` (drop the typedef alias; use the bare name).
- POD data structs → `struct` with **public** members (components, UBOs, config).
- Behavy objects (systems, managers, passes) → `class` with **private** members + public methods.
- Enums: prefer `enum class X { … };`; keep unscoped only where interop/flag-combos need it.
- Replace `NULL` → `nullptr`; `char showStats;` booleans → `bool`.
- Fixed-width aliases (`u32/i32/u64`) stay (they're in `Utils.h`); they're fine in C++.

### Memory & ownership

- Own resources with **RAII**. No manual `malloc/free` pairs for owned objects.
  - Unique ownership → `std::unique_ptr<T>`.
  - Shared ownership → `std::shared_ptr<T>` (sparingly).
  - Non-owning → raw pointer or `std::reference_wrapper`.
- Containers own their elements: `std::vector<T>`, `std::vector<std::unique_ptr<T>>`.
- Custom **buddy allocator** (`memoryAlloc/Free/Realloc`): retire in favor of the standard heap once containers move to `std::vector`. Handle the `memoryInit(ALLOCATOR_*)` call site at startup (see Phase 2). If a perf regression appears, re-introduce a `std::allocator`-based pool behind `std::vector` rather than global alloc functions.

### Containers (stb_ds → std)

Provide a **compatibility shim** in `c-utils/container/Array.h` so the bulk of existing call sites compile unchanged during transition, then replace call sites with native STL as each module is touched:

| Old (stb_ds macro)        | Target                                                  |
| ------------------------- | ------------------------------------------------------- |
| `Array(T)` = `T*`         | `std::vector<T>`                                        |
| `arrayPut(a, v)`          | `a.push_back(v)` / `a.emplace_back(v)`                  |
| `arraySize(a)`            | `static_cast<i32>(a.size())`                            |
| `arrayLast(a)`            | `a.back()`                                              |
| `arrayPop(a)`             | `a.pop_back()`                                          |
| `arrayDeleteSlow(a,i)`    | `a.erase(a.begin()+i)`                                  |
| `arrayDeleteSwap(a,i)`    | swap-with-last + `pop_back()`                           |
| `arraySetSizeZeroed(a,n)` | `a.resize(n); std::fill(...)`                           |
| `arrayFree(a)`            | no-op (vector owns memory)                              |
| `foreach(item, a)`        | `for (auto& item : a)` (range-for)                      |
| `foreachptr(item, a)`     | `for (auto* item : a)`                                  |
| `Map(K,V)` (stb_ds hash)  | `std::unordered_map<K,V>` (or `std::map` where ordered) |

The shim keeps `arrayPut/arraySize/foreach` as thin wrappers over `std::vector` so untouched files still build; migrated files use native STL. Remove the shim once no call sites remain.

**Sequencing reality (important):** `Array(T)`/`Map(K,V)`/`StrMap(T)` are a _global contract_ — they appear in public headers consumed across modules (`DataManager.h`, `String.h`, `Ecs.h`). Flipping them breaks every consumer at once, so it is **not** bottom-up-safe. Therefore:

- **Phases 1–4 keep the stb_ds container macros and `memoryAlloc` intact.** Each module's idiom conversion migrates only _module-internal_ container usage that does not cross a public-header boundary; public container APIs stay stb_ds until the flip.
- **The container flip + allocator retirement happen in Phase 5** (cross-cutting), where the macro definitions change and all fallout (incl. public-header signatures + every consumer) is fixed together, then stb_ds is removed.

`SparseSet` (ECS SoA storage): **keep the algorithm**, re-implement internals on `std::vector<u32>`/`std::vector<char>` instead of raw `u32*`/`char*` + manual realloc. Public API (`ssNew/ssInsert/ssContainsValue/…`) stays stable so the ECS layer is untouched until its own phase.

**Gotcha (stb_ds macros + braced-init):** while the stb_ds container macros are still in use, passing a C++ braced-init like `Template{a,b,c}` to a _function-like_ macro such as `arrayPut(arr, Template{a,b,c})` breaks — braces do **not** shield the inner commas from the macro's argument splitter ("too many arguments"). Wrap it: `arrayPut(arr, (Template{a,b,c}))`. (This also replaces the old C99 `((Template){…})` compound literal, which is what triggered `-Wc99-extensions`.)

**More gotchas (learned during Phase 1–2b):**

- **`const_cast` vs `reinterpret_cast` to drop const:** casting a `const T*` to a non-const pointer (e.g. `memoryFree(entity->name)` where `name` is `const char*`) needs `const_cast<char*>(p)` (then implicit `char*`→`void*`). `reinterpret_cast<void*>(constP)` and `static_cast` both **fail** with "casts away qualifiers".
- **`vec3`/`vec4`/`mat4` are fixed arrays** (`float[3]`/`float[4]`, cglm `types.h`), not structs. Pass them **by name** (they decay to element pointers) into `glm_*` functions — `glm_vec4_copy(dirVec, dst)`, NOT `glm_vec4_copy(&dirVec, dst)` (`&dirVec` is `float(*)[4]`, no matching overload). Named `vec4 v = {…};` replaces the C99 `(vec4){…}` compound literal.
- **Bulk sed cast conversion is treacherous.** A pattern like `s/(i32)\([A-Za-z_][A-Za-z0-9_]*\)/…/` greedily matches `(i32)floorf` / `(u32)arraySize` as if the operand were a bare identifier, producing `static_cast<i32>(floorf)(args)` — which breaks because `floorf`/`arraySize` are a function/macro that must keep its call parens attached. Prefer targeted literal seds per known case, or restrict the operand class; always build after.
- **`arraySize` is an stb_ds macro**, not a function: `static_cast<u32>(arraySize(x))` keeps the macro invocation intact (fine); but `static_cast<u32>(arraySize)(x)` (paren detached) leaves `arraySize` unexpanded → "undeclared identifier".

### Functions & parameters

- Pass by **const reference** (`const T&`) for read-only, by value for cheap/copiable, by **non-const reference** for out-params (replace `T* out` + `*out =` pattern).
- Return by value; avoid `void f(T* out)` where a return is natural.
- Mark pure/virtual methods appropriately; `override` on every override.
- `const`-correct member getters; `noexcept` where obvious.

### Systems (function-pointer struct → class)

Today:

```c
struct System transformSystem = { .name="transform", .added=added, .update=update, … };
systemAdd(order, &transformSystem);
```

Target:

```cpp
class TransformSystem : public System {
public:
    void added()  override;
    void removed() override;
    void update()  override;
    void postUpdate() override;
private:
    /* member state replaces file-static globals */
};
```

- `System` becomes an abstract base with virtual `added/removed/update/postUpdate` (empty defaults) + a `const char* name` and an `order` field.
- `ecs.systems` becomes `std::vector<std::unique_ptr<System>>` (owned) or `std::vector<System*>` (non-owned, if lifetime is app-scoped). Decide per ownership model in Phase 2.
- File-static state inside each system `.c` moves into **member variables** of the class.

### Global singletons → owned instances

- `extern Ecs ecs;` and friends become **owned objects** created in `main`/app bootstrap and passed down (composition over ambient globals).
- Pragmatic ceiling: full constructor-injection through every layer is the largest, riskiest item. Target: eliminate _header-defined_ globals and ad-hoc `extern` state; allow a small set of explicitly-owned engine singletons (one instance, clear owner) where threading DI is disproportionate. Track any that remain in the milestone notes.

### Namespaces (applied LAST, cross-cutting)

Namespaces break every consumer, so they cannot be done bottom-up while keeping per-module green builds. Phases 1–4 keep the **global namespace** and do the idiom work (struct→class, RAII, `std::vector`, references, `nullptr`, virtual systems) so each module still builds. Namespaces are introduced in a dedicated final pass (Phase 5), one module at a time, qualifying references across the whole codebase and building green after each module is namespaced:

- `c-utils` → `utils` (e.g. `utils::container`, `utils::memory`)
- `c-engine` → `engine` (e.g. `engine::ecs`, `engine::renderer::vulkan`, `engine::gui`)
- `c-game` → `game` (e.g. `game::azgaar`, `game::player`)

### Style

- Keep existing brace/indent style (Allman-ish, 4-space) to minimize diff noise; only change what the idiom requires.
- No new compiler warnings; `-pedantic-errors` must stay clean.
- Prefer `auto` where the type is obvious; range-for over index loops where mutation isn't needed.
- `std::string`/`std::string_view` replace `char*` buffers in non-hot, non-ABI code (parsing, logging, UI labels). Keep `char*`/fixed buffers where they cross C/third-party APIs (Vulkan, stb, FSR).

---

## Execution phases

### Phase 1 — `c-utils` (~4.5k lines) ✅ done

Foundational; everything depends on it. Order within the module:

1. `container/` — introduce `std::vector`-backed `Array` shim + `Map`→`unordered_map`; re-implement `SparseSet` internals on `std::vector`. **(unlocks everything)**
2. `memorymanager/` — retire buddy allocator behind the STL heap; keep `memoryInit` as a no-op/compat stub for now.
3. `string/`, `file/`, `json/`, `logger/`, `settings/` — `std::string`, RAII file handles, RAII.
4. `thread/`, `futuretask/`, `timer/`, `events/`, `signalcatcher/`, `platform/`, `datamanager/`, `database/`, `image/`, `cfgpath/` — RAII, classes, namespaces.

- **Status:** all `c-utils` modules converted (typedefs→struct/enum, `NULL`→`nullptr`, `char`→`bool`, C-casts→`static_cast`/`reinterpret_cast`/`const_cast`, C99 compound literals removed). **0 c-utils warnings.** stb_ds container macros + `memoryAlloc` kept intact (deferred to Phase 5). `SparseSet`/`SparseSetSimple` internals now on `std::vector`.

- **Gate:** `./scripts/build.sh` green (met).

### Phase 2 — `c-engine` ECS core (bounded, safe subset) ✅ done

- `SparseSet` / `SparseSetSimple` internals → `std::vector` (RAII; public API kept stable, `char`→`bool`). **Done.**
- `Ecs` core modernized: `struct Ecs` (drop typedef aliases), `bool showStats`, `nullptr`. **Done.**
- `System` still a fn-ptr struct for now (see Phase 5e below).

- **Gate:** build green (done; milestone run pending).

> **Scoping discovery:** there are **~40** `struct System … = {…}` definitions across _both_ `c-engine` and `c-game`. Converting `System` to a virtual base class breaks all 40 at once (the base-type change is a compile dependency of every system definition), so it is **not** bottom-up-safe. It is grouped with the other cross-cutting atomic flips in **Phase 5e**.

### Phase 2b — `c-engine` ECS systems (safe idiom subset) ✅ done

All 11 `ecs/system/*` modules converted with the safe idiom subset (typedefs→`struct`, `NULL`→`nullptr`, `char`→`bool`, C-casts→`static_cast`/`reinterpret_cast`/`const_cast`, C99 compound literals removed). The `System` fn-ptr struct + stb_ds containers are **kept** (deferred to 5e / 5a). Build green + runtime smoke test passed after this batch.

- Small/medium: `lua`, `mesh`, `sound`, `light`, `transform`, `physics`, `camera`.
- Large: `heightmap` (HeightmapTerrain/Source/Job typedefs, ~40 numeric casts), `animation` (AnimatorComponent.h ×11 typedefs, AnimationSystem.cpp), `scene` (Scene.h Entity/Scene, SceneParser, SceneSystem), `window` (WindowSystem + X11/GLFW/SDL backends, `WindowBackendApi` ×4).

- **Gate:** build green + `./scripts/run.sh play log 5000` (met — clean boot + shutdown, no errors).

### Phase 3 — `c-engine` renderer + GUI

- `renderer/vulkan/*` (resources, pipeline, passes, swapchain, barriers, scene) — RAII wrappers over Vulkan handles (deferred destruction), classes for passes, `std::vector` for resource arrays. Keep raw `Vk*` at the API boundary; wrap ownership.
- `renderer/{material,texture,decal,gui}/` — classes + RAII.
- **Gate:** build green + **milestone run** with screenshot (`./scripts/run.sh play screenshot /tmp/milestone.jpg`) to catch visual regressions.

### Phase 4 — `c-game` (~17k)

- `game/azgaar/*`, `game/player/*`, `game/enemy/*`, `game/combat/*`, `game/character/*`, GUI systems, menus, navmesh, gameState.
- Same idiom rules; systems → `System` subclasses; gameplay state → owned classes.
- **Gate:** build green + **final milestone run** + screenshot.

### Phase 5 — cross-cutting flip (containers → allocator → namespaces → verify)

Each sub-phase ends green before the next starts.

- **5a — Container flip:** change `Array(T)`→`std::vector<T>`, `Map(K,V)`/`StrMap(T)`→`std::unordered_map`; fix all fallout (public-header signatures + every consumer, `= NULL` inits, `arrayFree`, `if(ptr)` truthiness); remove stb_ds dependency. Build green.
- **5b — Retire allocator:** replace raw `memoryAlloc/Free/Realloc` arrays with `std::vector`/`new`/`std::unique_ptr`; drop the buddy allocator (`memoryInit` becomes a no-op or is removed). Build green.
- **5c — Namespaces:** wrap each module in its namespace (`utils`/`engine`/`game`), qualify references across the codebase, one module at a time. Build green after each.
- **5e — `System` → virtual class (cross-cutting, atomic):** convert the `System` fn-ptr struct to an abstract base with virtual `added/removed/preUpdate/update/postUpdate`; convert all **~40** system definitions (across `c-engine` + `c-game`) to subclasses (free fns → member methods, file-static state → members); `ecs.systems` → owned container; de-globalize `Ecs`. This is one atomic change (the base-type change breaks every system definition at once), so it must land in a single green-ending push. Build green + milestone run.
- **5d — Verify + optional `tools`:** convert `tools/*` builders if desired; final full `./scripts/build.sh` + `./scripts/run.sh` + screenshot; confirm zero warnings and identical rendering.

---

## Risk register & mitigations

| Risk                                                    | Mitigation                                                                                                                                    |
| ------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `std::vector` perf regression in ECS/renderer hot loops | Compat shim lets us keep `std::vector` but back it with a pooled `std::allocator` if profiling shows a regression; measure at milestone runs. |
| Function-pointer → virtual class changes dispatch/ABI   | Confine to Phase 5e (atomic, all ~40 systems at once); keep `System` interface identical; milestone-run after.                                |
| Global-singleton removal is large/risky                 | Allow a bounded set of explicitly-owned engine singletons; document leftovers rather than force full DI.                                      |
| `SparseSet` re-implementation bugs (ECS correctness)    | Keep public API byte-stable; reimplement internals only; milestone run validates entity behavior.                                             |
| Vulkan handle ownership (RAII) mis-ordering at shutdown | Preserve existing destroy-ordering comments/logic; wrap, don't reorder.                                                                       |
| Warning-strict build breaks mid-migration               | Build after each module; never leave a phase red.                                                                                             |
| Designated-initializer reliance (C)                     | Replace with CTAD / constructors / aggregate init as classes are introduced.                                                                  |

## Definition of done

- All three modules compile with **zero warnings** under `-pedantic-errors`.
- No `typedef struct X{…}X`, no raw `malloc/free` ownership pairs, no `NULL`, no stb_ds `Array`/`Map` call sites (shim removed).
- Systems are `System` subclasses; globals replaced by owned instances (documented exceptions allowed).
- Namespaces applied per module.
- Game boots and renders identically (screenshot diff vs. pre-migration baseline) at the final milestone.
