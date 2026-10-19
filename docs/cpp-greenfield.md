# C++ greenfield (`feat/cpp-greenfield`)

Route **A**: rewrite DeskTidy as a native Win32 app with a Fences-class
desktop layer. Python/PyQt remains on `main` until a product cutover; this
branch’s `native/` is the new shell + overlays host.

## Native stages

| Stage | Status | Notes |
|-------|--------|-------|
| Desktop shell attach | done | DefView host, never overlay `HWND_TOP` |
| Multi-fence + layout | done | LocalAppData `layout.json` |
| Icon grid | done | Desktop scan; click to open |
| OLE drag / pin rules | done | `DoDragDrop` + `IDropTarget`; `pins.json` |
| Page switch / Win+D | done | Ctrl+Alt+1/2, page bar, settle timer |
| Organize / dock / pet / notes | done (MVP) | Virtual+physical organize; dock; drawn pet; notes window |
| Retire Python host | **deferred** | Keep shipping Python on `main` until parity/QA gate |

## Build

```bat
cd native
build.bat
```

Requires Visual Studio 2022 Build Tools (C++) + CMake.

## Runtime map (`native/src`)

| Module | Role |
|--------|------|
| `desktop_shell.*` | DefView attach / band seat / DefView-adjacent raise |
| `fence_window.*` | Fence panel, icon grid, OLE drop, icon drag-out |
| `ole_drag.*` | Shell `IDataObject` drag + `PathDropTarget` |
| `pin_store.*` | Path → fence pin persistence |
| `layout.*` | Fences + `current_page` / dock / pet flags |
| `page_bar.*` | Page 1/2 switcher in desktop band |
| `organize.*` | Virtual pins + move into `Desktop\DeskTidy\…` |
| `dock_window.*` | Bottom shortcut dock |
| `pet_window.*` | Desktop-band pet sprite (drawn MVP) |
| `notes_window.*` | Simple notes (`notes.txt`) |
| `settings_window.*` | APPWINDOW manager (stays above fences) |
| `app.*` / `main.cpp` | Tray, hotkeys, Win+D settle timer |

## Hotkeys / tray

- `Ctrl+Shift+S` — settings  
- `Ctrl+Alt+1` / `Ctrl+Alt+2` — page switch  
- Tray: notes, organize, toggle dock/pet, refresh, restore chrome  

## Retire Python (cutover checklist)

Do **not** delete the Python host until all are true:

1. Native covers fences + pages + OLE pin for daily use  
2. Organize / dock / pet / notes meet product bar (assets, rules UI)  
3. Installer ships `desktidy_native.exe` as primary entry  
4. Selftests / smoke QA signed off on Win10/11  

Until then: `main` = Python product; `feat/cpp-greenfield` = native growth branch.
