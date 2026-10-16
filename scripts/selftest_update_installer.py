"""Regression: update installer — asset/checksum selection, sha256, download, host allow-list.

Offline by design: ``download_asset`` is exercised through a monkeypatched
``update_installer._open``, never a real socket. No Qt / GUI imports here.
"""

from __future__ import annotations

import hashlib
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

passes: list[str] = []
failures: list[tuple[str, str]] = []


def run(name: str, fn) -> None:
    try:
        fn()
        passes.append(name)
        print(f"  OK  {name}")
    except Exception as exc:  # noqa: BLE001
        failures.append((name, str(exc)))
        print(f"  FAIL {name}: {exc}")


def _sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def test_select_installer_asset_basic() -> None:
    from src.update_installer import select_installer_asset

    release = {
        "assets": [
            {
                "name": "DeskTidy_Setup_4.0.30.exe",
                "browser_download_url": "https://github.com/a/b/releases/download/v4.0.30/DeskTidy_Setup_4.0.30.exe",
                "size": 1234,
            },
            {"name": "README.txt", "browser_download_url": "https://github.com/a/b/x", "size": 10},
        ]
    }
    got = select_installer_asset(release)
    assert got is not None
    assert got["name"] == "DeskTidy_Setup_4.0.30.exe"
    assert got["url"].startswith("https://github.com/")
    assert got["size"] == 1234


def test_select_installer_asset_no_match() -> None:
    from src.update_installer import select_installer_asset

    assert select_installer_asset({"assets": []}) is None
    assert select_installer_asset({}) is None
    assert select_installer_asset(None) is None  # type: ignore[arg-type]
    assert (
        select_installer_asset(
            {"assets": [{"name": "readme.md", "browser_download_url": "https://github.com/a/b", "size": 1}]}
        )
        is None
    )


def test_select_installer_asset_multi_prefers_largest() -> None:
    from src.update_installer import select_installer_asset

    release = {
        "assets": [
            {"name": "DeskTidy_Setup_4.0.29.exe", "browser_download_url": "https://github.com/a/old", "size": 900},
            {"name": "DeskTidy_Setup_4.0.30.exe", "browser_download_url": "https://github.com/a/new", "size": 1500},
            {"name": "other.exe", "browser_download_url": "https://github.com/a/other", "size": 99999},
        ]
    }
    got = select_installer_asset(release)
    assert got is not None
    assert got["name"] == "DeskTidy_Setup_4.0.30.exe"
    assert got["size"] == 1500


def test_find_checksum_from_body() -> None:
    from src.update_installer import find_checksum

    digest = "a" * 64
    body = f"DeskTidy_Setup_4.0.30.exe\nsha256: {digest}\n"
    release = {"body": body, "assets": []}
    assert find_checksum(release, "DeskTidy_Setup_4.0.30.exe") == digest


def test_find_checksum_no_match() -> None:
    from src.update_installer import find_checksum

    assert find_checksum({"body": "", "assets": []}, "DeskTidy_Setup_4.0.30.exe") is None
    assert find_checksum({"body": "no hash here", "assets": []}, "x.exe") is None
    assert find_checksum({}, "x.exe") is None
    assert find_checksum(None, "x.exe") is None  # type: ignore[arg-type]


def test_find_checksum_from_sidecar_asset() -> None:
    import src.update_installer as ui

    digest = "b" * 64
    release = {
        "body": "",
        "assets": [
            {
                "name": "DeskTidy_Setup_4.0.30.exe.sha256",
                "browser_download_url": "https://github.com/a/checksum",
            }
        ],
    }
    orig = ui._fetch_text
    ui._fetch_text = lambda url, timeout=15.0: f"{digest}  DeskTidy_Setup_4.0.30.exe\n"  # type: ignore[assignment]
    try:
        assert ui.find_checksum(release, "DeskTidy_Setup_4.0.30.exe") == digest
    finally:
        ui._fetch_text = orig  # type: ignore[assignment]


def test_verify_sha256() -> None:
    from src.update_installer import verify_sha256

    data = b"DeskTidy installer payload\x00\x01\x02"
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "setup.exe"
        p.write_bytes(data)
        assert verify_sha256(p, _sha256_hex(data)) is True
        assert verify_sha256(p, "0" * 64) is False
        assert verify_sha256(p, "") is True  # empty checksum → pass-through
        assert verify_sha256(p, None) is True  # type: ignore[arg-type]
        assert verify_sha256(Path(td) / "missing.exe", _sha256_hex(data)) is False


class _FakeResp:
    """Minimal urllib-response stand-in for ``update_installer._open``."""

    def __init__(self, data: bytes):
        self._data = data
        self.headers = {"Content-Length": str(len(data))}

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def read(self, size: int = -1) -> bytes:
        if size < 0:
            out = self._data
            self._data = b""
            return out
        out = self._data[:size]
        self._data = self._data[size:]
        return out


class _BoomResp(_FakeResp):
    def read(self, size: int = -1) -> bytes:
        raise OSError("connection reset")


