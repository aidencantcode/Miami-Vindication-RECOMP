# Gameplay benchmark: boot, reach gameplay with the standard script, stand still at the spawn,
# and average "game ms/frame" (time inside nativeRender) and fps over a fixed window.
#   .\tools\bench.ps1 [-Runs 2] [-From 64] [-To 78] [-Extra "--set graphics.quality=low"]
param([int]$Runs = 2, [double]$From = 64, [double]$To = 78, [string]$Extra = "")
$ErrorActionPreference = "Continue"
$root = Split-Path $PSScriptRoot -Parent
$exe = (Get-ChildItem -Recurse (Join-Path $root "build") -Filter gangstar.exe | Select-Object -First 1).FullName
$script = "t20:tap 427 300; t32:tap 200 210; t44:tap 800 35; t48:tap 800 35; t52:tap 800 35; t56:tap 800 35"
$log = Join-Path $root "build\runtime.log"
for ($r = 1; $r -le $Runs; $r++) {
    $args = @("--run", "--seconds", "$To", "--input", $script) + ($Extra -split " " | Where-Object { $_ })
    $t0 = Get-Date
    & $exe @args | Out-Null
    # The 2 s report lines carry no timestamps; map them to seconds by order (first report ~2 s).
    $lines = Select-String -Path $log -Pattern "^frame \d+: ([\d.]+) fps, game ([\d.]+) ms/frame" |
        ForEach-Object { [pscustomobject]@{ fps = [double]$_.Matches[0].Groups[1].Value; ms = [double]$_.Matches[0].Groups[2].Value } }
    $n = $lines.Count
    $take = [math]::Max(1, [int](($To - $From) / 2))
    $win = $lines | Select-Object -Last $take
    $fps = ($win | Measure-Object fps -Average).Average
    $ms = ($win | Measure-Object ms -Average).Average
    "run {0}: {1:N1} fps, game {2:N1} ms/frame (last {3} reports of {4})" -f $r, $fps, $ms, $take, $n
}
