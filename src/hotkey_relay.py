"""Hotkey relay (main-app side).

Binds a localhost listener that receives hotkey ACTION lines from the elevated
hotkey helper (``src.hotkey_helper``) and dispatches them onto the Qt GUI
thread, then launches the helper elevated. The main process stays non-elevated
so Explorer OLE drag-drop still works.
"""

from __future__ import annotations

import json
import os
import socket
import sys
import tempfile
import threading
from pathlib import Path

from PyQt6.QtCore import QObject, pyqtSignal

_BIND_HOST = "127.0.0.1"


class _RelayBridge(QObject):
    action_received = pyqtSignal(str)


class HotkeyRelay:
    """Owns the localhost listener + elevated helper lifecycle."""

    def __init__(self) -> None:
        self._bridge = _RelayBridge()
        self._sock: socket.socket | None = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._port = 0
        self._mapping_path = ""

    @property
    def port(self) -> int:
        return self._port

    def start(self, mapping: dict[str, str], dispatch) -> bool:
        """Bind the listener, start serving, write the mapping, launch the helper."""
        self._bridge.action_received.connect(dispatch)
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((_BIND_HOST, 0))
        self._sock.listen(1)
        self._port = self._sock.getsockname()[1]
        self._stop.clear()
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

        if not _write_mapping(mapping, self):
            return False
        return _launch_helper(self._port, self._mapping_path)

    def update_mapping(self, mapping: dict[str, str]) -> None:
        """Rewrite the mapping file; the helper reloads it on mtime change."""
        if self._mapping_path:
            _write_mapping(mapping, self)

    def _serve(self) -> None:
        sock = self._sock
        while not self._stop.is_set():
            try:
                sock.settimeout(0.5)
                conn, _ = sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                conn.settimeout(0.5)
                data = conn.recv(256)
                if data:
                    action = data.decode("utf-8", "replace").strip()
                    if action:
                        self._bridge.action_received.emit(action)
            except OSError:
                pass
            finally:
                try:
                    conn.close()
                except OSError:
                    pass

    def stop(self) -> None:
        self._stop.set()
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None


def _write_mapping(mapping: dict[str, str], relay: HotkeyRelay) -> bool:
    if not relay._mapping_path:
        fd, path = tempfile.mkstemp(suffix=".json", prefix="desktidy_hotkeys_")
        relay._mapping_path = path
        os.close(fd)
    path = relay._mapping_path
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(mapping, f)
        return True
    except OSError:
        return False


def _launch_helper(port: int, mapping_path: str) -> bool:
    from src.elevated_io import _shell_execute_runas

    if getattr(sys, "frozen", False):
        exe = str(Path(sys.executable))
        cwd = str(Path(sys.executable).parent)
        params = f'--hotkey-helper --port {port} --mapping "{mapping_path}"'
    else:
        exe_path = Path(sys.executable)
        windowed = exe_path.with_name("pythonw.exe")
        launcher = str(windowed if windowed.is_file() else exe_path)
        script = Path(sys.argv[0]).resolve()
        exe = launcher
        cwd = str(script.parent)
        params = f'"{script}" --hotkey-helper --port {port} --mapping "{mapping_path}"'

    try:
        code = _shell_execute_runas(exe, params, cwd)
        return code > 32
    except Exception:
        return False
