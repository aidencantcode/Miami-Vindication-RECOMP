# Build the native runtime with Visual Studio's MSVC toolchain + bundled vcpkg/CMake/Ninja.
#   .\tools\build.ps1                 # RelWithDebInfo
#   .\tools\build.ps1 -Config debug
# Visual Studio users can instead "Open Folder" on the repo; CMakePresets.json does the rest.
# The recompiled game code (Phase 4, `--cpu recomp`, the default backend when present) is built
# whenever work/recomp has been generated: -Recomp generates it the first time, -NoRecomp leaves it
# out (Unicorn only). The generator re-runs by itself when tools/recomp/*.py changed.
#   .\tools\build.ps1 -Recomp
#   .\tools\build.ps1 -Recomp -Target game_recomp
param([ValidateSet("debug", "release")][string]$Config = "release", [switch]$Recomp, [switch]$NoRecomp,
      [string]$Target = "")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ workload was not found" }

# Enter the x64 developer environment (cl.exe, link.exe, cmake, ninja) for this process.
Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = Join-Path $vs "VC\vcpkg" }

Push-Location $root
try {
    if (-not (Test-Path "third_party\unicorn\lib\unicorn.lib")) { & "$PSScriptRoot\setup_unicorn.ps1" }
    # Native tools print warnings on stderr; PowerShell 5.1 would treat those as errors
    # under "Stop", so judge success by exit codes instead.
    $ErrorActionPreference = "Continue"
    $gen = Join-Path $root "work\recomp"
    $useRecomp = (-not $NoRecomp) -and ($Recomp -or (Test-Path (Join-Path $gen "rc_table.c")))
    if ($useRecomp) {
        # Regenerate when the translator changed since the last run (only changed files are
        # rewritten, so the build then recompiles just those).
        $stamp = Join-Path $gen ".codegen_stamp"
        $newest = (Get-ChildItem (Join-Path $PSScriptRoot "recomp\*.py") | Measure-Object LastWriteTime -Maximum).Maximum
        if (-not (Test-Path $stamp) -or (Get-Item $stamp).LastWriteTime -lt $newest) {
            . (Join-Path $PSScriptRoot "env.ps1") | Out-Null
            python -I (Join-Path $PSScriptRoot "recomp\codegen.py") 2>&1 | ForEach-Object { "$_" }
            if ($LASTEXITCODE) { throw "codegen failed" }
            Set-Content -Path $stamp -Value (Get-Date -Format o) -Encoding ascii
        }
    }
    $recompFlag = if ($useRecomp) { "-DGANGSTAR_RECOMP=ON" } else { "-DGANGSTAR_RECOMP=OFF" }
    cmake --preset win-x64 $recompFlag 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE) { throw "configure failed" }
    $targetArgs = if ($Target) { @("--target", $Target) } else { @() }
    cmake --build --preset $Config @targetArgs 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE) { throw "build failed" }
} finally { Pop-Location }
