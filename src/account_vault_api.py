"""HTTP client for DeskTidy account vault API."""

from __future__ import annotations

import http.cookiejar
import json
import re
import urllib.error
import urllib.request
from typing import Any
from urllib.parse import urlparse

from src.vault_crypto import decrypt_field, encrypt_field


class VaultApiError(Exception):
    def __init__(self, message: str, *, error: str = "error", status: int = 0):
        super().__init__(message)
        self.error = error
        self.status = status
        self.message = message


def _aes128_cbc_decrypt(key: bytes, iv: bytes, data: bytes) -> bytes:
    """AES-128-CBC decrypt (byethost testcookie). Prefer PyCryptodome; else Win CNG."""
    try:
        from Cryptodome.Cipher import AES  # type: ignore

        cipher = AES.new(key, AES.MODE_CBC, iv)
        return cipher.decrypt(data)
    except ImportError:
        pass
    try:
        from Crypto.Cipher import AES  # type: ignore

        cipher = AES.new(key, AES.MODE_CBC, iv)
        return cipher.decrypt(data)
    except ImportError:
        pass
    return _aes128_cbc_decrypt_win(key, iv, data)


def _aes128_cbc_decrypt_win(key: bytes, iv: bytes, data: bytes) -> bytes:
    """Windows BCrypt AES-128-CBC (no extra pip dep on DeskTidy's target OS)."""
    import ctypes
    from ctypes import wintypes

    bcrypt = ctypes.windll.bcrypt
    ok = 0

    class BCRYPT_KEY_DATA_BLOB_HEADER(ctypes.Structure):
        _fields_ = [
            ("dwMagic", wintypes.ULONG),
            ("dwVersion", wintypes.ULONG),
            ("cbKeyData", wintypes.ULONG),
        ]

    h_alg = wintypes.HANDLE()
    status = bcrypt.BCryptOpenAlgorithmProvider(
        ctypes.byref(h_alg), ctypes.c_wchar_p("AES"), None, 0
    )
    if status != ok:
        raise RuntimeError(f"BCryptOpenAlgorithmProvider {status}")
    try:
        status = bcrypt.BCryptSetProperty(
            h_alg,
            ctypes.c_wchar_p("ChainingMode"),
            ctypes.c_wchar_p("ChainingModeCBC"),
            (len("ChainingModeCBC") + 1) * 2,
            0,
        )
        if status != ok:
            raise RuntimeError(f"BCryptSetProperty {status}")

        BCRYPT_KEY_DATA_BLOB_MAGIC = 0x4D42444B
        hdr = BCRYPT_KEY_DATA_BLOB_HEADER(BCRYPT_KEY_DATA_BLOB_MAGIC, 1, len(key))
        blob = bytes(hdr) + key
        h_key = wintypes.HANDLE()
        status = bcrypt.BCryptImportKey(
            h_alg,
            None,
            ctypes.c_wchar_p("KeyDataBlob"),
            ctypes.byref(h_key),
            None,
            0,
            blob,
            len(blob),
            0,
        )
        if status != ok:
            raise RuntimeError(f"BCryptImportKey {status}")
        try:
            iv_buf = ctypes.create_string_buffer(iv)
            in_buf = ctypes.create_string_buffer(data)
            out_buf = ctypes.create_string_buffer(len(data))
            out_len = wintypes.ULONG(0)
            status = bcrypt.BCryptDecrypt(
                h_key,
                in_buf,
                len(data),
                None,
                iv_buf,
                len(iv),
                out_buf,
                len(data),
                ctypes.byref(out_len),
                0,
            )
            if status != ok:
                raise RuntimeError(f"BCryptDecrypt {status}")
            return out_buf.raw[: out_len.value]
        finally:
            bcrypt.BCryptDestroyKey(h_key)
    finally:
        bcrypt.BCryptCloseAlgorithmProvider(h_alg, 0)


def solve_byethost_test_cookie(html: str) -> str | None:
    """Return __test cookie value from byethost challenge HTML, or None."""
    if "toNumbers(" not in html or "__test" not in html:
        return None
    nums = re.findall(r'toNumbers\("([0-9a-fA-F]+)"\)', html)
    if len(nums) < 3:
        return None
    key, iv, cipher = (bytes.fromhex(x) for x in nums[:3])
    try:
        plain = _aes128_cbc_decrypt(key, iv, cipher)
    except Exception:
        return None
    return plain.hex()


