@echo off
setlocal
cd /d "%~dp0.."
if not exist "native\build\Release\desktidy_native.exe" (
  echo Building native...
  call native\build.bat
  if errorlevel 1 exit /b 1
)
start "" "native\build\Release\desktidy_native.exe"
exit /b 0
