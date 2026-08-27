#include "Engine.h"
#include "Utils.h"
#include "game/Game.h"

int main(void) {
    utils::utilsInit();
    engine::engineSetGameSystem(&game::gameSystem);
    engine::engineStart();
    utils::utilsDestroy();
}
