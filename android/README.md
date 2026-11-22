# Android build (Phase 5B)

The same recompiled game code and support layer as the Windows build, built for arm64 handhelds
(Retroid Pocket, Odin and similar) with the GLES 3 shader renderer.

## Build the APK (on the Windows PC)

Needs: the Android SDK + NDK r27c + CMake 3.22.1 (`C:\Users\aiden\devtools\android-sdk`), JDK 21,
Gradle 8.7, and the recompiled code generated once on Windows (`work/recomp`, made by the Windows
build). The SDL2 / Dear ImGui / stb sources are expected in `third_party/` (copied from the vcpkg
build tree; see NOTES.md, 5B).

```powershell
.\tools\build_android.ps1            # -> build\android\gangstar-arm64.apk
```

## Put the game on the device

1. Install the APK (`adb install -r build\android\gangstar-arm64.apk`, or copy it over and open it).
2. Start it once: it creates its folder and shows which files are missing, then closes.
3. Copy these into `Android/data/com.recomp.gangstar/files/` on the device (USB file transfer works;
   no storage permission is needed for this folder):
   - `libGangster2.so` (from your APK: `work/apk/lib/armeabi/libGangster2.so` on the PC)
   - `sound_table.txt` (from `work/sound_table.txt`; optional, fixes some sound names)
   - the game data folder as `gameloft/games/Gangstar2/` (from `original/gameloft/games/Gangstar2`)
4. Start the game. Saves, `user/settings.ini` and `runtime.log` are written to the same folder.

The APK contains none of the game's files; it only runs your copy.

## Controls

Touch works like the phone. A controller works like on Windows (see the main README). Tilt steering
uses the device's motion sensor when no controller is steering (if it turns the wrong way: settings,
`tilt_invert`). **Three fingers on the screen** open the settings menu (or F1 / Guide / L3+R3).
