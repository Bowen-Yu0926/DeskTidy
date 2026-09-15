"""Selftest: account vault helpers (no live network / GUI)."""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def main() -> int:
    from src import account_vault as av
    from src.account_vault_api import VaultApiClient, VaultApiError

    # --- normalize_login ---
    assert av.normalize_login("13800138000") == ("13800138000", "phone")
    assert av.normalize_login("  Foo@Example.COM ") == ("foo@example.com", "email")
    try:
        av.normalize_login("not-an-account")
        raise AssertionError("expected ValueError")
    except ValueError:
        pass

    assert "****" in av.mask_login("13800138000")
    assert "***@" in av.mask_login("ab@example.com")

    # --- settings ---
    s: dict = {}
    cfg = av.account_vault_settings(s)
    assert cfg["enabled"] is False
    assert "desktidy-vault" in cfg["api_base"]
    assert cfg["refresh_interval_hours"] == av.DEFAULT_REFRESH_INTERVAL_HOURS
    assert av.account_vault_enabled(s) is False
    cfg["enabled"] = True
    assert av.account_vault_enabled(s) is True
    base = av.account_vault_api_base(s)
    assert base.endswith("/")
    assert av.account_vault_refresh_interval_hours(s) == 2
    assert av.account_vault_refresh_interval_ms(s) == 2 * 3600 * 1000
    cfg["refresh_interval_hours"] = 999
    assert av.account_vault_refresh_interval_hours(s) == av.MAX_REFRESH_INTERVAL_HOURS
    cfg["refresh_interval_hours"] = 0
    assert av.account_vault_refresh_interval_hours(s) == av.MIN_REFRESH_INTERVAL_HOURS
    cfg["refresh_interval_hours"] = 2

    # --- session roundtrip ---
    with tempfile.TemporaryDirectory() as td:
        sess_path = Path(td) / "account_vault_session.json"
        old = av.SESSION_FILE
        av.SESSION_FILE = sess_path
        try:
            av.clear_session()
            assert av.load_session() is None
            av.save_session(
                api_base=base,
                login="13800138000",
                token="tok_test",
                user_id=7,
                display_name="小鱼",
            )
            loaded = av.load_session()
            assert loaded is not None
            assert loaded["token"] == "tok_test"
            assert loaded["login"] == "13800138000"
            assert loaded["display_name"] == "小鱼"
            assert loaded["user_id"] == 7
            assert av.session_display_name(loaded) == "小鱼"
            av.clear_session()
            assert av.load_session() is None
        finally:
            av.SESSION_FILE = old

    assert av.normalize_display_name("  阿 波  ") == "阿 波"
    try:
        av.normalize_display_name("a")
        raise AssertionError("expected ValueError")
    except ValueError:
        pass

    # --- launcher position helpers (Doubao-style float) ---
    s_pos: dict = {"account_vault": {"enabled": True}}
    assert av.account_vault_launcher_pos(s_pos) is None
    av.set_account_vault_launcher_pos(s_pos, 120, 340)
    pos = av.account_vault_launcher_pos(s_pos)
    assert pos == (120, 340)
    assert not hasattr(av, "credential_note_path")
    assert not hasattr(av, "write_credential_note")

    # --- import / export ---
    payload = av.export_credentials_payload(
        [
            {
                "title": "G",
                "username": "u",
                "password": "p",
                "url": "https://x",
                "note": "",
                "pinned": True,
            }
        ]
    )
    assert payload["format"] == "desktidy-vault"
    assert len(payload["items"]) == 1
    assert payload["items"][0]["pinned"] is True
    parsed = av.parse_credentials_import(payload)
    assert parsed[0]["title"] == "G" and parsed[0]["password"] == "p"
    assert parsed[0]["pinned"] is True
    parsed_default = av.parse_credentials_import(
        {"format": "desktidy-vault", "items": [{"title": "N", "username": "x"}]}
    )
    assert parsed_default[0]["pinned"] is False
    trimmed = av.parse_credentials_import(
        {
            "format": "desktidy-vault",
            "items": [
                {
                    "title": "  T  ",
                    "username": " u ",
                    "password": " p ",
                    "url": " https://x ",
                    "note": " n ",
                }
            ],
        }
    )
    assert trimmed[0] == {
        "title": "T",
        "username": "u",
        "password": "p",
        "url": "https://x",
        "note": "n",
        "pinned": False,
    }

    # --- reorder within pin group ---
    items = [
        {"id": 1, "pinned": True, "title": "a"},
        {"id": 2, "pinned": True, "title": "b"},
        {"id": 3, "pinned": False, "title": "c"},
        {"id": 4, "pinned": False, "title": "d"},
    ]
    assert av.reorder_credential_ids(items, 2, 1) == [2, 1, 3, 4]
    assert av.reorder_credential_ids(items, 4, 3) == [1, 2, 4, 3]
    assert av.reorder_credential_ids(items, 3, 1) is None
    assert av.reorder_credential_ids(items, 1, 1) is None
    with tempfile.TemporaryDirectory() as td:
        exp = Path(td) / "out.json"
        av.write_credentials_export_file(exp, [{"title": "A", "username": "1"}])
        back = av.read_credentials_import_file(exp)
        assert back[0]["title"] == "A"

    # API client URL building / health probe helper (no live host required)
    client = VaultApiClient("https://example.com/desktidy-vault")
    assert client.api_base.endswith("/")
    assert client._url("auth/login").endswith("auth/login")
    assert callable(VaultApiClient.health)
    from src.account_vault_api import probe_vault_reachable

    ok_probe, msg_probe = probe_vault_reachable("http://127.0.0.1:1/", timeout=1.0)
    assert ok_probe is False
    assert msg_probe

    # Simulate error payload shape
    err = VaultApiError("账号或密码错误", error="bad_credentials", status=401)
    assert err.error == "bad_credentials"
    assert "密码" in err.message

    # vault uses Doubao-style launcher — never on float bar
    from src.desktop_pet import float_bar_tool_flags

    flags = float_bar_tool_flags({"account_vault": {"enabled": True}})
    assert flags.get("vault") is False
    flags2 = float_bar_tool_flags({"account_vault": {"enabled": False}})
    assert flags2.get("vault") is False

    # --- async runner delivers on UI thread ---
    from PyQt6.QtCore import QEvent, QPoint, QPointF, QRect, QSize, QTimer
    from PyQt6.QtWidgets import QApplication
    from src.ui.account_vault_async import VaultAsyncRunner

    app = QApplication.instance() or QApplication([])
    runner = VaultAsyncRunner()
    box: dict = {}

    def work() -> str:
        return "vault-async-ok"

    def on_ok(result: object) -> None:
        box["ok"] = result
        app.quit()

    def on_err(exc: BaseException) -> None:
        box["err"] = exc
        app.quit()

    assert runner.run(work, on_ok=on_ok, on_err=on_err, busy_text="t")
    QTimer.singleShot(5000, app.quit)
    app.exec()
    assert box.get("ok") == "vault-async-ok", box
    assert not runner.busy

    # Queued follow-up runs after the in-flight job.
    box2: dict = {}
    order: list[str] = []

    def work_a() -> str:
        order.append("a")
        return "A"

    def work_b() -> str:
        order.append("b")
        return "B"

    def on_a(result: object) -> None:
        box2["a"] = result

    def on_b(result: object) -> None:
        box2["b"] = result
        app.quit()

    assert runner.run(work_a, on_ok=on_a, on_err=on_err, busy_text="a")
    assert runner.run(work_b, on_ok=on_b, on_err=on_err, busy_text="b") is False
    QTimer.singleShot(5000, app.quit)
    app.exec()
    assert box2.get("a") == "A" and box2.get("b") == "B", box2
    assert order == ["a", "b"], order

    # panel docks beside buoy (left preferred)
    from src.ui.account_vault_widget import panel_pos_beside_anchor

    pos = panel_pos_beside_anchor(
        QRect(900, 400, 56, 56), QSize(360, 480), QRect(0, 0, 1920, 1080)
    )
    assert pos.x() + 360 + 12 == 900
    pos_right = panel_pos_beside_anchor(
        QRect(20, 400, 56, 56), QSize(360, 480), QRect(0, 0, 1920, 1080)
    )
    assert pos_right.x() == 20 + 56 + 12

    # copy-field hover accent (fixed tip lives on the panel title bar)
    from PyQt6.QtCore import QPointF
    from PyQt6.QtGui import QEnterEvent
    from src.ui.account_vault_widget import VaultCredentialRow

    row = VaultCredentialRow(
        {
            "id": 1,
            "title": "示意",
            "username": "alice",
            "password": "secret",
            "url": "https://example.com",
            "note": "",
        }
    )
    assert row.user_lbl.toolTip() == "双击复制账号"
    assert row.pass_lbl.toolTip() == "双击复制密码"
    assert not hasattr(row, "_hint_lbl")
    enter = QEnterEvent(QPointF(0, 0), QPointF(0, 0), QPointF(0, 0))
    row.user_lbl.enterEvent(enter)
    assert row.user_lbl.property("copyHot") == "true"
    leave = QEvent(QEvent.Type.Leave)
    row.user_lbl.leaveEvent(leave)
    assert row.user_lbl.property("copyHot") == "false"
    row.close()
    row.deleteLater()

    print("selftest_account_vault: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
