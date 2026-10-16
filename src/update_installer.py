"""Download, verify and launch a DeskTidy release installer.

Pure logic — no Qt imports, no third-party dependencies. Network access goes
through :mod:`urllib.request` (stdlib) and is restricted to a strict host
allow-list so a malicious release feed cannot make DeskTidy fetch an installer
from an arbitrary server.

Flow (driven by the caller / UI layer):

    asset = select_installer_asset(release)          # {"name","url","size"} | None
    checksum = find_checksum(release, asset["name"]) # sha256 hex | None
    path = download_asset(asset["url"], dest_dir)    # .part -> atomic rename
    if checksum and not verify_sha256(path, checksum):
        raise UpdateInstallError("...", stage="verify")
    launch_installer(path)                            # Inno Setup silent args

Every entry point that can fail raises :class:`UpdateInstallError` with a
``stage`` of ``download`` / ``verify`` / ``launch`` so the caller can render a
specific, actionable message.
"""

from __future__ import annotations

import hashlib
import re
import subprocess
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Callable

_USER_AGENT = "DeskTidy-Updater"
_CHUNK_SIZE = 64 * 1024
_INSTALLER_PREFIX = "DeskTidy_Setup_"
_FALLBACK_FILENAME = "DeskTidy_Setup_download.exe"

# Only these hosts may ever be contacted for installer bytes. ``github.com``
# serves the release page / asset link, ``objects.githubusercontent.com`` and
# ``*.githubusercontent.com`` are the redirect targets for signed release assets.
_ALLOWED_HOSTS_EXACT = frozenset({"github.com", "objects.githubusercontent.com"})
_ALLOWED_HOST_SUFFIX = ".githubusercontent.com"

_HEX64_RE = re.compile(r"\b[0-9a-fA-F]{64}\b")


class UpdateInstallError(Exception):
    """Raised when a download / verify / launch step fails.

    ``stage`` is one of ``download`` / ``verify`` / ``launch``.
    """

    def __init__(self, message: str, *, stage: str = "download") -> None:
        super().__init__(message)
        self.stage = stage


def _validate_url(url: str) -> str:
    """Return *url* unchanged when it is an https URL on an allowed host.

    Raises :class:`UpdateInstallError` (``stage="download"``) otherwise.
    """
    text = str(url or "").strip()
    try:
        parsed = urllib.parse.urlsplit(text)
    except ValueError as exc:
        raise UpdateInstallError("无效的下载链接", stage="download") from exc
    if parsed.scheme.lower() != "https":
        raise UpdateInstallError("仅接受 https 下载链接", stage="download")
    host = (parsed.hostname or "").lower()
    if not host:
        raise UpdateInstallError("下载链接缺少主机名", stage="download")
    if host in _ALLOWED_HOSTS_EXACT or host.endswith(_ALLOWED_HOST_SUFFIX):
        return text
    raise UpdateInstallError(f"拒绝从非白名单主机下载：{host}", stage="download")


def select_installer_asset(
    release: dict,
    *,
    name_prefix: str = _INSTALLER_PREFIX,
) -> dict | None:
    """Pick the Windows installer asset from a GitHub release JSON.

    Returns ``{"name": str, "url": str, "size": int}`` for the best match, or
    ``None`` when no ``.exe`` asset whose name starts with *name_prefix* exists.
    Among several matches the largest asset wins (real exe beats a stub).
    """
    if not isinstance(release, dict):
        return None
    assets = release.get("assets")
    if not isinstance(assets, list):
        return None
    best: dict | None = None
    for item in assets:
        if not isinstance(item, dict):
            continue
        name = str(item.get("name") or "")
        url = str(item.get("browser_download_url") or "")
        if not name or not url:
            continue
        if not name.lower().endswith(".exe"):
            continue
        if not name.startswith(str(name_prefix)):
            continue
        try:
            size = int(item.get("size") or 0)
        except (TypeError, ValueError):
            size = 0
        if size < 0:
            size = 0
        if best is None or size > int(best["size"]):
            best = {"name": name, "url": url, "size": size}
    return best


