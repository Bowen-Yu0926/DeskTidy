# C++ greenfield (`feat/cpp-greenfield`)

Native Win32 host with Fences-class desktop layer. Feature surface below is
**MVP-complete** relative to the Python product map (not pixel/behavior parity).

## Migration matrix

| Python feature | Native status |
|----------------|---------------|
| Desktop fences + OLE pin | done |
| Pages / Win+D settle | done |
| Organize (virtual/physical) | done |
| Extension rules | done |
| Dock | done |
| Sticky notes | done |
| Pet (drawn) + drop→Recycle | done |
| Public icon strip | done |
| Layout snapshots | done |
| Peek / toggle fences | done |
| Todos | done |
| File search | done |
| Local vault (DPAPI) | done (not cloud ZK) |
| Wallpaper picker | done |
| Screenshot | done (fullscreen MVP) |
| Screen record | done (needs local ffmpeg) |
| Meeting minutes | done (notepad md) |
| Shell background verbs | done |
| Update check | done (open Releases) |
| Autostart / single-instance / installer | done |
| Cloud vault / byethost sync | **not ported** (local DPAPI instead) |
| Full DeskNote editor / markdown WYSIWYG | **not ported** (notes + notepad) |
| Pet asset pack / trash dissolve anim | **not ported** (drawn pet) |
| Region screenshot / pinned image | **not ported** (fullscreen shot) |
| fd-powered search / ffmpeg auto-fetch | **not ported** |
| Full hotkey settings UI / i18n / first-run | **not ported** |

## Build

```bat
native\build.bat
scripts\run_native.bat
scripts\build_native_installer.bat
```

## Hotkeys

| Hotkey | Action |
|--------|--------|
| Ctrl+Shift+S | Settings |
| Ctrl+Shift+N | Notes |
| Ctrl+Shift+F | Toggle fences |
| Ctrl+Space | Peek |
| Ctrl+Alt+1/2 | Page |
| Ctrl+Shift+1 | Screenshot |
| Ctrl+Alt+4 | Search |

Data: `%LOCALAPPDATA%\DeskTidyNative\`
