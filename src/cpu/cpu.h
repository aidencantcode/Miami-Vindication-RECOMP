#pragma once
// One guest thread = one Cpu = one Unicorn instance (ARM926 / ARMv5TE). Every Cpu maps
// the same host pages of core::GuestMemory, so all threads share guest memory.
//
// Import calls: game -> PLT -> GOT slot -> trap stub (`svc #0; bx lr`) -> on_intr ->
// HLE handler -> registers updated -> `bx lr` back to the game. Same mechanism as the
// probe (tools/probe/emu.py), whose Unicorn behaviours are checked by
// tools/probe/test_unicorn_semantics.py.
#include <cstdint>
#include <initializer_list>
#include <string>

struct uc_struct;

namespace rt { class Runtime; }

namespace cpu {

enum Reg { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC, CPSR };

class Cpu;

// Argument access for an HLE handler, following AAPCS soft-float: r0-r3 then the
// stack; 64-bit values in even register pairs / 8-byte aligned stack slots.
class Args {
public:
    Args(Cpu& c, uint32_t ncrn = 0);
    uint32_t u32();
    int32_t s32() { return int32_t(u32()); }
    uint64_t u64();
    int64_t s64() { return int64_t(u64()); }
    float f32();   // float passed as bits in a core register (soft-float)
    double f64();  // double in a register pair / stack slot
    uint32_t raw(int i) const { return regs_[i]; }

    // va_list reader: arguments laid out in memory from `ptr`
    static Args from_va_list(Cpu& c, uint32_t ptr);

private:
    Cpu& cpu_;
    uint32_t regs_[4];
    uint32_t ncrn_, nsaa_;
};

using Handler = void (*)(Cpu& cpu, Args& a);

class Cpu {
public:
    Cpu(rt::Runtime& rt, uint32_t stack_top, const char* name);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    rt::Runtime& rt() { return rt_; }
    const std::string& name() const { return name_; }

    uint32_t reg(Reg r) const;
    void set_reg(Reg r, uint32_t v);

    // Return values (written to r0 / r0:r1)
    void ret(uint32_t v) { set_reg(R0, v); }
    void ret64(uint64_t v) { set_reg(R0, uint32_t(v)); set_reg(R1, uint32_t(v >> 32)); }
    void ret_f(float v);
    void ret_d(double v);
    // Continue at `addr` (Thumb bit honoured) with current registers when the handler returns.
    void redirect(uint32_t addr) { set_reg(PC, addr); }

    // Call guest code; nests correctly when used from inside a handler. Returns r0 | r1<<32.
    uint64_t call(uint32_t addr, std::initializer_list<uint32_t> args = {},
                  std::initializer_list<uint32_t> stack_args = {});

    // Diagnostics
    std::string where(uint32_t addr) const;
    std::string dump() const;

private:
    static void on_intr(uc_struct* uc, uint32_t intno, void* self);
    static bool on_bad_mem(uc_struct* uc, int type, uint64_t addr, int size, int64_t value, void* self);
    void fail(const char* what);

    rt::Runtime& rt_;
    std::string name_;
    uc_struct* uc_ = nullptr;
    uint32_t stack_top_;
    int depth_ = 0;
    bool returned_ = false;
    std::string last_fault_;
};

}  // namespace cpu
