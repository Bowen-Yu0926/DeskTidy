@echo off
setlocal
rem Prefer DeskTidy Native at user Startup (cutover helper).
set "EXE="
if exist "%LOCALAPPDATA%\Programs\DeskTidyNative\desktidy_native.exe" (
  set "EXE=%LOCALAPPDATA%\Programs\DeskTidyNative\desktidy_native.exe"
)
if exist "%LOCALAPPDATA%\Programs\DeskTidy\desktidy_native.exe" (
  set "EXE=%LOCALAPPDATA%\Programs\DeskTidy\desktidy_native.exe"
)
if exist "%~dp0..\native\build\Release\desktidy_native.exe" (
  set "EXE=%~dp0..\native\build\Release\desktidy_native.exe"
)
if "%EXE%"=="" (
  echo desktidy_native.exe not found. Build native or install DeskTidyNative first.
  exit /b 1
)

set "STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
del /f /q "%STARTUP%\DeskTidy.lnk" 2>nul
del /f /q "%STARTUP%\DeskTidy Native.lnk" 2>nul

powershell -NoProfile -Command ^
  "$ws=New-Object -ComObject WScript.Shell; $s=$ws.CreateShortcut('%STARTUP%\DeskTidy Native.lnk'); $s.TargetPath='%EXE%'; $s.WorkingDirectory=Split-Path '%EXE%'; $s.Description='DeskTidy Native'; $s.Save()"
if errorlevel 1 exit /b 1
echo Startup -> DeskTidy Native
echo Target: %EXE%
exit /b 0
