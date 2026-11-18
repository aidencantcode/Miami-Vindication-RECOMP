#include "cpu/cpu.h"

#include <unicorn/unicorn.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "core/layout.h"
#include "core/log.h"
#include "rt/runtime.h"

namespace cpu {

namespace {

const int kUcReg[] = {UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2,  UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5,
                      UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,  UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
                      UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR};
constexpr uint64_t kUntil = 0xFFFFFFF0;

void check_uc(uc_err e, const char* what) {
    if (e != UC_ERR_OK) FATAL("unicorn: %s failed: %s", what, uc_strerror(e));
}

thread_local std::exception_ptr t_pending;  // handler exception, re-thrown after uc_emu_start

}  // namespace

// ---------------------------------------------------------------------------- Args
Args::Args(Cpu& c, uint32_t ncrn) : cpu_(c), ncrn_(ncrn) {
    for (int i = 0; i < 4; ++i) regs_[i] = c.reg(Reg(R0 + i));
    nsaa_ = c.reg(SP);
}

Args Args::from_va_list(Cpu& c, uint32_t ptr) {
    Args a(c, 4);
    a.nsaa_ = ptr;
    return a;
}

uint32_t Args::u32() {
    if (ncrn_ < 4) return regs_[ncrn_++];
    uint32_t v = cpu_.rt().mem().r32(nsaa_);
    nsaa_ += 4;
    return v;
}

uint64_t Args::u64() {
    if (ncrn_ < 4 && (ncrn_ & 1)) ++ncrn_;
    if (ncrn_ <= 2) {
        uint64_t v = regs_[ncrn_] | (uint64_t(regs_[ncrn_ + 1]) << 32);
        ncrn_ += 2;
        return v;
    }
    ncrn_ = 4;
    nsaa_ = (nsaa_ + 7) & ~7u;
    auto& m = cpu_.rt().mem();
    uint64_t v = m.r32(nsaa_) | (uint64_t(m.r32(nsaa_ + 4)) << 32);
    nsaa_ += 8;
    return v;
}

float Args::f32() {
    uint32_t b = u32();
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

double Args::f64() {
    uint64_t b = u64();
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}

// ----------------------------------------------------------------------------- Cpu
Cpu::Cpu(rt::Runtime& rt, uint32_t stack_top, const char* name) : rt_(rt), name_(name), stack_top_(stack_top) {
    check_uc(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc_), "uc_open");
    // Cortex-A15 (ARMv7-A + VFPv4 + hardware divide). The game is ARMv5TE code, which ARMv7 runs
    // unchanged; the phone itself (Tegra 2) was a Cortex-A9, so ARMv7 behaviour (e.g. unaligned
    // LDR) is the faithful one. VFP + SDIV are what the guest-side fastpath helpers use.
    check_uc(uc_ctl_set_cpu_model(uc_, UC_CPU_ARM_CORTEX_A15), "set cpu model Cortex-A15");
    uint32_t cpacr = 0;
    uc_reg_read(uc_, UC_ARM_REG_C1_C0_2, &cpacr);
    cpacr |= 0xF << 20;  // full access to cp10/cp11 (VFP)
    check_uc(uc_reg_write(uc_, UC_ARM_REG_C1_C0_2, &cpacr), "enable cp10/cp11");
    uint32_t fpexc = 0x40000000;  // FPEXC.EN
    check_uc(uc_reg_write(uc_, UC_ARM_REG_FPEXC, &fpexc), "enable VFP");

    // Mirror every guest region (present and future) into this Unicorn instance.
    rt.mem().add_listener({
        [this](const core::Region& r) {
            check_uc(uc_mem_map_ptr(uc_, r.addr, r.size, r.prot, rt_.mem().ptr(r.addr)), "uc_mem_map_ptr");
        },
        [this](uint32_t addr, uint32_t size, uint32_t prot) {
            check_uc(uc_mem_protect(uc_, addr, size, prot), "uc_mem_protect");
        },
    });
    uc_hook h;
    check_uc(uc_hook_add(uc_, &h, UC_HOOK_INTR, reinterpret_cast<void*>(&Cpu::on_intr), this, 1, 0), "hook intr");
    check_uc(uc_hook_add(uc_, &h, UC_HOOK_MEM_INVALID, reinterpret_cast<void*>(&Cpu::on_bad_mem), this, 1, 0),
             "hook mem");
}

Cpu::~Cpu() {
    if (uc_) uc_close(uc_);
}

uint32_t Cpu::reg(Reg r) const {
    uint32_t v = 0;
    uc_reg_read(uc_, kUcReg[r], &v);
    return v;
}

void Cpu::set_reg(Reg r, uint32_t v) { uc_reg_write(uc_, kUcReg[r], &v); }

void Cpu::ret_f(float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    ret(b);
}

void Cpu::ret_d(double v) {
    uint64_t b;
    std::memcpy(&b, &v, 8);
    ret64(b);
}

void Cpu::on_intr(uc_struct* uc, uint32_t intno, void* self_) {
    Cpu& self = *static_cast<Cpu*>(self_);
    const uint32_t svc_addr = self.reg(PC) - 4;
    const int idx = self.rt_.imports().trap_index(svc_addr);
    if (intno != 2 || idx < 0) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "unexpected interrupt %u at %08x", intno, svc_addr);
        self.last_fault_ = buf;
        uc_emu_stop(uc);
        return;
    }
    if (idx == 0) {  // return-to-host sentinel
        self.returned_ = true;
        uc_emu_stop(uc);
        return;
    }
    try {
        Args a(self);
        self.rt_.dispatch(self, idx, a);
    } catch (...) {
        t_pending = std::current_exception();
        uc_emu_stop(uc);
    }
}

