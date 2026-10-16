"""Selftest: client-side zero-knowledge vault crypto (offline, no network).

Run (workdir=E:\\Desktidy):
    $env:DESKTIDY_SELFTEST=1; python scripts/selftest_vault_crypto.py
"""

from __future__ import annotations

import base64
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def main() -> int:
    from src import account_vault as av
    from src import vault_crypto as vc
    from src.account_vault_api import (
        VaultApiClient,
        decrypt_field,
        migrate_legacy_to_e2e,
    )

    # --- new default matches OWASP 2023 PBKDF2-HMAC-SHA256 (600k) ---
    assert vc.DEFAULT_KDF_ITERATIONS == 600000, vc.DEFAULT_KDF_ITERATIONS
    assert vc.derive_key("pw", b"1234567890123456", iterations=200000) != vc.derive_key(
        "pw", b"1234567890123456"
    ), "explicit 200000 must differ from new 600000 default"

    # --- kdf_algo() returns a valid, normalized algorithm name ---
    algo = vc.kdf_algo()
    assert algo in ("pbkdf2", "argon2id"), algo

    # --- derive_key: deterministic, salt-sensitive (pbkdf2 path, explicit) ---
    salt = b"1234567890123456"
    k1 = vc.derive_key("master-pw", salt, algo="pbkdf2", iterations=1000)
    k2 = vc.derive_key("master-pw", salt, algo="pbkdf2", iterations=1000)
    assert k1 == k2, "derive_key must be deterministic"
    assert len(k1) == 32
    # default algo=None must match the explicit pbkdf2 path when argon2 is absent
    assert vc.derive_key("master-pw", salt, iterations=1000) == k1
    k3 = vc.derive_key("master-pw", b"abcdefghijklmnop", algo="pbkdf2", iterations=1000)
    assert k1 != k3, "different salt must yield different key"
    k4 = vc.derive_key("master-pw2", salt, algo="pbkdf2", iterations=1000)
    assert k1 != k4, "different password must yield different key"

    # --- algo="argon2id" must raise (not silently downgrade) when unavailable ---
    if vc.kdf_algo() == "pbkdf2":
        try:
            vc.derive_key("pw", salt, algo="argon2id")
            raise AssertionError("argon2id must raise VaultCryptoError when unavailable")
        except vc.VaultCryptoError:
            pass
    else:
        # argon2 installed: sanity-check it is deterministic + 32 bytes.
        a1 = vc.derive_key("pw", salt, algo="argon2id", time_cost=1, memory_kb=64, parallelism=1)
        a2 = vc.derive_key("pw", salt, algo="argon2id", time_cost=1, memory_kb=64, parallelism=1)
        assert a1 == a2 and len(a1) == 32

    # --- unknown algo is rejected ---
    try:
        vc.derive_key("pw", salt, algo="bogus")
        raise AssertionError("unknown algo must raise VaultCryptoError")
    except vc.VaultCryptoError:
        pass

    # --- encrypt / decrypt roundtrip ---
    key = vc.derive_key("pw", salt, algo="pbkdf2", iterations=1000)
    for plain in ("", "hello", "中文账号", "p@ss w0rd!", "x" * 500):
        token = vc.encrypt_field(plain, key)
        assert token.startswith("v1:"), token
        assert vc.decrypt_field(token, key) == plain

    # --- non-v1 token is returned as plaintext ---
    assert vc.decrypt_field("not-a-token", key) == "not-a-token"
    assert vc.decrypt_field("", key) == ""

    # --- tamper / wrong key -> VaultCryptoError ---
    token = vc.encrypt_field("secret", key)
    raw = base64.b64decode(token[3:])
    flipped = bytearray(raw)
    flipped[0] ^= 0xFF
    tampered = "v1:" + base64.b64encode(bytes(flipped)).decode("ascii")
    try:
        vc.decrypt_field(tampered, key)
        raise AssertionError("tampered ciphertext must raise")
    except vc.VaultCryptoError:
        pass
    wrong_key = vc.derive_key("other", salt, algo="pbkdf2", iterations=1000)
    try:
        vc.decrypt_field(token, wrong_key)
        raise AssertionError("wrong key must raise")
    except vc.VaultCryptoError:
        pass

    # --- key check ---
    check = vc.make_key_check(key)
    assert vc.verify_key_check(key, check) is True
    assert vc.verify_key_check(wrong_key, check) is False
    assert vc.decrypt_field(check, key) == "desktidy-vault"

    # --- fake client: upload body carries ciphertext + enc_ver=1 ---
    class _FakeClient(VaultApiClient):
        def __init__(self):
            self.calls = []

        def _request(self, method, path, body=None, *, auth=False, _retry=True):
            self.calls.append((method, path, body))
            return {"id": 7}

    client = _FakeClient()
    payload = {
        "title": "GitHub",
        "username": "alice",
        "password": "s3cret",
        "url": "https://github.com",
        "note": "work",
    }
    client.create_credential(payload, key=key)
    method, path, body = client.calls[-1]
    assert method == "POST" and path == "credentials"
    assert body["enc_ver"] == 1
    assert body["title"] == "GitHub"  # title stays plaintext
    assert body["username"].startswith("v1:")
    assert body["password"].startswith("v1:")
    assert decrypt_field(body["password"], key) == "s3cret"

    # update with title → encrypted; pin-only (no title) → left alone
    client.calls.clear()
    client.update_credential(7, payload, key=key)
    assert client.calls[-1][2]["enc_ver"] == 1
    client.calls.clear()
    client.update_credential(7, {"pinned": True})
    assert "enc_ver" not in client.calls[-1][2]

    # --- _decrypt_item enc_ver branches ---
    enc_item = {
        "id": 1,
        "title": "T",
        "username": vc.encrypt_field("u", key),
        "password": vc.encrypt_field("p", key),
        "url": vc.encrypt_field("", key),
        "note": vc.encrypt_field("", key),
        "enc_ver": 1,
        "pinned": 0,
        "sort_order": 0,
    }
    dec = VaultApiClient._decrypt_item(enc_item, key)
    assert dec["username"] == "u" and dec["password"] == "p"
    legacy = {"id": 2, "title": "L", "username": "plain", "enc_ver": 0}
    assert VaultApiClient._decrypt_item(legacy, key) is legacy
    missing = {"id": 3, "title": "M", "username": "plain"}
    assert VaultApiClient._decrypt_item(missing, key) is missing

    # --- migrate_legacy_to_e2e via fake client (exercises real encrypt path) ---
    class _MigrateClient(VaultApiClient):
        def __init__(self, items):
            self._items = items
            self.puts = []

        def _request(self, method, path, body=None, *, auth=False, _retry=True):
            if method == "GET" and path == "credentials":
                return {"items": self._items}
            if method == "PUT":
                self.puts.append((path, body))
                return {"id": int(path.rsplit("/", 1)[-1])}
            return {}

    mig = _MigrateClient(
        [
            {"id": 1, "title": "A", "username": "u1", "password": "p1", "url": "",
             "note": "", "enc_ver": 0, "pinned": 0},
            {"id": 2, "title": "B", "username": "v1:x", "password": "v1:y", "url": "",
             "note": "", "enc_ver": 1, "pinned": 0},
        ]
    )
    n = migrate_legacy_to_e2e(mig, key)
    assert n == 1
    assert len(mig.puts) == 1
    path, up_payload = mig.puts[0]
    assert path == "credentials/1"
    assert up_payload["enc_ver"] == 1
    assert decrypt_field(up_payload["username"], key) == "u1"

    # --- account_vault master-password session roundtrip (offline) ---
    with tempfile.TemporaryDirectory() as td:
        sess = Path(td) / "account_vault_session.json"
        old = av.SESSION_FILE
        av.SESSION_FILE = sess
        try:
            av.clear_session()
            av.lock()
            assert not av.has_master_password()
            av.save_session(
                api_base="https://example.com/desktidy-vault",
                login="13800138000",
                token="tok",
                user_id=1,
            )
            assert not av.has_master_password()
            av.set_master_password("correct horse battery staple")
            assert av.has_master_password()
            assert av.is_unlocked()
            mk = av.current_key()
            assert mk is not None and len(mk) == 32

            salt_b64, iters, check = av.kdf_params()
            assert salt_b64 and iters == vc.DEFAULT_KDF_ITERATIONS and check
            sess_dict = av.load_session()
            assert sess_dict is not None
            assert sess_dict.get("kdf_salt") == salt_b64
            assert sess_dict.get("key_check") == check
            # set_master_password records the auto-selected algorithm
            assert sess_dict.get("kdf_algo") == vc.kdf_algo()
            assert av.session_kdf_algo() == vc.kdf_algo()

            av.lock()
            assert not av.is_unlocked()
            assert av.current_key() is None
            assert av.unlock("wrong password") is False
            assert not av.is_unlocked()
            assert av.unlock("correct horse battery staple") is True
            assert av.is_unlocked()

            av.clear_session()
            assert not av.has_master_password()
            assert not av.is_unlocked()
        finally:
            av.SESSION_FILE = old

    # --- backward compat: a legacy 200000-iteration session still unlocks ---
    with tempfile.TemporaryDirectory() as td:
        sess = Path(td) / "account_vault_session.json"
        old = av.SESSION_FILE
        av.SESSION_FILE = sess
        try:
            av.clear_session()
            av.lock()
            legacy_salt = vc.new_salt(16)
            legacy_key = vc.derive_key("legacy master pw", legacy_salt, algo="pbkdf2", iterations=200000)
            av._write_session_raw(
                {
                    "api_base": "https://example.com/desktidy-vault",
                    "login": "13800138000",
                    "token": "tok",
                    "user_id": 1,
                    "kdf_salt": base64.b64encode(legacy_salt).decode("ascii"),
                    "kdf_iterations": 200000,
                    "key_check": vc.make_key_check(legacy_key),
                }
            )
            assert av.has_master_password()
            # legacy session has no kdf_algo field → normalized to pbkdf2
            assert av.session_kdf_algo() == "pbkdf2"
            av.lock()
            assert av.unlock("legacy master pw") is True
            assert av.is_unlocked()
            _, iters, _ = av.kdf_params()
            assert iters == 200000, "legacy 200k session must keep its iteration count"
            av.clear_session()
        finally:
            av.SESSION_FILE = old

    print(f"selftest_vault_crypto: OK (gcm_backend={vc.gcm_backend()}, kdf_algo={vc.kdf_algo()})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
