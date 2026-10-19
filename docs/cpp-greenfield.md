# C++ greenfield (`feat/cpp-greenfield`)

Route **A**: rewrite DeskTidy as a native Win32 app with a Fences-class
desktop layer. Python/PyQt remains on `main` until parity; this branch adds
`native/` as the new product root for shell + overlays.

## MVP (`native/`)

| Piece | Role |
|-------|------|
| `desktop_shell.*` | DefView host lookup, `GWLP_HWNDPARENT` attach, seat / DefView-adjacent raise (**never** overlay `HWND_TOP`) |
| `fence_window.*` | Toolwindow overlay panel (drag, paint) |
| `settings_window.*` | Normal `WS_EX_APPWINDOW` management UI |
| `app.*` / `main.cpp` | Tray + `Ctrl+Shift+S` → settings; seat fence then raise settings |

## Build

Requires Visual Studio 2022 (C++ desktop) + CMake. See `native/README.md` and
`native/build.bat`.

This agent environment may not have MSVC installed — build on a VS machine.

## Next stages (not in MVP)

1. Multiple fences + layout persistence  
2. Icon grid / shell drag (OLE)  
3. Page switch / Win+D restore  
4. Port organize rules, dock, pet, notes…  
5. Retire Python host when parity is enough  
