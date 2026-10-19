# DeskTidy Native (C++ greenfield)

Fences-class desktop layer: DefView-owned overlays under normal APPWINDOWs.

Python on `main` remains the shipping product until installer cutover. See
`docs/cpp-greenfield.md`.

## Features

- Multi-fence: drag, **resize**, **scroll**, context menus
- Icon grid + OLE drag/drop (multi-file HDROP) + pin store
- Extension rules (`rules.json`) + desktop folder watch
- Pages (Ctrl+Alt+1/2) + Win+D settle
- Organize (virtual pins / physical folders)
- Dock, pet, sticky notes (Ctrl+Shift+N)
- Settings: rename / page / toggles / edit rules

## Build / run

```bat
cd native
build.bat
..\scripts\run_native.bat
..\scripts\selftest_native.bat
```

Data: `%LOCALAPPDATA%\DeskTidyNative\`
