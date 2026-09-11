@echo off
setlocal

echo ========================================
echo   DeskTidy Fast Build (exe only)
echo ========================================
echo.
echo Skips: version bump / pip reinstall / Inno installer
echo Use scripts\build_installer.bat for a release package.
echo.

cd /d "%~dp0.."

call scripts\stop_desktidy.bat
echo.

set "DESKTIDY_SKIP_PIP=1"
call scripts\build.bat
if errorlevel 1 (
    echo Fast build failed.
    exit /b 1
)

echo.
echo Starting DeskTidy.exe ...
rem Avoid inheriting QT_QPA_PLATFORM=offscreen from selftest shells.
set "QT_QPA_PLATFORM="
set "QT_QPA_PLATFORM_PLUGIN_PATH="
start "" "dist\DeskTidy\DeskTidy.exe"
endlocal
