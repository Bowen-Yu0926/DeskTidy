# DeskTidy Native (C++ greenfield)

Greenfield rewrite toward a **Fences-class desktop layer**: native Win32
overlay HWNDs owned by the SHELLDLL_DefView host, under normal APPWINDOWs
(including the settings window).

Python/PyQt on `main` remains the shipping product until cutover. See
`docs/cpp-greenfield.md`.

## Features (native MVP)

- Multi-fence panels + LocalAppData layout / pins
- Desktop icon grid (pin rules; default `.lnk` → 软件)
- OLE drag-out / drop-in (shell `IDataObject` + `IDropTarget`)
- Page switch (Ctrl+Alt+1/2 + page bar) + Win+D settle timer
- Organize (virtual pins / move into `Desktop\DeskTidy`)
- Dock, pet sprite, notes window
- Settings APPWINDOW stays above desktop-band overlays

## Build (MSVC + CMake)

```bat
cd native
build.bat
```

## Run

```bat
build\Release\desktidy_native.exe
```

- Tray / **Ctrl+Shift+S** — settings  
- **Ctrl+Alt+1/2** — pages  
- Drag icons between fences or to Explorer; drop files onto a fence to pin  
