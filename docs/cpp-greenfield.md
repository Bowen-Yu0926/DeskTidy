# C++ greenfield (`feat/cpp-greenfield`)

Native Win32 host — feature MVP map vs Python product.

## Migration matrix

| Feature | Native |
|---------|--------|
| Fences / OLE / pins / rules / pages | done |
| Organize / dock / public strip | done |
| Pet + drop→Recycle (+ optional `pet.png`) | done |
| Notes (save / save-as / font / template) | done |
| Snapshots / Peek / shell verbs | done |
| Local DPAPI vault (+ export/import) | done (not cloud ZK) |
| Wallpaper | done |
| **Region screenshot + pin to desktop** | done |
| Fullscreen screenshot | done |
| Record (ffmpeg discovery) | done |
| Search (fd discovery + fallback) | done |
| First-run tip | done |
| Hotkey help + rebinding (`hotkeys.json`) | done |
| Autostart / installer / single-instance | done |
| Cloud vault sync | not ported |
| DeskNote WYSIWYG | not ported |
| Pet asset pack / dissolve anim | not ported |
| Full i18n | not ported |

## Build

```bat
native\build.bat
scripts\run_native.bat
scripts\build_native_installer.bat
```

## Hotkeys (defaults; editable via tray)

| Hotkey | Action |
|--------|--------|
| Ctrl+Shift+S | Settings |
| Ctrl+Shift+N | Notes |
| Ctrl+Shift+F | Toggle fences |
| Ctrl+Space | Peek |
| Ctrl+Alt+1/2 | Page |
| Ctrl+Shift+1 | Region capture |
| Ctrl+Alt+4 | Search |

Config: `%LocalAppData%\DeskTidyNative\hotkeys.json`

Helpers: place `ffmpeg.exe` / `fd.exe` under `assets\ffmpeg\` / `assets\fd\` or PATH. Optional `pet.png` beside exe or in data dir.
