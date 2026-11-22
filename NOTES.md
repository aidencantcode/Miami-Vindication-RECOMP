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
- It draws into an FBO of any size with optional MSAA: `render_scale` N = logical × N
  (supersampling), **0 = native** = exactly the rectangle the image occupies in the window (1:1
  present, no resampling; follows window resizes; never below 480p). Until 2026-11-20 "auto" was
  ceil(window height / 480), so 1080p rendered 1440 lines and 720p 960 lines, then resampled.
  glViewport/glScissor are mapped by their edges at any ratio (2.25 at 1080p); point size/line
  width scale by the vertical ratio; glReadPixels/glCopyTexImage2D are translated. The game's own
  viewport/scissor values are tracked: queries return them, and they are re-mapped when the game
  switches between the screen and its own FBOs.
- Exclusive fullscreen (`window_mode = fullscreen`) uses the monitor's desktop mode (resolution
  and refresh). Before, SDL switched the display to `window_width × window_height` (1280×720 in
  the owner's settings), so the monitor upscaled a 720p picture: the "upscaling" the owner saw.
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

### Android activities that no longer exist
- Some menu buttons made Java start another activity: **Gameloft LIVE!** (`OpenGLive` → `GLiveMain`) and
  **More Games!** (`launchIGP` → `IGPActivity`). On the phone the game went to the background and came
  back (`nativePause`/`nativeResume`). With the calls as no-ops, GL Live left the main menu frozen
  (black 3D background) and More Games left all sounds paused. Now both request a "bounce": on the
  next frame `game.cpp` calls `nativePause` + `nativeResume` and shows a toast. Verified: the menu
  recovers, `resumeAllSounds` runs, and CONTINUE works afterwards.

### Audio (`src/audio/`)
- **Output path:** `SoundManager::playEx/stop/...` → `nativePlaySound`/`nativePlaySoundBig`/... (globals
  `playSoundID` etc. hold the method IDs) → JNI `CallStaticVoidMethod` → the Java `GLMediaPlayer`
  statics. Each of the 1,736 sounds has an "instances" short: 0 = "big" (MediaPlayer stream), else
  SoundPool effect (`loadSound(i, n)` then `playSound(i, n, 50.0, rate)`). The game always passes volume
  **50.0** and gains (logged: sfx 0.5, voice 0.425, music 0.5). Effects: `clamp(50 × sfxGain, 0, 1)`.
  Streams: `gain(prefix) × 50 / 40` (`dx = 40` on non-Motorola phones), clamped. `setPitch` is called
  ~25×/s (engine sounds). `setVolume(IIF)` is a no-op in Java too.
- Java semantics re-implemented in `media_player.cpp` (header lists the deliberate differences). The
  mixer is SDL2 float stereo with stb_vorbis: effects decoded up front, streams decoded while playing.
- Sound names: Java's table (1,737, extracted by `tools/extract_sound_table.py` into the git-ignored
  `work/sound_table.txt`), plus the game's own name for missing ones (Q11).
- Verified with `--audio-dump`: menu music after the title tap, intro cutscene audio, gameplay
  ambience and engine; 0 missing files.

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
| Q11 | Restore renamed sounds, or keep the original's silent failures? | **Implemented as a setting** (`[audio] restore_missing_sounds`, default on, §6c Audio): a slot whose Java name is missing falls back to the game's own name + `.ogg`. **Owner: say if you prefer the phone's behaviour as the default** |
| Q12 | Where is the `FileManager` singleton created? | **Answered (2a):** `Application::PostPostInit` ("Initializing file manager..."), during the first frames |
| Q13 | How do we run the `vox::funcUpdate` audio thread safely? | **Not needed for sound:** `SoundManager` plays everything through the Java `GLMediaPlayer` callbacks, and vox isn't the output path in this build. The thread stays unstarted (logged STUB). Revisit only if something turns out to depend on vox state (watch for missing sounds/hangs) |
| Q14 | Which performance profile should the PC build use? | **Answered (Phase 3, §6c):** user setting `quality` (default high = profile 1) + `draw_distance` + `mipmaps`, applied by hooking `loadPerformanceProfile` |
| Q15 | Are the 14 missing `*.english.*.glyph` files and `dummy.tga` optional (the phone data lacked them too)? Does text still render everywhere? | Watch text in menus during Phase 3. So far all menu, tutorial and HUD text seen renders |
| Q16 | Which steering mode is which (0x80000, 2, 1), and which on-screen control does each use (Wheel +0x2C, SlideControl +0x54/+0x58, buttons)? | Switch "control sets" in Options, then `--dump-hud` while driving. Input already drags a visible Wheel/SlideControl horizontally (untested) |
| Q17 | How does weapon switching work? Types 7/8 (next/prev) never showed a visible button; the weapon selector is a HUD element at CHudManager+0xD0 | Ghidra on the WeaponSelector class + `--dump-hud` after picking up a second weapon |
| Q18 | What are vehicle +0x3E8 and [vehicle+0x280]+0x448, which the tilt handler clears on every event? | Ghidra xrefs to those offsets in `Vehicle::` methods |
| Q19 | Controller in the pause menu: Start/B send BACK, which opens the game's "quit to main menu?" dialog, and resuming needs the cursor on "< BACK". Map B/Start to that button? | Find the gameswf menu's back handler (RenderFX / `Widgets::`), or detect the pause menu and tap its BACK |
| Q20 | Does the game cap its own frame rate? Is game timing frame-rate independent at 120-240 fps? | **Mostly answered (§6c):** no cap, variable integer-ms step from an absolute timestamp, so no drift. Still open: per-frame (not dt-scaled) logic and Box2D behaviour at 4-8 ms steps. Check when the recompiled build runs gameplay at 120+ |
| Q21 | Helicopters/planes: `FlyingControlHandler::handleAcceleration` also uses tilt. Needs the same virtual-steering treatment | Reach a flying section; replicate the driving hook |
| Q22 | Ghidra marked `__aeabi_fsub` (and 2 more helpers) as non-returning, truncating functions | **Fixed (2026-11-19):** `tools/ghidra/FixNoReturn.java` ran once on the project: 3 helpers cleared, 1,053 fall-throughs disassembled, 473 bodies re-computed. Decompiles now show whole functions (Ghidra may still pull junk ARM-in-Thumb "coprocessor" lines into a few bodies) |

## 11. Recompiler (Phase 4)

Tools: `tools/recomp/` (`thumb.py` exact ARMv5T Thumb decoder, `elfinfo.py` ELF/symbols/PLT,
`analyze.py` per-function analysis shared by survey and code generator, `survey.py` → 
`analysis/recomp_survey.md`). Run: `python -I tools/recomp/survey.py`.

