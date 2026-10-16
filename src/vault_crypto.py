"""Client-side zero-knowledge crypto for the DeskTidy account vault.

Pure standard library + Windows CNG (bcrypt.dll) AES-256-GCM, so the vault
works with no third-party crypto dependency. If ``cryptography`` happens to be
installed it is preferred, but never required.

Ciphertext format: ``"v1:" + base64(nonce(12) | tag(16) | ciphertext)``.

The derived key (32 bytes) is produced by PBKDF2-HMAC-SHA256 and must never be
written to disk — it lives only in module-level memory of ``account_vault``.
"""

from __future__ import annotations

import base64
import binascii
import hashlib
import os

DEFAULT_KDF_ITERATIONS = 600000
#: argon2id defaults (used only when argon2-cffi is installed).
DEFAULT_ARGON2_MEMORY_KIB = 65536
DEFAULT_ARGON2_PARALLELISM = 2
DEFAULT_ARGON2_TIME_COST = 3
_KEY_CHECK_PLAINTEXT = "desktidy-vault"
_TOKEN_PREFIX = "v1:"
_NONCE_LEN = 12
_TAG_LEN = 16
_KEY_LEN = 32

#: Active AES-GCM backend name: "cryptography" | "cng" | "unknown".
_GCM_BACKEND = "unknown"
_GCM_IMPL: tuple | None = None


class VaultCryptoError(Exception):
    """Raised when a field cannot be decrypted (bad key / tampered ciphertext)."""


def gcm_backend() -> str:
    """Name of the active AES-GCM backend (for diagnostics / selftests)."""
    if _GCM_IMPL is None:
        _load_gcm_impl()
    return _GCM_BACKEND


def new_salt(n: int = 16) -> bytes:
    """Return ``n`` cryptographically-random bytes."""
    if int(n) < 1:
        raise ValueError("salt length must be positive")
    return os.urandom(int(n))


def kdf_algo() -> str:
    """Return the strongest available KDF: ``"argon2id"`` when argon2-cffi is
    importable, otherwise ``"pbkdf2"`` (pure stdlib, zero-dependency fallback).

    The result is a capability probe only — callers that must NOT silently
    downgrade should request ``algo="argon2id"`` explicitly (see ``derive_key``).
    """
    try:
        import argon2.low_level  # type: ignore  # noqa: F401
    except ImportError:
        return "pbkdf2"
    return "argon2id"


def derive_key(
    master_password: str,
    salt: bytes,
    *,
    algo: str | None = None,
    iterations: int = DEFAULT_KDF_ITERATIONS,
    memory_kb: int = DEFAULT_ARGON2_MEMORY_KIB,
    parallelism: int = DEFAULT_ARGON2_PARALLELISM,
    time_cost: int = DEFAULT_ARGON2_TIME_COST,
) -> bytes:
    """Derive a 32-byte key from *master_password*.

    ``algo=None`` auto-selects via :func:`kdf_algo` (argon2id if installed,
    otherwise PBKDF2). ``"argon2id"`` uses memory-hard argon2-cffi (type=ID);
    if the backend is unavailable it raises :class:`VaultCryptoError` instead of
    silently downgrading. ``"pbkdf2"`` uses ``hashlib.pbkdf2_hmac``.
    """
    pw = str(master_password).encode("utf-8")
    salt_b = bytes(salt)
    chosen = kdf_algo() if algo is None else str(algo)
    if chosen == "argon2id":
        try:
            import argon2.low_level  # type: ignore
        except ImportError as exc:
            raise VaultCryptoError(
                "argon2id 后端不可用（未安装 argon2-cffi）"
            ) from exc
        if int(time_cost) < 1:
            raise ValueError("time_cost must be positive")
        return argon2.low_level.hash_secret_raw(
            secret=pw,
            salt=salt_b,
            time_cost=int(time_cost),
            memory_cost=int(memory_kb),
            parallelism=int(parallelism),
            hash_len=_KEY_LEN,
            type=argon2.low_level.Type.ID,
        )
    if chosen == "pbkdf2":
        if int(iterations) < 1:
            raise ValueError("iterations must be positive")
        return hashlib.pbkdf2_hmac(
            "sha256",
            pw,
            salt_b,
            int(iterations),
            dklen=_KEY_LEN,
        )
    raise VaultCryptoError(f"未知 KDF 算法: {chosen!r}")


