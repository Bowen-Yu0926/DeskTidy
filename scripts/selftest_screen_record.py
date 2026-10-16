"""Self-test for segmented screen recording (C2: pause/resume, quality, audio).

Run:  workdir=E:\\Desktidy
      $env:DESKTIDY_SELFTEST=1; python scripts/selftest_screen_record.py

Covers:
  1. quality preset -> encoder args mapping (pure functions)
  2. segment list -> concat command / playlist construction (pure, no FFmpeg)
  3. list_audio_devices() -> [] when ffmpeg missing / no devices (never raises)
  4. state machine start -> pause -> resume -> stop segment count (fake ffmpeg)

No real ffmpeg is spawned and no real recording runs.
"""

from __future__ import annotations

import os
import sys
import tempfile
import time
from pathlib import Path

os.environ.setdefault("DESKTIDY_SELFTEST", "1")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def _pump(app, cond, timeout: float = 8.0) -> bool:
    """Drive the Qt event loop (queued cross-thread signals) until cond()."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        app.processEvents()
        if cond():
            return True
        time.sleep(0.02)
    return False


def test_quality_mapping() -> None:
    import src.screen_record_manager as m

    assert m.normalize_quality_preset("high") == "high"
    assert m.normalize_quality_preset("HIGH") == "high"
    assert m.normalize_quality_preset("low") == "low"
    assert m.normalize_quality_preset("medium") == "medium"
    assert m.normalize_quality_preset(None) == "medium"
    assert m.normalize_quality_preset("garbage") == "medium"

    assert m.quality_preset_params("high") == {"crf": "18", "mf_quality": "85"}
    assert m.quality_preset_params("medium") == {"crf": "23", "mf_quality": "70"}
    assert m.quality_preset_params("low") == {"crf": "28", "mf_quality": "55"}

    # libx264 -> -crf
    args = m.build_encoder_args("libx264", quality_preset="high")
    assert args[args.index("-crf") + 1] == "18"
    args = m.build_encoder_args("libx264", quality_preset="low")
    assert args[args.index("-crf") + 1] == "28"
    # h264_mf -> -quality
    args = m.build_encoder_args("h264_mf", quality_preset="low")
    assert args[args.index("-quality") + 1] == "55"
    args = m.build_encoder_args("h264_mf", quality_preset="high")
    assert args[args.index("-quality") + 1] == "85"

    print("test_quality_mapping OK")


def test_concat_commands() -> None:
    import src.screen_record_manager as m

    segs = [Path("C:/rec/a.part001.mp4"), Path("C:/rec/a.part002.mp4")]
    content = m.build_concat_list_content(segs)
    assert content == "file 'C:/rec/a.part001.mp4'\nfile 'C:/rec/a.part002.mp4'\n", repr(content)

    out = Path("C:/rec/a.mp4")
    cmd = m.build_concat_command("ffmpeg", Path("C:/rec/list.txt"), out, has_audio=False)
    assert cmd[0] == "ffmpeg"
    assert "-f" in cmd and "concat" in cmd
    assert "-c" in cmd and "copy" in cmd
    assert "-c:a" not in cmd
    assert cmd[-1] == str(out)

    cmd = m.build_concat_command("ffmpeg", Path("C:/rec/list.txt"), out, has_audio=True)
    assert "-c:a" in cmd and "aac" in cmd
    assert "copy" not in cmd

    print("test_concat_commands OK")


def test_list_audio_devices() -> None:
    import src.screen_record_manager as m

    # No ffmpeg -> []
    assert m.list_audio_devices(r"E:\nonexistent\ffmpeg.exe") == []

    # find_ffmpeg returns None -> []
    orig_find = m.find_ffmpeg
    m.find_ffmpeg = lambda: None
    try:
        assert m.list_audio_devices() == []
    finally:
        m.find_ffmpeg = orig_find

    # Simulate a device-less / dshow-less ffmpeg (empty output) -> []
    class _Done:
        returncode = 0
        stdout = b""
        stderr = b""
    orig_run = m.subprocess.run
    orig_path_isfile = m.Path.is_file
    try:
        m.subprocess.run = lambda *a, **k: _Done()
        # Force is_file to True so the probe proceeds.
        m.Path.is_file = lambda self: True
        assert m.list_audio_devices("ffmpeg") == []
    finally:
        m.subprocess.run = orig_run
        m.Path.is_file = orig_path_isfile

    print("test_list_audio_devices OK")


def test_state_machine() -> None:
    import src.screen_record_manager as m
    import src.i18n as i18n

    from PyQt6.QtWidgets import QApplication

    tmp = Path(tempfile.mkdtemp(prefix="desktidy_srtest_"))

    # ---- monkeypatch module-level fakes -------------------------------------
    orig_popen = m.subprocess.Popen
    orig_find = m.find_ffmpeg
    orig_probe = m.probe_preferred_encoder
    orig_resolve = m.resolve_capture_region
    orig_write_state = m._write_state
    orig_read_state = m._read_state
    orig_clear_state = m._clear_state
    orig_merge = m._merge_segments

    class _FakeStdin:
        def __init__(self) -> None:
            self.data = b""

        def write(self, data: bytes) -> None:
            self.data += data

        def flush(self) -> None:
            pass

        def close(self) -> None:
            pass

    _fake_seq = [1000]

    class _FakeProc:
        def __init__(self, cmd, **kw):
            self.cmd = cmd
            self.pid = _fake_seq[0]
            _fake_seq[0] += 1
            out = Path(cmd[-1])
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(b"\x00" * 256)
            self.stdin = _FakeStdin()
            self.finished = False

        def poll(self):
            return 0 if self.finished else None

        def wait(self, timeout=None):
            self.finished = True
            return 0

    m.subprocess.Popen = _FakeProc
    m.find_ffmpeg = lambda: "fake-ffmpeg"
    m.probe_preferred_encoder = lambda ffmpeg: "libx264"
    m._preferred_encoder = "libx264"
    m.resolve_capture_region = lambda capture: {
        "left": 0, "top": 0, "width": 640, "height": 480, "label": "主屏幕",
    }
    m._write_state = lambda *a, **k: None
    m._read_state = lambda: {}
    m._clear_state = lambda: None
    m.ScreenRecordManager._warm_ffmpeg = lambda self: None
    m.ScreenRecordManager._announce_recording_active = lambda self: None
    m.ScreenRecordManager._hide_recording_chrome = lambda self: None
    m.ScreenRecordManager._sync_indicator_pause = lambda self: None

    merged = {}

    def _fake_merge(segments, final, *, has_audio, ffmpeg):
        merged["segments"] = list(segments)
        merged["final"] = final
        merged["has_audio"] = has_audio
        final.write_bytes(b"\x00" * 128)
        return final, True, ""

    m._merge_segments = _fake_merge

    def _fake_keep(parent, path, size_mb):
        return True, Path(path).stem

    orig_keep = i18n.ask_keep_recording_with_name
    i18n.ask_keep_recording_with_name = _fake_keep

    app = QApplication.instance() or QApplication([])
    settings = {
        "screen_record": {
            "output_dir": str(tmp),
            "fps": 30,
            "quality_preset": "medium",
            "audio_device": "",
        },
        "hotkeys": {"screen_record": "F3"},
    }
    mgr = m.ScreenRecordManager(lambda: settings)

    try:
        mgr.start_recording("primary")
        assert _pump(app, lambda: mgr.is_recording), "recording did not start"
        assert not mgr.is_paused

        mgr.pause_recording()
        assert _pump(app, lambda: mgr.is_paused), "pause did not settle"

        mgr.resume_recording()
        assert _pump(app, lambda: mgr.is_recording and not mgr.is_paused), "resume failed"

        mgr.stop_recording()
        assert _pump(app, lambda: not mgr._stopping and not mgr.is_recording), "stop did not settle"

        segs = merged.get("segments")
        assert segs is not None, "merge not invoked"
        assert len(segs) == 2, f"expected 2 segments, got {len(segs)}: {segs}"
        assert merged.get("has_audio") is False

        # After stop the segment state is reset.
        assert mgr._segments == []
        assert mgr._current_segment is None
        assert mgr._final_path is None
        assert not mgr.is_paused
    finally:
        try:
            mgr.shutdown()
        except Exception:
            pass
        m.subprocess.Popen = orig_popen
        m.find_ffmpeg = orig_find
        m.probe_preferred_encoder = orig_probe
        m.resolve_capture_region = orig_resolve
        m._write_state = orig_write_state
        m._read_state = orig_read_state
        m._clear_state = orig_clear_state
        m._merge_segments = orig_merge
        i18n.ask_keep_recording_with_name = orig_keep
        # cleanup
        try:
            for p in tmp.iterdir():
                p.unlink(missing_ok=True)
            tmp.rmdir()
        except OSError:
            pass

    print("test_state_machine OK")


def main() -> int:
    test_quality_mapping()
    test_concat_commands()
    test_list_audio_devices()
    test_state_machine()
    print("selftest_screen_record ALL OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
