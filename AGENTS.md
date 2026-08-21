# AGENTS.md

USE scripts/build.sh to compile sources AND SHADERS.
If there is crash use gdb to find out where. You need ENGINE_DEBUG=1 when running the application via c-game executable.

**Do not run the game executable directly when testing.** Always use `./scripts/run.sh` (or its variants) to launch the game. The script sets up required environment variables, working directory, and asset paths that the bare executable does not configure on its own.

export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
export ENGINE_DEBUG=1
these are important if you want to debug and need to run c-game without run.sh.

## Project overview

- `game-001` is a C23 game project built with CMake + Ninja.
- Main targets:
  - `c-utils`: shared utility library
  - `c-engine`: engine library built on top of `c-utils`
  - `c-game`: final executable
- Entry point: `c-game/main.c`

## Repository layout

- `CMakeLists.txt`: top-level build configuration
- `c-utils/`: utility modules, common headers, memory/platform/helpers
- `c-engine/`: engine systems, renderer, ECS, engine assets in `c-engine/data/`
- `c-game/`: game-specific code and assets in `c-game/data/`
- `scripts/`: build, run, packaging, shader, Blender export, and release scripts
- `build/`: local generated build directory (do not edit manually)

## Build and run

### Debug build

- Configure/build: `./scripts/build.sh`
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

- Compiler: `clang`
- Language standard: `C23`
- Generator: `Ninja`
- Build flags are strict: `-Wall -Wextra -Wpedantic -pedantic-errors`
- External prebuilt dependencies are expected under:
  - `/home/enes/Projects/c/cpp-thirdparty`
- Vulkan SDK include paths are hardcoded for Linux/Windows in CMake.

## Important implementation notes

- Precompiled headers are used:
  - `c-utils/Utils.h`
  - `c-engine/pch.h`
- `c-engine` links publicly to `c-utils`.
- `c-game` links the engine plus a large set of prebuilt static third-party libraries.
- Asset packaging is part of the normal build workflow via scripts; building code alone may not be enough to run the game.

## Conventions for contributors and coding agents

- Read existing code before changing structure or conventions.
- Prefer small, focused changes over broad refactors.
- Do not edit generated content in `build/`.
- Keep CMake changes minimal and consistent with the current style.
- Preserve hardcoded toolchain/dependency paths unless the task explicitly asks to change them.
- When adding source files, ensure they live under the appropriate module (`c-utils`, `c-engine`, or `c-game`).
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

## Camera

Key files:

- `c-engine/ecs/system/camera/CameraComponent.h` — `Camera` component and `CameraUbo` (view/projection matrices, jitter, exposure, frustum planes).
- `c-engine/ecs/system/camera/CameraSystem.h/.c` — ECS system that updates the active camera each frame (`cameraGetEntity()` returns the current camera entity).
- `c-engine/ecs/system/camera/flyingCamera/FlyingCamera.h/.c` — debug flying-camera controller.
- `c-game/game/cameraGui/CameraGui.h/.c` — in-game camera debug GUI.
- `c-game/game/player/Player.h/.c` — player-side camera setup.

## Files to inspect first for most tasks

- `CMakeLists.txt`
- `c-game/main.c`
- `c-utils/Utils.h`
- `c-engine/Engine.h`
- relevant module `CMakeLists.txt`
- relevant script in `scripts/`
