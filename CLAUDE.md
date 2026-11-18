# Gangstar Miami — native port project

You are helping port **Gangstar: Miami Vindication** (Gameloft, Android) so it runs on
modern hardware. The original APK no longer runs on current devices. The goal is the
original game running natively, with the original assets the owner supplies. A full
matching decompilation is NOT the goal.

Owner: Aiden (Blender/Unity dev, comfortable with C#/shaders, newer to C/C++ and RE).
Explain RE concepts briefly when they first come up, and keep explanations practical.

## Targets
1. **Primary: Windows PC, x86-64.** 
2. **Secondary: Android arm64 handhelds** (Retroid Pocket 6 / Odin-class), which may not run
   32-bit ARM code.
If the analysis shows a different order is much easier, say so and ask before switching.

## Hard rules
- `original/` holds the untouched APK and data. **Never modify, move or delete anything in it.**
- Never commit game binaries or assets (`.so`, `.apk`, `.obb`, data folders). Keep them
  out of git via `.gitignore`. The repo contains only our own code, scripts and notes.
- Ask before: downloading anything large, installing tools, deleting files, or changing
  the overall approach.
- Work in small, verifiable steps. Commit after each working step with a clear message.
- Never guess silently. If something is unknown (an ABI, a struct layout, what a JNI
  method returns), write it down in `NOTES.md` as an open question and say how to find out.

## Project layout
```
original/            # owner's untouched APK + data (read-only, git-ignored)
work/apk/            # extracted APK (git-ignored)
work/data/           # copy of game data used for testing (git-ignored)
analysis/            # readelf/objdump dumps, import lists, Ghidra exports, jadx output
src/                 # our loader / runtime / shims
tools/               # our helper scripts (Python)
NOTES.md             # running findings log (keep it current)
PLAN.md              # milestones and their status
```

## Memory between sessions
At the start of every session, read `NOTES.md` and `PLAN.md`. At the end of any step,
update them: what was learned, what changed, what's next. These files are the project's
memory, so a fresh session must be able to continue from them alone.

## Technical background (verify everything against the actual binary)
- Gameloft Android games of this era ship one big native library in
  `lib/armeabi/` or `lib/armeabi-v7a/` (32-bit ARM), plus a small Java layer in
  `classes.dex` that calls into it over JNI. Game data usually lives outside the APK, in
  `/sdcard/gameloft/games/<package name>/`.
- 32-bit ARM code can't run directly on x86-64 PCs, and many recent ARM chips
  have dropped 32-bit (AArch32) support. So for both targets, the ARM code has to be
  either **translated** (static recompilation to C/C++) or **emulated/JIT'd**
  (e.g. dynarmic or unicorn), with the Android system calls replaced by our own code.
- The import list of the `.so` defines the "Android surface" we must provide:
  bionic libc/libm/libstdc++, `liblog`, `libGLESv1_CM`/`libGLESv2`, `libEGL`,
  `libOpenSLES`, `libandroid`, and JNI calls into the Java side.

## Candidate approaches (pick after Phase 0, with the owner)
- **A. Loader + JIT (fastest route to "it boots"):** load the ARM `.so` into memory,
  run its code with an ARM JIT/emulator (dynarmic, unicorn), and trap every import to
  host-native implementations (HLE). Same idea as touchHLE runs 32-bit iOS apps on PC.
- **B. Static recompilation (Asphalt5-Native style):** translate the ARM machine code
  to C/C++ and compile it natively for x86-64 / arm64, linked against our shim layer.
  Best performance and final result, but more upfront work.
- **C. Hybrid:** use A to boot the game and map out the full import/JNI surface quickly,
  then move to B for the final native build. Recommend this unless the analysis says otherwise.
- The shim layer (libc, GLES→host GL/ANGLE, audio→SDL/OpenAL, input, file paths,
  fake JNI) is shared by all three approaches, so design it to be reusable.

## Reference projects (study, don't copy blindly; check licenses before reusing code)
- `mrbaazzman/Asphalt5-Native`: Asphalt 5 HD (Gameloft) recompiled to native x86-64.
  The closest proof of concept. Study how it handles the ARM→native translation and
  the Android shims.
- `TheOfficialFloW/gtactw_vita`: GTA Chinatown Wars Android .so loader (PS Vita). Classic
  so-loader structure: ELF loading, relocation, import resolution, function patching.
- `v-atamanenko/backstab-vita`: a **Gameloft** game loaded this way; shows Gameloft-
  specific quirks and data layout.
- `v-atamanenko/soloader-boilerplate` and `v-atamanenko/FalsoJNI`: a fake JVM/JNI in C
  for answering the game's JNI calls without Java.
- `JohnnyonFlame/droidports` and `binarycounter/Bogodroid`: Android `.so` loaders for
  Linux, closer to a desktop host than the Vita ports. Bogodroid documents JNI tracing.
- `EapRules/realracing3-native-arm`: a modern example of a bionic/JNI compatibility
  loader running a big 3D mobile game on Linux/ARM.
- `touchHLE/touchHLE`: 32-bit ARM iOS apps on PC via dynarmic + HLE. Reference for
  approach A.
- `LaurieWired/GhidraMCP`: lets you query Ghidra directly (functions,
  decompilation, xrefs, renaming) if it's set up.

## Tools expected on this machine (check what's actually installed; ask before installing)
Git, Python 3, 7-Zip, CMake + Ninja, LLVM/clang (provides `llvm-readelf`,
`llvm-objdump`, which handle ARM ELF), Ghidra (+ JDK 21), jadx (for `classes.dex`),
SDL2. Optional: ANGLE (GLES on Windows), ADB + Frida/jnitrace if a device that still
runs the original game is available.

## Workflow for the crash-fix loop (later phases)
1. Run, capture the log/crash.
2. Identify the failing import / JNI call / address. Use Ghidra on the `.so` around that address.
3. Implement or stub the smallest thing that gets past it. Log every stub call loudly
   (`STUB: <name>`) so stubs never fail silently.
4. Re-run, record the result in `NOTES.md`, commit.
