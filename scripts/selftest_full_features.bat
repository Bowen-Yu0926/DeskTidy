@echo off
cd /d "%~dp0.."
set PYTHONIOENCODING=utf-8
set PYTHONUTF8=1
set DESKTIDY_SELFTEST=1
python scripts\selftest_full_features.py %*
exit /b %ERRORLEVEL%