**4.1 survey results (2026-11-19):**
- `.text` 3.19 MB: 3.0 MB `$t`, 12.5 KB `$a`, 165 KB `$d` (literal pools, jump tables).
- 24,199 functions: 23,415 Thumb, 784 ARM. All Thumb functions decode with 0 undefined
  instructions (1.40 M instructions). The 5 "undefined" halfwords the raw mapping-symbol walk finds
  are ARM thunks carrying a stray `$t` symbol: **function symbol type wins over mapping symbols**.
- 783 of 784 ARM functions are C++ non-virtual thunks `ldr ip,=off; add ip,pc,ip; (sub|add rN,rN,#k)+;
  bx ip` (rN = r0, once r1): adjust `this`, tail-call target = add-address + 8 + literal. The last
  one is `__aeabi_f2uiz` (float → unsigned), which will be implemented in C.
- **No cross-function branches** (the 18 the raw walk saw come from the mislabelled thunks), no extra
  entry points. 2,210 `bl` target a label inside their own function: GCC Thumb-1 "far jump" (long
  branch through `bl`, LR saved in the prologue), so they become `goto`. **A `bl` to the function's
  own entry is recursion, not a far jump** (232 sites, e.g. rb-tree `_M_erase`): a real call.
- Calls: 58,401 `bl` to function starts, 40,129 `blx` to the PLT (imports), 381 `blx` to ARM thunks,
  13,083 `blx reg` (virtual calls / function pointers). Returns: 18,331 `pop {..pc}`, 5,013 `bx lr`,
  57 `bx r3` right after `add sp` (the return address was popped into r3).
- **Jump tables**: all 227 `mov pc, reg` recovered. Pattern: table = GOT (`0x6349d8`) + literal,
  target = table + entry. ARMv5T `mov pc` does not interwork (bit 0 may be clear). Bound from `cmp
  rX,#N` + `bhi/bls` (N+1) or `bcs/bcc` (N) **on the path into the jump**: walk back through code
  that falls through into it, never across `b`/`bx`/`pop {pc}`; if that boundary comes first, the
  table block is only entered by branches, and the bound is the guard of the branch that targets it
  (`cmp r3,#12; bls table_block`). 224 exact, 3 unbounded guesses (FreeType `pfr_glyph_load_rec`,
  `parse_dict`, `ft_gzip_file_fill_output`). The first version (nearest `cmp` above, in address
  order) read a neighbouring switch's bound for 3 tables: `Vehicle::Load` 2 instead of 13 (crashed
  at the first car load), `CImageLoaderPVR::loadTextureHeader` 88/87, `t1_decoder` 13/27. Generated
  code fails loudly (`rc_bad_jump`) on a target outside the table.
- 727 functions end with a call to a no-return function (abort/longjmp/assert): generated code puts
  a fatal "fell off the end" after it. Trailing `mov r8, r8` / `0x0000` is alignment padding.
- `add rX, pc` appears 8,979 times (PIC GOT base). Literal pools are constants (no relocations in
  the code segment, checked: 0 of 47,173 relocations land in the RX segment, no DT_TEXTREL), so
  they can be folded at translation time.

**4.2 code generator** (`tools/recomp/codegen.py` → `work/recomp/`, git-ignored, 51 MB C):
- 24,199 functions, 1.40 M instructions, 0 unhandled, 121 `rc_part_NNN.c` + `rc_table.c`. Runtime
  contract in `src/recomp/rc.h` (registers in `RcState`, NZCV as C locals, fastmem macros, ARM
  semantics for shifts by register, VFP-style float→int). 35 soft-float/division helpers are emitted
  inline as C; `__aeabi_f2uiz` (the one non-thunk ARM function) is C.
- Only changed files are rewritten, so a regeneration rebuilds only what changed (full build of the
  library ≈ 8 min on 12 threads; one file ≈ 30 s).
- MSVC's optimizer needs 6+ minutes each for the gameswf debug disassemblers `log_disasm_avm2` /
  `log_disasm` (found with `/d2cgsummary`): they are compiled with `#pragma optimize("", off)`.
- `setjmp` call sites become `setjmp(*rc_setjmp_slot(s))` in the calling function's frame; guest
  `longjmp` (direct or through a pointer) restores r4-r11/sp and host-`longjmp`s there.
- Functions hooked by mods (list `HOOKABLE` in codegen.py, keep in sync with `src/game/mods.cpp`)
  get `rc_hook_enter`/`rc_hook_leave` around their body, which run the same
  `rt::Runtime::hook_function` hooks as the Unicorn code hooks. `--probe NAME` adds more (debug).

**4.4 runtime backend** (`src/recomp/rc_runtime.cpp`, `--cpu recomp`, build `tools\build.ps1 -Recomp`):
- `cpu::Cpu` keeps its interface: in recomp mode registers live in the `RcState`, `call()` is a C
  call, a handler's PC write is a "redirect" for the caller. HLE/JNI/GL handlers are unchanged.
- Indirect calls: 8192-entry cache + binary search of the sorted function table; trap-region
  addresses go to HLE. Main thread stack 64 MB (every guest call is a host call).
- Crash report: unhandled host exceptions print the guest address (if inside the 4 GB reservation),
  guest registers and the host stack mapped back to game functions. Read-only guest regions (code,
  rodata) are made read-only on the host too.
- `GANGSTAR_RC_SAMPLE=<s>`: after s seconds, sample the main thread's stack 4 times (hangs).
- **Result (2026-11-19):** boots to the title screen and stays there; game logic **0.2–0.6 ms per
  frame** (Unicorn: ~49 ms in gameplay, a few ms in menus); loading the first scene 27 ms/frame
  peak. Not yet tested: gameplay, mods enabled in recomp mode.

**Backend comparison tools** (how the two bugs above were found):
- `GANGSTAR_FIXED_TIME=1`: gettimeofday advances exactly 1 ms per call from a fixed start, `time()` is
  constant → both backends run the same deterministic boot.
- `GANGSTAR_TRACE_IMPORTS=<file>` (+ `GANGSTAR_TRACE_MAX`): one line per import call (name, return
  address, r0-r3). Helpers the recompiler inlines are left out of the trace and its numbering. Run
  the Unicorn side with `--no-fastpath` so trap addresses match. `tools/recomp/trace_diff.py`.
- `GANGSTAR_MEMDUMP_AT=<n>`: dump all guest memory + registers at import call n →
  `tools/recomp/mem_diff.py` (identical except one return-to-host sentinel on the stack).
