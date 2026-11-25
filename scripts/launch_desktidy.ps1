# Launch DeskTidy native as a detached process on the interactive desktop.
# Path is derived from this script so the repo can live anywhere (no hardcoded E:).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'native\build\Release\desktidy_native.exe'
$workDir = Split-Path -Parent $exe
if (-not (Test-Path -LiteralPath $exe)) {
    Write-Error "DeskTidy native executable not found: $exe (build native\ first)"
    exit 1
}
$procArgs = @{
    CommandLine = ('"{0}"' -f $exe)
    CurrentDirectory = $workDir
}
$res = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments $procArgs
if ($null -eq $res -or $res.ProcessId -le 0) {
    Write-Error "Failed to start DeskTidy (CIM create returned no PID)"
    exit 1
}
Write-Host "DeskTidy started with PID: $($res.ProcessId)"
exit 0
