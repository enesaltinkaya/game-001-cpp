#!/bin/bash
# Build the terrain-chunker tool (standalone, outside the game's CMake build).
set -e

TOOLS_DIR="$(dirname "$(realpath "$0")")"
ROOT="$TOOLS_DIR/.."
THIRDPARTY="/home/enes/Projects/c/cpp-thirdparty"
SRC="$ROOT/tools/terrain-chunker/main.c"
OUT="$ROOT/tools/terrain-chunker/terrain-chunker"

# Skip if already up-to-date
if [ "$OUT" -nt "$SRC" ] 2>/dev/null; then
    exit 0
fi

echo "building terrain-chunker..."
clang -O2 -std=c17 -I"$THIRDPARTY/cgltf/git" -I"$THIRDPARTY/meshoptimizer/git/src" \
    -o "$OUT" \
    "$SRC" \
    "$THIRDPARTY/meshoptimizer/git/build-linux/libmeshoptimizer.a" \
    -lm

echo "terrain-chunker built: $OUT"
