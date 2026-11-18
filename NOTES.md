# NOTES: Gangstar: Miami Vindication native port

Running findings log. Newest log entries at the bottom. Read this and `PLAN.md`
at the start of every session.

---------------------------------------------------------------------------
## 0. How to resume

- Tools are **portable**, in `C:\Users\aiden\devtools\` (not on the system PATH).
  In every PowerShell session first run: `. .\tools\env.ps1`
- Python scripts that read game files run with `python -I` (isolated mode).
- Regenerate the analysis outputs:
  ```
  python -I tools\elf_report.py work\apk\lib\armeabi\libGangster2.so --json analysis\raw\libGangster2.elf.json
  python -I tools\elf_report.py work\apk\lib\armeabi\libStormGLOFT.so --json analysis\raw\libStormGLOFT.elf.json
  python -I tools\import_report.py analysis\raw\libGangster2.elf.json --sibling analysis\raw\libStormGLOFT.elf.json --out analysis\libGangster2
  python -I tools\import_report.py analysis\raw\libStormGLOFT.elf.json --sibling analysis\raw\libGangster2.elf.json --out analysis\libStormGLOFT
  python -I tools\so_strings.py work\apk\lib\armeabi\libGangster2.so analysis\raw\libGangster2.strings.txt
  python -I tools\data_inventory.py original\gameloft\games\Gangstar2 analysis\data_inventory.md
  jadx --log-level ERROR --show-bad-code -d analysis\jadx original\game.apk
  ```
- Phase 1 boot probe (Python + Unicorn), each step re-runnable, report in `analysis/probe_<step>.md`:
  ```
  python -I tools\probe\test_unicorn_semantics.py   # Unicorn behaviours the probe relies on
  python -I tools\probe\test_hle.py                 # soft-float / strtol / printf unit tests
  python -I tools\probe\probe.py 1a|1b|1c           # load+verify | constructors | first JNI calls
  ```
  The probe never writes into `original/`: guest writes go to `work/probe_sandbox/`.
- Git-ignored, local-only: `original/`, `work/`, `analysis/raw/`, `analysis/jadx/`.
  Committed: our tools, these notes, and small derived reports in `analysis/*.md`.

## 1. Environment (checked 2026-11-18)

Machine: Windows 10 IoT Enterprise LTSC 2021 (no winget), AMD Ryzen 5 5500,
24 GB RAM, NVIDIA GeForce RTX 3060 (driver 32.0.16.2114), 1920×1080.
Nothing from CLAUDE.md's tool list was installed.

Installed in Phase 0 (portable, official sources, SHA-256/512 verified):

| tool | version | path under `C:\Users\aiden\devtools\` |
|---|---|---|
| Git for Windows (PortableGit) | 2.56.1 | `git\cmd\git.exe` |
| Python (python.org NuGet build) | 3.13.17 | `python-nupkg\tools\python.exe` |
| pip packages | pyelftools 0.33, capstone 5.0.9, unicorn 2.1.4, itanium_demangler 1.1, pillow 13.0.0 | (in that Python) |
| **Visual Studio Community 2026** (installer, admin) | MSVC 19.52 (14.52.37025), bundled CMake, Ninja, vcpkg 2026-09-26, Clang | `C:\Program Files\Microsoft Visual Studio\18\Community` |
| vcpkg packages (manifest `vcpkg.json`) | SDL2 2.32.10 | `build\win-x64\vcpkg_installed` |
| Unicorn C library | 2.1.4: the DLL + headers from the pip wheel, import lib generated | `third_party\unicorn` (git-ignored; `tools\setup_unicorn.ps1`) |

The VS installer returned 3010 (success, **reboot recommended**; the tools work before it).
**Licence note:** Unicorn is GPLv2. That's fine for this private port; it disappears from the
final build once the game is statically recompiled (Phase 4).

Native build: `.\tools\build.ps1` (or open the folder in Visual Studio; `CMakePresets.json`).
Run: `build\win-x64\RelWithDebInfo\gangstar.exe --run [--frames N] [--shot-every K] [--demo 0|1]`,
`--selftest-load`, `--selftest-fastpath`, `--window`. Log: `build\runtime.log`. Screenshots:
`build\shots\run_<time>\` (convert with `python -I tools\shots_to_png.py`).
More run options (Phase 3): `--seconds S`, `--shots-at 650,t62.5` (frames or seconds), `--input
"<script>"`, `--fps N` (0 = unlimited), `--no-vsync`, `--game-size WxH`, `--quality
phone|low|high|ultra`, `--touch-controls show|hide|auto`, `--dump-hud S` (log the HUD controls at S
seconds), `--profile-traps` (busiest imports + hook calls every 2 s), `--profile-guest S` (guest
instruction profile from S seconds), `--no-mods` (vanilla game, no hooks), `--set section.key=value`
(override any settings.ini key for this run, repeatable), `--open-settings S` (open the F1 menu at S).
**Input script** (`;`-separated, each `frame:cmd` or `t<seconds>:cmd`, at most one step per frame):
`tap x y`, `down/move/up x y` (game pixels, e.g. 854×480), `key <android keycode>`,
`pad <a|b|x|y|lb|rb|lt|rt|start|back|up|down|left|right> <0|1>`, `stick <l|r> <x> <y>` (−100..100).
Script that reaches gameplay from a fresh start (generous waits for loads):
`t20:tap 427 300; t32:tap 200 210; t44:tap 800 35; t48:tap 800 35; t52:tap 800 35; t56:tap 800 35`
(title → CONTINUE → skip the intro cutscene). Gameplay by ~t60. Then e.g. `t61:pad y 1;
t62:stick l 0 -100; t65:stick l -100 0; t67:stick l 0 0; t70:pad y 0` usually ends in the car.
Walks are timing-sensitive (frame rate varies), so prefer "hold" commands to exact paths.
Ghidra: `.\tools\decompile.ps1 -Out analysis\raw\X.c -Names "sym1","callers:sym2",...` (query
forms in `tools\ghidra\DecompileFunctions.java`). Ghidra address = lib offset + 0x10000.
Settings: `work\user\settings.ini` (written with defaults on first run).
| Temurin JDK | 21.0.13+9 | `jdk-21.0.13+9\` |
| jadx | 1.5.6 | `jadx\bin\jadx.bat` |

Downloads kept in `devtools\_downloads\` (can be deleted). Built-ins also
available: `tar.exe`, WinRAR, .NET Framework (PowerShell).

**Install permission (2026-11-18):** the owner said "install whatever u need however
u need pip python everything". This replaces CLAUDE.md's "ask before installing";
still ask before deleting files or changing the approach. Keep installs portable under
`devtools\`, verify checksums, record versions here.

**Installed in Phase 3:** Ghidra 12.2 (`ghidra_12.2_PUBLIC\`), which needs **JDK 25** (Temurin
25.0.5+8, `jdk-25.0.5+8\`; set as `JAVA_HOME_OVERRIDE` in Ghidra's `support\launch.properties`).
The analysed project is `analysis\ghidra\Gangstar` (git-ignored). Headless use: `tools\decompile.ps1`.

**Not installed yet:** ANGLE, ADB/Frida, 7-Zip (not needed;
zip handled by .NET). (CMake, Ninja, Clang and MSVC now come with Visual Studio; see below.)

Git: repo-local identity `Aiden <owner's email>` (set with `git config`, not global).

## 2. Inputs (`original/`, read-only)

Moved here from the project root on 2026-11-18 at the owner's request (same-drive
rename; 2,405 data files / 375,000,925 bytes verified before and after).

| file | SHA-256 |
|---|---|
| `original/game.apk` (5,594,662 B) | `f9ada99ed99c2aa3afe76f98c9ba52892f9a7e9e8e81eb2e2c8285d8d8994bd3` |
| `lib/armeabi/libGangster2.so` | `136fa825c6ef72a1f1eb1be3625777cdb50eb2ec1e0c5bf12cbd47c48697a00d` |
| `lib/armeabi/libStormGLOFT.so` | `8d391dec21b8ca2bd1355291612dd53ada630308f32722e59ecbf46d84883044` |
| `classes.dex` | `7745c9fd7e1200a9bca1575be29d004e39f85d2b21c694f000c9a24c3628ba5e` |

`original/gameloft/games/Gangstar2/`: game data (section 7).

## 3. The APK

- **Package:** `com.gameloft.android.GAND.GloftGMHP.ML`, versionName 1.0.6, versionCode 106.
- **minSdkVersion 5, no targetSdkVersion** ⇒ effective targetSdk = **5** (Android 2.0).
- `compileSdkVersion 27` / `platformBuildVersionCode 27` ⇒ re-packed with 2018-era tools.
- `uses-feature glEsVersion 0x20000` (claims GLES 2.0), although the code only uses GLES 1.1.
- Permissions: storage, internet, Wi-Fi/network state+change, READ_PHONE_STATE,
  Bluetooth, wake lock, vibrate, kill background processes.
- Activities: `SplashActivity` (launcher) → `installer.GameInstaller` → `Gangster2`
  (main) + `MyVideoView`, `IGPActivity`, `GLiveMain`. All landscape.
- **This is a repackaged APK, not Gameloft's original release:** signed with
  `META-INF/MYAPP.RSA`, `MANIFEST.MF` says `Created-By: 1.8.0_102` (Java 8u102, 2016),
  compile SDK 27, and it carries a 2018 third-party library (`libStormGLOFT.so`, §5).
  `SplashActivity` (a 5-second sleep with its own theme) may also be an addition.
- `res/raw/data.txt`: data download URLs, `PVRT:` and `ATC:` **both point at**
  `http://dl.gameloft.com/hdplus/android/Gangster2/LG_P990/ML/1.0.5/Gangstar2_LG_P990_DXT.zip`.
  So this is the **LG Optimus 2X (P990, NVIDIA Tegra 2) "HD+" build, using DXT textures**.
- `res/raw/infoversion.txt` = `1.0.6`; `res/raw/serialkey.txt` holds an installer
  key (value deliberately not copied here); `installer_res*.txt` = file lists for the installer.
- No `assets/` folder. No `arm64-v8a`, `armeabi-v7a` or `x86` folders.

### Native libraries

| lib | ABI folder | size | stripped? | built with | role |
|---|---|---|---|---|---|
| `libGangster2.so` | `armeabi` | 9,475,002 B | **No**: full `.symtab` (70,389 symbols) + a little DWARF | GCC 4.4.0 (NDK r4-era, ~2010) | the game |
| `libStormGLOFT.so` | `armeabi` | 900,952 B | yes (dynamic symbols only) | NDK **r16b**, clang 5.0 + gold 1.12, API 14 (2018) | third-party compatibility patch |

## 4. libGangster2.so (the game)

### ELF / ABI facts
- ELF32 little-endian, `ET_DYN`, EM_ARM, EABI v5, `e_flags 0x5000002`.
- Build attributes: **CPU arch v5TE**, ARM ISA + **Thumb-1** (`Tag_THUMB_ISA_use=1`),
  **no FP arch tag ⇒ soft-float, no VFP/NEON instructions**. wchar_t = 4 bytes, enums = int-sized.
- Segments: RX `0x0–0x5fcc44` (file 6.28 MB), RW `0x5fdc44`, memsz 4.87 MB
  (`.bss` 4.59 MB), `PT_ARM_EXIDX`, `PT_GNU_STACK` **RWX**. No `PT_GNU_RELRO`, no `PT_PHDR`.
- Dynamic: `DT_NEEDED` = libc, libGLESv2, libGLESv1_CM, libstdc++, libm, libdl, liblog
  (libdl and the GLESv2-only API are not actually used). **No `DT_SONAME`.**
  `DT_SYMBOLIC` set. `DT_HASH` only. `DT_INIT_ARRAY` with **448 constructors** (0x700 bytes).
- Relocations: 43,374 `R_ARM_RELATIVE`, 3,502 `R_ARM_ABS32`, 7 `R_ARM_GLOB_DAT`,
  290 `R_ARM_JUMP_SLOT`. **No relocations land in the read-only segment (no text relocations).**
- Sections: `.text` 3.19 MB @ `0x274c98`, `.rodata` 270 KB, `.data.rel.ro` 223 KB
  (vtables), `.got` 13.5 KB, `.data` 40 KB.

### Symbols (this is the big win)
- 24,492 `STT_FUNC` symbols with real (C++-mangled) names; 25,437 dynamic exports.
- ARM **mapping symbols**: 24,665 `$t` (Thumb), 786 `$a` (ARM), 14,876 `$d` (data).
  Every code/data boundary in `.text` is labelled.
- Defined functions: 23,415 Thumb, 785 ARM. Decoded instructions: **1,403,136 Thumb, 3,136 ARM**.
  The ARM code is almost entirely tiny `ldr ip,=x; bx ip` interworking stubs (778 `bx ip`).

### Code shape (relevant to recompilation)
- **No C++ exceptions**: no `__cxa_throw`, `__cxa_begin_catch`, `__gxx_personality_v0`
  or `_Unwind_RaiseException` anywhere. (`__aeabi_unwind_cpp_pr0/1` imports exist only
  because `.ARM.exidx` references them.)
- **No runtime code generation**: no `mprotect` import.
- Indirect control flow in Thumb: 13,083 `blx reg` (virtual calls / function pointers),
  **227 `mov pc, reg`** (switch jump tables), 57 `bx reg` (non-`lr`), 18,331 `pop {…,pc}` returns.
  No `__gnu_thumb1_case_*` helpers. ARMv5T `pop {pc}` interworks (Thumb/ARM switch).
- **The game has its own allocator**: `malloc`, `free`, `operator new/delete[]` are defined
  in `.text`, and it imports **`sbrk`**. Our runtime must give `sbrk` a contiguous
  region. Risk to watch: libc functions that allocate (e.g. `strdup`) return memory
  the game may later pass to *its own* `free`.
- Uses `setjmp`/`longjmp` (probably libjpeg/libpng error paths), `pthread_create`
  (the game starts threads), wide-char functions (`swprintf`, `wcs*`).
- Engine: Gameloft **Glitch** (`lib\Glitch\...`), Gameloft framework `glf::`
  (`glf::App::Init(glf::CreationSettings&)`), **gameswf** (Flash UI), libjpeg.
  Built from a **Visual Studio 2008** solution (`project_vs2008`). The engine had
  Win32 builds in-house (`lib\gameswf\platform\win32`, a leftover
  `sconsmakedata platform=PC pvrtc=False` command string).

### Imports: the "Android surface" (299 symbols)
Full list: `analysis/libGangster2.imports.md`. Summary:

| group | count | notes |
|---|---:|---|
| libc (general) | 94 | stdio, string, file, time, `setjmp/longjmp`, `sbrk`, `opendir/readdir`, `stat`, `mmap` |
| GLES 1.1 common (ES1/ES2 names) | 47 | `glBindTexture`, `glDrawElements`, `glCompressedTexImage2D`, VBOs… |
| ARM EABI runtime helpers (libc/libgcc compat) | 39 | soft-float `__aeabi_f*/d*`, int division, 64-bit ops |
| GLES 1.1 fixed-function only | 34 | `glMatrixMode`, `glLightfv`, `glTexEnvf`, `glVertexPointer`… |
| libm | 31 | float + double math |
| pthreads / sched | 30 | mutex, cond, TLS keys, create/join, scheduling params |
| GLES 1.1 OES extensions | 14 | `glBindFramebufferOES`, `glGenerateMipmapOES`, `glMapBufferOES`, `glBlendEquationOES`… |
| libstdc++ (Android minimal) | 5 | `operator new[](nothrow)`, `std::nothrow`, `__cxa_guard_*`, `__cxa_pure_virtual` |
| C++ unwinder | 2 | `__aeabi_unwind_cpp_pr0/1` (referenced, never used) |
| liblog | 2 | `__android_log_print/write` |
| C++ ABI in libc | 1 | `__aeabi_atexit` |

- **No EGL, no OpenSL ES, no libandroid, no dlopen/dlsym, no GLES2 shader API.**
  The Java GLSurfaceView owns the EGL context, and Java plays all audio and video.
- **7 data imports with bionic layouts** we must reproduce exactly: `__sF` (bionic
  `FILE[3]`), `_ctype_`, `_tolower_tab_`, `_toupper_tab_`, `__stack_chk_guard`,
  `__dso_handle`, `std::nothrow`.

### JNI exports
23 `Java_*` functions, all Thumb, **no `JNI_OnLoad`**. See
`analysis/libGangster2.jni_exports.md` and the full boundary in `analysis/jni_surface.md`.

## 5. libStormGLOFT.so (third-party patch; do NOT use in the port)

Evidence it's not Gameloft's: NDK r16b (2018) vs GCC 4.4 (2010) for the game, ARMv7 +
VFPv3 + Thumb-2 code sitting in the `armeabi` (ARMv5) folder, symbol versioning,
loaded second (`System.loadLibrary("StormGLOFT")` after `"Gangster2"`).

What it does (from its exports and strings):
- `JNI_OnLoad` → **inline-hooks functions inside libGangster2** (`mprotect` +
  `cacheflush` + trampoline shellcode; strings "Hooked %s in lib %s",
  "hook_inline_function"). It finds the game library by the hard-coded path
  `/data/data/com.gameloft.android.GAND.GloftGMHP.ML/lib/libGangster2.so`.
- Hook targets (all exist in libGangster2): `_ZN3glf3App4InitERNS_16CreationSettingsE`,
  `Java_..._GameRenderer_nativeResize`, `Java_..._GameGLSurfaceView_nativeOnTouch`,
  `_Z10UseMipMapsv`, `_Z17GetLowResFarPlanev`, `appOnTouch`, `nativeGetDeviceType`.
  (`zx7Ffd89cDD` is also listed but doesn't exist in this game, so the patch is
  probably shared across several Gameloft titles.)
- Carries a fake NVIDIA **Tegra GL extension string** (`GL_EXT_texture_compression_s3tc
  GL_NV_texture_compression_s3tc GL_NV_coverage_sample …`) and imports `glTexImage2D` /
  `glCompressedTexImage2D`: most likely spoofs Tegra and converts DXT textures for
  non-Tegra GPUs (inferred, not disassembled).
- Embeds a Mesa GLSL compiler/optimizer (*glsl-optimizer*: `ir_print_glsl_visitor`,
  `ir_print_metal_visitor`), irrelevant to this GLES1 game.

**Use for the port:** the hook list is a free to-do list of the game functions that
needed fixing on non-original hardware (resolution/resize, touch, device type, mipmaps,
draw distance, creation settings). Study what each hook changes before Phase 3.

## 6. Java ↔ native boundary (summary; details in `analysis/jni_surface.md`)

- **Java → native:** 28 declared natives, 23 implemented. Missing:
  `Gangster2.nativeIsExit`, `GLBluetooth.nativeInit`,
  `GLBluetoothService.ConnectionStateChanged/ReceiveData`, `GLiveMain.nativeInit` (dead paths).
- **Native → Java:** 57 static callback methods in 3 classes: `Gangster2` (21: exit,
  browser, IGP, Gameloft Live, trophies, **demo/trial state**, **Verizon VCAST billing**),
  `GLMediaPlayer` (33: SoundPool/MediaPlayer audio, movie, device type, language,
  screen width), `GLResLoader` (3: read file bytes/length).
- **Init order** (GL render thread): `nativeGetJNIEnv` (game caches `JNIEnv*` in a global)
  → `GLResLoader.nativeInit(0)` → `GLMediaPlayer.nativeInit(0)` + sound counts →
  `Gangster2.nativeInit(isDemo)` → `GameRenderer.nativeInit(1)` → `nativeResize(w,h)`
  → `nativeRender()` every frame.
- **Input:** touch via `nativeOnTouch(action, x, y, (long)pointerId)` (1 down / 2 move / 0 up),
  Android keycodes via `nativeKeyDown/Up`, accelerometer via `nativeAccelerometer`.
- Each class passes its own `jclass` through its static `nativeInit`; there are no
  class-name strings in the `.so` (inferred; confirm in Phase 1).
- `nativeGetInfo` (country/IMEI/model/OS version) is a no-op (`bx lr`).
- `GetDeviceType()` returns -1 for non-Motorola/Samsung/HTC devices (incl. the LG P990).

### How the game finds its data
- Hard-coded absolute paths in native code: `/sdcard/gameloft/games/Gangstar2/`
  (data), `/sdcard/gameloft/games/Gangstar2/tmp/`, saves in
  `/data/data/com.gameloft.android.GAND.GloftGMHP.ML/` (`Gangstar2.sav`).
- Java: `GameInstaller` checks/downloads into `/sdcard/gameloft/games/Gangstar2/`;
  `GLMediaPlayer` loads sounds/movies from there; `GLResLoader` falls back to
  `/sdcard/gameloft/games/gangster2/` (lowercase).
- **Port implication:** the path shim maps those prefixes to host folders
  (`work/data/` for testing, a saves folder for `/data/data/...`), case-insensitively.

## 6a. Runtime facts from the Phase 1 probe (Unicorn, ARM926 model)

Reports: `analysis/probe_1a.md`, `probe_1b.md`, `probe_1c.md`.

**Loading (1a).** Library mapped at `0x10000000` (11,157,504 bytes incl. `.bss`).
All 47,173 relocations applied with Android 2.x linker semantics, matching
`DT_RELCOUNT` / `DT_RELSZ` / `DT_PLTRELSZ`. After relocation, all 448 init_array
entries and **all 32,537 slots of the 1,456 C++ vtables** resolve to a function start
with the correct ARM/Thumb bit, a typeinfo, an import, 0 or a small offset.
2 of the 299 imports (`__aeabi_unwind_cpp_pr0/pr1`) are never referenced by any
relocation, only named by `.ARM.exidx`.

**Static init (1b).** All 448 constructors return (0.1 s emulated). Only 61 call
imports: 134 `__aeabi_atexit` destructor registrations, 10 guarded local statics,
23 mutexes, string copies, and the game's allocator taking **12 KB via 4 `sbrk`
calls**. No threads, no files, no GL. The game calls `sysconf(0x27/0x28)`
(`_SC_PAGESIZE`), matching bionic's constants. **STLport 5.1.4 is statically linked**
(`stlport514_src_locale_impl.cpp`), so `std::` lives inside the game.

**First JNI calls (1c).** Sequence `GameRenderer.nativeGetJNIEnv` →
`GLResLoader.nativeInit(0)` → `GLMediaPlayer.nativeInit(0)` →
`nativeGetTotalSounds()` → `nativeGetTotalSoundsOfSameInstance()` runs cleanly:
- `nativeGetJNIEnv` stores the env in the global **`mEnv`** (`lib+0xa6c5f0`).
- Each class's `nativeInit` does `NewGlobalRef(jclass)` then `GetStaticMethodID` on it:
  3 lookups for `GLResLoader`, 33 for `GLMediaPlayer`, **all 36 matching the decompiled
  Java exactly** (names, signatures, overloads). No `FindClass` anywhere so far.
  No libc imports are called by these JNI init functions.
- `nativeGetTotalSounds()` = **1736**, `…OfSameInstance()` = 1.
- `nativeGetSoundFileName(i)` faults on a NULL `FileManager` (`FileManager::_GetDvdName`
  via `AndroidGetSoundFielName`): the singleton is created by later init. **Java never
  calls `getSoundFile()` in this build**, so that native is dead code in practice.

**Sound table (Java side).** `GLMediaPlayer.cJ` is a hard-coded `String[]` of **1,737**
file names. Java allocates `nativeGetTotalSounds()` slots, plus one more on Motorola
(`GetDeviceType()==1 → cV++`), so cJ = 1736 + 1 Motorola-only slot
(`sfx_helicopter_stop.ogg`), consistent with the native count. Against the data folder:
1,670 names exist, **67 don't**. 40 are spelling differences (2 music: `m_m_pub_abraham`
vs `m_pub_abraham`, `m_pub_fix_o_matic` vs `m_pub_fix-o-matic`; 6 `sfx_aligator_*` vs
`sfx_alligator_*`; 32 `vfx_blackX` vs `vfx_black_X`) and 27 are absent (15
`cin_CH5_M_05_*`, `cin_CH2_M_03_MIDDLE_00`, `cin_CH3_M_06_MIDDLE_02`,
`vfx_{female,male}_hey`, `vfx_{female,male}_hit_1..3`, `sfx_motocross_down`,
`m_radio_hiphop_jerkit`). 52 `.ogg` files in the data aren't referenced. On the phone
those sounds would have failed to load silently (SoundPool returns an error, no crash).
The APK is 1.0.6 while `data.txt` points at the 1.0.5 data package, which may explain it.

## 6b. Phase 2 results

### 2a: headless full boot in the probe (`analysis/probe_2a.md`)
Same order as the phone: `nativeSetPhone(800)`, `nativeGetInfo` → `nativeGetJNIEnv` →
`GLResLoader/GLMediaPlayer.nativeInit` → `Gangster2.nativeInit(isDemo)` → `GameRenderer.nativeInit(1)`
→ `nativeResize(800,480)` → first frame `nativeResume` → `nativeRender` per frame. All return;
60 frames (and 1,500 in a long run) without faults or stubs.
- **Engine startup states (from its own log):** `appInit` → `Application::PostInit` (Glitch engine
  0.1.0.2, GL driver init, **vox** audio engine) → `PostPostInit` (language pack, HUD, script manager,
  achievements, **FileManager**, menu manager, **starts the vox sound thread**) → `GSMoviePlayer`
  (intro video; continues when `isMediaPlaying` returns 0) → `nativeGetDeviceType` picks
  **"Lower Setting Profile1"** and **disables mipmaps** (both are StormGLOFT hook targets!) →
  `GSLoading` → content-warning screen → title screen.
- **JNI surface for boot (Q1):** `NewGlobalRef`×3, `GetStaticMethodID`×57 (all callbacks, all
  matching Java), `CallStaticIntMethod`, `CallStaticVoidMethod`, `NewStringUTF`. The game reads all
  files itself with libc `fopen`; it never uses Java's `GLResLoader`.
- **GL surface:** 64 of 95 imported GL functions used through the title/world load; vertex arrays are
  FLOAT (positions, texcoords) and UNSIGNED_BYTE (colours), **no GL_FIXED**; draws are ushort-indexed
  triangles/strips plus point sprites; textures DXT5/DXT1 (~1,200 uploads, 90 MB by the city load),
  one RGBA5551, one ALPHA. The engine never `strstr`s the extension string; it parses `GL_VERSION`
  with `sscanf` at offset 13 (`"OpenGL ES-CM 1.1"`).
- **Threads:** exactly one requested so far: `vox::funcUpdate` (the audio engine's update loop). Not
  started yet (single guest thread); boot works without it.
- **Files:** the game probes `<name>_lo.<ext>` / `<name>_hi.<ext>` variants before the normal file,
  tries `./sdcard/...` 8× before `/sdcard/...` (cwd `/`), looks for `/data/data/<pkg>/Gangstar2.sav`
  and `Gangstar2.preferences` (absent on first run). Asked for but missing from the data:
  `dummy.tga` and 14 `*.english.*.glyph` files (e.g. `menu.english.button.glyph`).
- **Bionic layouts confirmed (Q10):** `_ctype_` and `_tolower_tab_` are read as pointers to tables
  (1 pointer load, then table reads), exactly as the Android 2.x headers say.
- **Sound names:** native `nativeGetSoundFileName` returns the original asset names with **`.wav`**;
  Java's table and the data use **`.ogg`**. Same stem in 1,673/1,736 slots. In the 63 differing slots
  the native name exists in the data 41×, Java's 15×, neither 7×. **What the phone played: 1,669
  sounds; using whichever name exists: 1,710** (41 sounds recoverable). Table:
  `analysis/raw/probe_2a_sounds.tsv`.
- No kernel-user-helper calls (`0xffff0fc0` etc.) and no `swp`/`ldrex`/`strex`/`mrc` anywhere in the
  game code: all atomics/locking go through imported pthread functions.

### 2b: native C++ runtime (`src/`)
- **Design:** "fastmem" guest memory (one 4 GB host reservation, guest address A = host base + A),
  so HLE code and the GL driver use guest pointers with one addition. One Unicorn (ARM926) instance
  per guest thread, all mapping the same host pages; the HLE heap, sbrk heap, mmap area and stacks are
  committed up front so memory is never remapped under a running CPU. Address layout identical to the
  probe; trap 0 is the return sentinel in both, so **trap addresses match the probe's**.
- **Verification:** `gangstar --selftest-load` (C++ twin of probe 1a) gives the same numbers as the
  probe. The GPU check shows GL 4.6 compatibility on the RTX 3060 with S3TC, FBO, VBO, texture combiners.
- **Result:** `gangstar --run` boots exactly like the probe (448 constructors in 0.02 s, every init call
  returns, no import without a host implementation, no STUB hit), shows the **content warning**
  (~5 s), loads data (heap 2 → 14 MB in a few seconds), then shows the **title screen "Touch the screen to
  continue"** at ~90-100 fps (vsync). Screenshots verified visually.
- GL identity reported to the game: vendor "NVIDIA Corporation", renderer "NVIDIA Tegra", version
  "OpenGL ES-CM 1.1", a Tegra-like extension list (representative, not captured from a device).

**Unicorn behaviours relied on** (`test_unicorn_semantics.py`, 2.1.4): in an SVC hook,
PC = svc+4; writing PC (with the Thumb bit) from a hook redirects execution; nested
`emu_start` from inside a hook works (used to call the game's own `malloc` from HLE
`strdup`, and comparators from `qsort`).

## 6c. Phase 3 results (playable under emulation, Windows)

### CPU speed: guest-side float helpers
- The game is soft-float, so every float operation is a call to `__aeabi_fadd` and friends. Trapping
  those to C++ cost 4M traps/s and gameplay ran at ~12 fps. The CPU model is now **Cortex-A15**
  (ARMv7 + VFPv4 + SDIV; VFP enabled through CPACR/FPEXC), and `src/cpu/fastpath.S` implements 35
  EABI helpers + `sqrt/sqrtf` as **guest VFP code** mapped at `0x0E080000` (the imports resolve
  there). Assembled by VS clang (`tools/gen_fastpath.py` → `fastpath_code.inc`). `--selftest-fastpath`
  checks every helper bit-exact against host IEEE math. Traps dropped to ~80k/s.
- **Gameplay speed under Unicorn: ~20-40 fps**, depending on the scene (traffic, NPCs, camera). The same
  spot can measure 21 or 37 fps on different runs, so compare A/B runs on the same build, never against
  old numbers. "game ms/frame" in the log is time inside `nativeRender`.
- Guest profile (`--profile-guest`): ~55% of executed instructions are float-call overhead (helpers
  ~35%, PLT stubs ~20%); the rest is skinning, culling and Box2D. **120+ fps in gameplay needs the
  static recompiler (Phase 4)**; emulation won't get there.
- **Frame timing (Q20).** The game has **no frame cap and no fixed step**. Each frame
  `Application::UpdateAfterMovie` takes `t = glitch timer getTime()`, which is integer ms from
  `gettimeofday`. It computes `dt = t − last` (clamped to 100 ms), stores `last = t`, and calls
  `Application::_Update(int dt_ms)`, which passes `(double)dt` to every registered `IUpdatable`,
  the state machine and `MenuManager`. The absolute timestamp is stored, so game time never drifts at
  any frame rate. The only effect of high fps is ±1 ms quantization per step: at 144 fps the steps go
  7,7,7,6,... ms. Checked: the content warning lasts the same ~4.7 s at 60, 120, 144, 165, 240 and
  ~530 fps. `gettimeofday` is now monotonic (a wall-clock jump would give a negative/huge step).
  Optional sleeps in the loop (`glf::Thread::Sleep(global)`, `Sleep(66)` on some flag) hit our no-op
  `usleep`. Still to check once gameplay can run at 120+ (Phase 4): per-frame (not dt-scaled)
  logic and Box2D at small steps.
- The menus/title ran ~128 fps uncapped only because of emulation speed. The warning screen does
  ~530 fps uncapped.
- **Frame limiter** (`game.cpp`): fixed-cadence deadlines with sleep + spin. Measured exactly
  60.0/120.0/144.0/165.0/240.0 fps. `fps_limit = -1` follows the display refresh rate (the dev PC's
  monitor is **100 Hz**).
- Unicorn check (`hookbench.py` experiment): bounded `UC_HOOK_CODE` hooks cost nothing for code outside
  the hooked addresses. Function hooks are cheap (~500 calls/s in gameplay).

### Display: virtual framebuffer (`gles1::vfb_*`)
- The game is always told **height 480**, width = 480 × aspect (640..1280). 800 and 854 lay out correctly.
  At 1280×720 the UI breaks (it is authored for 480 high).
- It draws into an FBO of logical × scale (auto scale = ceil(window height / 480)) with optional MSAA.
  glViewport/glScissor/point size/line width are scaled; glReadPixels/glCopyTexImage2D are translated.
  The present step blits into the window (aspect / stretch / integer fit) and saves/restores the GL state it touches.
- Overlays (menu cursor) are scissored clears after the present, so no game GL state is touched.

### Graphics profile (Q14 answered)
- `GS3DStuff::setPerformanceType(renderer)` picks `s_perfConfig` from `PhonePerformanceSelector.array`
  ("PowerVR MBX" → 0, "PowerVR SGX" → 1; anything else → 0). `GS3DStuff::loadPerformanceProfile` loads
  entry `s_perfConfig` of `PhonePerformanceProfiles.gmap` into **`gPhonePerf`** (44 bytes): 10 flag bytes
  (+0x04 = mipmaps, read by `UseMipMaps()`); +0x0C texture streaming radius (7500/12000); +0x10 low-res
  city far plane (20000/60000, `GetLowResFarPlane()`); +0x14, +0x18 unknown; +0x1C/+0x20/+0x24 max NPCs per
  spawn-zone type (3,3,2 / 10,10,10); +0x28/+0x29 flags (+0x29 = `_lo`/`_hi` asset variants; none exist).
- The phone (and our "NVIDIA Tegra" string) gets profile 0. The `quality` setting forces it:
  phone = untouched, low = 0, high = 1, ultra = 1 + 1.5× NPCs. Then `draw_distance` multiplies +0x0C/+0x10,
  and `mipmaps` sets +0x04. Verified in the log line `graphics profile (high): index 1, ...`.
  Not yet compared visually (draw distance A/B screenshots are still to do).

### Function hooks (`rt::Runtime::hook_function`)
- A bounded Unicorn code hook on a game function's entry. `pre` can skip the function (PC = LR; it must
  set r0 itself if needed). `post` runs on return: LR is swapped for a `<post-hook>` trap, with a
  per-CPU return stack. Game-specific hooks live in `src/game/mods.cpp`.

### HUD and touch controls
- `CHudManager::load` (r0 = the manager, captured by a hook) creates the on-screen controls at fixed
  fields: +0x28 AnalogStick (v-stick), +0x2C Wheel, +0x54/+0x58 SlideControl, +0x74 ToggleButton,
  +0x7C/+0x94 AnimatedButton, the rest VirtualButton (full table in `mods.cpp`).
- `HudElement`: flags at +0x0C (bit 0 = visible), touch rect x0,y0,x1,y1 floats at +0x10 in **480×320
  authoring space**, scaled by screen/480 and screen/320 (`Application::GetScreenScaleFactors`).
  VirtualButton/AnimatedButton: `ButtonType` at +0x58.
- **ButtonType** (from the `OnFoot`/`Driving`/`Flying`/`Sniper` `ControlHandler::handleVirtualButton`
  jump tables): 0 attack, 2 enter/exit car, 3 enter shop, 4 accelerate, 5 brake (driving: the vehicle's
  virtual calls with 1.0f), 7/8 next/prev weapon, 9 mini-map, 10 radio toggle, 11 radio, 12 (seen,
  unknown), 13 side mission start, 14 take cover, 15 sprint, 16 mini-save.
- On foot the visible set is: v-stick, attack (+0x50), cover (+0x6C), and context buttons that appear
  near things (enter car +0x48, shop +0x4C, ...). Driving (tilt mode): accelerate +0x40 (bottom right),
  brake +0x44 (bottom left), exit car +0x64. No steering control is shown in tilt mode.
- Touch path: `nativeOnTouch(action 1 down / 2 move / 0 up, x, y, jlong id)` → `appOnTouch` →
  `AndroidAddTouchEvent` → `glf::TouchPad`. The v-stick only grabs a touch that **starts on it** and then
  drags (EvStickMove, type 7, angle +8, magnitude +0xC). Buttons raise EvVirtualButton (type 0xD).
  A drag on empty screen turns the camera.
- Hiding the touch controls = skipping their `draw2d` (AnalogStick/AnimatedButton/HudElement/
  SlideControl/SwipeButton/ToggleButton/Wheel) when the object is one of CHudManager's control fields.
  Touch hit-testing is unaffected.

### Steering / accelerometer
- `Gangster2.nativeAccelerometer(x, y, z)` (raw m/s², Java `onSensorChanged`) stores x/−10, y/−10 and
  z/10 − 1.5 (the last one is unconfirmed: Ghidra cut the function short) into `mAccelerometerX/Y/Z`.
  The values reach handlers as `EvAcceleration`.
- `CHudManager::setSteeringMode` (+0x1C): **0x800000 = tilt** (the default; the tutorial says "use the
  pedals and tilt your device"). The other modes (0x80000, 2, else 1) are the other "control sets" in
  Options (Q16). `Widgets::Controls::OnEvent` cycles steering mode (3 options) and throttle mode (2).
- `DrivingControlHandler::handleAcceleration` (tilt mode): steering = sign(y) × min(|y| × 1.8, 1), written to
  **vehicle (+4 of the handler) +0x3E4**, where −1 = left and +1 = right (verified by driving). It also
  rolls the camera with the tilt, and pitch has a neutral band of 0.22..0.78.
- Controller/keyboard steering in tilt mode: the handler is replaced while virtual steering is on
  (writes +0x3E4 directly, clears +0x3E8 and [+0x280]+0x448 like the original). A neutral accelerometer
  reading is fed every frame so the handler runs. There is no camera roll. Verified left and right.

### Input design (`src/input/`)
- Keyboard and controller fill an abstract `GameInput` (move, camera, 15 actions). That state becomes
  **virtual touches** on whatever controls the game currently shows (live positions from CHudManager),
  so the game's own context logic decides what a button does. It works the same with the HUD hidden
  and on Android. Pointer ids: 0 mouse, 1 stick/cursor, 2 camera, 3-9 buttons.
- Held actions (attack, sprint, pedals, cover) hold the touch. One-shot actions (car, interact, map,
  radio, weapons) tap once per press, or as soon as their button appears while the key is still held.
- Menus/cutscenes (no gameplay control visible): the stick/d-pad moves a drawn cursor and A taps.
  Start/Esc = Android BACK (in gameplay: opens the pause/map menu; in that menu: the game's own "quit
  to main menu?" dialog). Tab = MENU.
- Mapping (controller): LS move/steer, RS camera, RT attack / accelerate, LT cover / brake, A sprint
  (menus: confirm), B cover (driving: attack/drive-by; menus: BACK), X interact (shop / side mission /
  mini-save), Y enter/exit car, LB/RB previous/next weapon, Back map, d-pad up radio, d-pad right next
  station. Keyboard: WASD/arrows move (W/S = pedals when driving), Space/LCtrl attack, Shift sprint,
  F car, E interact, Q cover, M map, R/T radio, 1/2 or mouse wheel weapons, right mouse drag camera,
  Enter confirm, Esc/Backspace BACK, Tab MENU. Left mouse = touch.
- `touch_controls = auto` hides the touch HUD while a pad/keyboard drives and shows it again on a
  mouse click (touch).

## 7. Data folder (`original/gameloft/games/Gangstar2/`)

Flat folder (no subdirectories), 2,405 files, 375 MB. Full table:
`analysis/data_inventory.md`.

| ext | files | MB | actual format |
|---|---:|---:|---|
| `.tga` (225) + `.bmp` (23) | 248 | 230 | **DDS, not TGA/BMP**: 205 DXT5 + 43 DXT1, mostly 1024² with full mip chains |
| `.ogg` | 1,722 | 87.6 | Ogg **Vorbis** (SFX, music, voiced cutscene lines `cin_CH*`) |
| `.bdae` | 216 | 33.8 | Gameloft "binary DAE" 3D scenes/models: 214 are **ZIP (stored)** holding an inner `.bdae` + `dependancies.txt` + `.nfo` (+ a few `.dae`/`.bin`); 2 are raw `BRES` (`anim_alligator`, `anim_ingame`) |
| `.ttf` | 5 | 18.7 | TrueType fonts (incl. CJK) |
| `.bin`, `.pyarray`, `.array`, `.gmap` | 62 | 1.8 | Gameloft binary tables (scripts `*_pyarray.bin`, maps, achievements, anims) |
| `.fua` | 3 | 0.76 | **Compressed Flash SWF** (`CWS`, v8): gameswf menus/HUD |
| `.m4v` | 1 | 0.72 | `intro.m4v`: MP4, **H.264 + AAC** |
| `.english/.french/.german/.italian/.spanish/.japanese/.korean/.chinese/.type/.character/.mood` | ~110 | 1.2 | localized string tables + per-chapter dialogue metadata |
| `.pycst` | 1 | 0.16 | text "# File generated…": constants table |
| `.bsprite`, `.glyph` | 23 | 0.04 | Gameloft sprite + glyph-list files |
| `.nomedia` | 1 | | text `/storage/emulated/0/gameloft/games/Gangstar2` |
| `InsTime` | 1 | | `1757921053/` = Unix time **2025-09-15**: the installer last ran on a device then |

Port implications: DXT1/5 can be uploaded as-is on any desktop GPU
(`GL_EXT_texture_compression_s3tc`, universally available on Windows drivers).
Vorbis → stb_vorbis/SDL_mixer. H.264/AAC → Windows Media Foundation (built in) or
FFmpeg. The engine reads everything else itself through libc `fopen`/`fread`.

## 8. Why the APK doesn't run on modern devices (evidence-ranked)

Confirmed from the files:
1. **32-bit ARM only** (`lib/armeabi/`, no `arm64-v8a`). Arm's newer cores
   (e.g. Cortex-A715/X3 and later) have no AArch32 support, and several vendors ship
   64-bit-only Android builds, so on those devices the code physically can't run.
2. **targetSdkVersion is effectively 5.** Android 14 refuses to install apps targeting
   API < 23 (Android 15 reportedly raised that floor to 24). Install-time block,
   bypassable only with `adb install --bypass-low-target-sdk-block`.
3. **The data is the Tegra 2 DXT build.** All 248 textures are DXT1/DXT5 and the engine
   has no software decoder (`"decompression of DXT formats not implemented"`). Mobile
   GPUs other than NVIDIA's (Adreno, Mali, PowerVR, Xclipse) generally don't expose S3TC
   in GLES, so `glCompressedTexImage2D` fails ⇒ missing textures. This is exactly what
   `libStormGLOFT.so` was added to patch around.
4. **The patch itself is fragile:** it rewrites the game's code in memory and finds it
   via the old `/data/data/<pkg>/lib/` path. Newer Android versions store native libs
   elsewhere and restrict modifying file-backed code pages (exact behaviour per version
   **unverified**).
5. **Gameloft's servers:** the installer calls
   `http://confirmation.gameloft.com/partners/android/update_check.php` and downloads data
   from `dl.gameloft.com`. Presumed long dead (**not tested**; we won't contact them).
   A fresh install can't get its data; with data already in place the installer's
   offline behaviour is unverified.

Plausible, unverified:
6. `TelephonyManager.getDeviceId()` (called in `GameInstaller.onCreate` and
   `Gangster2.onCreate`) returns `null` on Android 10+ for old apps, or throws if the
   phone permission is denied. Native ignores the value and `Tracking.init` only
   stores it, so likely harmless unless permission is denied (then: crash).
7. Hard-coded `/sdcard/...` paths + scoped storage on Android 11+; legacy-target apps
   still get legacy access in most cases.
8. Future: 16 KB-page Android devices can't load this 4 KB-aligned `.so`
   (`p_align 0x1000`).

None of this applies to a PC port: we replace the whole Android/Java layer ourselves.

## 9. Open questions (and how to find out)

| # | question | how to find out |
|---|---|---|
| Q1 | Which `JNIEnv` functions does the native code use? | **Answered for boot→title (2a/2b):** `NewGlobalRef`, `GetStaticMethodID`, `CallStaticIntMethod`, `CallStaticVoidMethod`, `NewStringUTF`. Gameplay may add more (byte arrays for VZ strings, etc.); the fake JNI logs any new one |
| Q2 | Is each callback really looked up on the `jclass` from its class's `nativeInit`? | **Answered (1c): yes**, after `NewGlobalRef(jclass)`. No `FindClass` |
| Q3 | Which overloads does native request? | **Answered (1c):** `loadSound (II)V`, `loadSoundBig (I)V`, `getResourceFull (Ljava/lang/String;)[B` |
| Q4 | What do the 448 init_array constructors call? | **Answered (1b):** atexit/guards/mutexes/string copies/12 KB sbrk; no threads, files or GL |
| Q5 | How big must the `sbrk` heap be, and does any libc-allocated memory reach the game's `free`? | 12 KB after static init. Probe HLE `strdup` already allocates with the game's own `malloc`; watch the full boot in Phase 2a |
| Q6 | What does each StormGLOFT hook change (e.g. `nativeResize`, `UseMipMaps`, `GetLowResFarPlane`)? | Ghidra on libStormGLOFT `JNI_OnLoad` + hook bodies (Ghidra not installed yet) |
| Q7 | What do the demo/trial (`DisableLaunchGame`, `unlockDemo`) and VZ billing callbacks need to return for the normal game flow? | Boot currently passes `isDemo=1`, which is what Java passes on a fresh install (`playMode` pref defaults to 0). None of these callbacks were called up to the title screen. **How the port should treat the trial/Verizon purchase gate is the owner's decision**; it will matter once the menus run |
| Q8 | The 227 `mov pc, reg` jump tables: one GCC pattern or several? | Script over the disassembly (Phase 4 prep) |
| Q9 | Did this APK + data last run on the owner's device in Sept 2025 (InsTime)? If such a device still exists, it could be used to trace real JNI/GL calls (Frida/jnitrace). | Ask the owner |
| Q10 | Exact bionic layouts needed for `__sF` (FILE), `_ctype_` tables, `jmp_buf`, `struct stat`, `dirent` | **`_ctype_`/`_tolower_tab_` pointer layout confirmed (2a).** `sysconf` constants confirmed by the game's own calls. `struct stat`/`dirent`/`struct tm` follow the Android 2.x headers but haven't been exercised yet (no `stat` success path, no `opendir` so far); check against AOSP bionic source when they are |
| Q11 | Restore renamed sounds, or keep the original's silent failures? | Owner's call in Phase 3 (audio). 2a shows a per-slot "use whichever name exists" rule recovers 41 sounds (1,669 → 1,710) |
| Q12 | Where is the `FileManager` singleton created? | **Answered (2a):** `Application::PostPostInit` ("Initializing file manager..."), during the first frames |
| Q13 | How do we run the `vox::funcUpdate` audio thread safely (second Unicorn instance on a host thread; real mutexes/condition variables in the pthread HLE)? | Phase 3 |
| Q14 | Which performance profile should the PC build use? | **Answered (Phase 3, §6c):** user setting `quality` (default high = profile 1) + `draw_distance` + `mipmaps`, applied by hooking `loadPerformanceProfile` |
| Q15 | Are the 14 missing `*.english.*.glyph` files and `dummy.tga` optional (the phone data lacked them too)? Does text still render everywhere? | Watch text in menus during Phase 3. So far all menu, tutorial and HUD text seen renders |
| Q16 | Which steering mode is which (0x80000, 2, 1), and which on-screen control does each use (Wheel +0x2C, SlideControl +0x54/+0x58, buttons)? | Switch "control sets" in Options, then `--dump-hud` while driving. Input already drags a visible Wheel/SlideControl horizontally (untested) |
| Q17 | How does weapon switching work? Types 7/8 (next/prev) never showed a visible button; the weapon selector is a HUD element at CHudManager+0xD0 | Ghidra on the WeaponSelector class + `--dump-hud` after picking up a second weapon |
| Q18 | What are vehicle +0x3E8 and [vehicle+0x280]+0x448, which the tilt handler clears on every event? | Ghidra xrefs to those offsets in `Vehicle::` methods |
| Q19 | Controller in the pause menu: Start/B send BACK, which opens the game's "quit to main menu?" dialog, and resuming needs the cursor on "< BACK". Map B/Start to that button? | Find the gameswf menu's back handler (RenderFX / `Widgets::`), or detect the pause menu and tap its BACK |
| Q20 | Does the game cap its own frame rate? Is game timing frame-rate independent at 120-240 fps? | **Mostly answered (§6c):** no cap, variable integer-ms step from an absolute timestamp, so no drift. Still open: per-frame (not dt-scaled) logic and Box2D behaviour at 4-8 ms steps. Check when the recompiled build runs gameplay at 120+ |
| Q21 | Helicopters/planes: `FlyingControlHandler::handleAcceleration` also uses tilt. Needs the same virtual-steering treatment | Reach a flying section; replicate the driving hook |
| Q22 | Ghidra marks `__aeabi_fsub` (and likely other EABI helpers) as non-returning, which truncates functions after the call (e.g. `handleAcceleration` shows 476 of 712 bytes) | Clear "no return" on the `__aeabi_*` thunks and re-run "fix function bodies" in the project (one-off, not `-readOnly`) |

## 10. Glossary (RE terms as they come up)

- **ELF / `.so`**: Linux/Android binary format. "Imports" = undefined dynamic symbols
  the loader must supply; "exports" = symbols the library defines for others.
- **Stripped**: the full symbol table (`.symtab`) removed. Ours isn't, so we have names
  for ~24.5k functions.
- **Mapping symbols `$a/$t/$d`**: markers saying "ARM code / Thumb code / data starts
  here". Without them a disassembler has to guess.
- **Thumb (Thumb-1)**: ARM's compact 16-bit instruction encoding; most of this game is Thumb.
- **Soft-float**: no FPU instructions; float math is done by calling helper functions
  (`__aeabi_fadd`, …), which we can implement as plain host float ops.
- **Relocation**: a fix-up the loader applies after placing the `.so` at some address
  (`RELATIVE` = add load base; `ABS32`/`GLOB_DAT`/`JUMP_SLOT` = write a symbol's address).
- **Text relocation**: a fix-up inside read-only code. Modern Android rejects them; this lib has none.
- **Literal pool**: constants stored right after a function's code and loaded PC-relatively.
- **Jump table**: how a `switch` compiles; an indexed table of code addresses + `mov pc, reg`.
- **JNI**: Java↔native bridge. Java calls `Java_<pkg>_<Class>_<method>`; native calls back
  through the `JNIEnv` function table (`GetStaticMethodID`, `CallStaticVoidMethod`, …).
- **HLE** (high-level emulation): instead of emulating Android's libc/GL, implement
  those functions natively on the host and route the game's calls to them.

---------------------------------------------------------------------------
## Log

### 2026-11-18: Phase 0 (analysis) complete
- Checked tools: none installed. With the owner's OK installed portable Git, Python
  (+ pyelftools, capstone), JDK 21, jadx into `C:\Users\aiden\devtools\`.
- Moved `game.apk` + `gameloft/` into `original/` (owner's choice). Saved CLAUDE.md
  into the repo (it only existed as an attachment).
- Created layout, `.gitignore`, `git init`. Extracted APK → `work/apk/`.
- Wrote `tools/elf_report.py`, `import_report.py`, `so_strings.py`, `data_inventory.py`.
- Findings: sections 3–8 above. Headline: **unstripped, Thumb-1, soft-float, no exceptions,
  GLES 1.1 only, no EGL/OpenSL; the APK is a 2018 repack with a hooking compatibility patch;
  data is the Tegra DXT build.**
- Recommendation and Phase 1 proposal: `PLAN.md`. Waiting for the owner's OK.

### 2026-11-18: Phase 1 (boot probe) complete
- Owner OK'd Phase 1 and gave blanket install permission. Installed `unicorn` 2.1.4
  and `itanium_demangler` 1.1 (pip).
- Wrote `tools/probe/` (ELF loader, bionic data objects, Unicorn harness with svc traps,
  HLE libc, fake JNI) plus two test files (Unicorn semantics, 30 HLE unit tests).
- 1a ✅ load + relocate, cross-checked against the dynamic section and every vtable.
- 1b ✅ all 448 constructors run; no stubs hit.
- 1c ✅ onSurfaceCreated JNI sequence runs; 36 method lookups all match Java;
  `nativeGetTotalSounds()` = 1736, consistent with Java's 1,737-entry table.
  Changed the planned 1c criterion: `nativeGetSoundFileName` needs later init and is
  never called by Java, so the check became "native count vs Java's hard-coded table".
- Findings: §6a. New open questions Q11, Q12.
- Next: Phase 2 proposal in `PLAN.md`. Waiting for the owner's OK.

### 2026-11-18: Phase 2 complete (title screen renders natively)
- Owner OK'd Phase 2 and Visual Studio. Installed VS Community 2026 (C++ workload + Clang), SDL2 via
  vcpkg, Pillow (pip). Unicorn C library taken from the pip wheel (same build as the probe).
- 2a: probe boots the whole startup + 60/1,500 frames headless with a recording fake GL (`gl.py`);
  added real `sscanf` and path normalisation to the probe HLE. Findings §6b.
- 2b: wrote the C++ runtime (`src/`): fastmem memory, ELF loader, Unicorn CPU, HLE libc/stdio, fake
  JNI, GLES1→GL bridge, frame loop with screenshots. Self-test matches the probe; `--run` shows the
  content warning and then the title screen at ~90-100 fps. Findings §6b.
- New open questions Q13-Q15; Q1, Q10 (mostly), Q12 answered.
- Next: Phase 3 proposal in `PLAN.md` (input first). Waiting for the owner's OK.

### 2026-11-19: Phase 3 (in progress): fast enough to play, resolution, quality, controls
- Owner OK'd Phase 3 and added: resolution options, keyboard + mouse + controller, a setting to hide
  the touch controls (Windows and Android alike), a graphics profile, better draw distance, QoL.
  Later: **120+ fps modes are required for the final game**, and **the Android port is the main focus
  once recompilation is done** (other platforms after that).
- Installed Ghidra 12.2 + JDK 25; wrote `tools/decompile.ps1` + `DecompileFunctions.java`.
- Done (findings §6c): guest-side float fastpath (~12 → 20-40 fps in gameplay); Cortex-A15 model;
  virtual framebuffer (any 480-high aspect, render scale, MSAA, fit modes); `settings.ini`;
  borderless toggle (Alt+Enter/F11); pause on focus loss; function hooks; graphics profile + draw
  distance; HUD control map and ButtonType enum; touch-control hiding; keyboard/controller →
  virtual touches; menu cursor; tilt steering for pads/keys; precise fps limiter; fps in the title.
- Verified by scripted runs + screenshots: walk, camera, enter car (hold Y), accelerate (RT), steer
  left/right without camera roll, title screen via cursor + A, pause menu via Start, HUD auto-hide.
- Perf A/B lesson: the same spot measures 21 or 37 fps on different runs (random traffic/NPCs), so a
  suspected hook slowdown turned out to be scene variance. Compare runs of the same build only.
- New open questions Q16-Q22; Q14 answered.
- Later the same day: settings overlay (ImGui), FPS presets with exact pacing, frame-timing analysis
  (Q20 mostly answered), anisotropic filtering, monotonic gettimeofday, `--set` overrides.
- Next: audio (vox thread + GLMediaPlayer, Q11/Q13), saves, draw-distance A/B; then the remaining
  input questions (Q16, Q17, Q19, Q21).
