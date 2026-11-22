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

## 🟡 Phase 4: Static recompiler (approach B) (started 2026-11-19, owner chose it over dynarmic)

Goal: the game's ARM code translated ahead of time into C, compiled natively (x86-64 now, arm64 for
Android in Phase 5), running on the existing shim layer (HLE libc, JNI, GL bridge, audio, input,
mods). **Exit criterion: gameplay at 120+ fps on the dev PC** (Ryzen 5 5500 / RTX 3060).

Design (details and reasons in NOTES.md §11 once written):
- **Decoder:** our own exact decoder, in Python (`tools/recomp/`). The code is Thumb-1 (ARMv5T,
  19 instruction formats) plus ~3k ARM instructions (mostly `ldr ip,=x; bx ip` stubs and the PLT).
  Mapping symbols label every code/data boundary.
- **One C function per guest function** (`.symtab` names). Basic blocks become labels + `goto`.
  `bl` to a known function is a direct C call. A branch into another function becomes a tail call.
  The 227 `mov pc, reg` jump tables are recovered into `switch` statements. Other indirect jumps,
  `blx reg` and vtable calls go through an address → function table.
- **CPU state:** a struct (r0-r15 + memory base) passed to every function. NZCV flags are C locals
  (AAPCS doesn't keep flags across calls), so the C compiler drops dead flag computations. Guest
  memory stays "fastmem": guest address + host base, as today.
- **Imports:** a call through the PLT becomes a direct host call: memcpy is host memcpy, and the GL
  bridge, JNI and libc handlers are called straight from the generated code. **The soft-float
  helpers become plain C float operations**, which removes the ~55% float-call overhead.
- **Literal pools** are constants: there are no relocations in the code segment, so they are folded
  at translation time.
- Generated C goes to `work/recomp/` (git-ignored: it is derived from the game's code). Only the
  translator is committed.
- **Verification:** per-function differential tests against Unicorn (random registers and memory,
  compare results), then whole-game differential traces (sequence of import calls and their
  arguments, Unicorn backend vs recompiled backend) until boot and gameplay match.

Milestones (each one or more commits, each with a pass/fail check):
- **4.1 Survey.** Functions and code ranges from `.symtab` + mapping symbols. Decode everything.
  Per-function CFG: branch targets, calls, returns, tail calls, cross-function branches, extra entry
  points, jump tables, other indirect branches. Instruction histogram. Report in
  `analysis/recomp_survey.md`. ✔ when 100% of code decodes and every `mov pc, reg` is explained.
- **4.2 Code generator v0.** C for every Thumb/ARM form the survey finds, plus the runtime header
  (state struct, memory access, flag helpers, dispatch table). Build the generated C in a test target.
  ✔ when all of it compiles. **Done 2026-11-19** (0 unhandled instructions, compiles with MSVC).
- **4.3 Differential tests.** Planned as per-function unit tests on random inputs; in practice the
  whole-run comparison (deterministic clock + import trace + full memory/register dumps at the N-th
  import call or the N-th entry/exit of any function, bisected) found both real bugs within minutes,
  so it replaces the unit-test harness for now (NOTES §11). Revisit unit tests only if a divergence
  cannot be bisected this way.
- **4.4 Runtime backend.** The CPU interface used by HLE/JNI/mods works on both backends;
  `--cpu recomp`; import calls bind to handlers; function hooks become checks at generated function
  entries; trace-compare mode. ✔ when boot reaches the title screen with an identical import trace
  up to it. **Done 2026-11-19**: title screen, stable, game logic 0.2–0.6 ms/frame. Open: run with
  mods on (hooks are generated but untested), crash/hang handling polish.
- **4.5 Gameplay.** Menus, intro, gameplay, driving; fix divergences found by trace comparison.
  ✔ when the scripted test runs match the Unicorn build (screenshots, audio).
  **2026-11-19:** menus, intro and on-foot gameplay at the spawn run (mods on). Open:
  driving, missions, longer sessions; screenshot comparison against the Unicorn build.
  Correction 2026-11-20: "audio OK" only meant the play calls happened (counted in the log, not
  listened to). The owner hears music, voice lines and radio break and loop: see 5A.3.
- **4.6 Performance.** Registers cached in locals, inlined float helpers, cheap dispatch, PGO/LTO.
  Measure with `tools/bench.ps1`. ✔ at 120+ fps in gameplay.
  **2026-11-19:** 380–485 fps uncapped at the spawn (game logic 1.3–1.6 ms) without any of these
  optimisations yet: met on PC. Still matters for Android (slower CPUs).
- **4.7 Cleanup.** Unicorn stays as the reference/debug backend (`--cpu unicorn`). Notes and docs.

## ▶ Work queue (owner, 2026-11-22): do these in order, unattended
The owner is away and asked for all of this to be done, resuming automatically after usage limits
(a recurring check-in job in the Claude session re-sends "continue"). **On resume: read this list,
`git log --oneline -15`, and the matching NOTES section; continue with the first item not marked
done.** Mark each item ✅ here when it is committed and verified.

Rules while unattended:
- Never close or kill a `gangstar.exe` the owner started. If the build can't overwrite the exe
  because it is running, rename the running exe out of the way (allowed by Windows; the running game
  is unaffected) and build again.
- Test runs must not touch the owner's save, settings or log: use `GANGSTAR_HOME=<dir>` (sandbox,
  user settings and runtime.log all go under that folder; Q0).
- Small verified steps, one commit each, NOTES/PLAN kept current, unknowns written down.

Queue:
- ✅ Q0 Test isolation: `GANGSTAR_HOME`, test script uses a copy of the owner's save.
- Q1 Controls (owner's list): ✅ a-d (f7536d9: smooth physics, fire auto-aim, middle-click lock,
  reload, Space brake, VC layout); e helicopters: RE done, blocked on testing (no helicopter
  reachable from the save; NOTES Q1). Revisit when the owner reaches one.
  - a. Input delay: controls feel unresponsive. Measure input-to-game latency, remove what adds frames.
  - b. Manual aim recoil/spread much smaller than the game's (random) auto-aim spread.
  - c. Mouse: left button alone fires **with the game's own auto-aim** (as on the phone when you shoot or
    melee); right button = manual aim; middle click **toggles** lock on nearest / unlock. No more
    auto-aim on hold. Wheel scroll stays weapon switching. Controller: RB fire auto-aims the same way.
  - d. In cars: **Space = brake** (no longer fires; left mouse fires). **R = reload.**
  - e. Keyboard/mouse layout modelled on GTA Vice City PC, including helicopter controls.
