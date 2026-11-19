#pragma once
// The recompiled backend (Phase 4): binds the generated code (work/recomp, built as game_recomp)
// to the runtime. After bind(), cpu.call() runs recompiled functions, imports go to the same HLE
// handlers as the Unicorn backend, and every handler keeps working because cpu::Cpu reads/writes
// registers in the RcState.
#include <cstdint>
#include <string>

namespace rt { class Runtime; }
namespace cpu { class Cpu; }

namespace recomp {

bool available();  // this build contains the recompiled code (GANGSTAR_RECOMP)
bool bind(rt::Runtime& rt, cpu::Cpu& cpu, std::string* err);
// Statistics for the 2 s report: indirect calls / imports since the last call.
void take_stats(uint64_t* indirect_calls, uint64_t* imports);

}  // namespace recomp
