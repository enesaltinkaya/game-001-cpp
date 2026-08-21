#!/usr/bin/bash
set -e
export PROJECT_NAME=c-game

clear
scripts/release.sh

if [[ $? -eq 0 ]] ; then
  export ASAN_OPTIONS=symbolize=1:detect_leaks=1:check_initialization_order=1
  export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer
  export LSAN_OPTIONS=suppressions=scripts/lsan_suppress.txt
  if [[ $1 == "renderdoc" ]]; then
    export LD_PRELOAD=/home/enes/Apps/renderdoc/build/lib/librenderdoc.so
    export ENABLE_VULKAN_RENDERDOC_CAPTURE=1
  fi
  release/${PROJECT_NAME}.linux
fi
