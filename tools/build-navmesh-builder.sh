#!/bin/bash
# Build the navmesh-builder tool (standalone, outside the game's CMake build).
set -e

TOOLS_DIR="$(dirname "$(realpath "$0")")"
THIRDPARTY="/home/enes/project/c/cpp-thirdparty"
TOOL_DIR="$TOOLS_DIR/navmesh-builder"
SRC="$TOOL_DIR/main.cpp"
OUT="$TOOL_DIR/navmesh-builder"

# Skip if already up-to-date
if [ "$OUT" -nt "$SRC" ] 2>/dev/null; then
    exit 0
fi

echo "building navmesh-builder..."
clang++ -std=c++17 -O2 -DNDEBUG \
    -I"$THIRDPARTY/cgltf/git" \
    -I"$THIRDPARTY/meshoptimizer/git/src" \
    -I"$THIRDPARTY/recast/git/Recast/Include" \
    -I"$THIRDPARTY/recast/git/Detour/Include" \
    -o "$OUT" \
    "$SRC" \
    "$THIRDPARTY/recast/git/build-linux/Recast/libRecast.a" \
    "$THIRDPARTY/recast/git/build-linux/Detour/libDetour.a" \
    "$THIRDPARTY/meshoptimizer/git/build-linux/libmeshoptimizer.a" \
    -lm \
    -pthread

echo "navmesh-builder built: $OUT"
