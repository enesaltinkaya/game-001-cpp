#!/bin/bash
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
BUILD_DIR="$ROOT/build"
SCRIPTS="$ROOT/scripts"


# ── CMake configure (once) + build ─────────────────────────────────────────
if [ ! -d "$BUILD_DIR" ]; then
    cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -S "$ROOT" -B "$BUILD_DIR"
fi
cmake --build "$BUILD_DIR"

# ── Pak + shader pipeline ───────────────────────────────────────────────────
# The game binary lives in build/c-game/; paks must land next to it.
export BIN_DIR="$BUILD_DIR/c-game"  # sourced scripts inherit this; not persisted to parent shell
export RELEASE=0
export SCRIPTS_TMP="$SCRIPTS/.tmp"
mkdir -p "$SCRIPTS_TMP"

timer() {
    local label="$1"; shift
    local start=$(date +%s%3N)
    "$@"
    local end=$(date +%s%3N)
    echo "$label took $((end - start)) ms"
}

# Engine: compile shaders → zip pak_0_engine
cd "$ROOT/c-engine"
timer "engine shaders"  . "$SCRIPTS/shaders.sh"
timer "engine data"     . "$SCRIPTS/data.sh"

# Build offline tools
timer "jolt-shape-builder"   "$ROOT/tools/build-jolt-shape-builder.sh"
timer "navmesh-builder"      "$ROOT/tools/build-navmesh-builder.sh"

# Game: blender exports → compile shaders → zip pak_1
cd "$ROOT/c-game"
timer "0-blender-terrain"    python3 "$SCRIPTS/0-blender-terrain.py"
timer "1-blender-scene"      . "$SCRIPTS/1-blender-scene.sh"
timer "2-blender-props"      . "$SCRIPTS/2-blender-props.sh"
timer "build-navmesh"         . "$SCRIPTS/build-navmesh.sh"
timer "game shaders"      . "$SCRIPTS/shaders.sh"
timer "game data"         . "$SCRIPTS/data.sh"
