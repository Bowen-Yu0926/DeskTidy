@echo off
cd /d "%~dp0.."
set PYTHONIOENCODING=utf-8
set PYTHONUTF8=1
echo Running DeskTidy full self-test x3...
python scripts\selftest_all_x3.py %*
exit /b %ERRORLEVEL%
