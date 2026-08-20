#pragma once

void flyingCameraInit(u32 cameraEntity);
void flyingCameraPreUpdate(u32 cameraEntity);
void flyingCameraUpdate(u32 cameraEntity);
void flyingCameraPostUpdate(void);
void flyingCameraLoadForGameplay(void);
char flyingCameraIsActive(void);