- `GANGSTAR_PROBE=<symbol>`, `GANGSTAR_PROBE_HIT=<n>`, `GANGSTAR_PROBE_OUT=<file>`,
  `GANGSTAR_PROBE_EXIT=1`: the same dump at the n-th entry (or exit) of a game function (recomp mode
  needs `codegen.py --probe <symbol>`). Bisection over n finds the first call where the backends
  differ; the dump log line includes the caller's `lr`.

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
- Later: audio (GLMediaPlayer on an SDL2 mixer, verified by WAV capture; Q11 as a setting, Q13 not
  needed). Saves already persist in the sandbox (CONTINUE works across runs). Fixed two menu traps:
  Gameloft LIVE! froze the main menu, More Games! left sounds paused (activity "bounce", §6c).
  Draw-distance A/B screenshots are still to do: time-based scripts drift with load times, so they
  need a reliable way to reach the same view (e.g. a script "wait until gameplay" step).

### 2026-11-19 (scheduled run, owner's list): exit, menus, text, FF, filtering, mouse, latency, perf
- Done and verified (commits d1265b2..bc0e2c9):
  - **In-game exit**: Java `Gangster2.Exit()` (finish + System.exit) now ends the loop cleanly;
    `sendAppToBackground` minimizes.
  - **Main menu**: Gameloft LIVE! (`widgetMainMenu.btnGLLive`, event 3) and More Games!
    (`btnMore`, event 4) hidden via `RenderFX::SetVisible(false)` after `MainMenu::GotFocus`, and
    their events dropped in `MainMenu::Notify` ([game] hide_store_buttons).
  - **Runtime fix**: the post-hook trap now returns via the stub's `bx lr` (LR = caller) instead of
    writing PC. Writing PC after nested guest calls in the same svc hook made Unicorn stop the
    outer emulation ("did not return").
  - **Text overrides** (`hle/overrides`, [game] custom_text): data files are rewritten into
    `work/sandbox/overrides/` at open time. init.english entry 1 is "Press any button to start";
    about.english entry 14 is the heading "DECOMP BY AIDENCANTCODE" at the top of the scrolling
    credits. String table format: u16 count, then (u16 len, bytes). `\0`..`\1` = heading style,
    `\n` = blank line, `|` = wrap point. Finding: lowercase letters in heading style aren't
    pre-rendered; adding them overflowed the glyph cache at boot and the title prompt vanished.
  - **Fast forward**: game time = steady clock × scale in `gettimeofday`. F2 / R3 click (R3
    toggles on release, not when part of L3+R3). 2-8x, indicator. The game clamps a step to
    100 ms, so below ~33 fps the effective speed is lower. The 2 s log now shows "game step".
  - **Texture filtering**: game / nearest / bilinear / trilinear, live. The bridge remembers each
    texture's game filters and keeps its mip/no-mip choice.
  - **Mouse look**: SDL relative mode in gameplay (unlocks in menus/cutscenes/F1/focus loss);
    mouse = camera drag, LMB = attack, wheel = weapons ([controls] mouse_look).
  - **Low-latency mode** (glFinish after swap) and an input-to-screen latency readout. Input is
    already sampled right before nativeRender; the remaining delay is the emulated frame time.
- Performance (tools/bench.ps1: spawn, standing, 2 runs each): baseline ~49 ms/frame (~19 fps).
  `quality=low` ~39 ms (-20%, fewer NPCs, shorter view). Tried and reverted:
  - **"fast traps"** (`nop; bx lr` stubs handled from a Unicorn code hook instead of svc):
    trivially correct handlers still broke the game (pthread_mutex_lock -> null write in
    gameswf, memcpy -> hang). Isolated Python tests of the same pattern work. Unexplained, so not
    used.
  - **PLT -> direct branch** for the 35 fastpath helpers: no measurable change.
  - **Unicorn TB cache**: already 1 GB. **Logging**: negligible (136 lines per gameplay stretch).
  - Conclusion: under Unicorn the cost is QEMU's translated code itself (soft-float helpers,
    memory paths). Next lever: dynarmic (JIT with host FP) or the Phase 4 recompiler.
- "quality = phone" really loads profile 1 (high): the game picks it for our "NVIDIA Tegra"
  identity. Earlier notes said profile 0; corrected in the UI labels.
- Ghidra project repaired (Q22, `tools/ghidra/FixNoReturn.java`).
- **Radio station box** (top HUD while driving): the frame's right border is cut by a straight
  vertical edge at the same authoring position at 800 and 854 wide, while the arrows and text
  are complete. Ruled out: glScissor (only one call, at init), face culling (same with culling
  off), screen width. **Solved 2026-11-22 (5A.2 / Q4):** the frame quad (HUD atlas 1024², u
  0.7656..0.8799) ends right after the right arrow on purpose; the power button and the frame's
  right border are the radio on/off **ToggleButton** (CHudManager +0x74), which the port hid with
  the other touch controls. It is now always drawn (`keep_drawn`). Verified complete at 800 (5:3)
  and 853 (16:9) wide. Found with the frame trace's new 2D detail: per-draw and per-quad vertex /
  uv bounds and a TGA dump of each traced 2D texture (`GANGSTAR_GL_FRAME_TRACE=<frame>`, files in
  GANGSTAR_HOME).
- **Camera RE so far**: `CameraManager::goBehind` is only called from `Player::initDriving`.
  `FollowCamera::update` places the camera at lookAt + v(character vfunc +300) × distance each
  frame; drags go through `FollowCamera::updateAngles(dx, dy)` -> this+0x34 (yaw) / +0x38
  (pitch). The auto-centering most likely lives in that vfunc/angle decay. Camera modes
  (freeform / first person) are not done yet.
- Next: camera modes (find where the yaw offset decays back to 0; first person via FPSCamera or
  a head-mounted FollowCamera), radio frame, dynarmic evaluation (time-boxed), Phase 4.
- Later in the run: **camera modes done** (commit 4545975). Mechanics: FollowCamera position
  +0x08, look-at +0x14. updateAngles writes per-frame yaw/pitch deltas (+0x34/+0x38), and
  updatePosition rotates the camera around the target by them and zeroes them.
  FollowCamera::update pulls the camera behind the character (the auto-centering).
  - Freeform keeps the camera's offset from its look-at across that update.
  - First person moves the scene camera node (vfuncs +0xA4 setPosition / +0x104 setTarget) to the
    look-at point after CameraManager::update.
  - Verified with screenshots (strafing: the classic heading swings, freeform holds; first person
    view at head height). Debug aid: `GS_WATCH_CAMERA=1` + `Cpu::watch_writes` logs guest code that
    writes a memory range.
  - Not yet: first person while driving, hiding the player's own body when looking down,
    freeform for the vehicle camera.
