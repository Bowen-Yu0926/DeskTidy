@echo off
setlocal
rem Prefer Python/Qt DeskTidy.exe at user Startup.
set "EXE="
if exist "%LOCALAPPDATA%\Programs\DeskTidy\DeskTidy.exe" (
  set "EXE=%LOCALAPPDATA%\Programs\DeskTidy\DeskTidy.exe"
)
if exist "%~dp0..\dist\DeskTidy\DeskTidy.exe" (
  set "EXE=%~dp0..\dist\DeskTidy\DeskTidy.exe"
)
if "%EXE%"=="" (
  echo DeskTidy.exe not found. Install or build the Python product first.
  exit /b 1
)

set "STARTUP=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup"
del /f /q "%STARTUP%\DeskTidy.lnk" 2>nul
del /f /q "%STARTUP%\DeskTidy Native.lnk" 2>nul

powershell -NoProfile -Command ^
  "$ws=New-Object -ComObject WScript.Shell; $s=$ws.CreateShortcut('%STARTUP%\DeskTidy.lnk'); $s.TargetPath='%EXE%'; $s.WorkingDirectory=Split-Path '%EXE%'; $s.Description='DeskTidy'; $s.Save()"
if errorlevel 1 exit /b 1
echo Startup -> DeskTidy (Python/Qt)
echo Target: %EXE%
exit /b 0