def _extract_sha256_from_text(text: str, asset_name: str = "") -> str | None:
    """Find a 64-hex sha256 in *text*, preferring the line/block of *asset_name*."""
    content = text or ""
    if asset_name:
        for match in re.finditer(re.escape(asset_name), content):
            window = content[match.end() : match.end() + 512]
            found = _HEX64_RE.search(window)
            if found:
                return found.group(0).lower()
    found = _HEX64_RE.search(content)
    return found.group(0).lower() if found else None


def _fetch_text(url: str, *, timeout: float = 15.0) -> str:
    """GET *url* and return its decoded text (allowed hosts only)."""
    _validate_url(url)
    req = urllib.request.Request(
        url,
        headers={"Accept": "text/plain", "User-Agent": _USER_AGENT},
        method="GET",
    )
    with _open(req, timeout) as resp:
        raw = resp.read()
    return raw.decode("utf-8", errors="replace")


def find_checksum(release: dict, asset_name: str) -> str | None:
    """Extract the sha256 for *asset_name* from a release, or return None.

    Lookup order: (1) the release ``body`` text near the asset name, then (2) a
    sidecar ``<asset>.sha256`` asset whose content is fetched and parsed. No
    match (or a failed fetch) yields ``None`` — the caller decides whether an
    unverifiable installer may still be installed.
    """
    if not isinstance(release, dict):
        return None
    name = str(asset_name or "")
    body = str(release.get("body") or "")
    found = _extract_sha256_from_text(body, name)
    if found:
        return found

    assets = release.get("assets")
    if not isinstance(assets, list):
        return None
    sidecar_names = {f"{name}.sha256", f"{name}.sha256sum", f"{name}.sha256.txt"}
    for item in assets:
        if not isinstance(item, dict):
            continue
        asset_name_candidate = str(item.get("name") or "")
        if asset_name_candidate not in sidecar_names:
            continue
        url = str(item.get("browser_download_url") or "")
        if not url:
            continue
        try:
            content = _fetch_text(url)
        except Exception:
            continue
        token = _extract_sha256_from_text(content)
        if token:
            return token
    return None


class _ValidatingRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Re-check the allow-list on every redirect hop (not just the first URL)."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        _validate_url(newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


_opener: urllib.request.OpenerDirector | None = None


def _get_opener() -> urllib.request.OpenerDirector:
    global _opener
    if _opener is None:
        _opener = urllib.request.build_opener(_ValidatingRedirectHandler())
    return _opener


def _open(req: urllib.request.Request, timeout: float):
    """Open a request through the validating opener (monkeypatchable in tests)."""
    return _get_opener().open(req, timeout=timeout)


def _filename_from_url(url: str) -> str:
    path = urllib.parse.urlsplit(url).path
    last = path.rstrip("/").rsplit("/", 1)[-1] if path.rstrip("/") else ""
    return urllib.parse.unquote(last) or _FALLBACK_FILENAME


def _content_length(resp) -> int:
    try:
        headers = getattr(resp, "headers", None)
        if headers is not None:
            value = headers.get("Content-Length")
            if value is not None:
                return int(value)
    except Exception:
        pass
    return 0


def _fmt_err(exc: BaseException) -> str:
    for attr in ("reason", "msg", "strerror"):
        value = getattr(exc, attr, None)
        if value:
            return str(value)
    return str(exc) or exc.__class__.__name__


def download_asset(
    url: str,
    dest_dir: Path,
    *,
    progress: Callable[[int, int], None] | None = None,
    timeout: float = 60.0,
) -> Path:
    """Download *url* into *dest_dir* (``.part`` temp file, then atomic rename).

    ``progress(done_bytes, total_bytes)`` is invoked as chunks arrive;
    ``total_bytes`` is ``0`` when the server omits ``Content-Length``. Returns
    the final ``Path``. Raises :class:`UpdateInstallError` (``stage="download"``).
    """
    url = _validate_url(url)
    dest_dir = Path(dest_dir)
    try:
        dest_dir.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise UpdateInstallError(
            f"无法创建下载目录：{_fmt_err(exc)}", stage="download"
        ) from exc

    filename = _filename_from_url(url)
    final_path = dest_dir / filename
    tmp_path = dest_dir / (filename + ".part")

    req = urllib.request.Request(
        url,
        headers={"Accept": "application/octet-stream", "User-Agent": _USER_AGENT},
        method="GET",
    )
    try:
        resp = _open(req, timeout)
    except UpdateInstallError:
        raise
    except Exception as exc:
        raise UpdateInstallError(
            f"下载请求失败：{_fmt_err(exc)}", stage="download"
        ) from exc

    done = 0
    try:
        with resp:
            total = _content_length(resp)
            with open(tmp_path, "wb") as fh:
                while True:
                    chunk = resp.read(_CHUNK_SIZE)
                    if not chunk:
                        break
                    fh.write(chunk)
                    done += len(chunk)
                    if progress is not None:
                        try:
                            progress(done, total)
                        except Exception:
                            pass
    except UpdateInstallError:
        raise
    except Exception as exc:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise UpdateInstallError(
            f"下载中断：{_fmt_err(exc)}", stage="download"
        ) from exc

    try:
        tmp_path.replace(final_path)
    except OSError as exc:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise UpdateInstallError(
            f"写入安装包失败：{_fmt_err(exc)}", stage="download"
        ) from exc
    return final_path


