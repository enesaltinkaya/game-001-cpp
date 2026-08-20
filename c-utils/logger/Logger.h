#pragma once


namespace utils {
typedef enum { LOGGER_DEBUG, LOGGER_INFO, LOGGER_WARNING, LOGGER_ERROR, LOGGER_CRITICAL } LogLevel;

void loggerInit(void);
void loggerDestroy(void);

void debug(const char* format, ...);
void info(const char* format, ...);
void warn(const char* format, ...);
void error(const char* format, ...);
void crit(const char* format, ...);
void slowLog(const char* format, ...);

void debugRml(const char*);
void errorRml(const char*);
void infoRml(const char*);
void warnRml(const char*);
}  // namespace utils
