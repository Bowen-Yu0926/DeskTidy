@echo off
rem BUG-09 fix: use the project venv interpreter and run every step,
rem summarizing exit codes instead of fail-fast on the first failure.
rem ASCII-only output: the file is saved UTF-8 without BOM, and a cp936
rem console mis-parses UTF-8 Chinese in echo lines (root cause of garbled
rem "is not recognized as an internal or external command" errors).
cd /d "%~dp0.."
set PYTHONIOENCODING=utf-8
set DESKTIDY_SELFTEST=1
set "PY=%~dp0..\.venv\Scripts\python.exe"
if not exist "%PY%" set "PY=python"
set FAILTOTAL=0
set STEPRUN=0

call :run "1/5 virtual fences regression"   scripts\selftest_virtual.py
call :run "2/5 recent features regression"  scripts\selftest_recent.py
call :run "3/5 public drag into fence"      scripts\selftest_public_drag.py
call :run "3b/5 public HWND merge"          scripts\selftest_public_host.py
call :run "3c/5 public multiselect drag"    scripts\selftest_public_multiselect.py
call :run "3d/5 folder portal"              scripts\selftest_portal.py
call :run "3e/5 fence blank shell menu"     scripts\selftest_fence_bg_menu.py
call :run "3e2/5 fence keys and clipboard"  scripts\selftest_fence_keys.py
call :run "3f/5 multi-monitor chrome"       scripts\selftest_multimon_chrome.py
call :run "4/5 system selftest"             scripts\selftest_system.py
call :run "5/5 packaged live e2e"           scripts\selftest_live_e2e.py

echo.
echo ========== cleanup test artifacts ==========
"%PY%" scripts\cleanup_selftest_artifacts.py
if errorlevel 1 set /a FAILTOTAL+=1

echo.
echo Steps run %STEPRUN%  failed %FAILTOTAL%
if %FAILTOTAL% gtr 0 (
    echo TEST SUITE HAS FAILING STEPS
    exit /b 1
)
echo ALL TEST SUITES PASSED
exit /b 0

:run
echo.
echo ========== %~1 ==========
"%PY%" %~2
set "RC=%errorlevel%"
set /a STEPRUN+=1
if not "%RC%"=="0" (
    echo [FAIL] %~1 exit code %RC%
    set /a FAILTOTAL+=1
) else (
    echo [ OK ] %~1
)
goto :eof
