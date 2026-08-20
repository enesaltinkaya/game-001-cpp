#!/bin/bash
set -uo pipefail

if [ ! -d "./data" ]; then
    echo "where is data dir?"
    exit 1
fi

TOOLS_DIR="$(dirname "$(realpath "${BASH_SOURCE[0]}")")/../tools"
NAVMESH_BUILDER="$TOOLS_DIR/navmesh-builder/navmesh-builder"
SCRIPTS_TMP="${SCRIPTS_TMP:-$(dirname "$(realpath "${BASH_SOURCE[0]}")")/.tmp}"
mkdir -p "$SCRIPTS_TMP"

terrainDir="data/pak_1/models/terrain"
modelsDir="data/pak_1/models"
outputNav="$modelsDir/combined.nav.dat"

# Dev productivity escape hatch: keep using the last baked navmesh while
# iterating on blend/material/animation changes that do not affect navigation.
if [ "${SKIP_NAVMESH:-${NAVMESH_SKIP:-0}}" = "1" ]; then
    if [ -f "$outputNav" ]; then
        echo "navmesh: skipped (SKIP_NAVMESH=1, using existing $outputNav)"
        return 0
    fi
    echo "navmesh: SKIP_NAVMESH=1 requested, but $outputNav does not exist"
fi

if [ ! -d "$terrainDir" ]; then
    echo "no terrain dir: $terrainDir"
    exit 1
fi

if [ ! -f "$NAVMESH_BUILDER" ]; then
    echo "navmesh-builder not found: $NAVMESH_BUILDER"
    exit 1
fi

# Keep these values in one place so the stamp changes when navmesh quality/config changes.
NAVMESH_CELL_SIZE_VALUE="${NAVMESH_CELL_SIZE:-0.3}"
NAVMESH_CELL_HEIGHT_VALUE="${NAVMESH_CELL_HEIGHT:-0.2}"
NAVMESH_AGENT_RADIUS_VALUE="${NAVMESH_AGENT_RADIUS:-0.5}"
NAVMESH_EDGE_MAX_LEN_VALUE="${NAVMESH_EDGE_MAX_LEN:-12.0}"
NAVMESH_EDGE_MAX_ERROR_VALUE="${NAVMESH_EDGE_MAX_ERROR:-0.3}"
NAVMESH_DETAIL_SAMPLE_DIST_VALUE="${NAVMESH_DETAIL_SAMPLE_DIST:-2.0}"
NAVMESH_DETAIL_SAMPLE_MAX_ERROR_VALUE="${NAVMESH_DETAIL_SAMPLE_MAX_ERROR:-0.25}"
NAVMESH_TILE_SIZE_VALUE="${NAVMESH_TILE_SIZE:-512}"
NAVMESH_BOUNDS_VALUE="${NAVMESH_BOUNDS:--1000,0,1000,300,900,2250}"

stampFile="$SCRIPTS_TMP/build-navmesh.stamp"

