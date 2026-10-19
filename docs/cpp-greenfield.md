# C++ greenfield (`feat/cpp-greenfield`)

Route **A**: rewrite DeskTidy as a native Win32 app with a Fences-class
desktop layer. Python/PyQt on `main` remains the full-product install until
you deliberately cut over.

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
| Settings + autostart + single-instance | done |
| Native standalone installer | done (`DeskTidyNative.iss`) |
| Retire Python from default product install | **pending** (manual cutover) |

## Build / run

```bat
cd native
build.bat
..\scripts\run_native.bat
..\scripts\selftest_native.bat
..\scripts\build_native_installer.bat
```

Outputs:

- `native\build\Release\desktidy_native.exe`
- `dist\DeskTidyNative_Setup_<version>.exe` (native-only package)

Data: `%LOCALAPPDATA%\DeskTidyNative\`

## Hotkeys

| Hotkey | Action |
|--------|--------|
| Ctrl+Shift+S | Settings |
| Ctrl+Shift+N | Sticky notes |
| Ctrl+Alt+1 / 2 | Page switch |

## Cutover notes

1. Ship/test `DeskTidyNative_Setup_*.exe` beside the Python installer.  
2. When daily use is OK, change product shortcuts/startup to native.  
3. Keep Python package only for vault/recording/DeskNote until those are ported or dropped.  
4. Do **not** delete the Python host from `main` until that decision is explicit.
