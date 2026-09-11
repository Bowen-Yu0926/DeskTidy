@echo off
cd /d "%~dp0.."
echo Running DeskTidy system self-test...
python scripts\selftest_system.py
exit /b %ERRORLEVEL%
