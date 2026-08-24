#pragma once


namespace engine {
extern volatile char engineRunning;

class System;
void engineSetGameSystem(System* system);
void engineStart(void);
void engineStop(void);
}  // namespace engine
