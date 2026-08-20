#pragma once

#include "azgaar/AzgaarHeightmapSource.h"
#include "azgaar/AzgaarWorld.h"
#include "ecs/system/System.h"

extern struct System loadingAzgaarSystem;

const char* loadingAzgaarStageText(void);

void loadingAzgaarOnEnter(void);
void loadingAzgaarOnExit(void);

// Access the loaded Azgaar world. Only valid while it is retained for gameplay
// streaming (i.e. after a successful loading -> gameplay transition). Returns
// NULL before load completes, after gameplay releases it, or on the cancel path.
const AzgaarWorld* loadingAzgaarGetWorld(void);

// The AzgaarHeightmapSource backing the active world (NULL before the map is
// loaded / after release). Its heightAt is the CANONICAL terrain surface
// (FMG macro heights + seeded fBm detail) — the same function the heightmap
// pass bakes into its textures and the Jolt heightfields collide against.
// Consumers that need the exact ground surface (grass scatter, collision
// fallbacks) must sample through it, never a private height function.
// (Non-const: the HeightmapSource vtable passes userData as void*; heightAt
// is a pure function and never mutates the source.)
AzgaarHeightmapSource* loadingAzgaarGetHeightmapSource(void);

// Release the retained world. Called by gameplay teardown to free the world
// data once streaming no longer needs it. Safe to call when nothing is retained.
void loadingAzgaarReleaseWorld(void);
