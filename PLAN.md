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
## ✅ Phase 1: Boot probe (done 2026-11-18)

- [x] 1a loader: all relocations applied and cross-checked (`analysis/probe_1a.md`)
- [x] 1b 448/448 static constructors run, no stubs hit (`analysis/probe_1b.md`)
- [x] 1c onSurfaceCreated JNI sequence runs; 36/36 method lookups match Java;
      native sound count agrees with Java's table (`analysis/probe_1c.md`)
- Deviation from the original plan: 1c's "every `nativeGetSoundFileName` result exists"
  check was replaced, because that native needs the `FileManager` from later init and Java
  never calls it. The replacement compares the native count against Java's hard-coded
  1,737-name table (= 1736 + one Motorola-only slot). Findings: NOTES §6a.

The original Phase 1 proposal is kept below for reference.

### (as proposed) Phase 1: Boot probe

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
## ✅ Phase 2: from "init runs" to "first frame on screen" (done 2026-11-18)

- [x] 2a headless full boot in the probe: startup + 60 frames (and a 1,500-frame run reaching the
      city load), no faults, no stubs; complete JNI/GL/file surface for boot (`analysis/probe_2a.md`)
- [x] 2b C++ runtime with Visual Studio 2026: boots identically to the probe and renders the
      **content-warning screen and the title screen** in a window at ~90-100 fps (NOTES §6b)
- Deviation: 2a was planned with `isDemo=0`; it runs with `isDemo=1`, which is what Java actually
  passes on a fresh install (NOTES Q7). It went further than the 2b goal (loading screen): title screen.

The original Phase 2 proposal is kept below for reference.

### (as proposed) Phase 2: from "init runs" to "first frame on screen"

Split in two so the C++ work starts from a fully mapped surface instead of guesses.

### 2a. Headless full boot in the probe (Python, no new installs)
**Goal:** run the rest of the real startup and the first frames entirely in the probe,
with a *recording fake GL*: `Gangster2.nativeInit(isDemo=0)` → `GameRenderer.nativeInit(1)`
→ `nativeResize(800, 480)` (the P990's screen) → `nativeRender()` × N.
- Fake GL: `glGen*` hand out ids, `glGetString` returns a plausible GLES 1.1 / Tegra-style
  vendor/renderer/extensions string, `glGetIntegerv` answers limits, FBO status = complete;
  everything else is recorded (call name + args) and returns 0. Texture uploads are recorded
  with format/size, not decoded.
- Real file reads via the existing read-only overlay; any write lands in `work/probe_sandbox/`.
- Java callbacks answered from the decompiled behaviour (`isDemo` = 0 path, `GetDeviceType` -1, …).
- ✔ Pass if: both inits return; N ≥ 60 `nativeRender` calls return without faults; the report
  lists every file opened, every JNI function and Java callback used (closes Q1), every GL
  function used with counts, the texture formats uploaded, and whether the `FileManager` exists
  afterwards (Q12; then `nativeGetSoundFileName` should work too). Also confirms the `_ctype_`
  layout (Q10) if any parsing code runs.
- Expected trouble spots: threads (`pthread_create`: the probe will have to run thread entry
  points cooperatively or on demand), `pthread_cond_wait` loops, timing (`gettimeofday`), and
  the demo/billing callbacks.

### 2b. C++ runtime skeleton on Windows (emulated CPU, real window)
**Toolchain (my recommendation; install is pre-approved):** the current **Visual Studio
Community with the C++ workload** (MSVC + its bundled CMake/Ninja + clang-cl), because its
debugger is what a Unity/C# developer already knows and the crash-fix loop lives in the
debugger. Alternative if the owner prefers small and portable: **LLVM-MinGW** (~150 MB,
debug with lldb/gdb). Plus SDL2, the Unicorn C library, and Ghidra (JDK 21 is already there)
for targeted RE.
- Port the probe's loader, HLE libc and fake JNI to C++ in `src/` (same structure, the probe
  stays as the reference/oracle).
- SDL2 window + OpenGL **compatibility** context (RTX 3060: GL 4.6 compat, fixed-function and
  S3TC available); GLES 1.1 → desktop GL mapping (`glClearDepthf`→`glClearDepth`, OES FBO →
  core FBO, `glMapBufferOES` → `glMapBuffer`, …), driven by 2a's list of GL functions actually used.
- ✔ when the same boot sequence as 2a runs in the C++ build and the first game-drawn frame
  (loading screen) appears in the window.

## 🟡 Phase 3: playable under emulation (approved 2026-11-18; in progress)

Goal: from the title screen into the menus and the first mission, with sound, on Windows,
plus the owner's additions: resolution options, keyboard + mouse + controller, hideable touch
controls (Windows and Android alike), graphics profile, better draw distance, QoL.
Each step is small, committed separately, and verified with screenshots + the runtime log.