def _require_key(key: bytes) -> bytes:
    if not isinstance(key, (bytes, bytearray)) or len(key) != _KEY_LEN:
        raise VaultCryptoError(f"密钥必须为 {_KEY_LEN} 字节")
    return bytes(key)


# --------------------------------------------------------------------------- #
# AES-256-GCM backends
# --------------------------------------------------------------------------- #


def _load_gcm_impl() -> tuple:
    """Return (encrypt_fn, decrypt_fn); prefer ``cryptography``, else CNG."""
    global _GCM_IMPL, _GCM_BACKEND
    if _GCM_IMPL is not None:
        return _GCM_IMPL

    try:  # pragma: no cover - optional path
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM  # type: ignore

        def _crypto_encrypt(key: bytes, nonce: bytes, plaintext: bytes) -> bytes:
            out = AESGCM(key).encrypt(nonce, plaintext, None)
            # AESGCM returns ciphertext || tag; our wire format is tag || ciphertext.
            return out[-_TAG_LEN:] + out[:-_TAG_LEN]

        def _crypto_decrypt(key: bytes, nonce: bytes, tag: bytes, ciphertext: bytes) -> bytes:
            try:
                return AESGCM(key).decrypt(nonce, ciphertext + tag, None)
            except Exception as exc:  # noqa: BLE001 - InvalidTag etc.
                raise VaultCryptoError("解密失败：密文被篡改或密钥不正确") from exc

        _GCM_BACKEND = "cryptography"
        _GCM_IMPL = (_crypto_encrypt, _crypto_decrypt)
        return _GCM_IMPL
    except ImportError:
        pass

    _GCM_BACKEND = "cng"
    _GCM_IMPL = (_cng_gcm_encrypt, _cng_gcm_decrypt)
    return _GCM_IMPL


def _cng_gcm_encrypt(key: bytes, nonce: bytes, plaintext: bytes) -> bytes:
    """AES-256-GCM encrypt via Windows CNG. Returns ``tag(16) || ciphertext``."""
    import ctypes
    from ctypes import wintypes

    bcrypt = ctypes.windll.bcrypt
    _ok = 0
    _auth_info_t = _build_auth_info_type(ctypes, wintypes)

    h_alg = wintypes.HANDLE()
    status = bcrypt.BCryptOpenAlgorithmProvider(
        ctypes.byref(h_alg), ctypes.c_wchar_p("AES"), None, 0
    )
    if status != _ok:
        raise VaultCryptoError(f"BCryptOpenAlgorithmProvider 失败: {status:#x}")
    try:
        _cng_set_gcm_mode(bcrypt, h_alg)
        h_key = wintypes.HANDLE()
        key_buf = ctypes.create_string_buffer(key, len(key))
        status = bcrypt.BCryptGenerateSymmetricKey(
            h_alg, ctypes.byref(h_key), None, 0, key_buf, len(key), 0
        )
        if status != _ok:
            raise VaultCryptoError(f"BCryptGenerateSymmetricKey 失败: {status:#x}")
        try:
            nonce_buf = ctypes.create_string_buffer(nonce, len(nonce))
            tag_buf = ctypes.create_string_buffer(_TAG_LEN)
            info = _auth_info_t()
            info.cbSize = ctypes.sizeof(_auth_info_t)
            info.dwInfoVersion = 1
            info.pbNonce = ctypes.cast(nonce_buf, ctypes.POINTER(ctypes.c_ubyte))
            info.cbNonce = len(nonce)
            info.pbTag = ctypes.cast(tag_buf, ctypes.POINTER(ctypes.c_ubyte))
            info.cbTag = _TAG_LEN
            info.cbData = len(plaintext)
            in_buf = ctypes.create_string_buffer(plaintext, max(1, len(plaintext)))
            out_buf = ctypes.create_string_buffer(max(1, len(plaintext)))
            out_len = wintypes.ULONG(0)
            status = bcrypt.BCryptEncrypt(
                h_key,
                in_buf,
                len(plaintext),
                ctypes.byref(info),
                None,
                0,
                out_buf,
                max(1, len(plaintext)),
                ctypes.byref(out_len),
                0,
            )
            if status != _ok:
                raise VaultCryptoError(f"BCryptEncrypt 失败: {status:#x}")
            return tag_buf.raw[:_TAG_LEN] + out_buf.raw[: out_len.value]
        finally:
            bcrypt.BCryptDestroyKey(h_key)
    finally:
        bcrypt.BCryptCloseAlgorithmProvider(h_alg, 0)


