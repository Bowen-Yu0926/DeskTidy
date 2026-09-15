"""Account vault: settings, session, login normalize, launcher position."""

from __future__ import annotations

import json
import re
import time
from pathlib import Path
from typing import Any

from src.settings import APP_DIR, ensure_app_dir

DEFAULT_API_BASE = "https://bowen-yu0926.byethost10.com/desktidy-vault/"
SESSION_FILE = APP_DIR / "account_vault_session.json"
DEFAULT_REFRESH_INTERVAL_HOURS = 2
MIN_REFRESH_INTERVAL_HOURS = 1
MAX_REFRESH_INTERVAL_HOURS = 168  # 1 week

_PHONE_RE = re.compile(r"^1\d{10}$")
_EMAIL_RE = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")


def account_vault_settings(settings: dict[str, Any] | None) -> dict[str, Any]:
    if settings is None:
        return {
            "enabled": False,
            "api_base": DEFAULT_API_BASE,
            "refresh_interval_hours": DEFAULT_REFRESH_INTERVAL_HOURS,
        }
    raw = settings.get("account_vault")
    if not isinstance(raw, dict):
        raw = {
            "enabled": False,
            "api_base": DEFAULT_API_BASE,
            "refresh_interval_hours": DEFAULT_REFRESH_INTERVAL_HOURS,
        }
        settings["account_vault"] = raw
    raw.setdefault("enabled", False)
    raw.setdefault("api_base", DEFAULT_API_BASE)
    raw.setdefault("refresh_interval_hours", DEFAULT_REFRESH_INTERVAL_HOURS)
    return raw


def account_vault_enabled(settings: dict[str, Any] | None) -> bool:
    return bool(account_vault_settings(settings).get("enabled", False))


def account_vault_api_base(settings: dict[str, Any] | None) -> str:
    base = str(account_vault_settings(settings).get("api_base") or DEFAULT_API_BASE).strip()
    if not base:
        base = DEFAULT_API_BASE
    return base.rstrip("/") + "/"


def account_vault_refresh_interval_hours(settings: dict[str, Any] | None) -> int:
    """Background list refresh interval in hours (clamped)."""
    raw = account_vault_settings(settings).get(
        "refresh_interval_hours", DEFAULT_REFRESH_INTERVAL_HOURS
    )
    try:
        hours = int(raw)
    except (TypeError, ValueError):
        hours = DEFAULT_REFRESH_INTERVAL_HOURS
    return max(MIN_REFRESH_INTERVAL_HOURS, min(MAX_REFRESH_INTERVAL_HOURS, hours))


def account_vault_refresh_interval_ms(settings: dict[str, Any] | None) -> int:
    return account_vault_refresh_interval_hours(settings) * 3600 * 1000


def account_vault_launcher_pos(
    settings: dict[str, Any] | None,
) -> tuple[int, int] | None:
    """Return saved launcher (x, y) or None if unset/invalid."""
    cfg = account_vault_settings(settings)
    raw = cfg.get("launcher_pos")
    if not isinstance(raw, dict):
        return None
    try:
        return int(raw["x"]), int(raw["y"])
    except (KeyError, TypeError, ValueError):
        return None


def set_account_vault_launcher_pos(
    settings: dict[str, Any] | None, x: int, y: int
) -> None:
    """Persist Doubao-style launcher top-left into settings dict (caller saves)."""
    if settings is None:
        return
    cfg = account_vault_settings(settings)
    cfg["launcher_pos"] = {"x": int(x), "y": int(y)}


def normalize_login(login: str) -> tuple[str, str]:
    """Return (normalized_login, 'phone'|'email'). Raises ValueError if invalid."""
    text = str(login or "").strip()
    if not text:
        raise ValueError("请输入手机号或邮箱")
    if _PHONE_RE.match(text):
        return text, "phone"
    lower = text.lower()
    if _EMAIL_RE.match(lower):
        return lower, "email"
    raise ValueError("请使用有效的手机号或邮箱")


def mask_login(login: str) -> str:
    text = str(login or "").strip()
    if not text:
        return ""
    if "@" in text:
        local, _, domain = text.partition("@")
        if len(local) <= 2:
            return local[:1] + "***@" + domain
        return local[:2] + "***@" + domain
    if len(text) >= 7:
        return text[:3] + "****" + text[-4:]
    return text[:1] + "***"


def normalize_display_name(name: str) -> str:
    """Validate nickname for register. Raises ValueError if invalid."""
    text = " ".join(str(name or "").strip().split())
    if len(text) < 2:
        raise ValueError("昵称至少 2 个字")
    if len(text) > 20:
        raise ValueError("昵称最多 20 个字")
    return text


def session_display_name(session: dict[str, Any] | None) -> str:
    if not session:
        return ""
    name = str(session.get("display_name") or "").strip()
    if name:
        return name
    return mask_login(str(session.get("login") or "").strip())