- **dynarmic assessment**: not in vcpkg; it needs its own deps (mcl, xbyak, zydis, fmt, boost
  headers). The runtime depends on Unicorn-specific features (nested guest calls from HLE
  handlers, UC_HOOK_CODE function hooks, context save/restore, shared mem mapping), so a dynarmic
  backend means a CPU-backend abstraction plus a re-entrancy scheme: several sessions, and it
  partly duplicates Phase 4 (the recompiler, also the Android path). **Owner decision:** go
  straight to Phase 4 (recommended), or do dynarmic first as a stopgap for a faster emulated build.
- Regression check after everything: still ~49-50 ms/frame at the spawn (same as before).

### 2026-11-19 (evening): Phase 4.2 + 4.4: the recompiled game boots to the title screen
- Owner chose Phase 4 over a dynarmic stopgap. 4.1 survey committed earlier (cd01ad1).
- **4.2:** `tools/recomp/codegen.py` translates all 24,199 functions (0 unhandled) into C that
  MSVC compiles (`-Recomp` build option, library `game_recomp`). Details in §11.
- **4.4:** `--cpu recomp` backend (`src/recomp/rc_runtime.cpp`). First boot ran static
  constructors, JNI_OnLoad, nativeInit/Resume and PostInit natively, then hung in material
  loading. Two translation bugs, both found by comparing against Unicorn (tools in §11):
  1. `bl` to the function's own entry (recursion, 232 functions) was emitted as a far-jump `goto`:
     the rb-tree `_M_erase` restarted itself instead of recursing, a process-buffer list ended up
     cyclic, and `SCreationState::~SCreationState` looped forever. Bisected with probes on
     `releaseProcessBuffer`: state identical at entry and exit of call 8, call 9 came from different
     callers (`lr`), which pointed at `_M_erase`.
  2. Jump-table bounds taken from the nearest `cmp` above the table in address order instead of the
     one on the path into it: `Vehicle::Load` got 2 of 13 entries (`rc_bad_jump` at the first car
     load); 2 more tables were off. All 227 tables re-checked against the machine code.
- Result: title screen, stable for 40 s+, game logic 0.2–0.6 ms/frame (Unicorn: ~49 ms in
  gameplay). Screenshot identical in look to the Unicorn build.
- Next: (1) run with mods on in recomp mode (hook checks are generated), (2) script into gameplay
  and compare with the Unicorn build (4.5), (3) measure gameplay frame time (4.6), (4) Android
  build of the same C (Phase 5).
- **Gameplay under recomp, mods on** (same input script as `tools/bench.ps1`): main menu (store
  buttons hidden by the mod), intro flyover, on foot at the spawn with the tutorial, traffic moving,
  audio (102 effects, 22 streams, none missing). Game logic **1.3–2.0 ms/frame** at the spawn
  (Unicorn ~49 ms). Uncapped (`--fps 0`, render x2): **380–485 fps**; with the default limiter it
  sits at the display's 100 Hz. The 120+ fps target is met on PC.
- The recompiled backend is now the default whenever the build contains it (`--cpu unicorn` for
  the emulator). `tools\build.ps1` builds it whenever `work/recomp` has been generated
  (`-Recomp` the first time, `-NoRecomp` to leave it out) and re-runs the generator when
  `tools/recomp/*.py` changed (stamp file `work/recomp/.codegen_stamp`).
- **Driving under recomp** (NOTES car-entry script + RT/steer): enters the car, starts "Get to the
  nearest gas station", accelerates and steers; game logic 2.4–2.9 ms/frame while driving. The
  same script on `--cpu unicorn` gives the same picture at t79 (blue car on the road, same HUD,
  47 ms/frame, 20 fps). Hooks are confirmed working in recomp mode through the main-menu store
  buttons (post hook on GotFocus, pre hook on Notify); camera modes not yet tried there.

### 2026-11-20: README for GitHub
- `README.md` with banner, screenshots (`docs/images/`, our own captures at 1600×900 → 1280×720
  JPEG; banner cropped from a title frame without the blinking prompt), how the recompiler works,
  performance table, build steps and debug options. Captured with `--set` overrides (windowed, no
  vsync, trilinear, 16× AF, 4× MSAA) so the owner's `settings.ini` stayed untouched.
- New numbers: 1600×900, render 2×, MSAA 4×, ultra: ~505 fps uncapped on foot (game 1.1 ms),
  title screen ~1,280 fps.
- `tools/setup_unicorn.ps1` falls back to the `unicorn` package of the Python on PATH when the
  owner's portable Python isn't there (other machines).
- Open for the owner: a license for the project's own code; the screenshots/banner show Gameloft's
  art (common for fan ports, but their call before the repo goes public).
- **Resolution fix (owner report: "the game is upscaling"):** two causes. (1) Exclusive fullscreen
  switched the display to the windowed size (1280×720) and the monitor upscaled it; it now uses
  the desktop mode. (2) The render scale only came in multiples of 480 lines; the default is now
  **native** (internal resolution = shown size, 1920×1079 at 1080p, ×2.25), integer scales stay for
  supersampling. Checked: 1600×900 window → 1600×899 internal, HUD/3D/speed blur aligned;
  owner's settings (exclusive fullscreen) → 1920×1080 display, 1920×1079 internal.
- The 2D art (HUD, fonts, menus) exists only at 480p in the data (`*_hi`/`*_lo` variants the game
  probes for are absent), so it is always magnified; the 3D scene is what gains resolution.
- **Owner's play-test list** (2026-11-20) recorded as PLAN Phase 5A (sprint W/S, radio clip,
  music/voice/radio looping, random auto aim → RMB, free aim crosshair, VC-style free look, mouse
  wheel weapons, night/sunrise/sunset lighting), then 5B Android. **Not started: the owner will
  say when.** Lead found while writing it: the mouse wheel is already mapped, but to a 4-frame
  pulse that is under 10 ms at today's frame rates (frame-count timing written for ~20 fps).
- **5A in progress (2026-11-20/21).** Findings so far, parked while the lighting goes first:
  - Uncapped fps can freeze the loading screen: at ~3,000 fps (1280×720, no MSAA, loading
    screen) the game stopped updating for 30+ s (no `_Update`, no draws) while 30 fps loaded
    fine. `glitch::os::Timer::getRealTime` = gettimeofday in whole ms. Likely cause: frame
    intervals < 1 ms. Fix candidate: never run the game faster than ~1,000 fps (5A.0).
  - Audio: the game loops sounds itself in `SoundManager::update` (restart when start + length
    < now_ms; now_ms = (1e6·sec + usec)/1000 in 32-bit int). The radio is restarted by
    `update` when `isSoundPlaying(track)` (→ GLMediaPlayer.isMediaPlaying) returns 0, so a stream
    that reports "not playing" too early is restarted (candidate cause of the ~1 s loops).
    `GANGSTAR_AUDIO_TRACE=1` logs every media call with a timestamp. The owner's save starts on
    foot downtown, so the old car-entry script no longer applies.
