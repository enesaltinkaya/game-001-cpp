#!/bin/bash
# Fast dev loop for TAA / scene / props shaders: recompile with glslc and
# refresh the engine pak entry without a full build.
# Usage:
#   ./scripts/dev_taa.sh taa            # taa.comp
#   ./scripts/dev_taa.sh scene_depth    # scene_depth.vert + scene_depth.frag
#   ./scripts/dev_taa.sh props_depth    # azgaar_props_depth.vert + .frag
#   ./scripts/dev_taa.sh props          # azgaar_props.vert + .frag
set -e
ROOT="$(dirname "$(realpath "$0")")/.."
ENGINE="$ROOT/c-engine"
PAK="$ROOT/build/c-game/data/pak_0_engine.pak"
SRC="$ENGINE/data/pak_0_engine/shaders"
INC="$SRC/includes"

compile_one() {
    local src="$1"   # e.g. pass/taa/taa.comp
    local rel="$2"   # pak-relative spv path, e.g. pass/taa/spv/taa.comp.spv.debug
    local spv
    spv="$SRC/${src%/*}/spv/${src##*/}.spv.debug"
    mkdir -p "$(dirname "$spv")"
    echo "compiling $src"
    glslc "$SRC/$src" -o "$spv" -I "$INC"
    ( cd "$ENGINE/data/pak_0_engine" && zip -q "$PAK" "$rel" )
    echo "updated pak: $rel"
}

case "${1:-taa}" in
    taa)           compile_one "pass/taa/taa.comp" "shaders/pass/taa/spv/taa.comp.spv.debug" ;;
    scene_depth)   compile_one "pass/scene/scene_depth.vert" "shaders/pass/scene/spv/scene_depth.vert.spv.debug"
                   compile_one "pass/scene/scene_depth.frag" "shaders/pass/scene/spv/scene_depth.frag.spv.debug" ;;
    props_depth)   compile_one "pass/azgaar_props/azgaar_props_depth.vert" "shaders/pass/azgaar_props/spv/azgaar_props_depth.vert.spv.debug"
                   compile_one "pass/azgaar_props/azgaar_props_depth.frag" "shaders/pass/azgaar_props/spv/azgaar_props_depth.frag.spv.debug" ;;
    props)         compile_one "pass/azgaar_props/azgaar_props.vert" "shaders/pass/azgaar_props/spv/azgaar_props.vert.spv.debug"
                   compile_one "pass/azgaar_props/azgaar_props.frag" "shaders/pass/azgaar_props/spv/azgaar_props.frag.spv.debug" ;;
    composite)     compile_one "pass/composite/composite.comp" "shaders/pass/composite/spv/composite.comp.spv.debug" ;;
    *) echo "unknown target '$1'"; exit 1 ;;
esac