bool Cpu::on_bad_mem(uc_struct*, int type, uint64_t addr, int size, int64_t, void* self_) {
    Cpu& self = *static_cast<Cpu*>(self_);
    const char* kind = "invalid access";
    switch (type) {
        case UC_MEM_READ_UNMAPPED: kind = "read unmapped"; break;
        case UC_MEM_WRITE_UNMAPPED: kind = "write unmapped"; break;
        case UC_MEM_FETCH_UNMAPPED: kind = "fetch unmapped"; break;
        case UC_MEM_WRITE_PROT: kind = "write to read-only"; break;
        case UC_MEM_FETCH_PROT: kind = "fetch from non-exec"; break;
        case UC_MEM_READ_PROT: kind = "read protected"; break;
    }
    char buf[160];
    bool handle = addr >= layout::kHandleBase && addr < layout::kTrapBase;
    std::snprintf(buf, sizeof buf, "%s at %08llx size %d%s", kind, (unsigned long long)addr, size,
                  handle ? " (a Java object handle: opaque!)" : "");
    self.last_fault_ = buf;
    return false;
}

void Cpu::on_block(uc_struct*, uint64_t addr, uint32_t size, void* self_) {
    // Thumb-1 instructions are 2 bytes, ARM 4; size/2 is close enough for a profile.
    (*static_cast<Cpu*>(self_)->block_counts_)[uint32_t(addr)] += size / 2;
}

void Cpu::enable_block_profile(std::unordered_map<uint32_t, uint64_t>* counts) {
    block_counts_ = counts;
    uc_hook h;
    check_uc(uc_hook_add(uc_, &h, UC_HOOK_BLOCK, reinterpret_cast<void*>(&Cpu::on_block), this, 1, 0), "hook block");
    // Re-translate everything so blocks compiled before the hook existed are counted too.
    check_uc(uc_ctl_flush_tb(uc_), "flush translation cache");
}

uint64_t Cpu::call(uint32_t addr, std::initializer_list<uint32_t> args, std::initializer_list<uint32_t> stack_args) {
    const bool nested = depth_ > 0;
    uc_context* ctx = nullptr;
    if (nested) {
        check_uc(uc_context_alloc(uc_, &ctx), "context alloc");
        check_uc(uc_context_save(uc_, ctx), "context save");
    }
    uint32_t sp = nested ? ((reg(SP) - 256) & ~7u) : stack_top_ - 64;
    if (stack_args.size()) {
        sp -= (4 * uint32_t(stack_args.size()) + 7) & ~7u;
        uint32_t p = sp;
        for (uint32_t v : stack_args) rt_.mem().w32(p, v), p += 4;
    }
    set_reg(SP, sp);
    int i = 0;
    for (uint32_t v : args) set_reg(Reg(R0 + i++), v);
    for (; i < 4; ++i) set_reg(Reg(R0 + i), 0);
    set_reg(LR, rt_.imports().trap_addr("<return-to-host>"));

    ++depth_;
    returned_ = false;
    last_fault_.clear();
    uc_err e = uc_emu_start(uc_, addr, kUntil, 0, 0);
    --depth_;
    const bool returned = returned_;
    returned_ = false;
    const uint64_t result = reg(R0) | (uint64_t(reg(R1)) << 32);

    if (t_pending) {
        std::exception_ptr p = t_pending;
        t_pending = nullptr;
        if (ctx) uc_context_free(ctx);
        std::rethrow_exception(p);
    }
    if (e != UC_ERR_OK || !returned) {
        std::string msg = "guest call to " + where(addr) + " failed: " +
                          (e != UC_ERR_OK ? std::string(uc_strerror(e)) : "did not return") +
                          (last_fault_.empty() ? "" : " (" + last_fault_ + ")") + "\n" + dump();
        if (ctx) uc_context_free(ctx);
        throw std::runtime_error(msg);
    }
    if (ctx) {
        check_uc(uc_context_restore(uc_, ctx), "context restore");
        uc_context_free(ctx);
    }
    return result;
}

std::string Cpu::where(uint32_t addr) const {
    const uint32_t lib_end = layout::kLibBase + rt_.elf().image_size();
    if (addr >= layout::kLibBase && addr < lib_end) return rt_.elf().sym_at(addr - layout::kLibBase);
    int idx = rt_.imports().trap_index(addr & ~7u);
    if (idx >= 0) return "<trap " + rt_.imports().trap_name(idx) + ">";
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", addr);
    return buf;
}

std::string Cpu::dump() const {
    static const char* names[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
                                  "sp", "lr", "pc", "cpsr"};
    std::string out = "  [" + name_ + "]";
    char buf[32];
    for (int r = R0; r <= CPSR; ++r) {
        std::snprintf(buf, sizeof buf, "%s %s=%08x", r % 4 == 0 ? "\n " : "", names[r], reg(Reg(r)));
        out += buf;
    }
    out += "\n  pc in " + where(reg(PC)) + "   lr in " + where(reg(LR)) +
           ((reg(CPSR) & 0x20) ? "   (Thumb)" : "   (ARM)");
    return out;
}

}  // namespace cpu
