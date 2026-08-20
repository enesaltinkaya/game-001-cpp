#pragma once


typedef struct Scene Scene;

extern struct System lightSystem;

/* Legacy compatibility stub. Lighting is rebuilt each frame from visible scenes. */
void lightMarkDirty(Scene* scene, u32 entity);
