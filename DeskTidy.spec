# -*- mode: python ; coding: utf-8 -*-
# Onedir (not onefile): boot autostart must not extract Qt/Python into %TEMP%\_MEI*
# on every login. Fences-class tray tools ship as a folder next to the exe.
# DeskTidy.exe + deskNote.exe share one COLLECT via MERGE.

block_cipher = None

_datas = [
    ('config/default_settings.json', 'config'),
    ('assets/app_icon.ico', 'assets'),
    ('assets/desknote_icon.ico', 'assets'),
    ('assets/pets', 'assets/pets'),
    ('assets/md_preview', 'assets/md_preview'),
    ('docs/desktidy.html', 'docs'),
    ('docs/desknote.html', 'docs'),
]
_hidden = [
    'win32timezone',
    'win32api',
    'win32gui',
    'win32con',
    'pywintypes',
    'pythoncom',
    'watchdog',
    'watchdog.observers',
    'watchdog.observers.winapi',
    'watchdog.events',
    'send2trash',
    'send2trash.win',
    'send2trash.win.modern',
    'docx',
    'markdown',
    'PyQt6.QtWebEngineWidgets',
    'PyQt6.QtWebEngineCore',
    'PyQt6.QtWebChannel',
    'src.uninstall_cleanup',
]
_rthooks = ['packaging/rthooks/pyi_rth_desktidy.py']

a = Analysis(
    ['main.py'],
    pathex=[],
    binaries=[],
    datas=_datas,
    hiddenimports=_hidden,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=_rthooks,
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

a_dn = Analysis(
    ['desknote_main.py'],
    pathex=[],
    binaries=[],
    datas=_datas,
    hiddenimports=_hidden,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=_rthooks,
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

MERGE(
    (a, 'DeskTidy', 'DeskTidy'),
    (a_dn, 'deskNote', 'deskNote'),
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)
pyz_dn = PYZ(a_dn.pure, a_dn.zipped_data, cipher=block_cipher)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='DeskTidy',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon='assets/app_icon.ico',
)

exe_dn = EXE(
    pyz_dn,
    a_dn.scripts,
    [],
    exclude_binaries=True,
    name='deskNote',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon='assets/desknote_icon.ico',
)

coll = COLLECT(
    exe,
    exe_dn,
    a.binaries,
    a.zipfiles,
    a.datas,
    a_dn.binaries,
    a_dn.zipfiles,
    a_dn.datas,
    strip=False,
    upx=False,
    upx_exclude=[],
    name='DeskTidy',
)