- ✅ **3a Input (mouse/touch).** Clicks → `nativeOnTouch`; title screen, menus, cutscene skip work.
- ✅ **Speed.** Guest-side float fastpath + Cortex-A15 model: gameplay ~12 → 20-40 fps (scene dependent).
- ✅ **3f Window / resolution.** Virtual framebuffer: any aspect at 480 high, render scale (auto =
  window height), MSAA, fit aspect/stretch/integer, resizable, Alt+Enter/F11 borderless, `settings.ini`.
- ✅ **3d Graphics profile & draw distance** (Q14): `quality` phone|low|high|ultra, `draw_distance`,
  `mipmaps`. Still to do: visual A/B screenshots (phone vs high vs ultra) and `anisotropy`.
- ✅ **Keyboard + controller** → virtual touches on the live HUD; menu cursor; tilt steering for
  pads/keys; touch controls hide automatically while a pad/keyboard drives (`touch_controls`).
  Open: Q16 (other steering modes), Q17 (weapon switch), Q19 (pause-menu resume), Q21 (flying).
- ✅ **FPS modes.** Presets: display refresh (default), 30/60/90/120/144/165/240/360, unlimited.
  Exact pacing measured. Game timing is frame-rate independent (Q20, §6c). Under emulation, gameplay
  can't reach 120 (CPU-bound, ~20-40 fps). That comes with Phase 4, which only has to be faster.
- ✅ **Settings overlay** (Dear ImGui; F1, controller Guide or L3+R3): display, graphics, controls, game;
  live-applies what it can, marks restart-only options, saves `settings.ini`. FPS counter, toasts.
- ➖ **3b Threads.** Not needed: sound doesn't go through vox in this build (Q13). The vox thread stays
  unstarted; revisit only if something depends on it.
- ✅ **3c Audio.** `GLMediaPlayer` on an SDL2 mixer + stb_vorbis; Java semantics; Q11 restore as a setting
  (default on); volume sliders; pauses with the menu/focus loss. Verified by WAV capture.
- ✅ **3e Saves (dev).** `/data/data/<package>/Gangstar2.sav` and `.preferences` live in `work/sandbox/data/`. They
  are read at boot, so the main menu offers CONTINUE across runs. Packaging moves them to a per-platform user
  folder (Windows: %APPDATA%; Android: the app's files dir).
- ✅ **Owner list (2026-11-19):** in-game exit, store buttons removed, "Press any button to start",
  credits line, fast forward, texture filtering, mouse look, low-latency mode, camera modes
  (classic / freeform / first person). Open: radio box clipping; performance (next: dynarmic
  stopgap or straight to Phase 4, owner decision; emulated gameplay is ~49 ms/frame, Low ~39 ms).
- ⬜ **QoL.** Skip the content warning / intro where possible (`skip_intro`), anisotropic filtering,
  FPS counter overlay. Pause on focus loss is done.
- **Owner decisions needed along the way:** Q7 (trial/Verizon purchase gate) and Q11 (sound names).
- Intro video (`intro.m4v`, H.264) is deferred: the game already continues when `isMediaPlaying` is 0.

## Direction (owner, 2026-11-19)
- **120 fps and higher must work in the final game** (presets up to 240 + unlimited).
- **The Android port (arm64 handhelds) is the main focus once the recompilation is done.** Windows
  remains the bring-up and test platform; other platforms come later.
- Consequences for the design:
  - Everything in `src/` except the CPU backend (HLE, JNI, GL bridge, input, settings, mods) must stay
    portable C++ (SDL2, GL/GLES). On Android the GL bridge targets GLES 3 directly.
  - Input already treats touch, keyboard and controller alike. On Android, real touches pass straight
    through and controllers use the same virtual-touch layer. Real tilt can feed `nativeAccelerometer`
    (virtual steering only replaces it while a pad steers).
  - Game hooks (`mods.cpp`) are keyed by symbol name and must work with both CPU backends: in the
    recompiled build a hook becomes a call at the top of the generated C function.
  - The recompiler output must be target-neutral C/C++ (no x86 assumptions), so the same output builds
    for x86-64 Windows and arm64 Android.

## ⬜ Later phases (tentative; refine after Phase 3)

**Phase 4: Static recompiler (approach B).** Python tool: symbols + mapping symbols →
one C function per ARM/Thumb function; CPU-state struct; indirect calls through an
address→function table; recovered jump tables; ARM↔Thumb interworking; `setjmp/longjmp`.
Differential testing against the Phase 1/2 emulator per function. Swap the CPU backend
⇒ the game runs as native x86-64 code. Soft-float helper calls become host float ops
(~55% of executed instructions today are float-call overhead). **Exit criterion now includes
120+ fps in gameplay on the dev PC** (Ryzen 5 5500 / RTX 3060).

**Phase 5: Android arm64 handhelds (main target after Phase 4).** Same generated C + shim layer,
built for arm64 with SDL2 for Android; GLES 3 backend for the GL bridge; settings UI usable with a
controller; touch controls shown/hidden per `touch_controls`; target devices Retroid Pocket 6 /
Odin-class at 120+ fps where the display allows.
