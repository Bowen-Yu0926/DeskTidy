@echo off
setlocal enabledelayedexpansion

echo ========================================
echo   DeskTidy Installer Build
echo ========================================
echo.

cd /d "%~dp0.."

set "BUILD_T0=%TIME%"

echo [0/4] Stopping DeskTidy if running...
call scripts\stop_desktidy.bat
set "DESKTIDY_ALREADY_STOPPED=1"
echo.

echo [1/4] Syncing version (no auto-bump)...
for /f "delims=" %%i in ('python scripts\bump_version.py') do set APP_VERSION=%%i
if errorlevel 1 (
    echo Version sync failed.
    exit /b 1
)
echo Version: !APP_VERSION!
echo.

echo [2/4] Building DeskTidy.exe ...
rem Daily packs: skip pip unless DESKTIDY_FORCE_PIP=1 (saves ~5-15s).
if /I not "%DESKTIDY_FORCE_PIP%"=="1" (
    set "DESKTIDY_SKIP_PIP=1"
    echo Skipping pip install ^(default^). Set DESKTIDY_FORCE_PIP=1 to reinstall.
)
call scripts\build.bat
if errorlevel 1 (
    echo EXE build failed.
    exit /b 1
)

echo.
echo [3/4] Looking for Inno Setup Compiler ...

set ISCC=
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" (
    set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
)
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" (
    set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
)
if exist "C:\Program Files\Inno Setup 6\ISCC.exe" (
    set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
)

if "%ISCC%"=="" (
    echo Inno Setup not found. Attempting install via winget...
    winget install --id JRSoftware.InnoSetup -e --accept-source-agreements --accept-package-agreements
    if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" (
        set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
    )
    if exist "C:\Program Files\Inno Setup 6\ISCC.exe" (
        set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
    )
)

if "%ISCC%"=="" (
    echo.
    echo ERROR: Inno Setup 6 is not installed.
    echo Please install from: https://jrsoftware.org/isdl.php
    echo Then run this script again.
    exit /b 1
)

echo Found: %ISCC%

echo.
echo Cleaning old installer packages...
if exist "dist\DeskTidy_Setup_*.exe" (
    for %%F in ("dist\DeskTidy_Setup_*.exe") do (
        echo   Removing %%~nxF
        del /f /q "%%F" 2>nul
    )
) else (
    echo   No old installers found.
)

if not exist "assets\ffmpeg\ffmpeg.exe" (
    echo.
    echo ERROR: assets\ffmpeg\ffmpeg.exe is required for screen recording.
    echo Run: python scripts\prepare_ffmpeg_recording.py
    exit /b 1
)

echo Validating FFmpeg bundle ^(h264_mf / ddagrab / gdigrab^)...
python scripts\verify_ffmpeg_bundle.py
if errorlevel 1 (
    echo FFmpeg validation failed.
    exit /b 1
)

if not exist "assets\fd\fd.exe" (
    echo.
    echo ERROR: assets\fd\fd.exe is required for file search.
    echo Place a Windows fd.exe there before packaging.
    echo Download: https://github.com/sharkdp/fd/releases
    exit /b 1
)

rem Optional smaller installer: set DESKTIDY_INSTALLER_MAX=1 for lzma2/max
set "ISCC_EXTRA="
if /I "%DESKTIDY_INSTALLER_MAX%"=="1" (
    echo Using max compression ^(DESKTIDY_INSTALLER_MAX=1^) — slower pack.
    set "ISCC_EXTRA=/DDeskTidyCompress=lzma2/max"
)

echo.
echo [4/4] Compiling installer...
"%ISCC%" /DMyAppVersion=!APP_VERSION! !ISCC_EXTRA! "installer\DeskTidy.iss"


if exist "dist\DeskTidy_Setup_!APP_VERSION!.exe" (
    echo.
    echo ========================================
    echo   Build successful!
    echo   Version:   !APP_VERSION!
    echo   Installer: dist\DeskTidy_Setup_!APP_VERSION!.exe
    echo   Started:   %BUILD_T0%
    echo   Finished:  %TIME%
    echo ========================================
    echo.
    echo Starting DeskTidy.exe ...
    rem Selftests often set QT_QPA_PLATFORM=offscreen in the parent shell; if that
    rem leaks into start, DeskTidy runs headless (no tray/overlays) with SetWindowPos 1400.
    set "QT_QPA_PLATFORM="
    set "QT_QPA_PLATFORM_PLUGIN_PATH="
    start "" "dist\DeskTidy\DeskTidy.exe"
) else (
    echo Installer build failed.
    exit /b 1
)

endlocal
