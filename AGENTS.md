# AGENTS.md

USE scripts/build.sh to compile sources AND SHADERS.
If there is crash use gdb to find out where. You need ENGINE_DEBUG=1 when running the application via c-game executable.

**Do not run the game executable directly when testing.** Always use `./scripts/run.sh` (or its variants) to launch the game. The script sets up required environment variables, working directory, and asset paths that the bare executable does not configure on its own.

export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
export ENGINE_DEBUG=1
these are important if you want to debug and need to run c-game without run.sh.

## Project overview

- `game-001` is a C++23 game project built with CMake + Ninja.
- Main targets:
  - `c-utils`: shared utility library
  - `c-engine`: engine library built on top of `c-utils`
  - `c-game`: final executable
- Entry point: `c-game/main.cpp`

## Repository layout

- `CMakeLists.txt`: top-level build configuration
- `c-utils/`: utility modules, common headers, memory/platform/helpers
- `c-engine/`: engine systems, renderer, ECS, engine assets in `c-engine/data/`
- `c-game/`: game-specific code and assets in `c-game/data/`
- `scripts/`: build, run, packaging, shader, Blender export, and release scripts
- `docs/`: topic deep-dives (terrain format, navmesh, vegetation, FSR, RenderDoc, OIT bug) — read the relevant doc before working on that topic
- `plans/`: implementation plans / workstreams per subsystem (e.g. `heightmap-terrain.md`, `cpp-migration.md`)
- `tools/`: standalone offline builders (navmesh, terrain-chunker, vegetation, jolt-shape); built by scripts, not part of the game's CMake build
- `build/`: local generated build directory (do not edit manually)

## Build and run

### Debug build

- Configure/build: `./scripts/build.sh`
  - Besides compiling code, it runs the full asset pipeline: Blender exports,
    shader compilation, pak zips, offline tool builds (jolt-shape-builder,
    navmesh-builder) and the navmesh bake (`SKIP_NAVMESH=1` to skip the bake).
- Run: `./scripts/run.sh`
- Optional RenderDoc preload: `./scripts/run.sh renderdoc`
- Skip main menu (go straight to gameplay): `./scripts/run.sh play`
  - With screenshot: `./scripts/run.sh play screenshot /tmp/screenshot.jpg`
  - With log timeout: `./scripts/run.sh play log 10000`

The `play` variant sets `ENGINE_SKIP_MAIN_MENU=1`, which is useful for
automated testing since the game won't wait for main menu input.

### Release packaging

- Linux + Windows release: `./scripts/release.sh`
- Linux only: `./scripts/release.sh linux`
- Windows only: `./scripts/release.sh win`

## Toolchain assumptions

- Compiler: `clang++`
- Language standard: `C++23`
- Generator: `Ninja`
- Build flags are strict: `-Wall -Wextra -Wpedantic -pedantic-errors`
- External prebuilt dependencies are expected under:
  - `/home/enes/Projects/c/cpp-thirdparty`
- Vulkan SDK include paths are hardcoded for Linux/Windows in CMake.

## Important implementation notes

- Precompiled headers are used:
  - `c-utils/Utils.h`
  - `c-engine/pch.h` (wraps third-party C headers like Lua in `extern "C"` guards)
- Code is organized into per-module namespaces: `utils::`, `engine::`, `game::`.
- The codebase has been migrated to idiomatic modern C++ (STL containers, RAII,
  classes with access control, virtual-method `System` base class) — see
  `plans/cpp-migration.md` for decisions. Keep new code idiomatic; do not
  reintroduce C idioms (raw arrays, global singletons, macro-based containers).
- `c-engine` links publicly to `c-utils`.
- `c-game` links the engine plus a large set of prebuilt static third-party libraries.
- Asset packaging is part of the normal build workflow via scripts; building code alone may not be enough to run the game.

## Conventions for contributors and coding agents

