#pragma once
// Minimal logging: stdout + build/runtime.log. Every stub logs through STUB().
#include <cstdarg>
#include <cstdio>

namespace core {

void log_open(const char* path);
void log_close();
void logf(const char* fmt, ...);
[[noreturn]] void fatalf(const char* fmt, ...);

}  // namespace core

#define LOG(...) ::core::logf(__VA_ARGS__)
#define FATAL(...) ::core::fatalf(__VA_ARGS__)
