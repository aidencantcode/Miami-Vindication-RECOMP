# Dot-source this to put the portable dev tools on PATH for the current shell:
#   . .\tools\env.ps1
# Nothing here touches the system or user PATH permanently.

$DevTools = "C:\Users\aiden\devtools"

$env:JAVA_HOME = "$DevTools\jdk-21.0.13+9"
$env:PATH = @(
    "$DevTools\git\cmd",
    "$DevTools\python-nupkg\tools",
    "$DevTools\python-nupkg\tools\Scripts",
    "$env:JAVA_HOME\bin",
    "$DevTools\jadx\bin",
    $env:PATH
) -join ";"

# Python reads text files as UTF-8 regardless of the Windows code page.
$env:PYTHONUTF8 = "1"
