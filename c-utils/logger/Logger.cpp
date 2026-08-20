#include "Logger.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include "Utils.h"
#include "memorymanager/MemoryManager.h"
#include "platform/Platform.h"
#include "logger/Logger.h"
#include "string/String.h"

#ifdef _WIN32
#define IS_WINDOWS 1
#else
#define IS_WINDOWS 0
#endif

#define MAX_LOG_LINE 1024

static struct {
    LogLevel min_level;
    FILE* file;
} logger = {.min_level = LOGGER_INFO, .file = NULL};

static const char* level_to_string(LogLevel level) {
    switch (level) {
        case LOGGER_DEBUG:
            return "DEBUG";
        case LOGGER_INFO:
            return "INFO ";
        case LOGGER_WARNING:
            return "WARN ";
        case LOGGER_ERROR:
            return "ERROR";
        case LOGGER_CRITICAL:
            return "CRIT ";
        default:
            return "UNKNOWN";
    }
}

static const char* get_level_color(LogLevel level) {
    if (IS_WINDOWS) {
        return "";
    }
    switch (level) {
        case LOGGER_DEBUG:
            return "\033[36m";
        case LOGGER_INFO:
            return "\033[32m";
        case LOGGER_WARNING:
            return "\033[33m";
        case LOGGER_ERROR:
            return "\033[31m";
        case LOGGER_CRITICAL:
            return "\033[1;31m";
        default:
            return "\033[0m";
    }
}

static const char* get_reset_color(void) {
    return IS_WINDOWS ? "" : "\033[0m";
}

static void get_current_time(char* buffer, size_t len) {
    time_t now   = time(NULL);
    struct tm* t = localtime(&now);
    strftime(buffer, len, "%Y-%m-%d %H:%M:%S", t);
}

static void logger_log(LogLevel level, const char* format, va_list args) {
    if (level < logger.min_level) {
        return;
    }

    char message[MAX_LOG_LINE];
    vsnprintf(message, sizeof(message), format, args);

    char time_str[64];
    get_current_time(time_str, sizeof(time_str));

    const char* level_str = level_to_string(level);
    const char* color     = get_level_color(level);
    const char* reset     = get_reset_color();

    char lua        = strStartsWith(message, "LUA:") ? 4 : 0;
    int offset      = lua ? 4 : 0;
    const char* tag = lua ? "LUA  " : level_str;

    char final_msg[MAX_LOG_LINE + 128];
    snprintf(final_msg, sizeof(final_msg), "[%s]%s [%s] %s%s", time_str, color, tag, message + offset, reset);

    printf("%s\n", final_msg);

    if (logger.file) {
        fprintf(logger.file, "[%s] [%s] %s\n", time_str, tag, message + offset);
        fflush(logger.file);
    }
}

void loggerInit(void) {
    logger.min_level = LOGGER_DEBUG;
    char path[1050];
    snprintf(path, sizeof(path), "%s%s%s%s", platform.cwd, "data", platform.seperator, "game.log");
    logger.file = fopen(path, "w");
    if (!logger.file) {
        fprintf(stdout, "Failed to open log file: %s\n", path);
    }

    info("logger: initializing");
    debug("logger: app path  %s", platform.cwd);
    debug("logger: log path  %s", path);
    debug("logger: allocator %s", getMemoryAllocType() == ALLOCATOR_SYSTEM ? "System" : "Buddy");
}

void loggerDestroy(void) {
    if (logger.file) {
        fclose(logger.file);
        logger.file = NULL;
    }
}

void debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logger_log(LOGGER_DEBUG, format, args);
    va_end(args);
}

void info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logger_log(LOGGER_INFO, format, args);
    va_end(args);
}

void warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logger_log(LOGGER_WARNING, format, args);
    va_end(args);
}

void error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logger_log(LOGGER_ERROR, format, args);
    va_end(args);
}

void crit(const char* format, ...) {
    va_list args;
    va_start(args, format);
    logger_log(LOGGER_CRITICAL, format, args);
    va_end(args);
}

void slowLog(const char* format, ...) {
    static double lastLogged;
    if (nanos() > lastLogged + BILLION / 2.0f) {
        lastLogged = nanos();
        va_list args;
        va_start(args, format);
        logger_log(LOGGER_DEBUG, format, args);
        va_end(args);
    }
}

void debugRml(const char* message) {
    debug(message);
}

void errorRml(const char* message) {
    error(message);
}

void infoRml(const char* message) {
    info(message);
}

void warnRml(const char* message) {
    warn(message);
}
