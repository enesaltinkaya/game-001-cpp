#!/bin/bash
# Build the vegetation-builder tool (standalone, outside the game's CMake build).
set -e

TOOLS_DIR="$(dirname "$(realpath "$0")")"
THIRDPARTY="/home/enes/project/c/cpp-thirdparty"
TOOL_DIR="$TOOLS_DIR/vegetation-builder"
SRC="$TOOL_DIR/main.cpp"
OUT="$TOOL_DIR/vegetation-builder"

# Skip if already up-to-date
if [ "$OUT" -nt "$SRC" ] 2>/dev/null; then
    exit 0
fi

echo "building vegetation-builder..."
clang++ -std=c++17 -O2 -DNDEBUG \
    -I"$THIRDPARTY/cgltf/git" \
    -I"$THIRDPARTY/meshoptimizer/git/src" \
    -I"$THIRDPARTY/stb/git" \
    -o "$OUT" \
    "$SRC" \
    "$THIRDPARTY/meshoptimizer/git/build-linux/libmeshoptimizer.a" \
    -lpthread -lm

echo "vegetation-builder built: $OUT"