def _cng_gcm_decrypt(key: bytes, nonce: bytes, tag: bytes, ciphertext: bytes) -> bytes:
    """AES-256-GCM decrypt via Windows CNG. Raises VaultCryptoError on mismatch."""
    import ctypes
    from ctypes import wintypes

    bcrypt = ctypes.windll.bcrypt
    _ok = 0
    _auth_info_t = _build_auth_info_type(ctypes, wintypes)

    if len(tag) != _TAG_LEN:
        raise VaultCryptoError("密文标签长度无效")

    h_alg = wintypes.HANDLE()
    status = bcrypt.BCryptOpenAlgorithmProvider(
        ctypes.byref(h_alg), ctypes.c_wchar_p("AES"), None, 0
    )
    if status != _ok:
        raise VaultCryptoError(f"BCryptOpenAlgorithmProvider 失败: {status:#x}")
    try:
        _cng_set_gcm_mode(bcrypt, h_alg)
        h_key = wintypes.HANDLE()
        key_buf = ctypes.create_string_buffer(key, len(key))
        status = bcrypt.BCryptGenerateSymmetricKey(
            h_alg, ctypes.byref(h_key), None, 0, key_buf, len(key), 0
        )
        if status != _ok:
            raise VaultCryptoError(f"BCryptGenerateSymmetricKey 失败: {status:#x}")
        try:
            nonce_buf = ctypes.create_string_buffer(nonce, len(nonce))
            tag_buf = ctypes.create_string_buffer(tag, _TAG_LEN)
            info = _auth_info_t()
            info.cbSize = ctypes.sizeof(_auth_info_t)
            info.dwInfoVersion = 1
            info.pbNonce = ctypes.cast(nonce_buf, ctypes.POINTER(ctypes.c_ubyte))
            info.cbNonce = len(nonce)
            info.pbTag = ctypes.cast(tag_buf, ctypes.POINTER(ctypes.c_ubyte))
            info.cbTag = _TAG_LEN
            info.cbData = len(ciphertext)
            in_buf = ctypes.create_string_buffer(ciphertext, max(1, len(ciphertext)))
            out_buf = ctypes.create_string_buffer(max(1, len(ciphertext)))
            out_len = wintypes.ULONG(0)
            status = bcrypt.BCryptDecrypt(
                h_key,
                in_buf,
                len(ciphertext),
                ctypes.byref(info),
                None,
                0,
                out_buf,
                max(1, len(ciphertext)),
                ctypes.byref(out_len),
                0,
            )
            if status != _ok:
                # STATUS_AUTH_TAG_MISMATCH (0xC000A002) lands here.
                raise VaultCryptoError("解密失败：密文被篡改或密钥不正确")
            return out_buf.raw[: out_len.value]
        finally:
            bcrypt.BCryptDestroyKey(h_key)
    finally:
        bcrypt.BCryptCloseAlgorithmProvider(h_alg, 0)


