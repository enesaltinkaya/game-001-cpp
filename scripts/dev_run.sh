#!/bin/bash
# Fast iteration run: launches the already-built c-game with the same env as
# run.sh, but skips build.sh (use ./scripts/dev_taa.sh to refresh shaders).
# Usage:
#   ./scripts/dev_run.sh play screenshot /tmp/taa_test 8
#   ./scripts/dev_run.sh play log 5000
set -e

ROOT="$(dirname "$(realpath "$0")")/.."

export ENGINE_DEBUG=1
export ASAN_OPTIONS=symbolize=1:detect_leaks=1:check_initialization_order=1
export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer
export LSAN_OPTIONS=suppressions="$ROOT/scripts/lsan_suppress.txt"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json

if [[ $1 == "play" ]]; then
    export ENGINE_SKIP_MAIN_MENU=1
    shift
fi

if [[ $1 == "screenshot" ]]; then
    export ENGINE_SCREENSHOT="${2:-/tmp/screenshot.jpg}"
    export ENGINE_SCREENSHOT_COUNT="${3:-1}"
elif [[ $1 == "log" ]]; then
    export ENGINE_LOG_TIMEOUT="${2:-5000}"
    export ENGINE_LOG_FILE="${3:-build/c-game/data/game.log}"
fi

# NOTE: must run from the repo root: the scene parser writes debug JSON to
# scripts/gltf-json-debug/ relative to the CWD (same as run.sh).
exec "$ROOT/build/c-game/c-game" "$@"
