@echo off
setlocal
cd /d "%~dp0.."
python scripts\release.py %*
exit /b %ERRORLEVEL%
