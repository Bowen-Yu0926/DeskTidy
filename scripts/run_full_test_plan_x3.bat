@echo off
cd /d "%~dp0.."
echo DeskTidy 全功能测试用例 x3
python scripts\run_full_test_plan_x3.py --rounds 3 --with-live %*
exit /b %ERRORLEVEL%
