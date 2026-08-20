#!/bin/bash
set -e

ROOT="$(dirname "$(realpath "$0")")/.."
clear

export ENGINE_DEBUG=1

"$ROOT/scripts/build.sh"

export ASAN_OPTIONS=symbolize=1:detect_leaks=1:check_initialization_order=1
export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer
export LSAN_OPTIONS=suppressions="$ROOT/scripts/lsan_suppress.txt"
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json

export LD_PRELOAD=/home/enes/Sdks/renderdoc_2026_03_26_18e6d687/lib/librenderdoc.so

"$ROOT/build/c-game/c-game"
