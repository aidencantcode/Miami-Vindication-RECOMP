#include "core/log.h"

#include <cstdlib>
#include <mutex>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace core {

static FILE* g_log = nullptr;
static std::mutex g_log_mutex;

void log_open(const char* path) {
    std::lock_guard lock(g_log_mutex);
    g_log = std::fopen(path, "w");
}

void log_close() {
    std::lock_guard lock(g_log_mutex);
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
}

static void vlog(const char* fmt, va_list ap) {
    char buf[4096];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    std::lock_guard lock(g_log_mutex);
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "gangstar", buf);  // logcat (stdout goes nowhere)
#else
    std::fputs(buf, stdout);
    std::fputc('\n', stdout);
#endif
    if (g_log) {
        std::fputs(buf, g_log);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
}

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void fatalf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
    log_close();
    std::exit(1);
}

}  // namespace core
