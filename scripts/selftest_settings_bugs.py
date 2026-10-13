"""Regression: settings load / page indicator / hosted save / autostart migrate."""

from __future__ import annotations

import inspect
import json
import sys
import tempfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

passes: list[str] = []
failures: list[tuple[str, str]] = []


def run(name: str, fn) -> None:
    try:
        fn()
        passes.append(name)
        print(f"  OK  {name}")
    except Exception as exc:
        failures.append((name, str(exc)))
        print(f"  FAIL {name}: {exc}")


def test_startup_does_not_force_page_indicator_on() -> None:
    from src.app import DeskTidyApp

    src = inspect.getsource(DeskTidyApp._run_startup_overlays)
    assert '["show_page_indicator"] = True' in src or "show_page_indicator" in src
    # Must only heal a missing key — never rewrite an explicit False.
    assert 'if "show_page_indicator" not in self.settings' in src, src
    assert "is not True" not in src


def test_load_settings_non_dict_json_recovers() -> None:
    import src.settings as settings_mod

    with tempfile.TemporaryDirectory(prefix="desktidy_nondict_") as td:
        app_dir = Path(td)
        settings_file = app_dir / "settings.json"
        settings_file.write_text("[]", encoding="utf-8")
        with (
            mock.patch.object(settings_mod, "APP_DIR", app_dir),
            mock.patch.object(settings_mod, "SETTINGS_FILE", settings_file),
            mock.patch.object(settings_mod, "SETTINGS_BACKUP_DIR", app_dir / "backups"),
        ):
            loaded = settings_mod.load_settings()
        assert isinstance(loaded, dict)
        assert loaded.get("organize_mode") == "virtual"
        bak = settings_file.with_suffix(".json.bak")
        assert bak.is_file()
        assert bak.read_text(encoding="utf-8").strip() == "[]"


def test_hosted_save_prefers_live_bridge() -> None:
    import src.settings as settings_mod
    from src import win_shell

    with tempfile.TemporaryDirectory(prefix="desktidy_hosted_") as td:
        app_dir = Path(td)
        settings_file = app_dir / "settings.json"
        disk = {
            "hotkeys": {"a": "1"},
            "desktop_pages": ["工作"],
            "organize_rules": {},
            "theme": "sky",
            "fences": [{"id": "disk", "name": "磁盘"}],
            "hosted_namespace_icons": {},
        }
        settings_file.write_text(json.dumps(disk, ensure_ascii=False), encoding="utf-8")
        live = {
            "hotkeys": {"a": "1"},
            "desktop_pages": ["工作"],
            "organize_rules": {},
            "theme": "sky",
            "fences": [{"id": "live", "name": "内存"}],
            "hosted_namespace_icons": {},
        }

        class FakeApp:
            _desktidy_settings = live

        with (
            mock.patch.object(settings_mod, "APP_DIR", app_dir),
            mock.patch.object(settings_mod, "SETTINGS_FILE", settings_file),
            mock.patch.object(settings_mod, "SETTINGS_BACKUP_DIR", app_dir / "backups"),
            mock.patch("PyQt6.QtWidgets.QApplication.instance", return_value=FakeApp()),
        ):
            win_shell._save_hosted_namespace_store(
                {"{00000000-0000-0000-0000-000000000000}": {"name": "回收站"}}
            )
            settings_mod.flush_settings()

        written = json.loads(settings_file.read_text(encoding="utf-8"))
        assert written["fences"][0]["id"] == "live", written["fences"]
        assert "{00000000-0000-0000-0000-000000000000}" in written["hosted_namespace_icons"]
        assert live["hosted_namespace_icons"]


def test_migrate_respects_auto_start_false() -> None:
    import src.settings as settings_mod

    with tempfile.TemporaryDirectory(prefix="desktidy_autostart_") as td:
        root = Path(td)
        startup = root / "Startup"
        startup.mkdir()
        (startup / "Desktidy.lnk").write_text("legacy", encoding="utf-8")
        app_dir = root / "app"
        app_dir.mkdir()
        settings_file = app_dir / "settings.json"
        settings_file.write_text(
            json.dumps(
                {
                    "hotkeys": {},
                    "desktop_pages": [],
                    "organize_rules": {},
                    "theme": "sky",
                    "auto_start": False,
                }
            ),
            encoding="utf-8",
        )
        calls: list[bool] = []

        with (
            mock.patch.object(settings_mod, "SETTINGS_FILE", settings_file),
            mock.patch.object(settings_mod, "_startup_dir", return_value=startup),
            mock.patch.object(settings_mod, "is_auto_start_enabled", return_value=False),
            mock.patch.object(
                settings_mod, "set_auto_start", side_effect=lambda enabled: calls.append(enabled)
            ),
            mock.patch.object(settings_mod, "_remove_registry_autostart"),
            mock.patch.object(settings_mod, "remove_desktop_guard_autostart"),
            mock.patch.object(
                settings_mod, "_desktop_guard_link_path", return_value=startup / "guard.lnk"
            ),
        ):
            settings_mod.migrate_legacy_autostart()

        assert True not in calls, calls
        assert not (startup / "Desktidy.lnk").exists()


def test_merge_defaults_keeps_dict_on_bad_type() -> None:
    from src.settings import _merge_defaults

    defaults = {"screen_record": {"enabled": False, "fps": 30}, "theme": "sky"}
    user = {"screen_record": "broken", "theme": "dark"}
    merged = _merge_defaults(user, defaults)
    assert merged["screen_record"] == {"enabled": False, "fps": 30}
    assert merged["theme"] == "dark"


def test_storage_origins_atomic_write_and_record() -> None:
    import src.settings as settings_mod

    src = inspect.getsource(settings_mod.save_storage_origins)
    assert "os.replace" in src or "replace(" in src
    assert "_ORIGINS_LOCK" in inspect.getsource(settings_mod)

    with tempfile.TemporaryDirectory(prefix="desktidy_origins_") as td:
        app_dir = Path(td)
        storage = app_dir / "storage"
        storage.mkdir()
        origins_file = app_dir / "storage_origins.json"
        with (
            mock.patch.object(settings_mod, "APP_DIR", app_dir),
            mock.patch.object(settings_mod, "_ORIGINS_FILE", origins_file),
            mock.patch.object(
                settings_mod, "get_fence_storage_root", return_value=storage
            ),
        ):
            a = storage / "a.txt"
            b = storage / "b.txt"
            a.write_text("a", encoding="utf-8")
            b.write_text("b", encoding="utf-8")
            desk = app_dir / "Desktop"
            desk.mkdir()
            settings_mod.record_storage_origin(a, desk)
            settings_mod.record_storage_origin(b, desk)
            data = settings_mod.load_storage_origins()
        assert "a.txt" in data and "b.txt" in data


def main() -> int:
    print("=== settings bug regressions ===")
    run("startup does not force page indicator on", test_startup_does_not_force_page_indicator_on)
    run("load_settings non-dict JSON recovers", test_load_settings_non_dict_json_recovers)
    run("hosted save prefers live bridge", test_hosted_save_prefers_live_bridge)
    run("migrate respects auto_start false", test_migrate_respects_auto_start_false)
    run("merge_defaults keeps dict on bad type", test_merge_defaults_keeps_dict_on_bad_type)
    run("storage origins atomic write and record", test_storage_origins_atomic_write_and_record)
    print(f"\n=== SUMMARY passed={len(passes)} failed={len(failures)} ===")
    for name, err in failures:
        print(f"  - {name}: {err}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
