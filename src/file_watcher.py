"""Watch desktop for new files so page-local overlay floats stay in sync.

Also watches Folder Portal directories (non-recursive) so mirrored fences refresh
when the real folder changes — without routing those events through auto-pin.
"""

from __future__ import annotations

from pathlib import Path
from typing import Callable

from watchdog.events import FileSystemEventHandler
from watchdog.observers import Observer

from src.settings import get_desktop_paths, invalidate_desktop_paths_cache


class DesktopEventHandler(FileSystemEventHandler):
    def __init__(
        self,
        settings: dict,
        on_created_file: Callable[[str], None] | None = None,
        on_removed_file: Callable[[str], None] | None = None,
        on_moved_file: Callable[[str, str], None] | None = None,
    ):
        self.settings = settings
        self.on_created_file = on_created_file
        self.on_removed_file = on_removed_file
        self.on_moved_file = on_moved_file

    def _notify_created(self, raw_path: str) -> None:
        if not self.on_created_file:
            return
        path = Path(raw_path)
        exclude = self.settings.get("exclude_patterns", [])
        from src.desktop_scanner import is_ignored_desktop_entry

        if is_ignored_desktop_entry(path.name, exclude):
            return
        self.on_created_file(str(path))

    def _notify_removed(self, raw_path: str) -> None:
        if not self.on_removed_file:
            return
        path = Path(raw_path)
        exclude = self.settings.get("exclude_patterns", [])
        from src.desktop_scanner import is_ignored_desktop_entry

        if is_ignored_desktop_entry(path.name, exclude):
            return
        self.on_removed_file(str(path))

    def on_created(self, event) -> None:
        # Files and folders: Explorer icons are hidden, so both need overlay sync.
        self._notify_created(event.src_path)

    def on_moved(self, event) -> None:
        # Save As / Office / WeChat often write a temp name then rename into the final path.
        src = getattr(event, "src_path", None)
        dest = getattr(event, "dest_path", None)
        if src and dest and self.on_moved_file is not None:
            exclude = self.settings.get("exclude_patterns", [])
            from src.desktop_scanner import is_ignored_desktop_entry

            src_name = Path(str(src)).name
            dest_name = Path(str(dest)).name
            if is_ignored_desktop_entry(dest_name, exclude):
                return
            if is_ignored_desktop_entry(src_name, exclude):
                self._notify_created(str(dest))
                return
            self.on_moved_file(str(src), str(dest))
            return
        if dest:
            self._notify_created(dest)
        if src:
            self._notify_removed(src)

    def on_deleted(self, event) -> None:
        self._notify_removed(event.src_path)


class PortalEventHandler(FileSystemEventHandler):
    """Debounced-friendly notifier for Folder Portal directory changes."""

    def __init__(self, on_changed: Callable[[str], None] | None = None):
        self.on_changed = on_changed

    def _fire(self, raw_path: str = "") -> None:
        if self.on_changed:
            try:
                self.on_changed(raw_path)
            except Exception:
                pass

    def on_created(self, event) -> None:
        self._fire(getattr(event, "src_path", "") or "")

    def on_deleted(self, event) -> None:
        raw = getattr(event, "src_path", "") or ""
        try:
            from src.organize_suppress import is_organize_suppressed

            if raw and is_organize_suppressed(raw):
                return
        except Exception:
            pass
        self._fire(raw)

    def on_moved(self, event) -> None:
        raw = getattr(event, "dest_path", "") or getattr(event, "src_path", "") or ""
        self._fire(raw)

    def on_modified(self, event) -> None:
        # Directory metadata noise — ignore; create/delete/move cover listing.
        return


def desktop_watch_fingerprint(
    settings: dict | None = None,
) -> tuple[str, ...]:
    """Stable key of folders currently watched (desktop + portal mirrors)."""
    keys: list[str] = []
    for desktop in get_desktop_paths():
        try:
            if desktop.is_dir():
                keys.append(str(desktop.resolve()).casefold())
        except OSError:
            continue
    if settings is not None:
        try:
            from src.fence_rules import collect_portal_watch_paths

            for portal in collect_portal_watch_paths(settings):
                try:
                    keys.append(str(portal.resolve()).casefold())
                except OSError:
                    continue
        except Exception:
            pass
    return tuple(sorted(set(keys)))


class DesktopWatcher:
    def __init__(self) -> None:
        self._observer: Observer | None = None
        self._fingerprint: tuple[str, ...] = ()

    @property
    def fingerprint(self) -> tuple[str, ...]:
        return self._fingerprint

    def start(
        self,
        settings: dict,
        on_created_file: Callable[[str], None] | None = None,
        on_removed_file: Callable[[str], None] | None = None,
        on_moved_file: Callable[[str, str], None] | None = None,
        on_portal_changed: Callable[[str], None] | None = None,
    ) -> None:
        self.stop()
        # Desktop folder can be relocated; never keep a stale path forever.
        invalidate_desktop_paths_cache()
        handler = DesktopEventHandler(
            settings, on_created_file, on_removed_file, on_moved_file
        )
        portal_handler = PortalEventHandler(on_portal_changed)
        observer = Observer()
        scheduled = False
        for desktop in get_desktop_paths():
            if not desktop.is_dir():
                continue
            observer.schedule(handler, str(desktop), recursive=False)
            scheduled = True
        try:
            from src.fence_rules import collect_portal_watch_paths

            for portal in collect_portal_watch_paths(settings):
                try:
                    if not portal.is_dir():
                        continue
                    observer.schedule(portal_handler, str(portal), recursive=False)
                    scheduled = True
                except OSError:
                    continue
        except Exception:
            pass
        if not scheduled:
            # Never keep an unstarted Observer on self (stop()/join would hang).
            self._fingerprint = ()
            return
        try:
            observer.start()
        except OSError:
            try:
                observer.stop()
                observer.join(timeout=2)
            except Exception:
                pass
            self._fingerprint = ()
            return
        self._observer = observer
        self._fingerprint = desktop_watch_fingerprint(settings)

    def stop(self) -> None:
        if self._observer:
            self._observer.stop()
            self._observer.join(timeout=2)
            self._observer = None
        self._fingerprint = ()
