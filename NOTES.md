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
| pip packages | pyelftools 0.33, capstone 5.0.9, unicorn 2.1.4, itanium_demangler 1.1 | (in that Python) |
| Temurin JDK | 21.0.13+9 | `jdk-21.0.13+9\` |
| jadx | 1.5.6 | `jadx\bin\jadx.bat` |

Downloads kept in `devtools\_downloads\` (can be deleted). Built-ins also
available: `tar.exe`, WinRAR, .NET Framework (PowerShell).

**Install permission (2026-11-18):** the owner said "install whatever u need however
u need pip python everything". This replaces CLAUDE.md's "ask before installing";
still ask before deleting files or changing the approach. Keep installs portable under
`devtools\`, verify checksums, record versions here.

**Not installed yet:** LLVM/clang, CMake, Ninja, Ghidra, SDL2, 7-Zip (not needed; zip
handled by .NET), ANGLE, ADB/Frida, and no C/C++ compiler of any kind (no MSVC either).

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

**Unicorn behaviours relied on** (`test_unicorn_semantics.py`, 2.1.4): in an SVC hook,
PC = svc+4; writing PC (with the Thumb bit) from a hook redirects execution; nested
`emu_start` from inside a hook works (used to call the game's own `malloc` from HLE
`strdup`, and comparators from `qsort`).

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
| Q1 | Which `JNIEnv` functions does the native code use? | **Partly answered (1c):** so far only `NewGlobalRef` + `GetStaticMethodID`. The rest appear once `Gangster2.nativeInit`/`nativeRender` run (Phase 2a) |
| Q2 | Is each callback really looked up on the `jclass` from its class's `nativeInit`? | **Answered (1c): yes**, after `NewGlobalRef(jclass)`. No `FindClass` |
| Q3 | Which overloads does native request? | **Answered (1c):** `loadSound (II)V`, `loadSoundBig (I)V`, `getResourceFull (Ljava/lang/String;)[B` |
| Q4 | What do the 448 init_array constructors call? | **Answered (1b):** atexit/guards/mutexes/string copies/12 KB sbrk; no threads, files or GL |
| Q5 | How big must the `sbrk` heap be, and does any libc-allocated memory reach the game's `free`? | 12 KB after static init. Probe HLE `strdup` already allocates with the game's own `malloc`; watch the full boot in Phase 2a |
| Q6 | What does each StormGLOFT hook change (e.g. `nativeResize`, `UseMipMaps`, `GetLowResFarPlane`)? | Ghidra on libStormGLOFT `JNI_OnLoad` + hook bodies (Ghidra not installed yet) |
| Q7 | What do the demo/trial (`DisableLaunchGame`, `unlockDemo`) and VZ billing callbacks need to return for the normal game flow? | Ghidra on the native callers; probe in Phase 2 |
| Q8 | The 227 `mov pc, reg` jump tables: one GCC pattern or several? | Script over the disassembly (Phase 4 prep) |
| Q9 | Did this APK + data last run on the owner's device in Sept 2025 (InsTime)? If such a device still exists, it could be used to trace real JNI/GL calls (Frida/jnitrace). | Ask the owner |
| Q10 | Exact bionic layouts needed for `__sF` (FILE), `_ctype_` tables, `jmp_buf`, `struct stat`, `dirent` | Implemented in the probe from the Android 2.x headers (`tools/probe/bionic.py`, `hle_libc.py` docstring). `sysconf` constants confirmed by the game's own calls. **`_ctype_` pointer-vs-table still unverified**: the probe watches it, but nothing has read it yet (Phase 2a will). Cross-check against AOSP bionic source when convenient |
| Q11 | Restore the 40 renamed sounds with an alias table, or keep the original's silent failures? | Owner's call in Phase 3 (audio). List in §6a |
| Q12 | Where is the `FileManager` singleton created, and what else must `Gangster2.nativeInit` find (files, GL) before `nativeRender` works? | Phase 2a headless boot in the probe |

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