class VaultApiClient:
    _cookie_cache: dict[str, str] = {}

    def __init__(self, api_base: str, token: str | None = None, *, timeout: float = 20.0):
        base = str(api_base or "").strip().rstrip("/") + "/"
        self.api_base = base
        self.token = str(token or "").strip() or None
        self.timeout = float(timeout)
        self._jar = http.cookiejar.CookieJar()
        self._opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self._jar)
        )
        host = urlparse(base).hostname or ""
        cached = self._cookie_cache.get(host)
        if cached:
            self._set_test_cookie(cached)

    def with_token(self, token: str | None) -> "VaultApiClient":
        return VaultApiClient(self.api_base, token, timeout=self.timeout)

    def _url(self, path: str) -> str:
        path = str(path or "").lstrip("/")
        return self.api_base + path

    def _set_test_cookie(self, value: str) -> None:
        host = urlparse(self.api_base).hostname or ""
        if not host or not value:
            return
        self._cookie_cache[host] = value
        cookie = http.cookiejar.Cookie(
            version=0,
            name="__test",
            value=value,
            port=None,
            port_specified=False,
            domain=host,
            domain_specified=True,
            domain_initial_dot=False,
            path="/",
            path_specified=True,
            secure=False,
            expires=None,
            discard=True,
            comment=None,
            comment_url=None,
            rest={},
            rfc2109=False,
        )
        self._jar.set_cookie(cookie)

    def _open(self, req: urllib.request.Request, timeout: float | None = None):
        return self._opener.open(
            req, timeout=self.timeout if timeout is None else float(timeout)
        )

    def _ensure_byethost(self, url: str, timeout: float | None = None) -> None:
        """If host uses testcookie challenge, solve once and cache __test.

        *timeout* overrides the per-probe budget (defaults to ``self.timeout``);
        a cached cookie makes this a no-op so a warm client never re-probes.
        """
        host = urlparse(self.api_base).hostname or ""
        if host and host in self._cookie_cache:
            return
        probe = urllib.request.Request(
            url,
            headers={
                "Accept": "text/html,application/json",
                "User-Agent": "Mozilla/5.0 DeskTidy-Vault/1",
            },
            method="GET",
        )
        try:
            with self._open(probe, timeout=timeout) as resp:
                raw = resp.read().decode("utf-8", errors="replace")
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
        except urllib.error.URLError:
            return
        token = solve_byethost_test_cookie(raw)
        if token:
            self._set_test_cookie(token)

    def _request(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
        *,
        auth: bool = False,
        _retry: bool = True,
        timeout: float | None = None,
        probe_timeout: float | None = None,
    ) -> Any:
        data = None
        headers = {
            "Accept": "application/json",
            "User-Agent": "Mozilla/5.0 DeskTidy-Vault/1",
        }
        if body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
            headers["Content-Type"] = "application/json; charset=utf-8"
        if auth:
            if not self.token:
                raise VaultApiError("未登录", error="unauthorized", status=401)
            # byethost / some CGI setups strip Authorization; send both.
            headers["Authorization"] = f"Bearer {self.token}"
            headers["X-Vault-Token"] = self.token
        url = self._url(path)
        self._ensure_byethost(self.api_base, timeout=probe_timeout)
        req = urllib.request.Request(url, data=data, headers=headers, method=method.upper())
        try:
            with self._open(req, timeout=timeout) as resp:
                raw = resp.read().decode("utf-8", errors="replace")
                status = int(getattr(resp, "status", 200) or 200)
        except urllib.error.HTTPError as exc:
            raw = exc.read().decode("utf-8", errors="replace")
            status = int(exc.code)
            token = solve_byethost_test_cookie(raw)
            if token and _retry:
                self._set_test_cookie(token)
                return self._request(
                    method, path, body, auth=auth, _retry=False,
                    timeout=timeout, probe_timeout=probe_timeout,
                )
            try:
                payload = json.loads(raw) if raw else {}
            except json.JSONDecodeError:
                payload = {}
            if isinstance(payload, dict) and payload.get("ok") is False:
                raise VaultApiError(
                    str(payload.get("message") or payload.get("error") or "请求失败"),
                    error=str(payload.get("error") or "error"),
                    status=status,
                ) from exc
            raise VaultApiError(f"HTTP {status}", error="http", status=status) from exc
        except urllib.error.URLError as exc:
            raise VaultApiError(f"网络错误: {exc.reason}", error="network", status=0) from exc

        token = solve_byethost_test_cookie(raw)
        if token and _retry:
            self._set_test_cookie(token)
            return self._request(
                method, path, body, auth=auth, _retry=False,
                timeout=timeout, probe_timeout=probe_timeout,
            )

        try:
            payload = json.loads(raw) if raw else {}
        except json.JSONDecodeError as exc:
            if "aes.js" in raw or "This site requires Javascript" in raw:
                raise VaultApiError(
                    "托管站防机器人校验未通过，请稍后重试",
                    error="byethost_cookie",
                    status=status,
                ) from exc
            raise VaultApiError("响应不是 JSON", error="bad_response", status=status) from exc
        if not isinstance(payload, dict):
            raise VaultApiError("响应格式错误", error="bad_response", status=status)
        if payload.get("ok") is False:
            raise VaultApiError(
                str(payload.get("message") or payload.get("error") or "请求失败"),
                error=str(payload.get("error") or "error"),
                status=status,
            )
        return payload.get("data")

    def health(
        self, timeout: float | None = None, probe_timeout: float | None = None
    ) -> dict[str, Any]:
        """Unauthenticated reachability check against /health.

        *timeout* bounds the health request itself; *probe_timeout* bounds the
        byethost cookie warm-up probe (both default to ``self.timeout``).
        """
        data = self._request("GET", "health", timeout=timeout, probe_timeout=probe_timeout)
        if not isinstance(data, dict):
            raise VaultApiError("健康检查失败", error="bad_response", status=0)
        return data

    def register(
        self, login: str, password: str, display_name: str = ""
    ) -> dict[str, Any]:
        payload: dict[str, Any] = {"login": login, "password": password}
        if display_name:
            payload["display_name"] = display_name
        data = self._request("POST", "auth/register", payload)
        return data if isinstance(data, dict) else {}

    def login(self, login: str, password: str) -> dict[str, Any]:
        data = self._request(
            "POST", "auth/login", {"login": login, "password": password}
        )
        return data if isinstance(data, dict) else {}

    def logout(self) -> None:
        self._request("POST", "auth/logout", {}, auth=True)

    _ENCRYPT_FIELDS = ("username", "password", "url", "note")

    @staticmethod
    def _encrypt_payload(payload: dict[str, Any], key: bytes) -> dict[str, Any]:
        """Encrypt the credential fields in *payload* and stamp ``enc_ver: 1``."""
        out = dict(payload)
        for field in VaultApiClient._ENCRYPT_FIELDS:
            out[field] = encrypt_field(str(payload.get(field) or ""), key)
        out["enc_ver"] = 1
        return out

    @staticmethod
    def _decrypt_item(item: dict[str, Any], key: bytes) -> dict[str, Any]:
        """Decrypt a credential row (enc_ver=1) into plaintext fields.

        enc_ver 0 / missing rows are already plaintext from the server and are
        returned unchanged. The response field names (username/password/url/note
        plus pinned/sort_order/id/title) are preserved.
        """
        if int(item.get("enc_ver") or 0) != 1:
            return item
        out = dict(item)
        for field in VaultApiClient._ENCRYPT_FIELDS:
            out[field] = decrypt_field(str(out.get(field) or ""), key)
        return out

    def list_credentials(self, key: bytes | None = None) -> list[dict[str, Any]]:
        """Fetch credential list; retry briefly for byethost/network cold starts.

        When *key* is provided, enc_ver=1 rows are decrypted client-side so the
        caller always receives plaintext ``username/password/url/note``.
        """
        import time

        last: VaultApiError | None = None
        for attempt in range(3):
            try:
                data = self._request("GET", "credentials", auth=True)
                if not isinstance(data, dict):
                    return []
                items = data.get("items")
                if not isinstance(items, list):
                    return []
                result = [x for x in items if isinstance(x, dict)]
                if key is not None:
                    result = [self._decrypt_item(x, key) for x in result]
                return result
            except VaultApiError as exc:
                last = exc
                retryable = exc.error in {
                    "network",
                    "byethost_cookie",
                    "bad_response",
                    "http",
                } or "防机器人" in (exc.message or "")
                if not retryable or attempt >= 2:
                    raise
                # Warm cookie / DNS after process start.
                time.sleep(0.35 * (attempt + 1))
                host = urlparse(self.api_base).hostname or ""
                self._cookie_cache.pop(host, None)
                try:
                    self._ensure_byethost(self.api_base)
                except Exception:
                    pass
        if last is not None:
            raise last
        return []

    def create_credential(
        self, payload: dict[str, Any], key: bytes | None = None
    ) -> dict[str, Any]:
        body = self._encrypt_payload(payload, key) if key is not None else payload
        data = self._request("POST", "credentials", body, auth=True)
        return data if isinstance(data, dict) else {}

    def update_credential(
        self, cred_id: int | str, payload: dict[str, Any], key: bytes | None = None
    ) -> dict[str, Any]:
        # Pin-only updates carry no title — leave them untouched (no encryption).
        body = payload
        if key is not None and "title" in payload:
            body = self._encrypt_payload(payload, key)
        data = self._request("PUT", f"credentials/{cred_id}", body, auth=True)
        return data if isinstance(data, dict) else {}

    def set_master_sync(
        self,
        salt_b64: str,
        iterations: int,
        key_check: str,
        kdf_algo: str | None = None,
    ) -> dict[str, Any]:
        """Upload only KDF params (never the master password) to the server.

        ``kdf_algo`` is optional and omitted when falsy, so the server keeps its
        default (``"pbkdf2"``) for older clients / legacy vaults.
        """
        body: dict[str, Any] = {
            "kdf_salt": str(salt_b64),
            "kdf_iterations": int(iterations),
            "key_check": str(key_check),
        }
        if kdf_algo:
            body["kdf_algo"] = str(kdf_algo)
        data = self._request("PUT", "auth/master", body, auth=True)
        return data if isinstance(data, dict) else {}

    def delete_credential(self, cred_id: int | str) -> None:
        self._request("DELETE", f"credentials/{cred_id}", auth=True)

    def reorder_credentials(self, ids: list[int | str]) -> dict[str, Any]:
        data = self._request(
            "PUT",
            "credentials/reorder",
            {"ids": [int(x) for x in ids]},
            auth=True,
        )
        return data if isinstance(data, dict) else {}


