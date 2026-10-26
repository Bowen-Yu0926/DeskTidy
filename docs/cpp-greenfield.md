# C++ greenfield (`feat/cpp-greenfield`)

Native Win32 host — feature MVP map vs Python product.

## Migration matrix

| Feature | Native |
|---------|--------|
| Fences / OLE / pins / rules / pages | done |
| Fence style presets (RMB) | done |
| Organize / whitelist (virtual-only) | done |
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
| Organize virtual preview | done |
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
| Hotkeys: record / fullscreen / organize / vault / todos | done |
| Snapshot preview before apply/delete | done |
| Rules filename globs (`Invoice*` / `name:…`) | done |
| Calculator open-or-focus | done |
| Shell file clipboard (copy/cut CF_HDROP) | done |
| Product toast tip | done |
| Multi-step first-run guide | done |
| Page bar quick tools (notes/search/shot) | done |
| Settings organize preview list | done |
| Fence paste from shell clipboard | done |
| Help center (local HTML) | done |
| Minutes RTF + recent list | done |
| Native uninstall cleanup | done |
| Organize kind rules (kind:image/doc/…) | done |
| Update check 6h cache | done |
| Vault local note field | done |
| Display change layout heal | done |
| Record pause / resume on badge | done |
| Vault copy username | done |
| Shell verbs + CLI (notes/search/shot/todos/peek) | done |
| Todos clear completed | done |
| Copy path (fence icon + tray data dir) | done |
| Fence icon rename / recycle / properties | done |
| Dock / public / search icon shell menu | done |
| Fence multi-select + cut ghost | done |
| Fence multi-drag, scroll thumb, title rename | done |
| Multi-page bar (+/-) | done |
| Portal fences (folder mirror) | done |
| Layout editor canvas | done |
| Fence rubber-band + keyboard (Del/F2/Ctrl+C/X/V/A) | done |
| App theme cycle (fence styles) | done |
| Update download/run installer | done |
| Pet climb/ledge + buoy bar | done |
| Notes toast / MD export / pandoc / HTML paste / open-with | done |
| DeskNote find next/prev + HTML export | done |
| Shimeji window ledges (auto climb, fall off edge) | done |
| Second-instance CLI forward | done |
| Rename current page (double-click page bar) | done |
| DeskNote Toast UI WebView2 WYSIWYG | done |
| Settings page list follows page names | done |
| Layout editor shows every page | done |
| Portal folder change refresh | done |
| Record quality + microphone name | done |
| Pet page bubbles | done |
| DeskNote outline / focus / typewriter | done |
| Theme colors on page bar + settings | done |
| DeskNote multi-tabs | done |
| Pet grab / throw physics | done |

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
| Ctrl+Shift+2 | Fullscreen capture |
| Ctrl+Shift+R | Record toggle |
| Ctrl+Shift+O | Virtual organize |
| Ctrl+Shift+V | Vault |
| Ctrl+Shift+T | Todos |
| Ctrl+Alt+4 | Search |

Config under `%LocalAppData%\DeskTidyNative\`: `layout.json`, `hotkeys.json`, `whitelist.json`, `wallpaper.json`, `minutes.json`, `todos.txt`, `app_prefs.json`, `snapshots\`.

Helpers: `ffmpeg.exe` / `fd.exe` under assets or PATH. Pet sprites: `assets/pets/` (Python-compatible `hoodie_*.png` / `voyage_*.png`; build copies them next to the exe). Optional single `pet.png` fallback beside exe or in data dir.
