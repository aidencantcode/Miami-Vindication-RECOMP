#include "rt/runtime.h"

#include <cstring>
#include <iterator>

#include "core/layout.h"
#include "core/log.h"

namespace rt {

namespace {

#include "cpu/fastpath_code.inc"

constexpr int kStubLogLimit = 3;

}  // namespace

bool Runtime::init(const Paths& paths, std::string* err, bool fastpath) {
    paths_ = paths;
    if (!mem_.init()) return *err = "cannot reserve guest address space", false;
    if (!imports_.init(mem_)) return *err = "cannot map trap/data regions", false;
    using namespace layout;
    if (fastpath) {
        if (!mem_.map(kFastBase, kFastSize, core::PROT_R | core::PROT_X, "fastpath"))
            return *err = "cannot map fastpath region", false;
        static_assert(sizeof(kFastpathCode) <= kFastSize, "fastpath code too large");
        mem_.write(kFastBase, kFastpathCode, sizeof kFastpathCode);
        std::unordered_map<std::string, uint32_t> m;
        for (const auto& s : kFastpathSyms) m[s.name] = kFastBase + s.offset;  // ARM code: even address
        imports_.set_fastpath(std::move(m));
        LOG("fastpath: %zu runtime helpers run as guest VFP code", std::size(kFastpathSyms));
    }
    // Everything the HLE layer hands out is committed up front, so guest memory is never
    // remapped while guest code runs (important once several guest threads exist).
    if (!mem_.map(kHleHeapBase, kHleHeapSize, core::PROT_R | core::PROT_W, "HLE heap") ||
        !mem_.map(kSbrkBase, kSbrkLimit - kSbrkBase, core::PROT_R | core::PROT_W, "sbrk heap") ||
        !mem_.map(kMmapBase, kMmapLimit - kMmapBase, core::PROT_R | core::PROT_W, "mmap area") ||
        !mem_.map(kStackBase, kStackSize, core::PROT_R | core::PROT_W, "main stack"))
        return *err = "cannot map heap/stack regions", false;
    heap_.init(kHleHeapBase, kHleHeapBase + kHleHeapSize);
    brk_ = kSbrkBase;
    mmap_next_ = kMmapBase;
    errno_addr_ = imports_.data_alloc().alloc(4, 4);

    if (!elf_.open(paths.so.string(), err)) return false;
    for (const loader::LoadSeg& s : elf_.loads()) {
        uint32_t lo = s.vaddr & ~0xFFFu, hi = (s.vaddr + s.memsz + 0xFFF) & ~0xFFFu;
        uint32_t prot = (s.flags & 4 ? core::PROT_R : 0) | (s.flags & 2 ? core::PROT_W : 0) |
                        (s.flags & 1 ? core::PROT_X : 0);
        if (!mem_.map(kLibBase + lo, hi - lo, prot, s.flags & 1 ? "lib RX" : "lib RW"))
            return *err = "cannot map library segments", false;
    }
    loader::RelocStats st;
    if (!elf_.relocate_into(mem_, kLibBase, [&](const loader::Sym& s) { return imports_.resolve(s); }, &st, err))
        return false;
    LOG("loaded %s: %u relative, %u abs32, %u glob_dat, %u jump_slot relocations; %zu traps",
        paths.so.filename().string().c_str(), st.relative, st.abs32, st.glob_dat, st.jump_slot,
        imports_.trap_count());

    main_ = std::make_unique<cpu::Cpu>(*this, kStackBase + kStackSize, "main");
    return true;
}

void Runtime::post_hook_trap(cpu::Cpu& c, cpu::Args&) {
    uint32_t lr;
    int idx;
    if (!c.pop_post(&lr, &idx)) FATAL("post-hook trap reached with an empty return stack");
    if (c.rt().fn_hooks_[idx].post) c.rt().fn_hooks_[idx].post(c);
    // Back to the original caller through the trap stub's own "bx lr" (LR is dead after a return).
    // Writing PC from the hook instead breaks when the post hook made nested guest calls: Unicorn
    // then stops the outer emulation ("did not return").
    c.set_reg(cpu::LR, lr);
}

void Runtime::hook_function(const std::string& symbol, FunctionHook h) {
    const uint32_t addr = lib_func(symbol) & ~1u;
    if (!post_trap_) post_trap_ = add_trap("<post-hook>", &Runtime::post_hook_trap);
    if (fn_hook_at_.count(addr)) FATAL("function %s hooked twice", symbol.c_str());
    fn_hooks_.push_back(std::move(h));
    fn_hook_names_.push_back(symbol);
    fn_hook_calls_.push_back(0);
    fn_hook_at_[addr] = int(fn_hooks_.size()) - 1;
    main_->install_code_hook(addr);  // (future guest threads install all hooks when created)
}

void Runtime::run_function_hook(cpu::Cpu& c, uint32_t addr) {
    auto it = fn_hook_at_.find(addr);
    if (it == fn_hook_at_.end()) return;
    ++fn_hook_calls_[it->second];
    const FunctionHook& h = fn_hooks_[it->second];
    if (h.pre && h.pre(c)) {
        c.set_reg(cpu::PC, c.reg(cpu::LR));  // skip: return straight to the caller
        return;
    }
    if (h.post) {
        c.push_post(c.reg(cpu::LR), it->second);
        c.set_reg(cpu::LR, post_trap_);
    }
}

std::vector<std::pair<std::string, uint64_t>> Runtime::hook_counts() const {
    std::vector<std::pair<std::string, uint64_t>> out;
    for (size_t i = 0; i < fn_hooks_.size(); ++i) out.push_back({fn_hook_names_[i], fn_hook_calls_[i]});
    return out;
}

uint32_t Runtime::lib_object(const std::string& name) const {
    const loader::Sym* s = elf_.object(name);
    if (!s) FATAL("game object %s not found", name.c_str());
    return layout::kLibBase + s->value;
}

uint32_t Runtime::lib_func(const std::string& name) const {
    uint32_t v = elf_.func(name);
    if (!v) FATAL("game function %s not found", name.c_str());
    return layout::kLibBase + v;
}

void Runtime::set_handler(const std::string& name, cpu::Handler h) {
    by_name_[name] = h;
    int idx = imports_.trap_index(imports_.trap_addr(name));
    if (idx > 0 && size_t(idx) < table_.size()) table_[idx] = h;
}

uint32_t Runtime::add_trap(const std::string& name, cpu::Handler h) {
    uint32_t addr = imports_.add_trap(name);
    by_name_[name] = h;
    ensure_table();
    return addr;
}

void Runtime::ensure_table() {
    size_t n = imports_.trap_count();
    if (table_.size() >= n) return;
    size_t old = table_.size();
    table_.resize(n, nullptr);
    counts_.resize(n, 0);
    stubbed_.resize(n, false);
    for (size_t i = old; i < n; ++i) {
        auto it = by_name_.find(imports_.trap_name(int(i)));
        table_[i] = it != by_name_.end() ? it->second : nullptr;
    }
}

void Runtime::dispatch(cpu::Cpu& c, int idx, cpu::Args& a) {
    ensure_table();
    if (counts_[idx]++ == 0) first_use_.push_back(idx);
    cpu::Handler h = table_[idx];
    if (!h) {
        stubbed_[idx] = true;
        if (counts_[idx] <= kStubLogLimit)
            LOG("STUB: %s(%#x, %#x, %#x, %#x) -> 0   [from %s]", imports_.trap_name(idx).c_str(), a.raw(0), a.raw(1),
                a.raw(2), a.raw(3), c.where(c.reg(cpu::LR)).c_str());
        c.ret(0);
        return;
    }
    h(c, a);
}

uint64_t Runtime::calls(const std::string& name) const {
    int idx = imports_.trap_index(imports_.trap_addr(name));
    return idx >= 0 && size_t(idx) < counts_.size() ? counts_[idx] : 0;
}

std::vector<std::string> Runtime::stubbed() const {
    std::vector<std::string> out;
    for (size_t i = 0; i < stubbed_.size(); ++i)
        if (stubbed_[i]) out.push_back(imports_.trap_name(int(i)));
    return out;
}

uint32_t Runtime::sbrk(int32_t incr) {
    uint64_t nb = uint64_t(brk_) + int64_t(incr);
    if (nb < layout::kSbrkBase || nb > layout::kSbrkLimit) return 0xFFFFFFFF;
    uint32_t old = brk_;
    brk_ = uint32_t(nb);
    return old;
}

uint32_t Runtime::mmap_anon(uint32_t size) {
    size = (size + 0xFFF) & ~0xFFFu;
    if (uint64_t(mmap_next_) + size > layout::kMmapLimit) return 0xFFFFFFFF;
    uint32_t a = mmap_next_;
    mmap_next_ += size;
    std::memset(mem_.ptr(a), 0, size);
    return a;
}

}  // namespace rt
