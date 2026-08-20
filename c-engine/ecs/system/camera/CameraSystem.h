#pragma once

#include "ecs/system/System.h"
extern struct System cameraSystem;

struct Camera;
struct Entity* cameraGetEntity(void);
