@echo off
echo Building DeskTidy...

rem build_installer.bat already stopped the app — skip a second wait.
if /I not "%DESKTIDY_ALREADY_STOPPED%"=="1" (
    call "%~dp0stop_desktidy.bat"
)

rem Daily fast builds set DESKTIDY_SKIP_PIP=1 to avoid ~5–15s pip churn.
if /I "%DESKTIDY_SKIP_PIP%"=="1" (
    echo Skipping pip install ^(DESKTIDY_SKIP_PIP=1^).
) else (
    pip install pyinstaller -q
    pip install -r requirements.txt -q
)

pyinstaller DeskTidy.spec --noconfirm --distpath dist_staging
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

rem Sync staging -> dist without deleting the live folder first (avoids file locks).
if not exist dist_staging\DeskTidy\DeskTidy.exe (
    echo Build failed: missing dist_staging\DeskTidy\DeskTidy.exe
    exit /b 1
)
echo Pruning WebEngine debug / unused Qt Quick3D / translations / Cython from staging...
python scripts\prune_release_payload.py dist_staging\DeskTidy
if errorlevel 1 (
    echo Build failed: prune_release_payload on staging.
    exit /b 1
)
if not exist dist mkdir dist
robocopy dist_staging\DeskTidy dist\DeskTidy /MIR /NFL /NDL /NJH /NJS /nc /ns /np >nul
if errorlevel 8 (
    echo Build failed: could not sync dist_staging to dist\DeskTidy
    exit /b 1
)

rem Drop legacy onefile next to the onedir folder so boot scripts cannot pick it.
if exist dist\DeskTidy.exe del /f /q dist\DeskTidy.exe

set "DIST_EXE=dist\DeskTidy\DeskTidy.exe"
if not exist "%DIST_EXE%" (
    echo Build failed: missing %DIST_EXE%
    exit /b 1
)

if exist assets\ffmpeg\ffmpeg.exe (
    if not exist dist\DeskTidy\assets\ffmpeg mkdir dist\DeskTidy\assets\ffmpeg
    xcopy /Y /Q assets\ffmpeg\* dist\DeskTidy\assets\ffmpeg\ >nul
    if errorlevel 1 (
        echo WARNING: could not refresh dist\DeskTidy\assets\ffmpeg ^(files in use^).
    )
) else (
    echo WARNING: assets\ffmpeg\ffmpeg.exe missing - run scripts\prepare_ffmpeg_recording.py
)
if exist assets\fd\fd.exe (
    if not exist dist\DeskTidy\assets\fd mkdir dist\DeskTidy\assets\fd
    copy /Y assets\fd\fd.exe dist\DeskTidy\assets\fd\fd.exe >nul
    if errorlevel 1 (
        echo WARNING: could not refresh dist\DeskTidy\assets\fd\fd.exe ^(file in use^).
    )
) else (
    echo WARNING: assets\fd\fd.exe missing - file search will not work.
)

echo.
echo Build successful: %DIST_EXE%
exit /b 0
