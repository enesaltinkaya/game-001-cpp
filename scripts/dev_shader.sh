#!/bin/bash
# Fast dev loop: recompile a heightmap_terrain shader + refresh the engine pak entry.
# Usage: ./scripts/dev_shader.sh [frag|vert|all]
set -e
ROOT="$(dirname "$(realpath "$0")")/.."
ENGINE="$ROOT/c-engine"
PAK="$ROOT/build/c-game/data/pak_0_engine.pak"
SRC="$ENGINE/data/pak_0_engine/shaders/pass/heightmap_terrain"
INC="$ENGINE/data/pak_0_engine/shaders/includes"

compile_one() {
    local name="$1"
    echo "compiling $name"
    glslc "$SRC/$name" -o "$SRC/spv/$name.spv.debug" -I "$INC"
    ( cd "$ENGINE/data/pak_0_engine" && zip -q "$PAK" "shaders/pass/heightmap_terrain/spv/$name.spv.debug" )
    echo "updated pak: $name"
}

case "${1:-all}" in
    frag) compile_one "heightmap_terrain.frag" ;;
    vert) compile_one "heightmap_terrain.vert" ;;
    all)  compile_one "heightmap_terrain.frag"; compile_one "heightmap_terrain.vert" ;;
    *) echo "unknown target '$1'"; exit 1 ;;
esac