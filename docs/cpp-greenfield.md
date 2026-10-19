# C++ greenfield (`feat/cpp-greenfield`)

Native Win32 host — feature MVP map vs Python product.

## Migration matrix

| Feature | Native |
|---------|--------|
| Fences / OLE / pins / rules / pages | done |
| Fence style presets (RMB) | done |
| Organize / whitelist / confirm physical | done |
| Dock / public strip | done |
| Pet + drop→Recycle (+ optional `pet.png`) | done |
| Notes (save / save-as / font / template) | done |
| Todo checklist (`[x]` / `[ ]`) | done |
| Snapshots list / apply / delete | done |
| Peek / shell verbs | done |
| Local DPAPI vault (+ export/import) | done (not cloud ZK) |
| Wallpaper pick / library random / restore | done |
| Meeting minutes (dated `.md` + folder) | done |
| Region screenshot + pin (virtual screen) | done |
| Fullscreen screenshot | done |
| Record (ffmpeg discovery) | done |
| Search (fd + open/reveal/Enter) | done |
| Multi-monitor work-area clamp | done (basic) |
| First-run tip | done |
| Hotkey help + rebinding (`hotkeys.json`) | done |
| Autostart / installer / single-instance | done |
| Cloud vault sync (login + enc_ver=0 REST) | done |
| DeskNote RichEdit (RTF bold/italic/underline) | done |
| Pet PNG frame pack + dissolve | done |
| UI language zh/en (`ui_lang.txt`) | done |
| Per-display layout profiles | done |
| Calculator / page folders / help | done |
| Record pick monitor | done |
| Snapshot + shell icon positions | done |
| Organize physical preview | done |
| Vault ZK (enc_ver=1 PBKDF2+AES-GCM) | done |
| Notes library (multi file under notes\) | done |
| Pet pose packs (idle/walk/sleep RMB) | done |
| Settings tabs (分区/整理/工具) | done |
| Markdown preview (browser HTML) | done |
| Pet characters + walk/fall physics | done |
| Auto-organize watch + startup pin | done |
| Organize suppress (unpin grace) | done |
| Record toggle + badge + duration/dir | done |
| Drag-to-create fence region | done |
| GitHub Releases version check | done |
| Notes find / replace | done |
| Region capture click-to-window | done |
| Notes library text search | done |
| Vault password generate + clipboard clear | done |
| DeskNote Toast UI / WebView editor | not ported |
| Full Shimeji climb/ledge physics | not ported |

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

Config under `%LocalAppData%\DeskTidyNative\`: `layout.json`, `hotkeys.json`, `whitelist.json`, `wallpaper.json`, `minutes.json`, `todos.txt`, `app_prefs.json`, `snapshots\`.

Helpers: `ffmpeg.exe` / `fd.exe` under assets or PATH. Optional `pet.png` beside exe or in data dir.
