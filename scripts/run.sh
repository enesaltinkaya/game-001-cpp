#!/bin/bash
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
clear

export ENGINE_DEBUG=1

"$ROOT/scripts/build.sh"

export ASAN_OPTIONS=symbolize=1:detect_leaks=1:check_initialization_order=1
export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer
export LSAN_OPTIONS=suppressions="$ROOT/scripts/lsan_suppress.txt"

if [[ $1 == "renderdoc" ]]; then
    # App-side API (TriggerCapture) via preload; hooks come from the implicit
    # Vulkan layer (required: the engine loads Vulkan entry points through
    # volk dlopen/dlsym, which plain symbol interposition cannot intercept).
    # Both use the same library file, so only one RenderDoc instance maps.
    export LD_PRELOAD=/home/enes/Apps/renderdoc/build/lib/librenderdoc.so
    export ENABLE_VULKAN_RENDERDOC_CAPTURE=1
fi

if [[ $1 == "screenshot" ]]; then
    export ENGINE_SCREENSHOT="${2:-/tmp/screenshot.jpg}"
    export ENGINE_SCREENSHOT_COUNT="${3:-1}"
fi

if [[ $1 == "log" ]]; then
    export ENGINE_LOG_TIMEOUT="${2:-5000}"
    export ENGINE_LOG_FILE="${3:-build/c-game/data/game.log}"
fi

# play: skip main menu, go straight to gameplay
# Usage: ./scripts/run.sh play [screenshot|log]
if [[ $1 == "play" ]]; then
    export ENGINE_SKIP_MAIN_MENU=1
    shift
    # fall through to handle screenshot/log sub-args
    if [[ $1 == "screenshot" ]]; then
        export ENGINE_SCREENSHOT="${2:-/tmp/screenshot.jpg}"
        export ENGINE_SCREENSHOT_COUNT="${3:-1}"
    elif [[ $1 == "log" ]]; then
        export ENGINE_LOG_TIMEOUT="${2:-5000}"
        export ENGINE_LOG_FILE="${3:-build/c-game/data/game.log}"
    fi
fi

# export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
"$ROOT/build/c-game/c-game"
