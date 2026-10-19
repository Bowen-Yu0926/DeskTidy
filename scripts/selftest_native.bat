@echo off
setlocal
cd /d "%~dp0.."

echo === DeskTidy Native smoke ===
call native\build.bat
if errorlevel 1 (
  echo FAIL: build
  exit /b 1
)

set EXE=native\build\Release\desktidy_native.exe
if not exist "%EXE%" (
  echo FAIL: missing %EXE%
  exit /b 1
)

taskkill /IM desktidy_native.exe /F >nul 2>&1
start "" "%EXE%"
timeout /t 3 /nobreak >nul

tasklist /FI "IMAGENAME eq desktidy_native.exe" | find /I "desktidy_native.exe" >nul
if errorlevel 1 (
  echo FAIL: process not running
  exit /b 1
)

echo OK: desktidy_native running
echo OK: layout dir %%LOCALAPPDATA%%\DeskTidyNative
if exist "%LOCALAPPDATA%\DeskTidyNative\layout.json" (
  echo OK: layout.json present
) else (
  echo WARN: layout.json not yet written
)
if exist "%LOCALAPPDATA%\DeskTidyNative\rules.json" (
  echo OK: rules.json present
) else (
  echo WARN: rules.json not yet written
)

echo === smoke passed ===
exit /b 0
