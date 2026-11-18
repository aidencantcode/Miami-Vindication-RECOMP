# PLAN: milestones and status

Status legend: ✅ done · 🟡 proposed / waiting for owner OK · ⬜ later (tentative)

---------------------------------------------------------------------------
## ✅ Phase 0: Analysis (done 2026-11-18)

- [x] Tool check; portable Git, Python + pyelftools/capstone, JDK 21, jadx installed (owner OK)
- [x] Layout, `.gitignore`, `git init`; originals moved into `original/` (owner OK)
- [x] APK extracted; both `.so` analysed (ABI, symbols, relocations, imports, exports)
- [x] jadx decompile; Java ↔ native boundary mapped (`analysis/jni_surface.md`)
- [x] Data folder inventoried (`analysis/data_inventory.md`)
- [x] Why it fails on modern devices (`NOTES.md` §8)
- [x] Approach recommendation (below)

---------------------------------------------------------------------------
## Recommended approach for Windows x86-64: **C (hybrid)**, with B as the destination

**The binary is unusually well-suited to static recompilation (B):**
- Not stripped: 24,492 named functions, so every function entry and every possible
  indirect-call target is known.
- ARM mapping symbols label every code/data boundary, which removes the hardest
  problem in static translation (telling code from data).
- 99.8% Thumb-1 (1.40M Thumb vs 3.1k ARM instructions), ARMv5TE, **soft-float**: no
  VFP/NEON. A small, regular instruction set; float math is already function calls
  (`__aeabi_*` imports) that we implement natively.
- No C++ exceptions, no runtime code generation, no text relocations.
- Bounded indirect flow: 227 switch jump tables; 13k indirect calls all resolvable
  through one address→function table.
- B is also what the secondary target needs: the generated C compiles for arm64
  Android handhelds that can't run 32-bit ARM.

**Why not go straight to B:** a recompiler can't be tested without a working runtime
(fake JNI, GLES 1.1 → desktop GL, files, audio, input). Without a known-good CPU,
every bug is ambiguous: translation error or shim error? An emulator (approach A)
gives correct CPU semantics for free, so we can:
1. build and debug the **shim layer** (shared by A and B) against real game behaviour,
2. discover the real runtime surface (which imports, JNI functions and callbacks are
   actually used, and in what order),
3. later use it as a **differential-testing oracle**: run each recompiled function and
   the emulated original on the same inputs and compare results.

**Why not stop at A:** fine as a bootstrap, but every import/JNI/GL call crosses the
emulator boundary (slow, awkward to debug), multi-threading (`pthread_create`) under
Unicorn is clumsy, and it doesn't serve the arm64 target. So we keep the emulator
**lightweight (Unicorn), not a product**, and don't invest in a JIT like dynarmic.

Target order stays as in CLAUDE.md (Windows first). Nothing found suggests switching.

---------------------------------------------------------------------------
## 🟡 Phase 1: Boot probe (proposed; waiting for owner OK)

**Goal:** prove we can load `libGangster2.so`, relocate it, run its static
constructors, and call real JNI entry points with correct results, all *before*
committing to a C++ toolchain, a window or graphics.

**Form:** a Python research tool, `tools/probe/`, using **Unicorn** (CPU model
ARM926 = ARMv5TE). It stays useful afterwards as the import/JNI tracer and as the
reference oracle for Phase 4.

**Needs (ask first):** `pip install unicorn` (Unicorn 2.x, prebuilt Windows wheel,
into the portable Python). Nothing else.

### Steps (each one commit; each has its own pass/fail check)

**1a. Loader, no execution**
- Map both `PT_LOAD` segments at a fixed base, zero-fill `.bss`, apply all 47,173
  relocations; bind the 299 imports to trap addresses; give the 7 data imports
  (`__sF`, `_ctype_`, `_tolower_tab_`, `_toupper_tab_`, `__stack_chk_guard`,
  `__dso_handle`, `std::nothrow`) memory blocks with bionic layouts.
