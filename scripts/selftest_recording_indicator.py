"""Isolated smoke for recording indicator (run separately from full suite)."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def main() -> int:
    from PyQt6.QtWidgets import QApplication

    from src.ui import recording_indicator as ri

    app = QApplication.instance() or QApplication([])
    clicks: list[int] = []
    tip = ri.show_recording_indicator(lambda: clicks.append(1))
    assert tip.isVisible(), "indicator not visible"
    assert tip._timer_lbl.text() == "00:00"
    assert "结束录制" in tip._stop_btn.text()
    # Border strips are created for each screen.
    assert hasattr(ri, "_BorderStrip")
    # HiDPI: physical capture region must map to Qt logical geometry before move().
    from PyQt6.QtGui import QGuiApplication

    from src.screen_record_manager import qt_geometry_for_capture_region

    primary = QGuiApplication.primaryScreen()
    assert primary is not None
    logical = primary.geometry()
    mapped = qt_geometry_for_capture_region(
        {
            "left": int(logical.x()),
            "top": int(logical.y()),
            "width": int(logical.width()),
            "height": int(logical.height()),
        }
    )
    assert mapped is not None
    tip2 = ri.show_recording_indicator(
        lambda: None,
        capture_region={
            "left": int(logical.x()),
            "top": int(logical.y()),
            "width": int(logical.width()),
            "height": int(logical.height()),
            "label": "主屏幕",
        },
    )
    tip2._place_chrome()
    pos = tip2._chrome.pos()
    assert logical.x() <= pos.x() <= logical.x() + logical.width()
    assert logical.y() <= pos.y() <= logical.y() + logical.height()
    assert hasattr(tip2, "_ensure_chrome_topmost")
    tip._handle_stop()
    tip._handle_stop()
    for _ in range(20):
        app.processEvents()
    assert clicks == [1], clicks
    assert tip._stopping is True
    assert tip._stop_btn.text() == "结束中…"
    ri.hide_recording_indicator()
    for _ in range(5):
        app.processEvents()
    print("recording_indicator_smoke OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
