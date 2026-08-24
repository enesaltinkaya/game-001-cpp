#!/bin/bash
# Build the navmesh-tester tool.
set -e

TOOLS_DIR="$(dirname "$(realpath "$0")")"
THIRDPARTY="/home/enes/Projects/c/cpp-thirdparty"
TOOL_DIR="$TOOLS_DIR/navmesh-tester"
SRC="$TOOL_DIR/main.cpp"
OUT="$TOOL_DIR/navmesh-tester"

# Skip if already up-to-date
if [ "$OUT" -nt "$SRC" ] 2>/dev/null; then
    exit 0
fi

echo "building navmesh-tester..."
clang++ -std=c++17 -O2 \
    -I"$THIRDPARTY/recast/wrapper/src" \
    -I"$THIRDPARTY/recast/git/Recast/Include" \
    -I"$THIRDPARTY/recast/git/Detour/Include" \
    -I"$THIRDPARTY/zstd/git/lib" \
    -o "$OUT" \
    "$SRC" \
    "$THIRDPARTY/recast/wrapper/build-linux/libcrecast.a" \
    "$THIRDPARTY/recast/git/build-linux/Recast/libRecast.a" \
    "$THIRDPARTY/recast/git/build-linux/Detour/libDetour.a" \
    "$THIRDPARTY/zstd/git/build-linux/lib/libzstd.a" \
    -lm

echo "navmesh-tester built: $OUT"