def _build_auth_info_type(ctypes, wintypes):
    """Return a per-process BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ctypes type."""
    class _BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO(ctypes.Structure):
        _fields_ = [
            ("cbSize", wintypes.ULONG),
            ("dwInfoVersion", wintypes.ULONG),
            ("pbNonce", ctypes.POINTER(ctypes.c_ubyte)),
            ("cbNonce", wintypes.ULONG),
            ("pbAuthData", ctypes.POINTER(ctypes.c_ubyte)),
            ("cbAuthData", wintypes.ULONG),
            ("pbTag", ctypes.POINTER(ctypes.c_ubyte)),
            ("cbTag", wintypes.ULONG),
            ("pbMacContext", ctypes.POINTER(ctypes.c_ubyte)),
            ("cbMacContext", wintypes.ULONG),
            ("cbAAD", wintypes.ULONG),
            ("cbData", ctypes.c_ulonglong),
            ("dwFlags", wintypes.ULONG),
        ]

    return _BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO


def _cng_set_gcm_mode(bcrypt, h_alg) -> None:
    """Switch the algorithm handle to GCM chaining mode."""
    import ctypes

    mode = "ChainingModeGCM"
    status = bcrypt.BCryptSetProperty(
        h_alg,
        ctypes.c_wchar_p("ChainingMode"),
        ctypes.c_wchar_p(mode),
        (len(mode) + 1) * 2,
        0,
    )
    if status != 0:
        raise VaultCryptoError(f"BCryptSetProperty ChainingModeGCM 失败: {status:#x}")


# --------------------------------------------------------------------------- #
# Public field encrypt / decrypt + key check
# --------------------------------------------------------------------------- #


def _gcm_encrypt(key: bytes, nonce: bytes, plaintext: bytes) -> bytes:
    if _GCM_IMPL is None:
        _load_gcm_impl()
    return _GCM_IMPL[0](key, nonce, plaintext)


def _gcm_decrypt(key: bytes, nonce: bytes, tag: bytes, ciphertext: bytes) -> bytes:
    if _GCM_IMPL is None:
        _load_gcm_impl()
    return _GCM_IMPL[1](key, nonce, tag, ciphertext)


def encrypt_field(plaintext: str, key: bytes) -> str:
    """Encrypt *plaintext* into a ``"v1:..."`` token using *key*."""
    k = _require_key(key)
    nonce = new_salt(_NONCE_LEN)
    tag_cipher = _gcm_encrypt(k, nonce, str(plaintext).encode("utf-8"))
    raw = nonce + tag_cipher
    return _TOKEN_PREFIX + base64.b64encode(raw).decode("ascii")


def decrypt_field(token: str, key: bytes) -> str:
    """Decrypt a ``"v1:..."`` token. Non-``v1:`` values are returned as-is.

    Raises :class:`VaultCryptoError` when the token is malformed or fails
    authentication (wrong key / tampered ciphertext).
    """
    text = str(token or "")
    if not text.startswith(_TOKEN_PREFIX):
        # Legacy / plaintext value — treat as already decrypted.
        return text
    k = _require_key(key)
    try:
        raw = base64.b64decode(text[len(_TOKEN_PREFIX):], validate=True)
    except (ValueError, binascii.Error) as exc:
        raise VaultCryptoError("密文格式无效") from exc
    if len(raw) < _NONCE_LEN + _TAG_LEN:
        raise VaultCryptoError("密文长度无效")
    nonce = raw[:_NONCE_LEN]
    tag = raw[_NONCE_LEN:_NONCE_LEN + _TAG_LEN]
    ciphertext = raw[_NONCE_LEN + _TAG_LEN:]
    plain = _gcm_decrypt(k, nonce, tag, ciphertext)
    try:
        return plain.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise VaultCryptoError("解密结果不是有效 UTF-8") from exc


def make_key_check(key: bytes) -> str:
    """Produce a verifier ciphertext used to check the master password."""
    return encrypt_field(_KEY_CHECK_PLAINTEXT, key)


def verify_key_check(key: bytes, check: str) -> bool:
    """Return True when *check* decrypts to the fixed key-check plaintext."""
    try:
        return decrypt_field(check, key) == _KEY_CHECK_PLAINTEXT
    except VaultCryptoError:
        return False
