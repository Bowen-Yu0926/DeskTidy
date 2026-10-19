# C++ greenfield (`feat/cpp-greenfield`)

Route **A**: rewrite DeskTidy as a native Win32 app with a Fences-class
desktop layer. Python/PyQt on `main` remains the shipping product until
installer cutover; `native/` is the new host.

## Status

| Capability | Status |
|------------|--------|
| DefView desktop band (never overlay `HWND_TOP`) | done |
| Multi-fence + resize + scroll + context menus | done |
| Icon grid + OLE drag/drop + multi-file HDROP | done |
| Pin store + extension rules (`rules.json`) | done |
| Pages + page bar + Win+D settle timer | done |
| Desktop folder watch auto-refresh | done |
| Organize virtual/physical | done |
| Dock / pet / sticky notes | done |
| Settings manager (rename/page/toggles/rules) | done |
| Smoke: `scripts\selftest_native.bat` | done |
| Installer primary = native / retire Python | **pending cutover** |

## Build / run

```bat
cd native
build.bat
scripts\run_native.bat
scripts\selftest_native.bat
```

Data: `%LOCALAPPDATA%\DeskTidyNative\` (`layout.json`, `pins.json`, `rules.json`, `notes.txt`).

## Hotkeys

| Hotkey | Action |
|--------|--------|
| Ctrl+Shift+S | Settings |
| Ctrl+Shift+N | Sticky notes |
| Ctrl+Alt+1 / 2 | Page switch |

## Cutover (retire Python)

1. Daily-driver QA on Win10/11 (fences/OLE/pages/Win+D)  
2. Point Inno `[Icons]` / startup at `desktidy_native.exe` (keep Python as fallback optional)  
3. Port remaining product chrome only if still required (vault, recording, full pet assets…)  
4. Then remove Python host from the default install path  

Until then: ship Python from `main`; grow native on this branch.
