"""Regression: folder bg-move UI marshal + missing-path RMB fallback."""

from __future__ import annotations

import inspect
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def test_call_on_main_thread_from_worker() -> None:
    from PyQt6.QtCore import QThread
    from PyQt6.QtWidgets import QApplication

    from src.qt_main_thread import call_on_main_thread, ensure_main_thread_bridge

    app = QApplication.instance() or QApplication([])
    ensure_main_thread_bridge(app)
    seen: list[bool] = []

    def _mark() -> None:
        seen.append(QThread.currentThread() is app.thread())

    def _worker() -> None:
        time.sleep(0.05)
        call_on_main_thread(_mark)

    threading.Thread(target=_worker, daemon=True).start()
    deadline = time.time() + 5.0
    while time.time() < deadline and not seen:
        app.processEvents()
        time.sleep(0.01)
    assert seen and seen[0] is True, f"expected GUI-thread callback, got {seen}"


def test_background_fs_move_uses_main_thread_bridge() -> None:
    from src.ui import fence_icon_item as fii

    bg = inspect.getsource(fii._start_background_fs_move)
    assert "_start_background_fs_transfer" in bg
    transfer = inspect.getsource(fii._start_background_fs_transfer)
    assert "call_on_main_thread" in transfer
    assert "ensure_main_thread_bridge" in transfer
    body = transfer.split('"""', 2)[-1] if '"""' in transfer else transfer
    assert "QTimer.singleShot" not in body
    paste = inspect.getsource(fii._start_background_fs_paste)
    paste_body = paste.split('"""', 2)[-1] if '"""' in paste else paste
    assert "call_on_main_thread" in paste
    assert "ensure_main_thread_bridge" in paste
    assert "QTimer.singleShot" not in paste_body
    pub = inspect.getsource(fii._move_public_into_folder)
    assert "_snapshot_public_float_entry" in pub
    assert "restore_snap" in inspect.getsource(fii._finalize_public_folder_move)
    virt = inspect.getsource(fii._move_virtual_into_folder)
    assert "moved_dest=None" in virt and "_finalize_virtual_folder_move" in virt
    after = inspect.getsource(fii.after_shell_file_menu)
    assert "_offer_missing_icon_cleanup" in after
    offer = inspect.getsource(fii._offer_missing_icon_cleanup)
    assert "移除失效图标" in offer


def main() -> None:
    test_background_fs_move_uses_main_thread_bridge()
    print("OK contracts")
    test_call_on_main_thread_from_worker()
    print("OK call_on_main_thread")


if __name__ == "__main__":
    main()
