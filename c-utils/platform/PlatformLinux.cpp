#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "Platform.h"  // IWYU pragma: keep
#include "Utils.h"
#include "logger/Logger.h"
#include "string/String.h"  // IWYU pragma: keep

// static int emptyTranslationUnit = 3;

#ifndef _WIN32
#include <sys/resource.h>
#include <libgen.h>

namespace utils {
Platform platform;

Platform* getPlatform(void) {
    return &platform;
}

void platformInit(void) {
    platform.isWindows = 0;

    char temp[1024] = {};
    ssize_t len     = readlink("/proc/self/exe", temp, 1024);
    if (len == -1) {
        printf("failed to read /proc/self/exe, not good...\n");
        printf("assuming launching from game directory\n");

        char cwd[1024] = {};
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("current working dir: %s\n", cwd);
        } else {
            printf("failed getcwd() try wine :(\n");
            abort();
        }
        strcpy(temp, cwd);
    }
    temp[len] = '\0';

    strcpy(platform.executablePath, temp);
    strcpy(platform.seperator, "/");
    snprintf(platform.cwd, 1023, "%s%s", dirname(temp), platform.seperator);
    snprintf(platform.dataDirectory, 1023, "%s%s%s", platform.cwd, "data", platform.seperator);
    printf("data directory: %s\n", platform.dataDirectory);
    createDirectory(platform.dataDirectory);
}

void platformDestroy(void) {
    info("---------------------------");
    debug("bye");
    debug("memory usage   : %zu", memoryUsage());
    info("---------------------------");
}

char* gnuBasename(char* path) {
    char* base = strrchr(path, '/');
    return base ? base + 1 : path;
}

static int mkdir_recursive(const char* path, mode_t mode) {
    char tmp[1024];
    char* p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }

    return mkdir(tmp, mode);  // final directory
}

void createDirectory(const char* path) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%s", path);

    char* dir;
    if (strEndsWithC(path, platform.seperator)) {
        dir = (char*) path;
    } else {
        dir = dirname(buffer);
    }

    mkdir_recursive(dir, 0755);
}

u64 memoryUsage(void) {
    static double lastTime = 0;
    static u64 lastResult  = 0;

    if (nanos() > lastTime + 1000000000) {
        struct rusage r_usage = {};
        getrusage(RUSAGE_SELF, &r_usage);
        lastResult = r_usage.ru_maxrss;
        lastTime   = nanos();
    }
    return lastResult;
}

int numberOfCores(void) {
    return (int)sysconf(_SC_NPROCESSORS_ONLN);
}

}  // namespace utils
#endif