- **5A.8 Lighting: done (2026-11-21).** Root cause: the shipped game never shows its night city.
  - Day/night model (`DayTime`, object `DayTime::s_dayTime`: state at +4, ms of the day at +8):
    dawn 03:00 (4 h, state 0), day 06:59 (8 h, 1), dusk 15:00 (4 h, 2), night 18:59 to 02:59 (8 h, 3).
    `DayTime::UpdateInfo` blends `SkyColors` and sends real night lighting to GL (light 0
    ambient ≈ 0.01, diffuse blue 0.38/0.63/0.84, fog dark blue). That only affects lit objects:
    characters and cars. The city is unlit; its look is baked into the atlas textures.
  - The data has a night twin for every city atlas: `atlasN` → `nightN`, `alpha_atlasN` →
    `alpha_nightN`, plus `vegetation_00_night`, `bridge_side_night`, `bridge_under_night`,
    `expaniolaway_night`, `guardfence_night`. All are DXT DDS files despite the `.tga` name, 1024²,
    11 mips. The swap code (`CStreamTextureManager::Update` → `CStreamTexture::setNightActive`
    on DayTime state 3) is **dead**: CStreamTextures are only created by
    `CCustomResFactory::getTexture` (for names with "atlas" and without "Bitmaptexture"), and that
    factory is only installed by `buildBatchMesh`, an offline tool `Application::Init` runs only
    with a developer flag and then `exit(0)`. Real phones showed the same daylight city at night.
  - Fix, `src/game/night_textures.cpp` (setting `[graphics] night_textures`, default on):
    - recognises day textures when the game uploads them (FNV fingerprint of the first 1 KB of
      level 0, plus levels 1-2 in case top mips are skipped);
    - at night (state 3) re-uploads the night twin into the same GL texture, 4 per frame;
    - textures without a twin (ground, grass, concrete, props, water, distant-city LOD: list
      `kTintNames`) get the night look by scaling DXT endpoint colours by (0.201, 0.211, 0.321).
      That factor is a least-squares fit over all 66 atlas pairs with lit windows excluded; DXT1
      endpoint order and indices are fixed up so the block mode doesn't change.
  - Dawn/dusk: a fixed-function tint stage on the last texture unit (unit 3 here; hidden from the
    game by reporting one less `GL_MAX_TEXTURE_UNITS`, because the game itself sets up all
    units). It is MODULATE(previous, constant) and only enabled for draws whose unit-0 texture is
    a registered world texture showing its day image. Keyframes in `night_textures.cpp`: golden
    17:00, orange 18:00, purple 18:30, night tint at 18:59 (then the night images take over with no
    visible step, only lit windows appear); dawn reverses through pink 05:00. Verified with
    screenshots at 05:00, 06:00, 12:00, 17:30, 18:30, 18:55, 19:05, 23:30, 02:55, 03:05, 04:00. No
    measurable fps cost.
  - Debug aids: `GANGSTAR_TIME=HH:MM` pins the clock through the game's `DayTime::setTime` (hook on
    `DayTime::update`, which is in codegen's HOOKABLE list); `GANGSTAR_GL_LIGHT_TRACE=1` logs
    each distinct light/material/fog/texenv call; `GANGSTAR_TINT_DEBUG=1` reads back the tint stage.
- **5A.0 frame-rate assumptions (2026-11-21):** "unlimited" fps now stops at 1,000 (the game's
  clock is whole ms; the loading screen froze at ~3,000 fps, now loads at 999.9 fps and gameplay
  follows). One-shot taps are time-based (≥ 80 ms and ≥ 2 frames, was 3 frames), and mouse-wheel
  notches are queued and spaced (tap + 60 ms gap), so behaviour no longer depends on the frame rate.
  **Finding:** the weapon selector (fist icon with `< >`, top right) is not a HUD VirtualButton
  (no type 7/8 control in the `--dump-hud` list), so RB / wheel / 1 / 2 have never switched weapons.
  Needs RE of that widget (5A.4/5A.7). Sprint (type 15) is a hidden button that appears while
  running (5A.1).

### 5A.1 Sprint forward/back (fixed 2026-11-22)
- `AnalogStick::processTouch` (decomp_input.c): drag in screen pixels → 480×320 units as
  dx / sy and dy / sx with sx = w/480, sy = h/320 (**swapped**; harmless on a 480×320 phone), then
  clamps to the radius at +0x5C (49) and stores magnitude 0..1 at +0x68 (1.0 when clamped);
  +0x84 = axis mode (0 x only, 1 y only, 2 both). Running, and with it the sprint button
  (ButtonType 15, HUD field +0x70), needs full deflection.
- Our virtual stick used one pixel radius (84 px at 854×480): 84/1.5 = 56 sideways but
  84/1.78 = 47.2 < 49 forward/back, so W/S never ran fast enough for the sprint button to appear.
  Now the touch is placed in the game's units (radius × 1.1 × the swapped scale per axis);
  `mods::Control::stick_radius` reads +0x5C. Verified forward, back, diagonal, sideways.

### 5A.4 / 5A.7 Targeting, weapons (2026-11-22; decomp_aim.c, decomp_aim2.c)
- **Tap-to-target**: `HudScreen::processTouchRelease` treats an empty-screen touch as a tap when
  it lasted less than a threshold and moved ≤ 9 px (+0x6C/+0x70 accumulate the drag), then
  `tappedTarget(point)` → `Player::lockTarget(t, true)` (or `_SetTarget(0)` when nothing is hit).
  Our camera drag starts at a fixed anchor (62%, 40% of the screen) and is lifted as soon as the
  mouse/stick stops, so small camera moves became taps at the anchor: the random locks.
- **Automatic lock**: `Player::updateLockTarget`, unless a tapped (manual) target is set, locks
  `Character::getAttackableTarget()` every update (`lockTarget(t, false)`, effective once the
  weapon is out, +0x688). getAttackableTarget = living characters not friendly to the player's
  faction that pass a "can attack" virtual (+400), scored by distance within weapon range and by
  facing. `Player::_GetTarget` = Player+0x650; Player has its Character at +4;
  `Player::s_player` is the global Player*.