- Read existing code before changing structure or conventions.
- Prefer small, focused changes over broad refactors.
- Do not edit generated content in `build/`.
- Keep CMake changes minimal and consistent with the current style.
- Preserve hardcoded toolchain/dependency paths unless the task explicitly asks to change them.
- When adding source files, use the `.cpp` extension (headers stay `.h`) and ensure they live under the appropriate module (`c-utils`, `c-engine`, or `c-game`).
- Be careful with shared headers because `Utils.h` and `pch.h` are precompiled and widely included.
- Avoid introducing compiler warnings; the project is configured to be warning-strict.

## Validation guidance

When making code changes, prefer validating with:

1. `./scripts/build.sh`
2. `./scripts/run.sh` if runtime verification is needed

### Visual verification via screenshot

When a change affects rendering (shaders, passes, materials, lighting, etc.),
you can capture and inspect the final frame:

Read scripts/run.sh to learn how to take a screenshot. Use the `play` variant
to skip the main menu and capture gameplay directly:

```bash
./scripts/run.sh play screenshot /tmp/screenshot.jpg
```

To capture frames without any in-world UI overlays (HUD, compass, zone,
camera/player debug GUIs), set `ENGINE_HIDE_GUI=1` before running — it is
inherited through `run.sh`:

```bash
ENGINE_HIDE_GUI=1 ./scripts/run.sh play screenshot /tmp/clean.jpg
```

### Saving Vulkan images to disk

- `vulkanScreenshot(path)` — saves the current swapchain image as JPG.
- `vulkanSaveImage(img, path)` — saves any `VulkanImage` (render targets, depth buffers, etc.) as JPG.
  - Float formats are auto-scaled to 0–255 per channel.
  - Integer formats get BGRA→RGBA swizzle for swapchain-like formats.

### Log analysis via timeout

For debugging crashes or investigating runtime issues, run the game with a
timeout so it automatically exits and produces a complete log file:

```bash
./scripts/run.sh log           # runs 5 seconds, then exits
./scripts/run.sh log 3000      # runs 3 seconds, then exits
./scripts/run.sh log && cat build/c-game/data/game.log
```

The engine checks `ENGINE_LOG_TIMEOUT` on startup and breaks from the main
loop after the specified duration (in ms). This follows the same env-var
pattern used for screenshots (`ENGINE_SCREENSHOT` / `ENGINE_LOG_TIMEOUT`).

**Important:** always use a timeout of at least `5000` ms when running with
the `play` variant (e.g. `./scripts/run.sh play log 5000`). The game needs
this time to finish loading assets after the main-menu skip. Exiting during
asset loading causes a crash.

## Terrain (heightmap streaming)

For any work involving terrain, heightmaps, or the Azgaar world, read
`plans/heightmap-terrain.md` (architecture and phases) and
`docs/azgaar-terrain.md` (Azgaar `.map` format) first.

- The terrain surface is **not a mesh**: an implicit lattice enumerated in the
  vertex shader, lifted from a per-tile R32F height texture (512² per
  2048 m tile). The experimental full-mesh terrain pass was removed in the
  heightmap cutover.
- Engine side: `c-engine/ecs/system/heightmap/HeightmapTerrain.h` (streaming
  tile service around the camera; O(1) access via `heightmapTerrainGetActive()`)
  plus the render pass in `c-engine/renderer/vulkan/pass/heightmap_terrain/`.
- Game side: `c-game/game/azgaar/AzgaarHeightmapSource.h` implements the
  engine's `HeightmapSource` vtable over the parsed `.map` file (`AzgaarWorld`);
  the world loads through the `loadingAzgaar` game state.
- Physics: a Jolt heightfield collision body per tile is generated from the
  same heights, so the character controller walks exactly on the rendered
  surface.
- **Determinism contract:** tile data is never persisted to disk. Evicted
  tiles are regenerated bit-identically from the source, so `heightAt` must
  stay a pure function of (source, wx, wz).

## Navmesh (Recast/Detour)

For AI pathfinding work, read `docs/recast-navmesh.md` first.

