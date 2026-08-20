#!/usr/bin/env bash
export PROJECT_NAME=c-game
DOCKER=0
export RELEASE=1

ROOT="$(dirname "$(realpath "$0")")/.."
SCRIPTS="$ROOT/scripts"
export SCRIPTS_TMP="$SCRIPTS/.tmp"
export BIN_DIR="$ROOT/release"

rm -rf "$BIN_DIR"
mkdir -p "$BIN_DIR/data"
mkdir -p "$SCRIPTS_TMP"

cp -r "$ROOT/licenses" "$BIN_DIR/licenses"

# ── Pak + shader pipeline (release variants) ────────────────────────────────
# Engine: compile .spv.release shaders → zip pak_0_engine-release.pak
cd "$ROOT/c-engine"
. "$SCRIPTS/shaders.sh"
. "$SCRIPTS/data.sh"

# Game: compile .spv.release shaders → zip pak_1.pak
cd "$ROOT/c-game"
. "$SCRIPTS/1-blender-scene.sh"
. "$SCRIPTS/shaders.sh"
. "$SCRIPTS/data.sh"

# ── Compile ─────────────────────────────────────────────────────────────────

function build_rmlui_wrapper_linux() {
    local wrapperRoot="/home/enes/Projects/c/cpp-thirdparty/rmlui/wrapper"
    local wrapperBuild="$wrapperRoot/build-linux"
    cmake -S "$wrapperRoot" -B "$wrapperBuild" -GNinja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$wrapperBuild"
}

function build_rmlui_wrapper_win() {
    local wrapperRoot="/home/enes/Projects/c/cpp-thirdparty/rmlui/wrapper"
    local wrapperBuild="$wrapperRoot/build-win"
    cmake -S "$wrapperRoot" -B "$wrapperBuild" -GNinja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$SCRIPTS/mingw.cmake"
    cmake --build "$wrapperBuild"
}
function linux() {
    local buildDir="$BIN_DIR/linux"

    build_rmlui_wrapper_linux
    cmake -GNinja -DCMAKE_BUILD_TYPE=Release -S "$ROOT" -B "$buildDir"
    cmake --build "$buildDir"

    mv "$buildDir/c-game/$PROJECT_NAME" "$BIN_DIR/$PROJECT_NAME.linux"
    rm -rf "$buildDir"
}

function win() {
    local buildDir="$BIN_DIR/win"
    export WIN32=1
    export _WINDOWS=1

    build_rmlui_wrapper_win
    cmake -GNinja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$SCRIPTS/mingw.cmake" -S "$ROOT" -B "$buildDir"
    cmake --build "$buildDir"

    mv "$buildDir/c-game/$PROJECT_NAME.exe" "$BIN_DIR/"
    rm -rf "$buildDir"

    mingw=/home/enes/Sdks/llvm-mingw-20250402-ucrt-ubuntu-20.04-x86_64
    cp "${mingw}/x86_64-w64-mingw32/bin/libomp.dll" "$BIN_DIR/"
}

start=$(date +%s%N)

if [[ $1 == linux ]]; then
    linux
elif [[ $1 == win ]]; then
    win
else
    linux
    win
fi

end=$(date +%s%N)

echo "---------"
printf "compiled in %'.f ms\n" $(( (end - start) / 1000000 ))
echo "---------"
