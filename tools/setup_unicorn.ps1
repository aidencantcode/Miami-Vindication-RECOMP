# Make the Unicorn C library available to the C++ build, using the SAME Unicorn
# build the Python probe runs (pip wheel 2.1.4), so both emulate identically.
# Produces third_party/unicorn/{include,bin/unicorn.dll,lib/unicorn.lib (import lib)}.
# Must run inside a Visual Studio developer shell (needs dumpbin + lib); build.ps1 does that.
# Unicorn is GPLv2: fine for this private port; it is dropped once the game is recompiled (Phase 4).
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$wheel = "C:\Users\aiden\devtools\python-nupkg\tools\Lib\site-packages\unicorn"
if (-not (Test-Path $wheel)) {  # other machines: the unicorn package of the Python on PATH (pip install unicorn==2.1.4)
    $wheel = "$(python -I -c "import os, unicorn; print(os.path.dirname(unicorn.__file__))")".Trim()
    if (-not $wheel -or -not (Test-Path "$wheel\lib\unicorn.dll")) { throw "Unicorn not found: pip install unicorn==2.1.4" }
}
$dst = Join-Path $root "third_party\unicorn"
New-Item -ItemType Directory -Force "$dst\bin", "$dst\lib", "$dst\include" | Out-Null
Copy-Item -Recurse -Force "$wheel\include\unicorn" "$dst\include\"
Copy-Item -Force "$wheel\lib\unicorn.dll" "$dst\bin\"

# Import library from the DLL's export table.
$exports = dumpbin /nologo /exports "$dst\bin\unicorn.dll"
$names = foreach ($line in $exports) {
    if ($line -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)') { $Matches[1] }
}
if (-not $names) { throw "no exports parsed from unicorn.dll" }
@("LIBRARY unicorn", "EXPORTS") + $names | Set-Content -Encoding ascii "$dst\lib\unicorn.def"
lib /nologo /machine:x64 /def:"$dst\lib\unicorn.def" /out:"$dst\lib\unicorn.lib" | Out-Null
if ($LASTEXITCODE) { throw "lib.exe failed" }
Write-Host "Unicorn ready: $($names.Count) exports -> $dst"
