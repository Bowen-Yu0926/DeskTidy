"""Account vault: settings, session, login normalize, launcher position."""

from __future__ import annotations

import base64
import binascii
import json
import re
import secrets
import string
import time
from pathlib import Path
from typing import Any

from src.settings import APP_DIR, ensure_app_dir
from src.vault_crypto import (
    DEFAULT_KDF_ITERATIONS,
    VaultCryptoError,
    derive_key,
    kdf_algo,
    make_key_check,
    new_salt,
    verify_key_check,
)

DEFAULT_API_BASE = "https://bowen-yu0926.byethost10.com/desktidy-vault/"
SESSION_FILE = APP_DIR / "account_vault_session.json"
DEFAULT_REFRESH_INTERVAL_HOURS = 2
MIN_REFRESH_INTERVAL_HOURS = 1
MAX_REFRESH_INTERVAL_HOURS = 168  # 1 week
DEFAULT_CLIPBOARD_CLEAR_SECONDS = 30

# Ambiguous glyphs excluded when avoid_ambiguous=True in generate_password.
_AMBIGUOUS_CHARS = frozenset("0O1lI|")

# Explicit safe symbol pool: no quotes, backslash, backtick, pipe or whitespace
# (these break JSON/CSV exports, shell, SQL, and some web password fields).
_SAFE_SYMBOLS = "!#$%&()*+,-./:;<=>?@[]^_{}~"

_PASSWORD_MIN_LENGTH = 4
_PASSWORD_MAX_LENGTH = 128

_PHONE_RE = re.compile(r"^1\d{10}$")
_EMAIL_RE = re.compile(r"^[^@\s]+@[^@\s]+\.[^@\s]+$")

# Derived master key kept in memory only — never persisted to disk.
_MASTER_KEY: bytes | None = None


def account_vault_settings(settings: dict[str, Any] | None) -> dict[str, Any]:
    if settings is None:
        return {
            "enabled": False,
            "api_base": DEFAULT_API_BASE,
            "refresh_interval_hours": DEFAULT_REFRESH_INTERVAL_HOURS,
            "clipboard_clear_seconds": DEFAULT_CLIPBOARD_CLEAR_SECONDS,
        }
    raw = settings.get("account_vault")
    if not isinstance(raw, dict):
        raw = {
            "enabled": False,
            "api_base": DEFAULT_API_BASE,
            "refresh_interval_hours": DEFAULT_REFRESH_INTERVAL_HOURS,
            "clipboard_clear_seconds": DEFAULT_CLIPBOARD_CLEAR_SECONDS,
        }
        settings["account_vault"] = raw
    raw.setdefault("enabled", False)
    raw.setdefault("api_base", DEFAULT_API_BASE)
    raw.setdefault("refresh_interval_hours", DEFAULT_REFRESH_INTERVAL_HOURS)
    raw.setdefault("clipboard_clear_seconds", DEFAULT_CLIPBOARD_CLEAR_SECONDS)
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


def account_vault_clipboard_clear_seconds(settings: dict[str, Any] | None) -> int:
    """Seconds to auto-clear a copied password from the clipboard (0 = never)."""
    raw = account_vault_settings(settings).get(
        "clipboard_clear_seconds", DEFAULT_CLIPBOARD_CLEAR_SECONDS
    )
    try:
        secs = int(raw)
    except (TypeError, ValueError):
        secs = DEFAULT_CLIPBOARD_CLEAR_SECONDS
    return max(0, secs)


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


def _normalize_kdf_algo(value: Any) -> str:
    """Coerce a session ``kdf_algo`` value to ``"pbkdf2"`` or ``"argon2id"``."""
    algo = str(value or "pbkdf2").strip() or "pbkdf2"
    return algo if algo in ("pbkdf2", "argon2id") else "pbkdf2"


