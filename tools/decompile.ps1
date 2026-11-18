# Decompile game functions with the analysed Ghidra project (analysis/ghidra).
#   .\tools\decompile.ps1 -Out analysis\raw\decomp_input.c -Names "appKeyPressed","AnalogStick::processTouch"
# Output stays in analysis/raw (git-ignored: it is derived from the game's code).
param([Parameter(Mandatory)][string]$Out, [Parameter(Mandatory)][string[]]$Names)
$ErrorActionPreference = "Continue"
$root = Split-Path $PSScriptRoot -Parent
$env:JAVA_HOME = (Get-ChildItem C:\Users\aiden\devtools -Directory -Filter "jdk-25*" | Select-Object -First 1).FullName
$env:PATH = "$env:JAVA_HOME\bin;$env:PATH"
$env:GHIDRA_HEADLESS_MAXMEM = "6G"
$ghidra = (Get-ChildItem C:\Users\aiden\devtools -Directory -Filter "ghidra_*" | Select-Object -First 1).FullName
$list = Join-Path $env:TEMP "ghidra_names_$PID.txt"
# No BOM (Set-Content -Encoding utf8 in PowerShell 5.1 would prepend one to the first name).
[IO.File]::WriteAllLines($list, [string[]]$Names, (New-Object Text.UTF8Encoding $false))
$outAbs = Join-Path $root $Out
& "$ghidra\support\analyzeHeadless.bat" (Join-Path $root "analysis\ghidra") Gangstar -process libGangster2.so `
    -noanalysis -readOnly -scriptPath (Join-Path $root "tools\ghidra") `
    -postScript DecompileFunctions.java $outAbs $list 2>&1 | ForEach-Object { "$_" } |
    Select-String -Pattern "ERROR|Exception|wrote " | ForEach-Object { $_.Line }
