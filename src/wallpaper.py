"""Change the Windows desktop wallpaper from DeskTidy.

Wallpaper is only applied when the user explicitly picks or randomizes an
image in「扩展功能」. Configuring an empty library folder does nothing.
Self-tests must pass ``apply=False`` so they never overwrite the user's desktop.
"""

from __future__ import annotations

import ctypes
import random
import shutil
import time
from pathlib import Path

IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp"}
IMAGE_FILTER = "图片文件 (*.jpg *.jpeg *.png *.bmp);;所有文件 (*.*)"

SPI_SETDESKWALLPAPER = 0x0014
SPIF_UPDATEINIFILE = 0x01
SPIF_SENDWININICHANGE = 0x02


def wallpaper_settings(settings: dict) -> dict:
    raw = settings.get("wallpaper")
    if not isinstance(raw, dict):
        raw = {}
        settings["wallpaper"] = raw
    raw.setdefault("library_folder", "")
    return raw


def get_library_folder(settings: dict) -> Path | None:
    cfg = wallpaper_settings(settings)
    folder = str(cfg.get("library_folder") or "").strip()
    if not folder:
        return None
    return Path(folder)


def list_library_images(folder: Path) -> list[Path]:
    if not folder.is_dir():
        return []
    images: list[Path] = []
    try:
        for entry in folder.iterdir():
            if entry.is_file() and entry.suffix.lower() in IMAGE_SUFFIXES:
                images.append(entry)
    except OSError:
        return []
    images.sort(key=lambda p: p.name.casefold())
    return images


def get_current_wallpaper_path() -> Path | None:
    """Return the path Windows currently has configured as wallpaper, if any."""
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER, r"Control Panel\Desktop"
        ) as key:
            value, _ = winreg.QueryValueEx(key, "WallPaper")
    except OSError:
        return None
    text = str(value or "").strip()
    if not text:
        return None
    path = Path(text)
    return path if path.is_file() else Path(text)


def _wallpapers_cache_dir() -> Path:
    root = Path.home() / ".desktidy" / "wallpapers"
    root.mkdir(parents=True, exist_ok=True)
    return root


def _backup_path_file() -> Path:
    return _wallpapers_cache_dir() / "previous_wallpaper.txt"


def remember_system_wallpaper() -> Path | None:
    """Remember the system wallpaper before DeskTidy changes it (once)."""
    current = get_current_wallpaper_path()
    if current is None:
        return None
    # Do not treat our own staged file as the user's original.
    try:
        if current.resolve().parent == _wallpapers_cache_dir().resolve():
            return None
    except OSError:
        pass
    marker = _backup_path_file()
    if marker.is_file():
        return None
    try:
        marker.write_text(str(current), encoding="utf-8")
    except OSError:
        return None
    return current


def restore_remembered_wallpaper() -> Path | None:
    """Restore the wallpaper that was active before DeskTidy first changed it."""
    marker = _backup_path_file()
    if not marker.is_file():
        return None
    try:
        text = marker.read_text(encoding="utf-8").strip()
    except OSError:
        return None
    if not text:
        return None
    path = Path(text)
    if not path.is_file():
        return None
    return set_desktop_wallpaper(path, apply=True, stage=False, remember=False)


def _stage_wallpaper(image_path: Path) -> Path:
    """Copy into ~/.desktidy/wallpapers so removable/network paths keep working."""
    src = image_path.resolve()
    if not src.is_file():
        raise ValueError(f"图片不存在：{src}")
    if src.suffix.lower() not in IMAGE_SUFFIXES:
        raise ValueError("请选择 JPG / PNG / BMP 图片。")
    dest = _wallpapers_cache_dir() / f"current{src.suffix.lower()}"
    if src != dest:
        shutil.copy2(src, dest)
    return dest


def _set_wallpaper_style_fill() -> None:
    """Prefer 'Fill' so images cover the desktop on modern Windows."""
    try:
        import winreg

        with winreg.OpenKey(
            winreg.HKEY_CURRENT_USER,
            r"Control Panel\Desktop",
            0,
            winreg.KEY_SET_VALUE,
        ) as key:
            winreg.SetValueEx(key, "WallpaperStyle", 0, winreg.REG_SZ, "10")
            winreg.SetValueEx(key, "TileWallpaper", 0, winreg.REG_SZ, "0")
    except OSError:
        pass


def set_desktop_wallpaper(
    image_path: str | Path,
    *,
    apply: bool = True,
    stage: bool = True,
    remember: bool = True,
) -> Path:
    """Apply ``image_path`` as the desktop wallpaper; return the used path.

    ``apply=False`` stages (or validates) without calling SystemParametersInfo —
    used by automated tests so the user's desktop is never overwritten.
    ``stage=False`` applies the given path in place (restore of a prior file).
    """
    src = Path(image_path)
    if not src.is_file():
        raise ValueError(f"图片不存在：{src}")
    if src.suffix.lower() not in IMAGE_SUFFIXES:
        raise ValueError("请选择 JPG / PNG / BMP 图片。")

    if remember and apply:
        remember_system_wallpaper()

    target = _stage_wallpaper(src) if stage else src.resolve()
    if not apply:
        return target

    _set_wallpaper_style_fill()
    ok = ctypes.windll.user32.SystemParametersInfoW(
        SPI_SETDESKWALLPAPER,
        0,
        str(target),
        SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE,
    )
    if not ok:
        raise OSError("SystemParametersInfo 设置壁纸失败。")
    try:
        target.touch()
    except OSError:
        pass
    return target


def pick_and_set_wallpaper(parent, settings: dict) -> Path | None:
    """Show an open-file dialog and apply the chosen image. Return path or None."""
    from PyQt6.QtWidgets import QFileDialog

    cfg = wallpaper_settings(settings)
    # Empty library is fine — start from home; never auto-apply anything.
    start = str(cfg.get("library_folder") or "").strip() or str(Path.home())
    path, _ = QFileDialog.getOpenFileName(
        parent,
        "选择桌面壁纸",
        start,
        IMAGE_FILTER,
    )
    if not path:
        return None
    applied = set_desktop_wallpaper(path)
    cfg["last_image"] = str(Path(path).resolve())
    cfg["last_applied_at"] = time.time()
    return applied


def set_random_wallpaper_from_library(settings: dict, *, apply: bool = True) -> Path:
    """Pick a random image from the configured library folder."""
    folder = get_library_folder(settings)
    if folder is None:
        raise ValueError("请先在「扩展功能」中配置壁纸库文件夹。")
    if not folder.is_dir():
        raise ValueError(f"壁纸库文件夹不可用：{folder}")
    images = list_library_images(folder)
    if not images:
        raise ValueError("壁纸库中没有可用的图片（支持 JPG / PNG / BMP）。")
    chosen = random.choice(images)
    applied = set_desktop_wallpaper(chosen, apply=apply)
    cfg = wallpaper_settings(settings)
    cfg["last_image"] = str(chosen.resolve())
    cfg["last_applied_at"] = time.time()
    return applied


def open_library_folder(settings: dict) -> Path:
    folder = get_library_folder(settings)
    if folder is None:
        raise ValueError("请先在「扩展功能」中配置壁纸库文件夹。")
    folder.mkdir(parents=True, exist_ok=True)
    if not folder.is_dir():
        raise ValueError(f"壁纸库文件夹不可用：{folder}")
    from src.win_shell import open_path

    open_path(folder)
    return folder