def session_kdf_algo() -> str:
    """Return the KDF algorithm recorded in the session (default ``"pbkdf2"``)."""
    return _normalize_kdf_algo(_read_session_raw().get("kdf_algo"))


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
    result = {
        "api_base": str(data.get("api_base") or DEFAULT_API_BASE),
        "login": login,
        "display_name": str(data.get("display_name") or "").strip(),
        "token": token,
        "user_id": data.get("user_id"),
    }
    # Master-password KDF material (absent on legacy sessions → no master set).
    kdf_salt = str(data.get("kdf_salt") or "").strip()
    key_check = str(data.get("key_check") or "").strip()
    if kdf_salt:
        result["kdf_salt"] = kdf_salt
        result["key_check"] = key_check
        try:
            result["kdf_iterations"] = int(data.get("kdf_iterations") or DEFAULT_KDF_ITERATIONS)
        except (TypeError, ValueError):
            result["kdf_iterations"] = DEFAULT_KDF_ITERATIONS
        # Legacy sessions predate kdf_algo → treat as pbkdf2.
        result["kdf_algo"] = _normalize_kdf_algo(data.get("kdf_algo"))
    return result


def save_session(
    *,
    api_base: str,
    login: str,
    token: str,
    user_id: Any = None,
    display_name: str = "",
    kdf_salt: str = "",
    kdf_iterations: int = 0,
    key_check: str = "",
    kdf_algo: str = "",
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
    # Master-password KDF material (client-side E2E). Omits empties so legacy
    # sessions remain byte-for-byte compatible.
    if kdf_salt:
        payload["kdf_salt"] = str(kdf_salt)
    if kdf_iterations:
        payload["kdf_iterations"] = int(kdf_iterations)
    if kdf_algo:
        payload["kdf_algo"] = _normalize_kdf_algo(kdf_algo)
    if key_check:
        payload["key_check"] = str(key_check)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    _notify_session_listeners()
    return path


def clear_session() -> None:
    lock()
    path = session_file_path()
    try:
        if path.is_file():
            path.unlink()
    except OSError:
        pass
    _notify_session_listeners()


def _read_session_raw() -> dict[str, Any]:
    """Read the raw session JSON dict (empty dict on any error)."""
    path = session_file_path()
    try:
        if not path.is_file():
            return {}
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, TypeError):
        return {}
    return data if isinstance(data, dict) else {}


def _write_session_raw(payload: dict[str, Any]) -> None:
    """Write a raw session dict to disk (preserving all unknown fields)."""
    ensure_app_dir()
    session_file_path().write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    _notify_session_listeners()


def has_master_password() -> bool:
    """True when the session file holds KDF salt + key-check material."""
    raw = _read_session_raw()
    return bool(str(raw.get("kdf_salt") or "").strip()) and bool(
        str(raw.get("key_check") or "").strip()
    )


def set_master_password(password: str) -> None:
    """Generate fresh KDF salt, persist kdf params, and unlock in memory.

    The master password itself is never written to disk or uploaded — only its
    KDF salt, iteration count, algorithm, and a key-check ciphertext are stored.
    The KDF algorithm auto-selects argon2id when available, else PBKDF2.
    """
    global _MASTER_KEY
    if not str(password):
        raise ValueError("主密码不能为空")
    salt = new_salt(16)
    algo = kdf_algo()
    key = derive_key(str(password), salt, algo=algo)
    check = make_key_check(key)
    raw = _read_session_raw()
    raw["kdf_salt"] = base64.b64encode(salt).decode("ascii")
    raw["kdf_iterations"] = DEFAULT_KDF_ITERATIONS
    raw["kdf_algo"] = algo
    raw["key_check"] = check
    _write_session_raw(raw)
    _MASTER_KEY = key


def unlock(password: str) -> bool:
    """Derive the key from the stored salt and verify the key-check.

    The KDF algorithm is read from the session (default ``"pbkdf2"`` for legacy
    sessions) so vaults are cross-algorithm compatible. Returns True and keeps
    the key in memory on success; False otherwise.
    """
    global _MASTER_KEY
    raw = _read_session_raw()
    salt_b64 = str(raw.get("kdf_salt") or "").strip()
    check = str(raw.get("key_check") or "").strip()
    if not salt_b64 or not check:
        return False
    try:
        salt = base64.b64decode(salt_b64, validate=True)
    except (ValueError, binascii.Error):
        return False
    algo = _normalize_kdf_algo(raw.get("kdf_algo"))
    try:
        iterations = int(raw.get("kdf_iterations") or DEFAULT_KDF_ITERATIONS)
    except (TypeError, ValueError):
        iterations = DEFAULT_KDF_ITERATIONS
    if iterations < 1:
        iterations = DEFAULT_KDF_ITERATIONS
    try:
        key = derive_key(str(password), salt, algo=algo, iterations=iterations)
    except VaultCryptoError:
        return False
    if not verify_key_check(key, check):
        return False
    _MASTER_KEY = key
    return True


