#include <stdio.h>
#include <stdlib.h>
#include "Utils.h"
#include "logger/Logger.h"
#ifdef __linux
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>

/*
this disables core dump
faster exit
 */
namespace utils {
void clean_exit_on_sig(int _) {
    crit("SIGSEGV");

    // print stack trace
#ifndef NDEBUG
#ifdef __linux
    void* array[100];
    size_t size = backtrace(array, 100);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
#endif
#endif

    exit(0);
}

void signalCatcherInit(void) {
    if (isDebug()) {
        signal(SIGSEGV, clean_exit_on_sig);
    }
}
}  // namespace utils
#else
namespace utils {
void signalCatcherInit(void) {}
}  // namespace utils
#endif