#pragma once

typedef struct System System;

extern System hud;

// Spawn a floating damage number at world position.
// Value > 0: damage dealt (white). Value < 0: damage received (red).
void hudDamageNumber(float x, float y, float z, float value);