- ✔ Pass if: relocation counts match the ELF (43,374 / 3,502 / 7 / 290); every
  import slot points into the trap region; a sample of ≥ 20 C++ vtables (`_ZTV*`) in
  `.data.rel.ro` has every slot resolving to a known function symbol after relocation.

**1b. Run the 448 `DT_INIT_ARRAY` constructors**
- Every import starts as a logging stub (`STUB: name(r0..r3) -> 0`). Implement real HLE
  versions only when a constructor needs one (expected: `memcpy`/`memset`/`strlen`,
  soft-float `__aeabi_*`, `__cxa_guard_*`, `__aeabi_atexit`, `pthread_mutex_*`, `sbrk`).
- ✔ Pass if: all 448 return; no unmapped memory access; we get the list of imports
  used during static init (answers NOTES Q4).

**1c. First JNI calls with a fake `JNIEnv`**
- Fake `JNIEnv`: a full JNI function table where every slot is a trap that logs its
  JNI name. Implement just what's hit (expected: `GetStaticMethodID`, `NewStringUTF`).
- Call, in the real startup order: `GameRenderer.nativeGetJNIEnv` →
  `GLResLoader.nativeInit(0)` → `GLMediaPlayer.nativeInit(0)` →
  `nativeGetTotalSounds()` → `nativeGetSoundFileName(i)` for every `i`.
- ✔ Pass if: sound count > 0 and **every returned file name exists** in
  `original/gameloft/games/Gangstar2/` (case-insensitive). That is an end-to-end check
  of ARM execution + relocation + fake JNI against independent data.
- Output: `analysis/probe_jni_trace.md` listing JNI functions used and every
  `GetStaticMethodID(class, name, sig)` (answers NOTES Q1–Q3).

**Out of scope for Phase 1:** `Gangster2.nativeInit` / `GameRenderer.nativeInit` /
`nativeRender` (these need a GL context and file I/O), threads, graphics, audio.

**Risk:** `nativeGetSoundFileName` might depend on state set up by a later init call.
If so, 1c reports exactly which import/JNI call it stopped at, and we adjust.

---------------------------------------------------------------------------
## ⬜ Later phases (tentative; refine after Phase 1)

**Phase 2: C++ runtime skeleton on Windows (still emulated CPU).**
Owner decision first: toolchain (LLVM-MinGW, portable and self-contained, vs Visual
Studio Build Tools + clang-cl, for the VS debugger), plus CMake, Ninja, SDL2, and
Ghidra for targeted RE. Port the loader to C++ (Unicorn C API); SDL2 window + OpenGL
**compatibility** context (RTX 3060: GL 4.6 compat, fixed-function and S3TC
available); GLES 1.1 → desktop GL mapping (`glClearDepthf`→`glClearDepth`, OES FBO →
core FBO, `glMapBufferOES` → `glMapBuffer`, …); fake JNI in C (FalsoJNI-style);
`/sdcard/...` and `/data/data/...` path mapping; bionic libc shims.
✔ when `nativeInit` + `nativeResize` + the first `nativeRender` frames run and
something game-drawn (loading screen) appears.

**Phase 3: Playable under emulation.** Input (mouse/keyboard/gamepad → touch +
keycodes), audio (`GLMediaPlayer` callbacks → Vorbis via SDL/miniaudio), intro video
(Media Foundation), saves, threads. Study the StormGLOFT hook list (resize, touch,
device type, mipmaps, draw distance) for resolution/aspect fixes.

**Phase 4: Static recompiler (approach B).** Python tool: symbols + mapping symbols →
one C function per ARM/Thumb function; CPU-state struct; indirect calls through an
address→function table; recovered jump tables; ARM↔Thumb interworking; `setjmp/longjmp`.
Differential testing against the Phase 1/2 emulator per function. Swap the CPU backend
⇒ the game runs as native x86-64 code.

**Phase 5: Android arm64 handhelds.** Same generated C + shim layer, built for arm64
with SDL2 for Android.
