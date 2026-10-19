# C++ greenfield (`feat/cpp-greenfield`)

Route **A**: rewrite DeskTidy as a native Win32 app with a Fences-class
desktop layer. Python/PyQt remains on `main` until parity; this branch adds
`native/` as the new product root for shell + overlays.

## MVP (`native/`) — stage 1 done

| Piece | Role |
|-------|------|
| `desktop_shell.*` | DefView host lookup, `GWLP_HWNDPARENT` attach, seat / DefView-adjacent raise (**never** overlay `HWND_TOP`) |
| `fence_window.*` | Toolwindow overlay panel (drag, paint) |
| `layout.*` | Multi-fence load/save JSON under LocalAppData |
| `settings_window.*` | APPWINDOW: list / add / remove fences |
| `app.*` / `main.cpp` | Tray + `Ctrl+Shift+S`; seat all fences then raise settings |

## Build

```bat
cd native
build.bat
```

Requires Visual Studio 2022 Build Tools (C++) + CMake.

## Next stages

1. ~~Multiple fences + layout persistence~~  
2. ~~Icon grid (desktop scan; .lnk -> 软件, else -> 常用; click to open)~~  
3. Shell drag (OLE) / pin rules  
4. Page switch / Win+D restore  
5. Port organize rules, dock, pet, notes…  
6. Retire Python host when parity is enough  
