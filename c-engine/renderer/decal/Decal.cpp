#include "Decal.h"

// Roads/trails cap their decal count at 8192; the pool must be larger so the
// river wet-strip decals (workstream C) have room to be added.
#define DECAL_MAX_PERSISTENT 16384u
#define DECAL_MAX_TRANSIENT  2048u

static DecalInstance persistentDecals[DECAL_MAX_PERSISTENT];
static u8            persistentAlive[DECAL_MAX_PERSISTENT];
// Remembers the last-used slot so repeated decalAdd calls don't rescan from
// 0 every time (a linear scan from 0 per call was the load-time bottleneck
// for the 8k river wet-strip decals).  Wraps to 0 when the tail is full.
static u32           decalScanHint;
static DecalInstance transientDecals[DECAL_MAX_TRANSIENT];
static size_t        transientCount;

u32 decalAdd(const DecalInstance* decal) {
    if (!decal) return DECAL_INVALID_HANDLE;
    for (u32 i = decalScanHint; i < DECAL_MAX_PERSISTENT; ++i) {
        if (!persistentAlive[i]) {
            persistentDecals[i] = *decal;
            persistentAlive[i]  = 1;
            decalScanHint      = i + 1;
            return i;
        }
    }
    for (u32 i = 0; i < decalScanHint; ++i) {
        if (!persistentAlive[i]) {
            persistentDecals[i] = *decal;
            persistentAlive[i]  = 1;
            decalScanHint      = i + 1;
            return i;
        }
    }
    warn("decalAdd: persistent decal capacity reached (%u)", DECAL_MAX_PERSISTENT);
    return DECAL_INVALID_HANDLE;
}

void decalRemove(u32 handle) {
    if (handle >= DECAL_MAX_PERSISTENT) return;
    persistentAlive[handle] = 0;
}

void decalClearPersistent(void) {
    memset(persistentAlive, 0, sizeof(persistentAlive));
    decalScanHint = 0;
}

void decalClearTransient(void) {
    transientCount = 0;
}

void decalSubmitTransient(const DecalInstance* decal) {
    if (!decal) return;
    if (transientCount >= DECAL_MAX_TRANSIENT) return;
    transientDecals[transientCount++] = *decal;
}

const DecalInstance* decalGetPersistent(size_t* count) {
    static DecalInstance compact[DECAL_MAX_PERSISTENT];
    size_t n = 0;
    for (u32 i = 0; i < DECAL_MAX_PERSISTENT; ++i) {
        if (persistentAlive[i]) compact[n++] = persistentDecals[i];
    }
    if (count) *count = n;
    return compact;
}

const DecalInstance* decalGetTransient(size_t* count) {
    if (count) *count = transientCount;
    return transientDecals;
}
