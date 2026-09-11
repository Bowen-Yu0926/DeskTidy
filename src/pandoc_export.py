"""Export Markdown via system Pandoc (optional dependency)."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


def find_pandoc() -> str | None:
    """Return path to pandoc executable, or None if not installed."""
    found = shutil.which("pandoc")
    return found


def export_markdown_to_docx(
    markdown: str,
    dest: Path,
    *,
    resource_dir: Path | None = None,
    pandoc: str | None = None,
) -> None:
    """Convert *markdown* text to ``.docx`` at *dest* using Pandoc.

    Raises ``FileNotFoundError`` if pandoc is missing, ``RuntimeError`` on failure.
    """
    exe = pandoc or find_pandoc()
    if not exe:
        raise FileNotFoundError("pandoc not found")
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        exe,
        "-f",
        "markdown",
        "-t",
        "docx",
        "-o",
        str(dest),
    ]
    if resource_dir is not None:
        cmd.extend(["--resource-path", str(resource_dir)])
    creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        proc = subprocess.run(  # noqa: S603
            cmd,
            input=markdown.encode("utf-8"),
            capture_output=True,
            check=False,
            creationflags=creationflags,
            cwd=str(resource_dir) if resource_dir is not None else None,
        )
    except OSError as exc:
        raise RuntimeError(f"无法启动 Pandoc：{exc}") from exc
    if proc.returncode != 0:
        err = (proc.stderr or b"").decode("utf-8", errors="replace").strip()
        raise RuntimeError(err or f"Pandoc 退出码 {proc.returncode}")


def pandoc_install_hint() -> str:
    return (
        "未检测到 Pandoc。\n\n"
        "请安装后重试：\n"
        "• 官网：https://pandoc.org/installing.html\n"
        "• 或用 winget：winget install --id JohnMacFarlane.Pandoc\n\n"
        "也可改用「导出 HTML / PDF」。"
    )