- Runtime: `c-game/game/navmesh/NavMeshSystem.h` wraps the prebuilt Recast C
  API (wrapper lives in `cpp-thirdparty/recast/wrapper/`).
- Navmeshes are baked offline by `tools/navmesh-builder` via
  `scripts/build-navmesh.sh` (run as part of `./scripts/build.sh`). Set
  `SKIP_NAVMESH=1` to reuse the last baked navmesh while iterating on
  non-navigation changes.

## Vegetation / world population

Read `docs/vegetation-map.md` (painted UDIM placement textures) and
`plans/azgaar-world-population.md` before touching object placement. The CPU
side is `c-game/game/azgaar/AzgaarProps.h` (deterministic per-tile scatter
pushed to the domain-agnostic `azgaar_props` render pass); standalone helpers
live in `tools/` (`build-vegetation-builder.sh`, `build-terrain-chunker.sh`).

## Renderer gotchas

- OIT / AMD DCC: before touching the OIT reveal pass, `R8_UNORM` multiplicative
  blending, or renderpass fast-clears, read `docs/oit-amd-dcc.md` — AMD GPUs
  intermittently mis-decompress DCC tiles under exactly that combination
  (black-square flicker on transparent objects).

## FSR 3.1 (upscaling)

For any work involving FSR, upscaling, or the FidelityFX SDK, read
`docs/fsr3.1.md` first. It documents the static library build, source
patches, volk integration, and the C API usage.

The library source and build script live at:
`/home/enes/Projects/c/cpp-thirdparty/fsr3.1/`

## RenderDoc (frame capture & inspection)

For any work involving RenderDoc, frame captures, or GPU debugging, read
`docs/renderdoc-capture.md` first. It documents the local v1.46-dev build,
the implicit-layer hooking setup (required because the engine uses volk),
the programmatic capture env vars, and the headless Python replay recipe.

Quick start (debug builds, Linux):

```bash
# Headless capture; .rdc lands in /tmp/RenderDoc/ (~1 GB each, clean up old ones)
ENGINE_RENDERDOC_CAPTURE=1 ENGINE_RENDERDOC_CAPTURE_DELAY_MS=6000 \
ENGINE_SKIP_MAIN_MENU=1 ENGINE_LOG_TIMEOUT=12000 \
./scripts/run.sh renderdoc

# Inspect:
qrenderdoc /tmp/RenderDoc/c-game_*.rdc          # GUI
PYTHONPATH=/home/enes/Apps/renderdoc/build/lib python3 <script>   # headless replay API
```

- `run.sh renderdoc` sets `LD_PRELOAD` **and** the implicit layer (`ENABLE_VULKAN_RENDERDOC_CAPTURE=1`) — both are required, since volk's dlopen/dlsym bypasses plain symbol interposition.
- The trigger delay must land in gameplay: a low value captures the asset-loading phase (0 draw calls).
- `renderdoc` cannot be combined with the `play` sub-arg (all sub-args test `$1`), hence the explicit `ENGINE_SKIP_MAIN_MENU` / `ENGINE_LOG_TIMEOUT` above.

## Camera

Key files:

- `c-engine/ecs/system/camera/CameraComponent.h` — `Camera` component and `CameraUbo` (view/projection matrices, jitter, exposure, frustum planes).
- `c-engine/ecs/system/camera/CameraSystem.h/.cpp` — ECS system that updates the active camera each frame (`cameraGetEntity()` returns the current camera entity).
- `c-engine/ecs/system/camera/flyingCamera/FlyingCamera.h/.cpp` — debug flying-camera controller.
- `c-game/game/cameraGui/CameraGui.h/.cpp` — in-game camera debug GUI.
- `c-game/game/player/Player.h/.cpp` — player-side camera setup.

## Files to inspect first for most tasks

- `CMakeLists.txt`
- `c-game/main.cpp`
- `c-utils/Utils.h`
- `c-engine/Engine.h`
- relevant module `CMakeLists.txt`
- relevant script in `scripts/`
