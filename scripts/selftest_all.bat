@echo off
cd /d "%~dp0.."
set PYTHONIOENCODING=utf-8
set DESKTIDY_SELFTEST=1
echo ========== 1/5 虚拟分区回归 ==========
python scripts\selftest_virtual.py
if errorlevel 1 exit /b 1
echo.
echo ========== 2/5 近期功能回归 ==========
python scripts\selftest_recent.py
if errorlevel 1 exit /b 1
echo.
echo ========== 3/5 公共区拖入分区 ==========
python scripts\selftest_public_drag.py
if errorlevel 1 exit /b 1
echo.
echo ========== 3b/5 公共区 HWND 合并 ==========
python scripts\selftest_public_host.py
if errorlevel 1 exit /b 1
echo.
echo ========== 3c/5 公共区多选多拖 ==========
python scripts\selftest_public_multiselect.py
if errorlevel 1 exit /b 1
echo.
echo ========== 3d/5 文件夹门户 ==========
python scripts\selftest_portal.py
if errorlevel 1 exit /b 1
echo.
echo ========== 3e/5 分区空白壳菜单 ==========
python scripts\selftest_fence_bg_menu.py
if errorlevel 1 exit /b 1
echo.
echo ========== 3e2/5 分区快捷键与剪贴板 ==========
python scripts\selftest_fence_keys.py
if errorlevel 1 exit /b 1
echo.
echo ========== 3f/5 多显示器 chrome ==========
python scripts\selftest_multimon_chrome.py
if errorlevel 1 exit /b 1
echo.
echo ========== 4/5 系统整体自测 ==========
python scripts\selftest_system.py
if errorlevel 1 exit /b 1
echo.
echo ========== 5/5 打包版活体 ==========
python scripts\selftest_live_e2e.py
if errorlevel 1 exit /b 1
echo.
echo ========== 清理测试残留 ==========
python scripts\cleanup_selftest_artifacts.py
echo 全部测试套件通过
exit /b 0