- ✅ Q2 Free look, GTA VC PC style (5A.6): direct mouse, camera stays put, slow return after a pause, on
  foot and in cars; settings for the pause and the return speed.
- ✅ Q3 Aim leftovers: arm pose above the crosshair, aim point near walls (world ray), RB drive-by test.
- ✅ Q4 Radio box (5A.2): it was still cut with touch controls hidden (the right end is the radio
  on/off ToggleButton, which was hidden with them); now always drawn, verified at 800 and 853.
- Q5 Draw distance slider live + LOD options (5C.2, 5C.3), memory and fps measured downtown.
- Q6 Post-process groundwork (5C.0): pass between the 3D scene and the HUD, readable depth.
- Q7 Bloom (5C.5).
- Q8 Fog step 2 (5C.4): height fog, sun haze at sunrise/sunset, bloom stronger in fog.
- Q9 Dynamic lights (5C.6, owner said yes): car headlights/taillights, lampposts, muzzle flash.
- Q10 Android (5B), renderer chosen by us for Snapdragon **and** Mali: arm64 build, GLES 3 shader
  backend, texture decode, touch/tilt/controller, speed work, APK with saves in the app folder.
  Shader code for Q6-Q9 must stay portable to it (GLSL that builds as desktop GL and GLES 3).

## ⏸ Phase 5: fixes from play-testing, then the Android port (planned 2026-11-20; owner says when to start)

**Owner's play-test list (2026-11-20).** Each item gets its own commit and a pass/fail check.
Suggested order: shared root causes first (5A.0), then audio, then input, then the visual items.

- **5A.0 Frame-rate assumptions (likely shared cause).** Under Unicorn the game ran at ~20 fps;
  recompiled it runs at 100–500 fps. Things counted in frames are now 5–25× shorter: the mouse
  wheel already sends weapon next/prev, but as a **4-frame** button pulse (200 ms at 20 fps, under
  10 ms at 400 fps), and one-shot taps (`tap_frames_`) work the same way. Make pulses and taps
  time-based (e.g. ≥ 2 game updates / ≥ 50 ms). Check whether the game's own logic misbehaves at
  high fps: its timer gives an integer-ms dt, which is only 2 ms at 500 fps. ✔ when the wheel
  switches weapons reliably at 60, 144 and unlimited fps.
  **Done 2026-11-21** (NOTES §5A.0): "unlimited" stops at 1,000 fps, taps are time-based, wheel
  notches are queued and spaced. Weapon switching itself was a separate problem (5A.7).
- **5A.1 Sprint only works left/right.** Shift + A/D sprints, Shift + W/S does not. First
  suspicion: how the keyboard direction is placed on the virtual stick, or how the game decides
  "sprint" (stick magnitude/angle, or the sprint button only counting along one axis). Find the
  game's sprint condition (Ghidra: the player's on-foot control handler) and feed it what it
  expects. ✔ when sprint works in all 8 directions with keyboard and stick.
  **Done 2026-11-22**: `AnalogStick::processTouch` divides dx by the vertical screen scale and dy by
  the horizontal one (swapped), then clamps to its radius (49 units); the sprint button only shows
  at full deflection. Our 84 px push gave 56 units sideways but 47 forward. The virtual touch is now
  placed in the game's own units, 10% past the radius (NOTES §5A.1).
