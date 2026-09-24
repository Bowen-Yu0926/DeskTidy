@echo off
REM Stop running DeskTidy so dist\DeskTidy.exe is not file-locked during build.
REM Also stop source launches (python main.py) that share the same instance mutex
REM and LL hooks - leaving those alive after packaging made the new exe look frozen.
setlocal enabledelayedexpansion

set "STOPPED=0"

tasklist /FI "IMAGENAME eq DeskTidy.exe" 2>nul | find /I "DeskTidy.exe" >nul
if not errorlevel 1 (
    echo Stopping DeskTidy.exe ...
    taskkill /IM DeskTidy.exe /F >nul 2>&1
    set "STOPPED=1"
)

tasklist /FI "IMAGENAME eq deskNote.exe" 2>nul | find /I "deskNote.exe" >nul
if not errorlevel 1 (
    echo Stopping deskNote.exe ...
    taskkill /IM deskNote.exe /F >nul 2>&1
    set "STOPPED=1"
)

powershell -NoProfile -Command "$root=(Resolve-Path -LiteralPath '%~dp0..').Path; $killed=0; Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(python|pythonw)\.exe$' -and $_.CommandLine -and $_.CommandLine -notmatch '(?i)selftest' -and ( ($_.CommandLine -match [regex]::Escape($root) -and $_.CommandLine -match '(?i)(main\.py|desknote_main\.py)') -or ($_.CommandLine -match '(?i)Desktidy.*(main\.py|desknote_main\.py)') -or ($_.CommandLine -match '(?i)-u\s+\S*main\.py') -or ($_.CommandLine -match '(?i)-u\s+\S*desknote_main\.py') ) } | ForEach-Object { Write-Host ('Stopping source DeskTidy/deskNote PID ' + $_.ProcessId); Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; $script:killed=1 }; if ($killed) { exit 10 } else { exit 0 }"
if errorlevel 10 set "STOPPED=1"

if "%STOPPED%"=="0" (
    echo DeskTidy is not running.
    exit /b 0
)

set /a _wait=0
:wait_loop
tasklist /FI "IMAGENAME eq DeskTidy.exe" 2>nul | find /I "DeskTidy.exe" >nul
if not errorlevel 1 (
    set /a _wait+=1
    if !_wait! GEQ 30 (
        echo WARNING: DeskTidy.exe still running after wait; build may fail if exe is locked.
        exit /b 1
    )
    timeout /t 1 /nobreak >nul
    goto wait_loop
)

REM Let LL hooks / mutex settle after forced kill (avoids next start hanging).
timeout /t 2 /nobreak >nul
echo DeskTidy stopped.
exit /b 0