- **Shots**: `Weapon::shoot(Character*, bool)` takes the gun's position/front (vfuncs +0x5C/+0x68)
  and calls `Weapon::shoot(Character*, from, dir, bool)` (vectors by value), which per bullet uses
  the camera's look direction if Character vfunc +0x174 says so (apparently sniper mode), else
  the locked target, else `dir`, then adds spread. Rocket/grenade types (0xC/0xD) for the player
  already aim at the screen centre. `Weapon::getTargetPointedByWeapon` raycasts from the camera.
- **Weapon switch**: `OnFootControlHandler::handleSwitchWeaponButton` = `Player::switchWeapon(
  s_player, dir, true)` (blocked while Player+0x7E8). The selector widget is not a VirtualButton.
- **Done**: hooks on `HudScreen::processTouchRelease` (pre: +0x6C = 100, always a drag) and
  `Character::getAttackableTarget` (post: 0 for the player unless our auto-aim asks), both added
  to codegen's HOOKABLE. Auto-aim (LT / middle mouse) locks getAttackableTarget's pick or else
  `HudScreen::tappedTarget(screen centre)` with `lockTarget(t, true)`, retries every 0.25 s while
  nothing is locked, `delockTarget` on release (verified: locked a pedestrian, player turned and
  fired at them). Weapon next/prev call `Player::switchWeapon` (verified fists → bat → pistol).
  `[controls] tap_to_target = true` restores the phone behaviour. Controller: RB fire, LT auto-aim,
  RT aim (manual aim: next step), B cover, d-pad left/right weapons, up/down radio.
- Seen once, not reproduced: during LT + RB the weapon went pistol → bat → fists (no switch call
  of ours in that run was logged; logging was added afterwards). Watch for it.
- **Manual aim (RT / right mouse, done 2026-11-22)**: `Character::aim(bool)` already aims the player at
  the screen centre when no target is locked (ray from the camera node through
  `Application::getScreenCenter`, body turned with `Character::rotate`, 9-way aim blend), and
  `Player::updateLockTarget` calls it whenever Player+0x688 (weapon out) is set. Holding aim calls
  `Character::drawWeapon` and keeps +0x688 non-zero (1 = set by us, cleared on release).
  World is **Z-up** (player z 185 at the feet, follow-camera look-at = head at +180, camera 455
  units back). Over-the-shoulder camera in the `CameraManager::update` post hook: eye = look-at
  − dir·0.62d + right·0.22d + up·0.02d (d = game camera distance), view direction tilted ~4° up
  (the follow camera looks ~9° down), blended in 0.15 s; set on the scene camera node like first
  person. Crosshair: ImGui foreground circle +
  dot at the centre of the game image (`ui::FrameStats::crosshair`). Verified: camera, crosshair,
  player turns and raises the gun, fires at the centre, release restores everything.
- `Weapon::shoot(Character*, vector3d, vector3d, bool)` takes the vectors **by invisible
  reference** (r2 = &from, r3 = &dir; vector3d has a copy constructor): reading r2/r3/[sp] as
  floats gave NaNs. The shot hook rewrites *dir to aim from the muzzle at the target under the
  crosshair (tappedTarget at the screen centre: chest +110 / vehicle +60), else where the
  crosshair ray meets the ground at the player's feet (no world raycast yet), else 60 m along it.
  (One earlier test run wrote into the caller's stack before this was understood.) Verified:
  shots follow the crosshair; the game's own gun direction pointed ~8° above it.
- Open: the aim animation holds the arm ~8° above the crosshair line (visual only; bullets are
  corrected). Fix candidates: offset the 9-way aim blend (`Character::aim(float, float)`), or
  give `Character::aim(bool)` a lower target. Walls are not raycast for the no-target case
  (`LevelObject::TestRay` signature still to confirm).

### Q1 Controls: latency, smooth physics, fire auto-aim, VC layout (2026-11-22)
- **Test isolation:** `GANGSTAR_HOME=<dir>` keeps sandbox (save), settings.ini and runtime.log under
  `<dir>`. Scripted tests use `work/testhome` with the save reset from `work/testhome/snapshot` each run.
- **Latency measured** (`GANGSTAR_INPUT_TRACE=1`: per-frame player/camera position, every virtual
  touch, every `Weapon::shoot`): host side input → game is the same frame; walking starts 2-3 frames
  (~25 ms) after the stick, camera turns on the next frame. Found instead: **the game steps physics
  at a fixed 1/60 s** (`GS3DStuff::update`: accumulator += min(dt, cap)/1000; while acc >= 1/60:
  every object's pre-physics vfunc +0x168, `b2World::Step(1/60, 10)`, post-physics +0x11c), with no
  interpolation, so at 120 fps people, cars and the camera following them moved on every other frame
  (measured 59/120). **Smooth physics** (`[game] smooth_physics`, default on): the step (literal pool
  word at 0x289dc0) is read from memory in the recompiled code (`codegen.py RUNTIME_LITERALS`) and the
  `Application::_Update` hook sets it to this frame's dt (slow frames > 16 ms keep 1/60), so there is
  one step per frame. The page is made host-writable on first use (`GuestMemory::host_writable`; the
  recompiled backend protects code pages). Verified: movement every frame (120/120); walking 578 vs
  568 units/s; driving 2,641 vs 2,655 units/s top speed and the same braking (1,599 → 68 vs
  1,578 → 70). Camera full-stick speed is only 72°/s (700 px/s drag): Q2 takes the camera over.
- **Bullet spread** (`Weapon::shoot`): per axis `rand(-100..100) * 0.01 * weapon(+0x4C) *
  shooter(Character +0x6C8, or +0x6CC when vfunc +0x110 is true)` is added to the unit direction
  (pistol: 0.156 × 0.300 = ±0.047 per axis). Manual-aim shots scale both shooter factors by
  `[controls] manual_aim_spread` (default 0.25) during the call; the post hook restores them.
- **Fire auto-aim** (owner): `Player::updateLockTarget` locks `getAttackableTarget()` with
  `lockTarget(t, false)` and delocks when it returns 0. Our getAttackableTarget post hook now lets the
  game's pick through while fire is held and 1 s after (semi-auto taps don't swing back), never while
  aiming with the crosshair or while a lock is held/toggled. Verified on foot and from a car (left
  click and RB: target locked, 6 shots).
- **Middle click** toggles a lock (`lockTarget(t, true)`, nearest attackable else the screen centre);
  clicking again (or the target going away) unlocks. Right-click aim drops any lock.
- **Reload:** `Character::reloadWeapon()` (what the script command ActorReload calls: plays anim 0x79
  and refills through `Weapon::reload`), on R / LB on foot.
- **Layout** modelled on GTA Vice City PC (original PC defaults: fire LMB/LCtrl, target RMB, wheel down
  = next weapon, F/Enter = vehicle, in a car Space = handbrake, R / wheel = radio, C = crouch). The game
  has no jump, horn or handbrake: Space sprints on foot and brakes in a car; C or Q = cover.
- **Test helpers:** input script `kb <SDLKeyName> 0|1`, `mouse left|right|middle 0|1`,
  `mouse wheel ±1`, `mod nearcar` (teleports beside `Character::getClosestVehicleFrom` and does what
  the enter button does: Player+0x704 = car, `Player::initDriving(true)`; the car-you-may-enter field
  is normally set by a physics contact). The teleport spot is in traffic: about half the runs die
  ("YOU BEEN MURKED") with either physics mode.
- **Helicopters** (Q1e, open): `FlyingControlHandler` takes the v-stick (EvStickMove →
  `Helicopter::tipRotorBlades(angle, magnitude)`), up/down and left/right stick events (player
  vfuncs +0xE0/+0xE4, +0xE8), buttons (jump table, types 0..11), and tilt like driving:
  `handleAcceleration` writes the vehicle's +0x3E4 (from tilt y) and +0x3E8 (tilt x minus a neutral
  value), and rolls the camera. Needs a reachable helicopter to map and test.
  Checked 2026-11-22: the object list near the spawn has **no helicopters** (`mod nearheli` walks
  `GameObjectManager::s_gom` → `GetFirst(0)`, next at +0x0C, `isHelicopter`); the data has only
  heli_news / heli_police models, so flyable helicopters come with missions. With the current code
  WASD / left stick drive the v-stick, which in a helicopter tilts the rotor (flight direction).
  Still to do once a helicopter is reachable: dump the flying HUD (`--dump-hud`), then map VC-style
  W/S = up/down (whatever raises EvUpDownStickMove: SlideControl +0x54 is the vertical one, +0x58
  the horizontal), A/D = turn (+0x3E4 via a FlyingControlHandler::handleAcceleration hook like the
  driving one, with neutral accelerometer events), controller RT/LT = up/down.

