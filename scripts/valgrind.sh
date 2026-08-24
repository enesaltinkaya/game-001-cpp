#!/usr/bin/bash
set -e
export PROJECT_NAME=c-game
export ENGINE_DEBUG=1
clear
scripts/build.sh

if [[ $? -eq 0 ]] ; then
   valgrind --leak-check=full build/${PROJECT_NAME}
fi