def lock() -> None:
    """Drop the in-memory derived key (keeps the on-disk KDF material)."""
    global _MASTER_KEY
    _MASTER_KEY = None


def is_unlocked() -> bool:
    return _MASTER_KEY is not None


def current_key() -> bytes | None:
    return _MASTER_KEY


def kdf_params() -> tuple[str, int, str]:
    """Return (kdf_salt_b64, kdf_iterations, key_check) for server sync."""
    raw = _read_session_raw()
    salt = str(raw.get("kdf_salt") or "").strip()
    try:
        iterations = int(raw.get("kdf_iterations") or DEFAULT_KDF_ITERATIONS)
    except (TypeError, ValueError):
        iterations = DEFAULT_KDF_ITERATIONS
    check = str(raw.get("key_check") or "").strip()
    return salt, iterations, check


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


# ---------------------------------------------------------------------------
# Password tools (pure, offline, no Qt).
# ---------------------------------------------------------------------------


def generate_password(
    length: int = 16,
    *,
    upper: bool = True,
    lower: bool = True,
    digits: bool = True,
    symbols: bool = True,
    avoid_ambiguous: bool = True,
) -> str:
    """Generate a cryptographically-random password.

    ``length`` is clamped to [4, 128]. At least one character class must be
    enabled or ``ValueError`` is raised. With ``avoid_ambiguous`` set, glyphs
    that are easy to misread (``0O1lI|``) are excluded. Every enabled class is
    guaranteed to appear at least once.
    """
    try:
        length = int(length)
    except (TypeError, ValueError):
        length = 16
    length = max(_PASSWORD_MIN_LENGTH, min(_PASSWORD_MAX_LENGTH, length))

    def _pool(source: str) -> str:
        if avoid_ambiguous:
            return "".join(c for c in source if c not in _AMBIGUOUS_CHARS)
        return source

    pools: list[str] = []
    if upper:
        pools.append(_pool(string.ascii_uppercase))
    if lower:
        pools.append(_pool(string.ascii_lowercase))
    if digits:
        pools.append(_pool(string.digits))
    if symbols:
        pools.append(_pool(_SAFE_SYMBOLS))

    if not pools:
        raise ValueError("至少启用一类字符（大写/小写/数字/符号）")

    alphabet = "".join(pools)
    # Guarantee one char from each enabled class, then fill the remainder.
    chosen = [secrets.choice(pool) for pool in pools]
    remaining = length - len(chosen)
    if remaining > 0:
        chosen.extend(secrets.choice(alphabet) for _ in range(remaining))
    # Fisher–Yates shuffle using the cryptographic PRNG.
    result = list(chosen)
    for i in range(len(result) - 1, 0, -1):
        j = secrets.randbelow(i + 1)
        result[i], result[j] = result[j], result[i]
    return "".join(result)


def password_strength(password: str) -> tuple[int, str]:
    """Return (0-4, Chinese label) via a simple length + class-count estimate."""
    pwd = str(password or "")
    if not pwd:
        return 0, "很弱"
    n = len(pwd)
    classes = 0
    if any(c.islower() for c in pwd):
        classes += 1
    if any(c.isupper() for c in pwd):
        classes += 1
    if any(c.isdigit() for c in pwd):
        classes += 1
    if any(not c.isalnum() for c in pwd):
        classes += 1

    score = 0
    if n >= 8:
        score += 1
    if n >= 12:
        score += 1
    if classes >= 2:
        score += 1
    if classes >= 3:
        score += 1
    if classes >= 4 and n >= 12:
        score += 1
    score = max(0, min(4, score))
    labels = ("很弱", "弱", "中", "强", "很强")
    return score, labels[score]


def should_clear_clipboard(current_text: str, copied_text: str, secs) -> bool:
    """Whether the clipboard should be auto-cleared.

    True only when auto-clear is enabled (secs > 0) and the clipboard still
    holds exactly the value we copied (so a later user copy is preserved).
    """
    try:
        secs = int(secs)
    except (TypeError, ValueError):
        secs = 0
    if secs <= 0:
        return False
    copied = str(copied_text)
    return bool(copied) and str(current_text) == copied
