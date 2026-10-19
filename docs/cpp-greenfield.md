# C++ greenfield (`feat/cpp-greenfield`)

Route **A**: rewrite DeskTidy as a native Win32 app with a Fences-class
desktop layer. Python/PyQt remains the full-product host; native is the
desktop-layer cutover track.

## Status

| Capability | Status |
|------------|--------|
| Native desktop layer (fences/OLE/pages/dock/pet/notes) | done |
| Standalone native installer | done (`DeskTidyNative.iss`) |
| Bundled into main `DeskTidy.iss` (task **native**) | done |
| Startup switch scripts | done |
| Retire Python host entirely | **pending** (explicit product decision) |

## Build / run / package

```bat
native\build.bat
scripts\run_native.bat
scripts\selftest_native.bat
scripts\build_native_installer.bat   :: dist\DeskTidyNative_Setup_*.exe
scripts\build_installer.bat          :: also builds native unless DESKTIDY_SKIP_NATIVE=1
```

Cutover helpers:

```bat
scripts\switch_startup_native.bat    :: Startup folder -> desktidy_native.exe
scripts\switch_startup_python.bat    :: Startup folder -> DeskTidy.exe
```

## Installer tasks (main package)

| Task | Meaning |
|------|---------|
| `native` | Ship `desktidy_native.exe` + Start Menu entry (checkedonce) |
| `startup` | Autostart Python/Qt `DeskTidy.exe` |
| `startupnative` | Autostart Native desktop layer |

Do not enable both `startup` and `startupnative` unless you intentionally want both.

## Data

`%LOCALAPPDATA%\DeskTidyNative\` — `layout.json`, `pins.json`, `rules.json`, `notes.txt`

## Hotkeys

| Hotkey | Action |
|--------|--------|
| Ctrl+Shift+S | Settings |
| Ctrl+Shift+N | Sticky notes |
| Ctrl+Alt+1 / 2 | Page switch |

## Remaining to fully retire Python

Vault / screen record / DeskNote full editor / pet asset packs / Explorer
background verbs — still Python-only. Keep `DeskTidy.exe` until those are
ported or consciously dropped.
