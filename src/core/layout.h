#pragma once
// Guest address map. Same layout as the Python probe (tools/probe/emu.py) so logs,
// addresses and reports from both can be compared directly.
#include <cstdint>

namespace layout {

constexpr uint32_t kHandleBase = 0x0D000000;  // fake Java object handles: deliberately UNMAPPED
constexpr uint32_t kTrapBase = 0x0E000000, kTrapSize = 0x00020000;  // 8-byte `svc #0; bx lr` stubs
constexpr uint32_t kDataBase = 0x0E100000, kDataSize = 0x00100000;  // bionic data objects, JNI tables
constexpr uint32_t kLibBase = 0x10000000;                            // libGangster2.so
constexpr uint32_t kHleHeapBase = 0x30000000, kHleHeapSize = 0x10000000;
constexpr uint32_t kSbrkBase = 0x40000000, kSbrkLimit = 0x58000000;  // the game's own malloc grows this
constexpr uint32_t kMmapBase = 0x60000000, kMmapLimit = 0x70000000;
constexpr uint32_t kStackBase = 0x7F000000, kStackSize = 0x01000000;  // main thread; more stacks below

}  // namespace layout
