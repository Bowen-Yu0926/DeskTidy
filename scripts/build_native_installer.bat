@echo off
setlocal enabledelayedexpansion

echo ========================================
echo   DeskTidy Native Installer Build
echo ========================================
echo.

cd /d "%~dp0.."

echo [1/3] Building desktidy_native.exe + deskNote.exe ...
call native\build.bat
if errorlevel 1 (
  echo Native build failed.
  exit /b 1
)

set "APP_VERSION="
if exist VERSION (
    set /p APP_VERSION=<VERSION
)
if "!APP_VERSION!"=="" set "APP_VERSION=4.0.31"
echo Version: !APP_VERSION!

echo.
echo [2/3] Looking for Inno Setup Compiler ...
set ISCC=
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if exist "C:\Program Files\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"

if "%ISCC%"=="" (
  echo Inno Setup not found. Attempting winget install...
  winget install --id JRSoftware.InnoSetup -e --accept-source-agreements --accept-package-agreements
  if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
  if exist "C:\Program Files\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
)

if "%ISCC%"=="" (
  echo ERROR: Inno Setup 6 is not installed.
  echo Install from https://jrsoftware.org/isdl.php
  exit /b 1
)
echo Found: %ISCC%

echo.
echo [3/3] Compiling installer\DeskTidy.iss ...
if not exist dist mkdir dist
"%ISCC%" /DMyAppVersion=!APP_VERSION! installer\DeskTidy.iss
if errorlevel 1 (
  echo Installer compile failed.
  exit /b 1
)

echo.
echo Built: dist\DeskTidy_Setup_!APP_VERSION!.exe
exit /b 0