# Use a content signature instead of mtimes. Blender/export scripts often rewrite
# files even when the relevant bytes did not change; mtime-only checks caused
# unnecessary 100s rebakes.
signatureFile="$SCRIPTS_TMP/build-navmesh.signature-input"
{
    printf 'builder-main '; sha256sum "$TOOLS_DIR/navmesh-builder/main.cpp" 2>/dev/null || true
    printf 'builder-bin '; sha256sum "$NAVMESH_BUILDER" 2>/dev/null || true
    printf 'script '; sha256sum "$(realpath "${BASH_SOURCE[0]}")" 2>/dev/null || true
    printf 'cfg cellSize=%s cellHeight=%s radius=%s edgeLen=%s edgeErr=%s detailDist=%s detailErr=%s tileSize=%s bounds=%s\n' \
        "$NAVMESH_CELL_SIZE_VALUE" \
        "$NAVMESH_CELL_HEIGHT_VALUE" \
        "$NAVMESH_AGENT_RADIUS_VALUE" \
        "$NAVMESH_EDGE_MAX_LEN_VALUE" \
        "$NAVMESH_EDGE_MAX_ERROR_VALUE" \
        "$NAVMESH_DETAIL_SAMPLE_DIST_VALUE" \
        "$NAVMESH_DETAIL_SAMPLE_MAX_ERROR_VALUE" \
        "$NAVMESH_TILE_SIZE_VALUE" \
        "$NAVMESH_BOUNDS_VALUE"
    for f in "$modelsDir"/*.dat "$terrainDir"/*.dat; do
        [ -f "$f" ] || continue
        case "$f" in *.nav.dat|*.jolt.dat) continue ;; esac
        printf 'asset %s ' "$f"
        sha256sum "$f"
    done
} > "$signatureFile"
currentStamp=$(sha256sum "$signatureFile" | awk '{print $1}')

if [ -f "$stampFile" ] && [ -f "$outputNav" ]; then
    savedStamp=$(cat "$stampFile")
    if [ "$savedStamp" = "$currentStamp" ]; then
        echo "navmesh: up to date"
        return 0
    fi
fi

tmpDir=$(mktemp -d /tmp/build-navmesh.XXXXXX)
trap 'rm -rf "$tmpDir"' EXIT

obstacles=()

for sceneDat in "$modelsDir"/*.dat; do
    [ -f "$sceneDat" ] || continue
    case "$sceneDat" in *.nav.dat|*.jolt.dat) continue ;; esac
    sceneStem=$(basename "$sceneDat" .dat)
    tmpScene="$tmpDir/${sceneStem}.glb"
    if zstd -q -d -f "$sceneDat" -o "$tmpScene" 2>/dev/null; then
        echo "obstacle: $sceneStem"
        obstacles+=("$tmpScene")
    else
        echo "warn: could not decompress $sceneDat"
    fi
done

if [ ${#obstacles[@]} -eq 0 ]; then
    echo "no scene obstacles found"
    return 0
fi

for terrainDat in "$terrainDir"/*.dat; do
    [ -f "$terrainDat" ] || continue
    case "$terrainDat" in *.nav.dat|*.jolt.dat) continue ;; esac
    terrainStem=$(basename "$terrainDat" .dat)
    tmpTerrain="$tmpDir/${terrainStem}.glb"
    if ! zstd -q -d -f "$terrainDat" -o "$tmpTerrain" 2>/dev/null; then
        echo "warn: could not decompress $terrainDat"
        continue
    fi

    tmpNav="$tmpDir/combined.nav"
    echo -n "combined navmesh (${terrainStem} + ${#obstacles[@]} scenes)... "

    export NAVMESH_CELL_SIZE="$NAVMESH_CELL_SIZE_VALUE"
    export NAVMESH_CELL_HEIGHT="$NAVMESH_CELL_HEIGHT_VALUE"
    export NAVMESH_AGENT_RADIUS="$NAVMESH_AGENT_RADIUS_VALUE"
    export NAVMESH_EDGE_MAX_LEN="$NAVMESH_EDGE_MAX_LEN_VALUE"
    export NAVMESH_EDGE_MAX_ERROR="$NAVMESH_EDGE_MAX_ERROR_VALUE"
    export NAVMESH_DETAIL_SAMPLE_DIST="$NAVMESH_DETAIL_SAMPLE_DIST_VALUE"
    export NAVMESH_DETAIL_SAMPLE_MAX_ERROR="$NAVMESH_DETAIL_SAMPLE_MAX_ERROR_VALUE"
    export NAVMESH_TILE_SIZE="$NAVMESH_TILE_SIZE_VALUE"
    if [ -n "$NAVMESH_BOUNDS_VALUE" ]; then
        export NAVMESH_BOUNDS="$NAVMESH_BOUNDS_VALUE"
    else
        unset NAVMESH_BOUNDS
    fi
    "$NAVMESH_BUILDER" "$tmpTerrain" "$tmpNav" --obstacles "${obstacles[@]}" 2>&1 || true
    unset NAVMESH_BOUNDS NAVMESH_TILE_SIZE NAVMESH_DETAIL_SAMPLE_MAX_ERROR NAVMESH_DETAIL_SAMPLE_DIST NAVMESH_EDGE_MAX_ERROR NAVMESH_EDGE_MAX_LEN NAVMESH_AGENT_RADIUS NAVMESH_CELL_HEIGHT NAVMESH_CELL_SIZE

    if [ -f "$tmpNav" ]; then
        zstd -q -10 --rm -f "$tmpNav"
        mv "$tmpDir/combined.nav.zst" "$outputNav"
        echo "ok (`du -sh "$outputNav" | cut -f1`)"
    else
        echo "no change"
    fi
    break
done

echo -n "$currentStamp" > "$stampFile"