def verify_sha256(path: Path, expected_hex: str) -> bool:
    """True when *path* hashes to *expected_hex* (chunked read).

    Missing / unreadable files return ``False``. An empty/None *expected_hex*
    returns ``True`` — there is nothing to compare, so the caller decides
    whether an unverifiable installer may be installed.
    """
    path = Path(path)
    if not path.exists() or not path.is_file():
        return False
    expected = (str(expected_hex or "")).strip().lower()
    if not expected:
        return True
    digest = hashlib.sha256()
    try:
        with open(path, "rb") as fh:
            while True:
                chunk = fh.read(_CHUNK_SIZE)
                if not chunk:
                    break
                digest.update(chunk)
    except OSError:
        return False
    return digest.hexdigest() == expected


def launch_installer(path: Path, *, silent: bool = True, wait: bool = False) -> bool:
    """Start the installer; returns True once launched.

    ``silent`` appends Inno Setup's ``/SILENT /NORESTART /CLOSEAPPLICATIONS``.
    The process is detached (mirrors ``main.py``'s ``creationflags``) so it
    survives DeskTidy exiting right afterwards. ``wait=True`` blocks until the
    installer finishes. Raises :class:`UpdateInstallError` (``stage="launch"``).
    """
    path = Path(path)
    if not path.exists() or not path.is_file():
        raise UpdateInstallError(f"安装包不存在：{path}", stage="launch")

    cmd = [str(path)]
    if silent:
        cmd += ["/SILENT", "/NORESTART", "/CLOSEAPPLICATIONS"]

    creationflags = 0
    try:
        creationflags = int(getattr(subprocess, "DETACHED_PROCESS", 0)) | int(
            getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
        )
    except Exception:
        creationflags = 0

    try:
        proc = subprocess.Popen(  # noqa: S603 — argv built from a fixed local path
            cmd,
            cwd=str(path.parent),
            creationflags=creationflags,
            close_fds=True,
        )
    except OSError as exc:
        raise UpdateInstallError(
            f"启动安装程序失败：{_fmt_err(exc)}", stage="launch"
        ) from exc

    if wait:
        try:
            proc.wait()
        except Exception:
            pass
    return True


def cleanup_old_installers(dest_dir: Path, keep: Path | None = None) -> int:
    """Delete stale ``.part`` files and old ``DeskTidy_Setup_*.exe`` installers.

    *keep* (an already-finalized download) is preserved. Returns the number of
    files removed; never raises.
    """
    dest_dir = Path(dest_dir)
    if not dest_dir.is_dir():
        return 0
    keep_resolved: Path | None = None
    if keep is not None:
        try:
            keep_resolved = Path(keep).resolve()
        except OSError:
            keep_resolved = None
    removed = 0
    for entry in dest_dir.iterdir():
        try:
            if not entry.is_file():
                continue
        except OSError:
            continue
        name = entry.name
        is_part = name.endswith(".part")
        is_installer = name.startswith(_INSTALLER_PREFIX) and name.lower().endswith(
            ".exe"
        )
        if not (is_part or is_installer):
            continue
        if keep_resolved is not None:
            try:
                if entry.resolve() == keep_resolved:
                    continue
            except OSError:
                pass
        try:
            entry.unlink()
            removed += 1
        except OSError:
            pass
    return removed
