@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>&1
if errorlevel 1 (
  echo [!] cmake not in PATH.
  echo     Install CMake and Visual Studio 2022 with "Desktop development with C++",
  echo     then open "x64 Native Tools Command Prompt for VS" and re-run build.bat
  exit /b 1
)

if not exist build (
  cmake -S . -B build -G "Visual Studio 17 2022" -A x64
  if errorlevel 1 (
    echo Trying default generator...
    cmake -S . -B build
    if errorlevel 1 exit /b 1
  )
)

cmake --build build --config Release
if errorlevel 1 exit /b 1

echo.
echo Built: build\Release\desktidy_native.exe
echo Run:   build\Release\desktidy_native.exe
echo Hotkeys: tray -^> modify chords / hotkeys.json
exit /b 0
