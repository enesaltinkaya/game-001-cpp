#include "Utils.h"
#include <errno.h>
#include "database/sqlite/Sqlite.h"
#include "datamanager/DataManager.h"
#include "events/Events.h"
#include "json/Json.h"
#include "logger/Logger.h"
#include "platform/Platform.h"
#include "settings/Settings.h"
#include "thread/Thread.h"
#include "timer/Timer.h"
#include "logger/Logger.h"
#include "string/String.h"

#ifdef _WIN32
#include <windows.h>
#include <synchapi.h>
#endif

void utilsInit(MemoryAllocatorType allocatorType) {
    // signalCatcherInit();
    memoryInit(allocatorType);
    platformInit();
    loggerInit();

    jsonInit();
    settingsInit();

    timerInit(settingsGetDouble("fpsLimit"), settingsGetBool("fpsLimitChecked"), settingsGetBool("busyLoopLinux"));
    threadPoolInit(0);
    dataManagerInit();

    // char cfgdir[MAX_PATH];
    // get_user_config_folder(cfgdir, sizeof(cfgdir), "minicasterv5");
    sqliteInit("db");
}

void utilsDestroy(void) {
    sqliteDestroy();
    dataManagerDestroy();
    threadPoolDestroy(NULL);
    platformDestroy();
    settingsDestroy();
    signalCleanUp();
    memoryDestroy();
    loggerDestroy();
}

double nanos(void) {
    struct timespec spec;
    clock_gettime(CLOCK_MONOTONIC, &spec);
    return ((double)spec.tv_sec * BILLION) + (double)spec.tv_nsec;
}

double millies(void) {
    return nanos() / MILLION;
}

void gotoSleepMS(long int millis) {
    if (millis < 0) {
        return;
    }
#ifdef _WIN32
    static HANDLE wtimer = NULL;
    if (!wtimer) wtimer = CreateWaitableTimer(NULL, TRUE, NULL);
    LARGE_INTEGER li;
    li.QuadPart = -(millis * 10000LL);
    if (!SetWaitableTimer(wtimer, &li, 0, NULL, NULL, FALSE)) return;
    WaitForSingleObject(wtimer, INFINITE);
#else
    struct timespec spec = {};
    spec.tv_sec          = millis / 1000;
    spec.tv_nsec         = (millis % 1000) * 1000000L;
    while (nanosleep(&spec, &spec) == -1 && errno == EINTR) {
    }

    // if (millis >= 1000) {
    //     sleep(millis / 1000);
    // }
    // usleep((millis % 1000) * 1000);
#endif
}

void gotoSleepNS(long int nanos) {
    if (nanos < 0) {
        return;
    }
#ifdef _WIN32
    static HANDLE timer = NULL;
    if (!timer) timer = CreateWaitableTimer(NULL, TRUE, NULL);
    LARGE_INTEGER li;
    li.QuadPart = -(nanos / 100);
    if (!SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE)) return;
    WaitForSingleObject(timer, INFINITE);
#else
    struct timespec req = {};
    req.tv_sec          = nanos / (long long)BILLION;
    req.tv_nsec         = nanos % (long long)BILLION;
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
    }
#endif
}

void terminate(const char* format, ...) {
    va_list args;
    String message = {};
    va_list args_copy;
    va_start(args, format);
    va_copy(args_copy, args);
    size_t len = vsnprintf(0, 0, format, args_copy);
    stringSetSize(&message, len + 1);
    vsnprintf(message.data, len + 1, format, args);
    va_end(args);
    va_end(args_copy);

    crit("---------------------------------------");
    crit(message.data);
    crit("---------------------------------------");

    stringDestroy(&message);

#ifdef _WIN32
    exit(1);
#else
    quick_exit(1);
#endif
}

double elapsedBegin(void) {
    return nanos();
}

double elapsedEnd(double start) {
    return (nanos() - start) / MILLION;
}

void randomChars(char* dest, long int length) {
    char charset[] =
        "0123456789"
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    while (length-- > 0) {
        int index = (int)((double)rand() / RAND_MAX * (sizeof charset - 1));
        *dest++   = charset[index];
    }
    *dest = '\0';
}

char isDebug(void) {
    static char debug = -1;
    if (debug == -1) {
        const char* s = getenv("ENGINE_DEBUG");
        if (s && strequals(s, "1")) {
            debug = 1;
        } else {
            debug = 0;
        }
    }
    return debug;
}

// static inline float ImSaturate(float f) {
//     return (f < 0.0f) ? 0.0f : (f > 1.0f) ? 1.0f : f;
// }
#define IM_F32_TO_INT8_SAT(_VAL) ((int)(((_VAL) * 255.0f) + 0.5f))  // Saturated, always output 0..255

u32 colorHexToUInt(const char* hex) {
    u32 r;
    u32 g;
    u32 b;
    u32 a;
    u32 out = 0;
    sscanf(hex, "%02x%02x%02x%02x", &r, &g, &b, &a);
    out = ((u32)IM_F32_TO_INT8_SAT(r / 255.F)) << 0;
    out |= ((u32)IM_F32_TO_INT8_SAT(g / 255.F)) << 8;
    out |= ((u32)IM_F32_TO_INT8_SAT(b / 255.F)) << 16;
    out |= ((u32)IM_F32_TO_INT8_SAT(a / 255.F)) << 24;
    return out;
}

struct Pcg32State {
    uint64_t state;
    uint64_t inc;
};

static Pcg32State pcg32_random_t;
static Pcg32State pcg32;

uint32_t pcg32_random_r(void* rng) {
    Pcg32State* _rng = static_cast<Pcg32State*>(rng);
    if (!_rng->state) {
        _rng->state = (u64)nanos();
    }
    uint64_t oldstate = _rng->state;
    // Advance internal state
    _rng->state = oldstate * 6364136223846793005ULL + (_rng->inc | 1);
    // Calculate output function (XSH RR), uses old state for max ILP
    uint32_t xorshifted = ((oldstate >> 18U) ^ oldstate) >> 27U;
    uint32_t rot        = oldstate >> 59U;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

// output range [0,1)
float pcg32_random_float_r(void* rng) {
    Pcg32State* _rng = static_cast<Pcg32State*>(rng);
    if (!_rng->state) {
        _rng->state = (u64)nanos();
    }
    float scale = 1.0F / 16777216.0F;
    return (float)(pcg32_random_r(rng) & ((1L << 24) - 1)) * scale;
}

u32 randomU32(void) {
    return pcg32_random_r(&pcg32);
}

float randomFloat(void) {
    return pcg32_random_float_r(&pcg32);
}

void* customMemmem(const void* haystack, int haystack_len, const void* needle, int needle_len) {
    unsigned char* csrc = (unsigned char*)haystack;
    unsigned char* ctrg = (unsigned char*)needle;
    unsigned char* tptr;
    unsigned char* cptr;
    int searchlen;
    int ndx = 0;

    if (haystack == NULL) {
        return NULL;
    }
    if (haystack_len == 0) {
        return NULL;
    }
    if (needle == NULL) {
        return NULL;
    }
    if (needle_len == 0) {
        return (void*)haystack;
    }

    while (ndx <= haystack_len) {
        cptr = &csrc[ndx];
        if ((searchlen = haystack_len - ndx - needle_len + 1) <= 0) {
            return NULL;
        }
        if ((tptr = static_cast<unsigned char*>(memchr(cptr, *ctrg, searchlen))) == NULL) {
            return NULL;
        }
        if (memcmp(tptr, ctrg, needle_len) == 0) {
            return tptr;
        }
        ndx += (int)(tptr - cptr + 1);
    }
    return NULL;
}
