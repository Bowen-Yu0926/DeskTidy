"""Remove leftover files created by DeskTidy selftests.

Call after every selftest run (pass or fail) so Videos / project folders
do not accumulate orphan recordings and temp dirs.
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Safe name prefixes / exact stems that selftests create.
_SELFTEST_NAME_PREFIXES = (
    "_selftest",
    "desktidy_selftest",
    "DeskTidy_test",
)
_TEMP_DIR_PREFIXES = (
    "desktidy_rec_rename_",
    "desktidy_msel_",
    "desktidy_dissolve_",
    "desktidy-ffmpeg-",
    "desktidy_rec_test",
)


def _is_selftest_filename(name: str) -> bool:
    stem = Path(name).name
    lower = stem.casefold()
    for prefix in _SELFTEST_NAME_PREFIXES:
        if lower.startswith(prefix.casefold()):
            return True
    if lower.endswith(".ffmpeg.log") and "selftest" in lower:
        return True
    return False


def _recording_dirs() -> list[Path]:
    dirs: list[Path] = []
    for candidate in (
        Path.home() / "Videos" / "DeskTidy",
        ROOT / "录屏",
        ROOT / "dist" / "DeskTidy" / "录屏",
        Path(os.environ.get("LOCALAPPDATA", "") or "") / "Programs" / "DeskTidy" / "录屏",
    ):
        if candidate and candidate.is_dir():
            dirs.append(candidate)
    custom = Path(tempfile.gettempdir()) / "desktidy_rec_test_dir"
    if custom.is_dir():
        dirs.append(custom)
    return dirs


def _explorer_cleanup_roots() -> list[Path]:
    roots: list[Path] = []
    for candidate in (
        ROOT,
        ROOT / "录屏",
        ROOT / "笔记",
        ROOT / "dist" / "DeskTidy",
        Path.home() / "Videos" / "DeskTidy",
    ):
        try:
            if candidate.exists():
                roots.append(candidate.resolve())
        except OSError:
            continue
    return roots


def _folder_under_roots(folder: Path, roots: list[Path]) -> bool:
    try:
        resolved = folder.resolve()
    except OSError:
        return False
    if resolved.is_file():
        resolved = resolved.parent
    for base in roots:
        try:
            resolved.relative_to(base)
            return True
        except ValueError:
            continue
    return False


def close_selftest_explorer_windows(*, verbose: bool = False) -> int:
    """Close Explorer windows that selftests opened under DeskTidy folders."""
    if sys.platform != "win32":
        return 0
    roots = _explorer_cleanup_roots()
    if not roots:
        return 0
    closed = 0
    try:
        from win32com.client import Dispatch

        shell = Dispatch("Shell.Application")
        to_close = []
        for window in shell.Windows():
            try:
                folder = Path(str(window.Document.Folder.Self.Path))
            except Exception:
                continue
            if _folder_under_roots(folder, roots):
                to_close.append(window)
        for window in to_close:
            try:
                path = str(window.Document.Folder.Self.Path)
                window.Quit()
                closed += 1
                if verbose:
                    print(f"  cleanup: closed explorer {path}")
            except Exception:
                pass
    except Exception:
        pass
    return closed


def _unlink(path: Path) -> bool:
    try:
        if path.is_file() or path.is_symlink():
            path.unlink(missing_ok=True)
            return True
        if path.is_dir():
            shutil.rmtree(path, ignore_errors=True)
            return not path.exists()
    except OSError:
        return False
    return False


def cleanup_selftest_artifacts(*, verbose: bool = False) -> int:
    """Delete known selftest leftovers. Returns number of paths removed."""
    removed = 0

    # Explicit one-offs.
    for path in (
        ROOT / "dist" / "selftest_out.txt",
        ROOT / "assets" / "ffmpeg.staging",
        Path(tempfile.gettempdir()) / "desktidy_rec_test_dir",
    ):
        if path.exists() and _unlink(path):
            removed += 1
            if verbose:
                print(f"  cleanup: {path}")

    # Recordings folders: only files matching selftest name patterns.
    for folder in _recording_dirs():
        try:
            children = list(folder.iterdir())
        except OSError:
            continue
        for child in children:
            if child.is_file() and _is_selftest_filename(child.name):
                if _unlink(child):
                    removed += 1
                    if verbose:
                        print(f"  cleanup: {child}")
                log = child.with_suffix(".ffmpeg.log")
                if log.exists() and _unlink(log):
                    removed += 1
            elif child.is_file() and child.name.endswith(".ffmpeg.log"):
                stem = child.name[: -len(".ffmpeg.log")]
                if _is_selftest_filename(stem + ".mp4") and _unlink(child):
                    removed += 1

    # Temp dirs left by mkdtemp(prefix=desktidy_*).
    tmp = Path(tempfile.gettempdir())
    try:
        for child in tmp.iterdir():
            if not child.is_dir():
                continue
            name = child.name
            if any(name.startswith(p) for p in _TEMP_DIR_PREFIXES):
                if _unlink(child):
                    removed += 1
                    if verbose:
                        print(f"  cleanup: {child}")
    except OSError:
        pass

    return removed


def report_cleanup() -> None:
    """Print a one-line cleanup summary (for selftest runners)."""
    try:
        removed = cleanup_selftest_artifacts()
        closed = close_selftest_explorer_windows()
        parts: list[str] = []
        if removed:
            parts.append(f"文件 {removed} 项")
        if closed:
            parts.append(f"资源管理器窗口 {closed} 个")
        if parts:
            print(f"已清理测试残留：{', '.join(parts)}")
    except Exception as exc:  # noqa: BLE001
        print(f"测试残留清理失败: {exc}")


def main() -> int:
    removed = cleanup_selftest_artifacts(verbose=True)
    closed = close_selftest_explorer_windows(verbose=True)
    print(f"Removed {removed} selftest artifact(s), closed {closed} explorer window(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
