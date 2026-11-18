# Java ↔ native boundary (libGangster2.so)

Sources: jadx decompilation of `classes.dex` (`analysis/jadx/`, git-ignored),
the `.so`'s dynamic exports (`analysis/libGangster2.jni_exports.md`) and the
method-name/signature strings in its `.rodata` (`0x58db70`–`0x58dfd0`).
Package for every class below: `com.gameloft.android.GAND.GloftGMHP.ML`.

JNI calling convention reminder (ARM EABI, soft-float): `r0 = JNIEnv*`,
`r1 = jclass` (static method) or `jobject this` (instance method), then the Java
arguments in `r2`, `r3`, then on the stack. `float` args travel in core
registers (soft-float). A `long` (J) is a 64-bit value in an even-aligned
register pair or 8-byte-aligned stack slot.

## 1. Java → native (methods Java declares `native`)

| Java class | method | JNI signature | in .so? | called from |
|---|---|---|---|---|
| Gangster2 | nativeSetPhone | (I)V | yes | `onCreate`, screen width in px. Stores arg in a global. |
| Gangster2 | nativeGetInfo | (Ljava/lang/String;×4)V | yes | `onCreate`: country, IMEI, "MANUFACTURER_MODEL", Android version. **Native body is just `bx lr` (ignored).** |
| Gangster2 | nativeInit | (I)V | yes | `onSurfaceCreated`, arg = 1 if demo mode else 0 |
| Gangster2 | nativeAccelerometer | (FFF)V | yes | sensor listener |
| Gangster2 | nativeKeyDown / nativeKeyUp | (I)V | yes | render thread, Android keycodes queued from `onKeyDown/Up` |
| Gangster2 | nativePause / nativeResume | ()V | yes | render thread event queue |
| Gangster2 | nativeonTrackballEvent | (I)V | yes | (not seen called) |
| Gangster2 | nativeOpenIGM | ()V | yes | (not seen called) |
| Gangster2 | nativeCanInterrupt | ()I | yes | private, not seen called |
| Gangster2 | nativeIsExit | ()I | **no** | not called |
| GameRenderer | nativeGetJNIEnv | ()V (instance) | yes | first call in `onSurfaceCreated`. **Stores `JNIEnv*` (r0) in a global; all later callbacks use it.** |
| GameRenderer | nativeInit | (I)V | yes | `onSurfaceCreated`, arg = 1 |
| GameRenderer | nativeResize | (II)V | yes | `onSurfaceChanged(w, h)` |
| GameRenderer | nativeRender | ()V | yes | every `onDrawFrame` |
| GameRenderer | nativeDone | ()V | yes | (not seen called) |
| GameGLSurfaceView | nativeOnTouch | (IIIJ)V | yes | `onTouchEvent`: (action 1=down 2=move 0=up, x, y, pointerId as long) |
| GLMediaPlayer | nativeInit | (I)V | yes | `GLMediaPlayer.init()`, arg 0 |
| GLMediaPlayer | nativeGetTotalSounds | ()I | yes | `init()` |
| GLMediaPlayer | nativeGetTotalSoundsOfSameInstance | ()I | yes | `init()` |
| GLMediaPlayer | nativeGetSoundFileName | (I)Ljava/lang/String; | yes | `getSoundFile(i)` |
| GLMediaPlayer | nativeSetStopOnMusic | (I)V | yes | |
| GLResLoader | nativeInit | (I)V | yes | `GLResLoader.init()`, arg 0 |
| GLBluetooth | nativeInit | ()V | **no** | Bluetooth multiplayer (unused path) |
| GLBluetoothService | ConnectionStateChanged / ReceiveData | (II)V / (I)V | **no** | (unused path) |
| GLiveMain | nativeInit | ()V | **no** | Gameloft Live (unused path) |

28 declared, 23 implemented. The 5 missing would throw `UnsatisfiedLinkError`
if called, so they are dead paths in this build.

### Startup order (all on the GLSurfaceView render thread unless noted)

1. *(UI thread)* `SplashActivity` sleeps 5 s → `Gangster2.onCreate` → if
   `GameInstaller.ex` is false, run `GameInstaller` first (data check/download),
   which relaunches `Gangster2`.
2. *(UI thread)* `MyVideoView` plays `/sdcard/gameloft/games/Gangstar2/intro.m4v`.
3. *(UI thread)* `nativeSetPhone(widthPixels)`, `nativeGetInfo(...)`.
4. `onSurfaceCreated`: `GameRenderer.nativeGetJNIEnv()` → `GLResLoader.nativeInit(0)`
   → `GLMediaPlayer.nativeInit(0)`, `nativeGetTotalSounds()`,
   `nativeGetTotalSoundsOfSameInstance()` → `Gangster2.nativeInit(isDemo)` →
   `GameRenderer.nativeInit(1)`.
5. `onSurfaceChanged`: `nativeResize(w, h)`.
6. Each frame: drain the event queue (focus/pause/resume → `nativePause`/`nativeResume`),
   `nativeKeyDown/Up` for one queued key each, then `nativeRender()`.

