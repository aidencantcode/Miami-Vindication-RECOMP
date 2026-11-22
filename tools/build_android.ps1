# Build the Android APK (Phase 5B): android/ Gradle project + the top-level CMakeLists.txt (ANDROID branch).
#   .\tools\build_android.ps1                 # release, arm64-v8a -> build\android\gangstar-arm64.apk
#   .\tools\build_android.ps1 -Abis arm64-v8a,x86_64   # also the emulator ABI
# Needs: Android SDK + NDK r27c + CMake 3.22.1, JDK 21, Gradle 8.7 (paths below), work/recomp generated
# (by the Windows build), and third_party/SDL2, imgui, stb (copies of the vcpkg sources; NOTES 5B).
param([string]$Abis = "arm64-v8a", [switch]$Debug)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$dev = "C:\Users\aiden\devtools"
$env:JAVA_HOME = (Get-ChildItem $dev -Directory -Filter "jdk-21*" | Select-Object -First 1).FullName
$env:ANDROID_HOME = "$dev\android-sdk"
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
$gradle = "$dev\gradle-8.7\bin\gradle.bat"
foreach ($need in "$root\work\recomp\rc_table.c", "$root\third_party\SDL2\CMakeLists.txt", "$root\third_party\imgui\imgui.cpp",
                  "$root\third_party\stb\stb_vorbis.c", $gradle) {
    if (-not (Test-Path $need)) { throw "missing: $need" }
}
if (-not (Test-Path "$root\android\local.properties")) {
    Set-Content "$root\android\local.properties" ("sdk.dir=" + ($env:ANDROID_HOME -replace '\\', '\\' -replace ':', '\:')) -Encoding ascii
}
$task = if ($Debug) { "assembleDebug" } else { "assembleRelease" }
$ErrorActionPreference = "Continue"  # Gradle prints warnings on stderr
Push-Location "$root\android"
& $gradle --no-daemon $task "-Pabis=$Abis" 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath "$root\build\gradle.log" |
    Select-String -Pattern 'BUILD|error:|What went wrong|FAILED' | ForEach-Object { $_.Line }
$rc = $LASTEXITCODE
Pop-Location
if ($rc) { throw "gradle failed ($rc), see build\gradle.log" }
$kind = if ($Debug) { "debug" } else { "release" }
$apk = Get-ChildItem "$root\android\app\build\outputs\apk\$kind" -Filter *.apk | Select-Object -First 1
New-Item -ItemType Directory -Force "$root\build\android" | Out-Null
$out = "$root\build\android\gangstar-$(($Abis -split ',')[0] -replace '-v8a','').apk"
Copy-Item $apk.FullName $out -Force
"APK: $out ($([math]::Round((Get-Item $out).Length / 1MB, 1)) MB)"
