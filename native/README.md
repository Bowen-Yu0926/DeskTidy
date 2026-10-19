# DeskTidy Native (C++ greenfield)

Fences-class desktop layer + product feature MVP map. See
`docs/cpp-greenfield.md` for the migration matrix.

## Highlights

- Fences / pages / OLE pins / rules / organize
- Dock · public strip · pet (drop→Recycle) · notes · todos
- Snapshots · Peek · shell verbs
- Search · local DPAPI vault · wallpaper · screenshot · ffmpeg record
- Meeting minutes · update link · autostart · installer

## Build / run

```bat
cd native
build.bat
..\scripts\run_native.bat
..\scripts\selftest_native.bat
..\scripts\build_native_installer.bat
```

Data: `%LOCALAPPDATA%\DeskTidyNative\`
