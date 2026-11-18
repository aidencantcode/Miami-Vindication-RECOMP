#pragma once
// Fake JNIEnv / JavaVM. C++ twin of tools/probe/jni.py.
//   JNIEnv* -> [pointer to a 233-slot function table]; each slot is a trap.
//   Java objects are opaque handles in an UNMAPPED range (layout::kHandleBase), so a
//   native dereference of a jobject faults with a clear message.
// The "Java side" (the 57 static callbacks the game makes) is implemented here with
// the behaviour of the decompiled Java (analysis/jni_surface.md).
#include <cstdint>
#include <string>

namespace rt { class Runtime; }

namespace jni {

void install(rt::Runtime& rt);

uint32_t env();                                    // guest JNIEnv*
uint32_t find_class(const std::string& name);      // jclass handle (created on first use)
uint32_t new_instance(const std::string& cls);     // jobject handle
uint32_t new_string(const std::string& utf8);      // jstring handle
std::string string_of(uint32_t handle);            // "" for null / non-strings

void report();  // log JNI functions + Java callbacks used

constexpr const char* kPkg = "com/gameloft/android/GAND/GloftGMHP/ML/";

}  // namespace jni