- **5A.2 Radio box clipped** on its right edge (known since Phase 3; not glScissor, not culling,
  not width). Next suspects: the sprite/gameswf path that draws the frame (mirrored right half,
  atlas UVs). ✔ when the box is complete at 800, 854 and 16:9 widths.
- **5A.3 Audio: music, voice lines and radio break and loop (~1 s); SFX are fine.** First
  isolate: same scene at `--fps 30` vs unlimited, and `--cpu unicorn` vs recomp. Suspects: the
  `GLMediaPlayer` stream semantics (isMediaPlaying / restart logic polled every frame), stream
  buffering in the mixer, or game-side timing at high fps (5A.0). ✔ when music, radio and
  cutscene voices play through without repeats, at any frame rate.
- **5A.4 Aiming and firing scheme (owner's design, 2026-11-20).** Fixes the random aim locks
  (on PC, clicks and mouse-look touches land on targets and count as the phone's "tap to target").
  - **Tap-to-target removed** for mouse, keyboard and controller play: nothing locks unless you ask.
    (Real touchscreens on Android: decide in 5B whether to keep the phone behaviour there.)
  - **Manual aim** while **RT** (controller) or the **right mouse button** is held: GTA Vice City
    PC-style crosshair (circle + centre dot), shots go where the camera points, no lock.
  - **Auto aim** while **LT** or the **middle mouse button** is held: lock onto the nearest target,
    stay locked while held, release to unlock.
  - **Fire:** controller **RB** on foot and in cars; mouse **left button** on foot, **right
    button** in cars (Space / Left Ctrl keep firing too).
  - Moves this forces on the controller: weapon prev/next LB/RB → **D-pad left/right**; radio
    next station D-pad right → **D-pad down** (radio on/off stays D-pad up); cover LT/B → **B**.
    In cars RT/LT stay accelerate/brake.
  - Needs RE of the targeting/shooting path: how the game picks a target (the tap handler, the
    nearest-target search), and how a shot direction is formed (ray from the camera instead of a
    picked target). Update the F1 controls table and README when done.
  - ✔ when walking and looking around never locks onto anything by itself; RT/RMB aims where the
    crosshair points (hit a chosen NPC, car or tyre); LT/MMB locks the nearest target; RB/RMB fires
    from a car.
  - **Done 2026-11-22** (NOTES §5A.4): no tap-to-target, no automatic lock; LT/MMB auto-aim; RT/RMB
    manual aim with GTA VC crosshair and a GTA V-style over-the-shoulder camera (owner's request),
    bullets redirected to what is under the crosshair; F1 table and README updated. Open: the aim
    animation holds the arm a little above the crosshair (visual only); tyres are not separate
    targets in this engine (Box2D bodies per vehicle); drive-by with RB not yet play-tested.
- **5A.5** merged into 5A.4.
- **5A.6 Real free look (camera).** GTA VC PC-style: snappy, direct mouse movement (no smoothing or
  acceleration), the camera **stays where you leave it**, and it only drifts back behind the player
  or car slowly, after a delay with no camera input, while moving or driving. Today's "freeform"
  only keeps the offset across one update and doesn't cover vehicles. Settings: return delay and
  speed. ✔ on foot and in cars.
- **5A.7 Weapon switching:** mouse wheel (already mapped, broken by 5A.0's too-short pulse), keys
  1/2, controller D-pad left/right (moved off LB/RB, see 5A.4). ✔ together with 5A.0.
  **Done 2026-11-22**: calls `Player::switchWeapon` directly (the selector is not a HUD button).
- **5A.8 Night, sunrise and sunset lighting too bright, no blue night tint.** The phone version
  had this problem on some hardware too. Leads: `CustomSkyBoxSceneNode::setSkyColor`, the
  time-of-day / ambient / fog parameters in the data, and GL state the bridge may not reproduce
  (texture env combiners, `GL_TEXTURE_ENV_COLOR`, lighting or material colours, fog). Compare what
  the game requests per time of day with what we render; fix in the bridge if we drop state, or
  in data/hook if the game picks the wrong (e.g. low-quality profile) values.
  ✔ when night is dark and blue-tinted, and sunrise/sunset are warm, like the intended look.
  **Done 2026-11-21**: the game's night textures were dead code; restored, plus a dawn/dusk tint
  (NOTES §5A.8). Setting `[graphics] night_textures`.
- Also still open from before: owner's call on Q11 (restore missing sounds by default), camera
  modes not yet tried in recomp mode, and Phase 4.6 speed work (matters most for Android).

**5C Visual upgrades (owner's list, 2026-11-21; 5C.1 and 5C.4 step 1 done, the rest after 5A, owner says when).**
The game tuned its distances for 2011 phones. Write every shader pass so it also runs on the GLES 3
bridge planned for 5B. **SSAO dropped** (owner, 2026-11-21: small gain on this low-poly city, costly
to tune).
- **5C.0 Groundwork for screen effects.** A hook at the exact point between the 3D scene and the HUD
  (a game function wrapped by the recompiler, like `DayTime::update`), the virtual framebuffer's depth
  as a readable texture (it is a renderbuffer today), and a small post-process pass chain run there,
  so effects never touch the HUD. ✔ when a test pass (e.g. invert colours) changes the 3D scene only.
- **5C.1 Characters, cars and the player follow the time of day.** They are lit by the game's one
  per-vertex light, whose night colour (ambient ≈ 0.01, diffuse 0.38/0.63/0.84) was tuned for the
  daylight city phones showed; next to the restored night city they look 2–4× too bright. Scale the
  light colours the game sends with the same dawn/dusk/night curve as the city, keep them a little
  brighter for readability (setting), and tint the unlit dynamic draws, leaving out headlights,
  glows, muzzle flashes, sky and HUD. First a night trace of which car/ped draws are lit.
  ✔ when people and cars sit in the night and sunset scene like the buildings do.
  **Done 2026-11-22** (NOTES §5C.1): the game never turns on GL lighting for them (colours are
  baked on the CPU), so the bridge tints those draws with the city's time-of-day colour, lifted by
  `night_people_lift` (F1 → Graphics). Glows and additive effects are left out.
- **5C.2 Draw distance slider does nothing.** Findings so far (NOTES §5C): it only applies at start
  (profile load), not at all with `quality = phone`, and it scales only the streaming radius and
  the low-res city boundary, which also set the fog range. It does not touch per-object LOD
  distances, terrain LOD or the camera far plane. Make it act on what limits the view, live if
  possible. ✔ with A/B screenshots at 0.5×, 1×, 2×, 4× from the same spot.
- **5C.3 LOD options: much less aggressive, or off.** Settings for the detail-to-low-res switch
  distance (multiplier up to "never switch inside the view") and per-object LOD. Leads:
  `LODSceneNode` / `GS3DStuff::parseSceneNodeForLOD`, `CLowResCitySceneManager::setNearPlane`
  (fed from `GetLowResFarPlane`), `CTerrainSceneNode::overrideLODDistance`, `g_perfConfigLowResFar`,
  `CCameraSceneNode::setFarValue`. Watch the guest memory and streaming: more detailed sectors
  loaded at once than a phone ever held. ✔ when driving downtown shows no visible pop-in at the
  highest setting, with memory and fps measured.
- **5C.4 Atmospheric fog (GTA SA-style).** Once the view reaches far, fog sells the depth. Step 1:
  retune the game's own fog (it is linear, from the streaming radius to the low-res boundary, colour
  from the time-of-day table) to scale with the new draw distance and match the sky's horizon
  colour. Step 2 (needs 5C.0): a depth-based fog pass with exponential falloff, height falloff
  (thicker near the ground and the sea) and a warm glow toward the sun at sunrise/sunset. Settings:
  strength, distance. ✔ with screenshots at noon, sunset, night.
  **Step 1 done 2026-11-21** (NOTES §5C.4): exponential, radial fog in the game's colours, now also
  on the near city, people and cars; `fog` and `fog_strength` in F1 → Graphics. Step 2 still open.
- **5C.5 Bloom.** Bright parts glow: the night city's lit windows and street lights, neon, headlights,
  the sun and sky at sunset. Threshold, then a downsample/upsample blur chain, added back before the
  HUD (5C.0). Light scattered through the fog (5C.4) so lights glow more in thick air. Settings: on/off,
  intensity, threshold, radius. ✔ with screenshots; cost measured at 1440p and on 5B hardware later.
- **5C.6 (tentative, owner to decide) Dynamic lights.** Car headlights/taillights first (the car's
  position and heading are known), street lights only once their positions are found (props are
  likely merged into batch meshes; placement data or hand-recorded list). Cheap GTA VC-style
  route: glow sprites, light pools on the ground, nearby lights tinting people and cars; full
  per-pixel lights only with a shader-based renderer.

**5B Android arm64 handhelds (the main target).** Same generated C + shim layer, built for arm64
with clang, SDL2 for Android; GLES 3 backend for the GL bridge; DXT textures need a software
decode path on GPUs without S3TC (Adreno/Mali); settings UI usable with a controller; touch
controls shown/hidden per `touch_controls`; target devices Retroid Pocket 6 / Odin-class at
120+ fps where the display allows. Fixes from 5A carry over automatically (shared code).