def test_download_asset_part_to_final() -> None:
    import src.update_installer as ui

    payload = b"PAYLOAD-" + b"x" * 100
    calls: list[tuple[int, int]] = []
    orig = ui._open
    ui._open = lambda req, timeout: _FakeResp(payload)  # type: ignore[assignment]
    try:
        with tempfile.TemporaryDirectory() as td:
            dest = Path(td)
            path = ui.download_asset(
                "https://github.com/a/b/releases/download/v9/DeskTidy_Setup_9.9.9.exe",
                dest,
                progress=lambda d, t: calls.append((d, t)),
            )
            assert path.exists()
            assert path.read_bytes() == payload
            assert not list(dest.glob("*.part"))
            assert path.name == "DeskTidy_Setup_9.9.9.exe"
    finally:
        ui._open = orig  # type: ignore[assignment]
    assert calls
    assert calls[-1] == (len(payload), len(payload))


def test_download_asset_cleans_part_on_error() -> None:
    import src.update_installer as ui

    orig = ui._open
    ui._open = lambda req, timeout: _BoomResp(b"some bytes")  # type: ignore[assignment]
    try:
        with tempfile.TemporaryDirectory() as td:
            dest = Path(td)
            try:
                ui.download_asset(
                    "https://github.com/a/b/releases/download/v9/DeskTidy_Setup_9.9.9.exe",
                    dest,
                )
                raise AssertionError("expected UpdateInstallError")
            except ui.UpdateInstallError as exc:
                assert exc.stage == "download"
            assert not list(dest.glob("*.part"))
            assert not list(dest.glob("*.exe"))
    finally:
        ui._open = orig  # type: ignore[assignment]


def test_host_allowlist() -> None:
    import src.update_installer as ui

    # Valid hosts pass validation.
    for url in (
        "https://github.com/a/b/releases/download/v1/x.exe",
        "https://objects.githubusercontent.com/x.exe",
        "https://raw.githubusercontent.com/x/x.exe",
    ):
        assert ui._validate_url(url) == url

    # http and non-allowlisted hosts are rejected.
    for bad in (
        "http://github.com/a/x.exe",
        "https://evil.com/x.exe",
        "https://github.com.evil.com/x.exe",
        "https://example.invalid/x.exe",
    ):
        try:
            ui._validate_url(bad)
            raise AssertionError(f"expected rejection for {bad}")
        except ui.UpdateInstallError as exc:
            assert exc.stage == "download"


def test_download_asset_rejects_bad_url_before_open() -> None:
    import src.update_installer as ui

    opened = []

    def fake_open(req, timeout):
        opened.append(req.full_url)
        return _FakeResp(b"never")

    orig = ui._open
    ui._open = fake_open  # type: ignore[assignment]
    try:
        with tempfile.TemporaryDirectory() as td:
            try:
                ui.download_asset("http://github.com/a/x.exe", Path(td))
                raise AssertionError("expected rejection")
            except ui.UpdateInstallError as exc:
                assert exc.stage == "download"
            try:
                ui.download_asset("https://attacker.io/x.exe", Path(td))
                raise AssertionError("expected rejection")
            except ui.UpdateInstallError as exc:
                assert exc.stage == "download"
    finally:
        ui._open = orig  # type: ignore[assignment]
    assert opened == [], "bad URLs must be rejected before any network open"


def test_cleanup_old_installers() -> None:
    from src.update_installer import cleanup_old_installers

    with tempfile.TemporaryDirectory() as td:
        dest = Path(td)
        old_exe = dest / "DeskTidy_Setup_1.0.0.exe"
        part = dest / "DeskTidy_Setup_2.0.0.exe.part"
        keep_exe = dest / "DeskTidy_Setup_9.9.9.exe"
        unrelated = dest / "notes.txt"
        old_exe.write_bytes(b"old")
        part.write_bytes(b"part")
        keep_exe.write_bytes(b"keep")
        unrelated.write_bytes(b"notes")

        removed = cleanup_old_installers(dest, keep=keep_exe)
        assert removed == 2
        assert not old_exe.exists()
        assert not part.exists()
        assert keep_exe.exists()
        assert unrelated.exists()


def test_update_install_error_stage() -> None:
    from src.update_installer import UpdateInstallError

    err = UpdateInstallError("boom", stage="verify")
    assert err.stage == "verify"
    assert "boom" in str(err)


def main() -> int:
    print("DeskTidy update installer regression")
    run("select_installer_asset 基础", test_select_installer_asset_basic)
    run("select_installer_asset 无匹配", test_select_installer_asset_no_match)
    run("select_installer_asset 多资产取最大", test_select_installer_asset_multi_prefers_largest)
    run("find_checksum 从 body", test_find_checksum_from_body)
    run("find_checksum 无匹配", test_find_checksum_no_match)
    run("find_checksum 从 .sha256 资产", test_find_checksum_from_sidecar_asset)
    run("verify_sha256", test_verify_sha256)
    run("download_asset .part→改名", test_download_asset_part_to_final)
    run("download_asset 出错清理 .part", test_download_asset_cleans_part_on_error)
    run("host 白名单", test_host_allowlist)
    run("坏 URL 在联网前拒绝", test_download_asset_rejects_bad_url_before_open)
    run("cleanup_old_installers", test_cleanup_old_installers)
    run("UpdateInstallError.stage", test_update_install_error_stage)

    print(f"\n通过 {len(passes)}  失败 {len(failures)}")
    if failures:
        for name, err in failures:
            print(f"  - {name}: {err}")
        return 1
    print("全部通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
