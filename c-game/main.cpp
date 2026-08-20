#include "Engine.h"
#include "Utils.h"
#include "game/Game.h"
#include "memorymanager/MemoryManager.h"

int main(void) {
    utilsInit(ALLOCATOR_SYSTEM);
    engineSetGameSystem(&gameSystem);
    engineStart();
    utilsDestroy();
}
