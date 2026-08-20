#pragma once

typedef struct Transform Transform;

void transformDbInit(void);
void transformDbSave(const char* name, Transform* transformIn);
char transformDbLoad(const char* name, Transform* transformOut);
