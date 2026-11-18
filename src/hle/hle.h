#pragma once
// HLE (high-level emulation) modules: host implementations of the game's imports.
#include <cstdint>
#include <string>

#include "cpu/cpu.h"

namespace rt { class Runtime; }

namespace hle {

void register_libc_core(rt::Runtime& rt);   // libc_core.cpp
void register_libc_stdio(rt::Runtime& rt);  // libc_stdio.cpp (printf, files, liblog)

// Allocate with the GAME's own malloc (it defines malloc/free itself), so the game
// can later free() what the HLE layer hands it.
uint32_t game_malloc(cpu::Cpu& c, uint32_t size);

// C printf/scanf over guest memory (also used by JNI/liblog code).
std::string cformat(cpu::Cpu& c, const char* fmt, cpu::Args& args);
int csscanf(cpu::Cpu& c, const char* input, const char* fmt, cpu::Args& args);

}  // namespace hle