The GL context (GLES 1.x, created by `GLSurfaceView`, no EGL calls from native)
is current on the render thread for all of these.

## 2. Native → Java (static methods the native code calls back)

There are no `com/gameloft/...` class-name strings in the `.so`. Each class
passes its own `jclass` through its static `nativeInit`, and the callbacks are
looked up on those (inferred; confirm in the Phase 1 probe by logging
`GetStaticMethodID`). Signatures come from the Java declarations and match the
signature strings in `.rodata`.

### Gangster2 (21): lifecycle, store, demo/trial state

| method | sig | what Java does |
|---|---|---|
| Exit | ()V | finish + `System.exit(0)` |
| sendAppToBackground | ()V | launches the HOME intent |
| openBrowser | (Ljava/lang/String;)V | VIEW intent |
| isWifiAlive | ()I | 1 if the active network is Wi-Fi |
| launchIGP | (I)V | Gameloft in-game promo activity (language arg) |
| OpenGLive | (I)V | Gameloft Live activity |
| NotifyTrophy | (I)V | records trophy index in a Java array |
| unlockDemo | ()I | sets `playMode=1` in SharedPreferences `GANGSTER2`, returns 0 |
| lockDemo | ()V | sets `playMode=0`, `lastNumOfLaunchs=0` |
| DisableLaunchGame | ()I | 1 if launched ≥ 5 times (trial limit) |
| IncreaseLaunchTimes | ()V | increments `lastNumOfLaunchs` |
| VZInitMobileNetwork / VZIsMobileNetworkReady / VZRestoreNetworkState | ()V / ()I / ()V | Verizon network toggling |
| VZRequestLogin / VZRequestPurchaseGame | ()V | Verizon VCAST billing |
| VZIsInProgress / VZIsErrorOcurred | ()I | billing state |
| VZGetGameName / VZGetGamePrice / VZGetLastServerMsg | ()[B | billing strings |

### GLMediaPlayer (33): audio, video, device info

Sound files: Java asks native for names (`nativeGetSoundFileName(i)`) and loads
`/sdcard/gameloft/games/Gangstar2/<name>` via **SoundPool** (`*Sound`, effects)
or **MediaPlayer** (`*SoundBig`, music/streams).

| method | sig | | method | sig |
|---|---|---|---|---|
| loadSound | (II)V ¹ | | loadSoundBig | (I)V ¹ |
| unloadSound | (II)V | | unloadSoundBig | (I)V |
| isSoundLoaded | (II)I | | isSoundLoadedBig | (I)I |
| playSound | (IIFF)V | | playSoundBig | (IFI)V |
| pauseSound | (II)V | | pauseSoundBig | (I)V |
| resumeSound | (II)V | | resumeSoundBig | (I)V |
| stopSound | (II)V | | stopSoundBig | (I)V |
| setVolume | (IIF)V | | setVolumeBig | (IF)V |
| setPitch | (IIF)V | | resetSound | (I)V |
| stopAllSounds | ()V | | stopAllPool | (I)V |
| pauseAllSounds | ()V | | stopAllBig | (I)V |
| resumeAllSounds | ()V | | destroySoundPool | ()V |
| initSoundPoolArray | ()V | | isMediaPlaying | (I)I |
| setMusicGain | (F)V | | setSfxGain | (F)V |
| setVfxGain | (F)V | | loadMovie | (Ljava/lang/String;)I |
| getDeviceWidth | ()I | | detectPhoneLang | ()I |
| GetDeviceType | ()I | | | |

¹ Java also has overloads with an extra `Context` argument; the `.rodata`
signature strings next to these names are `(II)V` / `(I)V`.

- `GetDeviceType()`: Motorola Milestone/Droid → 4, other Motorola → 1, Samsung → 2,
  HTC → 3, **anything else (incl. the LG P990 this build targets) → -1**.
- `detectPhoneLang()`: eng 0, fra 1, deu 2, spa 3, ita 4, jpn 5, zho-CN 6, else 0.
- `getDeviceWidth()`: the GLSurfaceView width.
- `loadMovie(name)`: plays `/sdcard/gameloft/games/Gangstar2/<name>` in `MyVideoView`.

### GLResLoader (3): file reads through Java

| method | sig | |
|---|---|---|
| getResourceFull | (Ljava/lang/String;)[B ² | whole file |
| getResourceBytes | (Ljava/lang/String;II)[B | `len` bytes at `offset` |
| getResourceLength | (Ljava/lang/String;)I | size |

Lookup order: APK `res/drawable/res_<name>` → APK `assets/<path>` →
`/sdcard/gameloft/games/gangster2/<path>` (**lowercase** "gangster2"; the native
side uses "Gangstar2", so the game relied on the case-insensitive sdcard FS).
This APK has no `assets/` folder and no `res_*` drawables.

² Overload `(I)[B` (raw resource by id) also exists.

## 3. JNIEnv functions the native code uses

Not knowable statically (calls go through the `JNIEnv` function table, not ELF
imports). **Open question.** The Phase 1 probe answers it by giving the game a
fake `JNIEnv` whose every slot is a logging trap.
