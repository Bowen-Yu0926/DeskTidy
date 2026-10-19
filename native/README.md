# DeskTidy Native (C++ greenfield)

Greenfield rewrite toward a **Fences-class desktop layer**: native Win32
overlay HWNDs owned by the SHELLDLL_DefView host, under normal APPWINDOWs
(including the settings window).

This tree is independent of the Python/PyQt app. Product parity is staged;
MVP scope:

- One dark fence panel (drag move, paint)
- Attach to DefView host (`GWLP_HWNDPARENT`)
- DefView-adjacent Z-order only (never band-top / `HWND_TOP` for overlays)
- Simple settings window (normal APPWINDOW) that must stay above the fence

## Build (MSVC + CMake)

```bat
cd native
build.bat
```

Or manually (x64 Native Tools Command Prompt for VS):

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\desktidy_native.exe
```

## Run

```bat
build\Release\desktidy_native.exe
```

- Fence appears on the desktop surface
- Double-click the tray icon (or press **Ctrl+Shift+S** in MVP) to open Settings
- Settings must remain clickable above the fence