_SESSION_LISTENERS: list[Any] = []


def add_session_listener(callback: Any) -> None:
    if callback not in _SESSION_LISTENERS:
        _SESSION_LISTENERS.append(callback)


def _notify_session_listeners() -> None:
    for cb in list(_SESSION_LISTENERS):
        try:
            cb()
        except Exception:
            pass


def session_file_path() -> Path:
    return SESSION_FILE


def load_session() -> dict[str, Any] | None:
    path = session_file_path()
    try:
        if not path.is_file():
            return None
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, TypeError):
        return None
    if not isinstance(data, dict):
        return None
    token = str(data.get("token") or "").strip()
    login = str(data.get("login") or "").strip()
    if not token or not login:
        return None
    return {
        "api_base": str(data.get("api_base") or DEFAULT_API_BASE),
        "login": login,
        "display_name": str(data.get("display_name") or "").strip(),
        "token": token,
        "user_id": data.get("user_id"),
    }


def save_session(
    *,
    api_base: str,
    login: str,
    token: str,
    user_id: Any = None,
    display_name: str = "",
) -> Path:
    ensure_app_dir()
    path = session_file_path()
    payload = {
        "api_base": str(api_base or DEFAULT_API_BASE).rstrip("/") + "/",
        "login": str(login),
        "display_name": str(display_name or "").strip(),
        "token": str(token),
        "user_id": user_id,
    }
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    _notify_session_listeners()
    return path


def clear_session() -> None:
    path = session_file_path()
    try:
        if path.is_file():
            path.unlink()
    except OSError:
        pass
    _notify_session_listeners()


def reorder_credential_ids(
    items: list[dict[str, Any]],
    source_id: int,
    target_id: int,
) -> list[int] | None:
    """Move source before target within the same pin group. Returns full id order or None."""
    if source_id == target_id:
        return None
    by_id: dict[int, dict[str, Any]] = {}
    for raw in items:
        try:
            cid = int(raw.get("id"))
        except (TypeError, ValueError):
            continue
        by_id[cid] = raw
    src = by_id.get(source_id)
    dst = by_id.get(target_id)
    if src is None or dst is None:
        return None
    if bool(src.get("pinned")) != bool(dst.get("pinned")):
        return None

    pinned_ids = [int(x["id"]) for x in items if x.get("pinned") and "id" in x]
    unpinned_ids = [int(x["id"]) for x in items if not x.get("pinned") and "id" in x]
    group = pinned_ids if src.get("pinned") else unpinned_ids
    if source_id not in group or target_id not in group:
        return None
    group = list(group)
    group.remove(source_id)
    group.insert(group.index(target_id), source_id)
    if src.get("pinned"):
        return group + unpinned_ids
    return pinned_ids + group


def export_credentials_payload(items: list[dict[str, Any]]) -> dict[str, Any]:
    """Build JSON-serializable export document (no server ids required)."""
    out_items: list[dict[str, str]] = []
    for raw in items:
        if not isinstance(raw, dict):
            continue
        title = str(raw.get("title") or "").strip()
        if not title:
            continue
        out_items.append(
            {
                "title": title,
                "username": str(raw.get("username") or "").strip(),
                "password": str(raw.get("password") or "").strip(),
                "url": str(raw.get("url") or "").strip(),
                "note": str(raw.get("note") or "").strip(),
                "pinned": bool(raw.get("pinned")),
            }
        )
    return {
        "format": "desktidy-vault",
        "version": 1,
        "exported_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "items": out_items,
    }


def parse_credentials_import(data: Any) -> list[dict[str, str]]:
    """Parse export JSON / list into create payloads. Raises ValueError on bad input."""
    if isinstance(data, dict):
        items = data.get("items")
        if items is None and any(k in data for k in ("title", "username", "password")):
            items = [data]
    elif isinstance(data, list):
        items = data
    else:
        raise ValueError("无法识别的账号文件格式")
    if not isinstance(items, list):
        raise ValueError("账号文件缺少 items 列表")
    out: list[dict[str, str]] = []
    for raw in items:
        if not isinstance(raw, dict):
            continue
        title = str(raw.get("title") or "").strip()
        if not title:
            continue
        out.append(
            {
                "title": title,
                "username": str(raw.get("username") or "").strip(),
                "password": str(raw.get("password") or "").strip(),
                "url": str(raw.get("url") or "").strip(),
                "note": str(raw.get("note") or "").strip(),
                "pinned": bool(raw.get("pinned")),
            }
        )
    if not out:
        raise ValueError("文件中没有可导入的账号条目")
    return out


def write_credentials_export_file(path: Path | str, items: list[dict[str, Any]]) -> Path:
    path = Path(path)
    payload = export_credentials_payload(items)
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    return path


def read_credentials_import_file(path: Path | str) -> list[dict[str, str]]:
    path = Path(path)
    try:
        data = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"无法读取账号文件：{exc}") from exc
    return parse_credentials_import(data)