### Q3 Aim leftovers (2026-11-22)
- **Walls:** `LevelObject::TestRay(this = shooter (skipped), const vec3& from, const vec3& delta,
  bool, bool, bool, vec3* hit, vec3* normal, b2Shape** shape, void*)`, called like
  `Weapon::getTargetPointedByWeapon` (false, true, false, ..., null). It **returns true when the ray
  is clear**; on a hit, `hit` is the point, `normal` the surface normal and `shape` the Box2D shape
  (0 for terrain: it also hits the ground). Manual-aim shots with no target under the crosshair aim
  at the first hit along the crosshair ray (verified: hotel wall 3,347 units along).
- **Free look feedback loop** found while testing: during manual aim the head moves a little, which
  counted as "moving", so the camera eased toward "behind the player" while the player turned toward
  the view: the view kept turning between shots. Now no swing back while aiming / locked, and
  "moving" uses the look-at speed smoothed over 0.25 s with a 150 units/s threshold.
- **Arm pose:** `Character::shoot` uses the 2-argument `Weapon::shoot`, which takes the shot direction
  from the gun bone, so the logged "game dir" is where the gun really points. With nothing locked,
  `Character::aim(bool)` aims from the feet toward the far end (~18,000 units) of the ray through the
  screen centre (`CSceneCollisionManager::getRayFromScreenCoordinates`, line3d via hidden pointer).
  Measured: gun ≈ 12.5° + 2 × (ray angle − drop). Hooks on Character::aim (player, manual aim) and
  the ray function (both added to HOOKABLE) turn the ray down by ray / 2 + 6.25°. Verified: gun vs
  crosshair 0.08 / 0.05, −0.18 / −0.21, 0.29 / 0.28 (sin of pitch) looking ahead, down and up.
- **Drive-by with RB:** verified in the Q1 car test (target locked, shots fired).

