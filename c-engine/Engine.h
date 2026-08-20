#pragma once


extern volatile char engineRunning;

struct System;
void engineSetGameSystem(struct System* system);
void engineStart(void);
void engineStop(void);
