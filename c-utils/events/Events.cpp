#include "Events.h"

// --- String-based signals ---

static StrMap(Array(FnPtr)) signals = NULL;

void signalSubscribe(const char* name, FnPtr fn) {
    Array(FnPtr) list = NULL;
    if (strmapContainsKey(signals, name)) {
        list = strmapGet(signals, name);
    }
    for (i32 i = 0, s = arraySize(list); i < s; i++) {
        if (list[i] == fn) {
            return;
        }
    }
    arrayPut(list, fn);
    strmapPut(signals, name, list);
}

void signalRemoveSubscription(const char* name, FnPtr fn) {
    if (!strmapContainsKey(signals, name)) return;
    Array(FnPtr) list = strmapGet(signals, name);
    for (i32 i = 0, s = arraySize(list); i < s; i++) {
        if (list[i] == fn) {
            arrayDeleteSwap(list, i);
            strmapPut(signals, name, list);
            return;
        }
    }
}

void signalEmit(const char* name, void* userData) {
    if (!strmapContainsKey(signals, name)) return;
    Array(FnPtr) list = strmapGet(signals, name);
    for (i32 i = 0, s = arraySize(list); i < s; ++i) {
        list[i](userData);
    }
}

void signalCleanUp(void) {
    for (i32 i = 0, s = strmapSize(signals); i < s; i++) {
        arrayFree(signals[i].value);
    }
    strmapFree(signals);
    signals = NULL;
}