def migrate_legacy_to_e2e(client: "VaultApiClient", key: bytes) -> int:
    """Re-encrypt legacy server-side-plaintext rows to client E2E.

    Rows already carrying ``enc_ver=1`` are left untouched. Returns the number
    of migrated rows. Only touch this after the vault has been unlocked.
    """
    items = client.list_credentials(key=None)
    migrated = 0
    for item in items:
        if not isinstance(item, dict):
            continue
        if int(item.get("enc_ver") or 0) == 1:
            continue
        cred_id = item.get("id")
        if cred_id is None:
            continue
        payload = {
            "title": str(item.get("title") or ""),
            "username": str(item.get("username") or ""),
            "password": str(item.get("password") or ""),
            "url": str(item.get("url") or ""),
            "note": str(item.get("note") or ""),
            "pinned": bool(item.get("pinned")),
        }
        client.update_credential(cred_id, payload, key=key)
        migrated += 1
    return migrated


def probe_vault_reachable(
    api_base: str, *, timeout: float = 8.0
) -> tuple[bool, str]:
    """Return (ok, message). Never raises — safe for background UI gating.

    The byethost cookie warm-up and the /health request share a single *timeout*
    budget: the cookie probe is capped (``min(timeout/2, 2.0)``) and the health
    request gets the remainder, so an unreachable host costs ≈ one *timeout*
    instead of two. A warm client (cached cookie) skips the probe entirely.
    """
    try:
        probe_budget = min(float(timeout) / 2.0, 2.0)
        VaultApiClient(str(api_base or ""), timeout=timeout).health(
            timeout=max(0.2, float(timeout) - probe_budget),
            probe_timeout=probe_budget,
        )
        return True, ""
    except VaultApiError as exc:
        return False, exc.message or "无法访问账号服务器"
    except Exception as exc:  # noqa: BLE001 — surface as unreachable
        return False, str(exc) or "无法访问账号服务器"