### Q2 Free look (2026-11-22)
- Camera stack: `CameraManager::update(int ms)` calls the active camera's `update(float s)` (vfunc
  +8; FollowCamera on foot, VehicleCamera in cars, both `Camera` with position +0x08 and look-at
  +0x14), then `updateFreeLook`, then starts 0.1 s position/look-at transitions toward the camera
  and moves the scene node (vfuncs +0xA4 / +0x104) with explosion shake added.
  `startPositionTransition` uses the 0.1 s only when the target moved more than sqrt(3000) ≈ 55
  units in x/y and the target is a character on foot, else 0 = snap (checked in the recompiled code:
  Ghidra showed the snap's t = 1.0f as 0.0). So at high fps the node normally snaps: not a latency
  source.
- Free look (`camera_mode = freelook`, the new default; old "freeform" maps to it): post hooks on
  `FollowCamera::update` and `VehicleCamera::update` (VehicleCamera added to HOOKABLE) put the
  camera at our yaw/pitch around the game's look-at, at the game's distance. Input goes straight to
  `mods::camera_input` (no screen drags; 0.0045 rad per game pixel, full stick ≈ 180°/s at speed 1).
  After `freelook_return_delay` s (1.5) without camera input, while the look-at point moves faster than
  60 units/s, yaw and pitch turn toward the game's own placement at `freelook_return_speed` (90°/s).
  Re-initialised from the game's view on a new camera, a cut or a jump. Pitch clamped -20°..69°.
  First person uses the same yaw/pitch without the return. Verified on foot (turned 75°, stayed for
  3.5 s standing, swung behind once walking; W walks along the view) and in a car (turned −19°
  parked, swung back to the car's heading 89° about 2 s after driving off).

### 5C.0 Screen-effect groundwork (done 2026-11-22, Q6)
- `src/gl/post.{h,cpp}`: run by the bridge at the end of the 3D scene, i.e. before the first
  orthographic draw on screen after a perspective one, else at present. It blits the virtual
  framebuffer (MSAA resolved) into a colour texture + a DEPTH24_STENCIL8 texture (same format as the
  framebuffer's depth, required for the blit), draws fullscreen passes back into the virtual
  framebuffer and restores the game's state (glPush/PopAttrib + PushClientAttrib for the
  fixed-function state, program / framebuffer / buffer bindings by hand; client arrays disabled while
  drawing, since the passes use gl_VertexID and an enabled array would read stale guest pointers).
- The bridge records per frame: split of the depth ranges, near/far of the near-city projection
  (`[0, split]`) and the far-city projection (`[split, 1]`, not the sky's), the projection's
  tan(fov/2), the camera view matrix (modelview of the first city batch, world textures are in
  world space), the fog colour (from glFogfv) and the EXP2 density in use.
- Shaders: GLSL 3.30 with a prelude; the bodies avoid anything GLSL ES 3.00 lacks, so the Android
  GLES 3 backend can reuse them (`#version 300 es` + precision under GANGSTAR_GLES).
- Debug: `GANGSTAR_POST_TEST=invert` (3D scene inverted, HUD untouched: verified) and `=depth`
  (eye distance reconstructed from depth, both ranges: verified continuous).

### 5C.2 / 5C.3 Draw distance live, LOD pushed out (done 2026-11-22, Q5)
- gPhonePerf +0x0C / +0x10 are read every frame: writing them live moves the near city's far plane
  at once (trace: f = 48001 right after switching 1× → 4×). The profile's unscaled values are kept
  at load (`apply_quality`), `mods::set_draw_distance` rescales them; F1 slider 0.5×..8×, default
  3× (was 1.5×, restart-only, max 4×). The near city ends at +0x0C, where the low-res city takes over,
  so this is the "LOD switch" distance; per-object LOD nodes were not the visible problem.
- Same spot, noon, uncapped fps / guest heap: 0.5× 470 fps 14.7 MB; 1.5× 400 / 15.2 MB; 4× 264 /
  17.1 MB; 8× 128 / 18.7 MB. Driving at full throttle: 3× 290-500 fps 16.6 MB, 8× 121-149 fps 19 MB.
  4× and 8× look the same down the beach boulevard (no low-res city in sight); 0.5× shows the blurry
  low-res city in fog.
- Fog end = min(far city end, 2 × the profile's far plane): it still hides the city's edge at short
  draw distances and stays as haze at long ones (it disappeared at 8× before).

### 5C draw distance and LOD (findings 2026-11-21, nothing changed yet)
- The `draw_distance` setting is applied once, in the `loadPerformanceProfile` hook, so it needs a
  restart (the F1 menu says so), and `apply_quality` is skipped entirely when `quality = phone`.
  The owner's `work/user/settings.ini` has `quality = ultra`, `draw_distance = 0.5` (the minimum:
  half the game's own distances).
- It scales `gPhonePerf` +0x0C (texture streaming radius, 12000 in profile 1) and +0x10 (60000).
  +0x10 is read by `GetLowResFarPlane()`, which is used as the **near** plane of the low-res city
  (`CLowResCitySceneManager::setNearPlane`, in `drawAllWater`), i.e. where the detailed city hands
  over to the LOD city. `DayTime::UpdateInfo` reads both +0x0C and `GetLowResFarPlane()`, apparently
  for the fog start and range (decomp_perf.c; not yet checked against the GL fog calls).
- Not touched by the setting: per-object LOD (`LODSceneNode`, `GS3DStuff::parseSceneNodeForLOD`),
  terrain LOD (`CTerrainSceneNode::overrideLODDistance`), the camera far value
  (`CCameraSceneNode::setFarValue`), and the separate global `g_perfConfigLowResFar` (role unknown).
- **Open:** which of these causes the visible pop-in the owner sees, and how much guest memory a
  longer detailed range needs. How to find out: log the far/LOD values per frame, A/B screenshots
  from a fixed spot, Ghidra on the LODSceneNode distance check.

### Frame structure (measured 2026-11-21, `GANGSTAR_GL_FRAME_TRACE=<frame>`, draw_distance 0.5)
- The engine's projections are left-handed (Irrlicht style): m[11] = +1, m[10] = (f+n)/(f-n),
  m[14] = -2fn/(f-n), so n = -m14/(m10+1), f = -m14/(m10-1). The bridge keeps shadow matrix stacks.
- One gameplay frame (~35 draws: the city is batched):
  1. clear colour + depth;
  2. **sky**, depth range [0.199, 1], n = 50, f = 153015, no fog (dome + one blended layer);
  3. **near city**, depth range [0, 0.199], n = 50, f = 6000 (= gPhonePerf +0x0C, the streaming
     radius): world batches with fog on; people/props drawn **without fog**;
  4. **far (LOD) city**, depth range [0.199, 1], n = 6000, f = 30000 (= +0x10): fogged batches plus
     one blended draw;
  5. transparent near objects, [0, 0.199], near projection, some fogged;
  6. HUD: orthographic, no depth test, ~2 batched draws.
  So the detailed city ends exactly where the LOD city starts, at the streaming radius.
- Game fog: `GL_LINEAR`, start 6000 (streaming radius), end 46000, colour from the time-of-day
  table (night 0.129/0.129/0.212). The near city ends at 6000, so **it is never fogged**; the LOD
  city gets at most ~60% at its end, so its cut-off against the sky stays visible.
- Lighting: the game sets light 0 and materials every frame (night: ambient ≈ 0.21/0.20/0.37,
  diffuse 0.38/0.63/0.80, material emission up to 0.32), but **never enables `GL_LIGHTING`**
  (checked with `glIsEnabled` per draw). People, cars and props are drawn with colour arrays +
  `GL_COLOR_MATERIAL`: the engine lights them on the CPU and passes vertex colours. World batches
  have normals but no colour array, texture env MODULATE.

### 5C.1 People and cars follow the time of day (done 2026-11-22)
- The tint stage now has two colours: world textures showing their day image get the world tint
  (as before); every other 3D draw of the scene (texture not managed by night_textures) gets the
  actor tint = world tint lifted toward 1 by `[graphics] night_people_lift` (default 0.35).
  Never tinted: world textures showing their night image, the sky (depth range [0.2, 1] with the
  camera's near plane 50; the far city uses the streaming radius as near plane), the HUD
  (orthographic) and additive draws (blend dst `GL_ONE`: lamp and headlight glows).
- night_textures reports Day / Night / None per texture (`gles1::WorldTex`) so props it already
  darkens are not darkened twice. Verified at 23:30, 18:15, 05:00, 12:00 (unchanged).

### 5C.4 Atmospheric fog (done 2026-11-21, step 1)
- `[graphics] fog = atmospheric` (default) | `game`, `fog_strength` (default 1.5). The bridge keeps
  the game's fog colour, replaces its distances with `GL_EXP2`, density 1.873 × strength / far end
  (97% at the far city's end at strength 1), radial eye distance via `GL_NV_fog_distance` when
  available. Opaque draws the game leaves unfogged in the near-city depth range (people, cars,
  props) get fog too; the sky (depth range up to 1.0) and blended draws are left alone.
  The far end comes from gPhonePerf +0x10 each frame, so the fog follows the draw distance.
- Step 2 (with bloom, needs a post-process pass): height fog and a warm glow toward the sun.
