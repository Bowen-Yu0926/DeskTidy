"""Watcher sync extracted from ``DeskTidyApp``.

Owns desktop/portal watcher lifecycle and the loose-sync background scan:
watcher (re)start, portal-folder change coalescing, watched-file create/move/
remove handling, sticky-missing reprune and the debounced watched-organize
path. Method bodies are copied verbatim from the original class; behaviour is
unchanged.

``DeskTidyApp`` inherits this as ``WatcherMixin``, so every ``self.*`` resolves
through the full MRO at runtime. No ``src.app`` import here (circular).
"""

from __future__ import annotations

from pathlib import Path

from src.app_logging import get_logger
from src.desktop_scanner import invalidate_desktop_scan_cache
from src.fence_rules import get_portal_path
from src.settings import save_settings


class WatcherMixin:
    """Desktop/portal watcher + loose-sync background scan."""

    def _start_watcher(self) -> None:
        self.watcher.start(
            settings=self.settings,
            on_created_file=self._ui_bridge.file_created.emit,
            on_removed_file=self._ui_bridge.file_removed.emit,
            on_moved_file=self._ui_bridge.file_moved.emit,
            on_portal_changed=self._ui_bridge.portal_changed.emit,
        )

    def _maybe_reschedule_desktop_watcher(self) -> None:
        """If Explorer's Desktop path or portal folders changed, restart watcher."""
        try:
            from src.file_watcher import desktop_watch_fingerprint
            from src.settings import invalidate_desktop_paths_cache

            # Cheap compare against cached paths first (45s TTL).
            current = desktop_watch_fingerprint(self.settings)
            known = getattr(self.watcher, "fingerprint", ())
            if current == known and current:
                return
            invalidate_desktop_paths_cache()
            current = desktop_watch_fingerprint(self.settings)
            if current == known:
                return
            self._start_watcher()
        except Exception:
            pass

    def _start_loose_sync_background(self) -> None:
        """Scan desktop off the UI thread; apply + refresh on the GUI thread."""
        if getattr(self, "_loose_sync_inflight", False):
            return
        if not getattr(self, "_loose_sync_needed", False):
            return
        self._loose_sync_inflight = True
        snap = {
            "exclude_patterns": list(self.settings.get("exclude_patterns") or []),
            "fences": list(self.settings.get("fences") or []),
            "current_page": self.settings.get("current_page", 0),
            "enable_public_desktop": self.settings.get("enable_public_desktop", False),
        }

        import threading

        def _worker() -> None:
            try:
                from src.public_desktop import scan_loose_desktop_wanted

                payload = scan_loose_desktop_wanted(snap)
            except OSError as exc:
                payload = exc
            from src.qt_main_thread import call_on_main_thread

            call_on_main_thread(lambda: self._finish_loose_sync_background(payload))

        from src.qt_main_thread import ensure_main_thread_bridge

        ensure_main_thread_bridge(self.qt_app)
        threading.Thread(
            target=_worker, name="desktidy-loose-scan", daemon=True
        ).start()

    def _finish_loose_sync_background(self, payload) -> None:
        self._loose_sync_inflight = False
        if isinstance(payload, OSError):
            return
        try:
            from src.public_desktop import apply_loose_desktop_scan, invalidate_loose_sync_cache

            if apply_loose_desktop_scan(self.settings, payload):
                save_settings(self.settings)
            invalidate_loose_sync_cache()
            self._loose_sync_needed = False
            self.refresh_public_desktop(immediate=True, relayout=False)
        except OSError:
            pass

    def _on_portal_folder_changed(self, raw_path: str = "") -> None:
        """Portal directory changed — refresh mirrored fences only (no auto-pin)."""
        if raw_path:
            try:
                import os

                portal_dir = Path(raw_path)
                # Prefer parent of a file event; directories stay as-is.
                try:
                    if portal_dir.is_file():
                        portal_dir = portal_dir.parent
                except OSError:
                    pass
                key = os.path.normpath(str(portal_dir)).casefold()
                self._portal_dirty_dirs.add(key)
            except OSError:
                self._portal_dirty_dirs.clear()
        timer = getattr(self, "_portal_refresh_timer", None)
        if timer is not None:
            timer.start(350)

    def _refresh_portal_fences(self) -> None:
        if self._exiting:
            return
        dirty = set(getattr(self, "_portal_dirty_dirs", None) or set())
        self._portal_dirty_dirs.clear()
        from src.fence_rules import is_portal_fence
        import os

        refreshed = 0
        for fence in list(self.fences):
            try:
                if not is_portal_fence(getattr(fence, "config", None)):
                    continue
                if dirty:
                    portal = get_portal_path(getattr(fence, "config", None))
                    if portal is None:
                        continue
                    try:
                        key = os.path.normpath(str(portal)).casefold()
                    except OSError:
                        continue
                    # Match exact portal root or a dirty child path under it.
                    matched = key in dirty or any(
                        d == key or d.startswith(key + os.sep)
                        for d in dirty
                    )
                    if not matched:
                        continue
                fence.refresh()
                refreshed += 1
            except RuntimeError:
                continue
            except Exception:
                continue
        # Path form mismatch: fall back to refreshing every portal fence once.
        if dirty and refreshed == 0:
            for fence in list(self.fences):
                try:
                    if is_portal_fence(getattr(fence, "config", None)):
                        fence.refresh()
                except RuntimeError:
                    continue
                except Exception:
                    continue

    def _on_watched_file(self, path: str) -> None:
        if path:
            try:
                from src.organize_suppress import is_organize_suppressed

                if is_organize_suppressed(path):
                    return
            except Exception:
                pass
        invalidate_desktop_scan_cache()
        from src.path_stat_cache import invalidate_path_stat_cache

        if path:
            invalidate_path_stat_cache(path)
        else:
            invalidate_path_stat_cache()
        self._loose_sync_needed = True
        if path:
            pending = getattr(self, "_pending_watch_paths", None)
            if not isinstance(pending, list):
                self._pending_watch_paths = []
                pending = self._pending_watch_paths
            pending.append(str(path))
            # Cap backlog so a desktop flood cannot grow forever.
            if len(pending) > 200:
                del pending[:-200]
        self._watch_debounce.start(300)

    def _on_watched_file_moved(self, src: str, dest: str) -> None:
        """Desktop rename vs leave-desktop move.

        Same-folder rename (Save As / Office / WeChat): relink pins + public floats.
        Move into another folder / off desktop: drop the public float immediately —
        rewriting public→dest would keep a ghost (dest still exists, sticky prune
        never fires). Fence pins still rewrite so they track the file.
        """
        try:
            from src.organize_suppress import is_organize_suppressed

            if is_organize_suppressed(src) or is_organize_suppressed(dest):
                return
        except Exception:
            pass
        invalidate_desktop_scan_cache()
        from src.path_stat_cache import invalidate_path_stat_cache
        from src.fence_rules import rewrite_pinned_path
        from src.public_desktop import remove_public_paths, rewrite_public_path
        from src.settings import get_desktop_paths, save_settings

        invalidate_path_stat_cache(src)
        invalidate_path_stat_cache(dest)
        self._loose_sync_needed = True
        old = Path(src)
        new = Path(dest)
        desktop_roots: set[str] = set()
        for root in get_desktop_paths():
            try:
                desktop_roots.add(str(root).casefold())
            except OSError:
                desktop_roots.add(str(root).casefold())
        try:
            src_parent = str(old.parent).casefold()
        except OSError:
            src_parent = ""
        try:
            dest_parent = str(new.parent).casefold()
        except OSError:
            dest_parent = ""
        leaving_desktop = src_parent in desktop_roots and dest_parent not in desktop_roots

        if leaving_desktop:
            changed = False
            try:
                if rewrite_pinned_path(self.settings, old, new):
                    changed = True
                # Drop public entry for the desktop path (and dest if a prior bug
                # already rewrote it). Never rewrite public → off-desktop.
                if remove_public_paths(self.settings, [old, new]):
                    changed = True
                if changed:
                    save_settings(self.settings)
            except OSError:
                pass
            try:
                self._remove_public_icon_widget(old)
                self._remove_public_icon_widget(new)
            except (RuntimeError, OSError, TypeError, ValueError):
                pass
            self._watch_debounce.start(300)
            return

        changed = False
        try:
            if rewrite_pinned_path(self.settings, old, new):
                changed = True
            if rewrite_public_path(self.settings, old, new):
                changed = True
            if changed:
                save_settings(self.settings)
        except OSError:
            pass
        try:
            target = str(old).casefold()
            for icon in list(self.public_icons):
                try:
                    same = str(icon.file_path).casefold() == target
                except OSError:
                    same = icon.file_path == old
                if not same:
                    continue
                icon.file_path = new
                refresh = getattr(icon, "refresh", None)
                if callable(refresh):
                    refresh()
                break
        except (RuntimeError, OSError, TypeError, ValueError):
            pass
        # Pins/icons already relinked — skip debounced full rebuild unless auto-pin.
        if changed:
            if bool(self.settings.get("auto_organize_watch", False)):
                pending = getattr(self, "_pending_watch_paths", None)
                if not isinstance(pending, list):
                    self._pending_watch_paths = []
                    pending = self._pending_watch_paths
                pending.append(str(new))
                self._watch_debounce.start(300)
            return
        pending = getattr(self, "_pending_watch_paths", None)
        if not isinstance(pending, list):
            self._pending_watch_paths = []
            pending = self._pending_watch_paths
        pending.append(str(new))
        self._watch_debounce.start(300)

    def _on_watched_file_removed(self, path: str) -> None:
        """Desktop delete/move-away — sticky-prune public floats (Office-save safe).

        Fence pins stay sticky (Fences-like): watcher remove must not unpin.
        Public floats share the same grace via sticky public prune — do not
        scrub public entries or retire widgets immediately (delete→recreate
        would lose grid position). Sustained absence is pruned after the
        sticky window; debounce refresh then drops ghosts.
        """
        # Recycle / folder-drop already dropped the one cell. A full overlay
        # rebuild here is the post-delete desktop flash.
        try:
            from src.organize_suppress import is_organize_suppressed

            if path and is_organize_suppressed(path):
                return
        except Exception:
            pass
        invalidate_desktop_scan_cache()
        from src.path_stat_cache import invalidate_path_stat_cache
        from src.public_desktop import prune_missing_public_items

        invalidate_path_stat_cache()
        self._loose_sync_needed = True
        try:
            if prune_missing_public_items(self.settings, force=True):
                save_settings(self.settings)
        except OSError:
            pass
        # First miss only stamps sticky; schedule one pass after the grace
        # window so permanent deletes drop without waiting for another event.
        self._schedule_sticky_missing_reprune()
        self._watch_debounce.start(400)

    def _schedule_sticky_missing_reprune(self) -> None:
        """Coalesce a post-sticky prune (Office-save safe, permanent-delete cleanup)."""
        from src.public_desktop import _STICKY_MISSING_S

        timer = getattr(self, "_sticky_reprune_timer", None)
        if timer is None:
            return
        delay_ms = int((float(_STICKY_MISSING_S) + 1.0) * 1000)
        timer.start(max(1000, delay_ms))

    def _run_sticky_missing_reprune(self) -> None:
        """Force sticky prune after grace — drop sustained-missing floats/pins."""
        from src.path_stat_cache import invalidate_path_stat_cache
        from src.fence_rules import prune_missing_virtual_items
        from src.public_desktop import prune_missing_public_items

        invalidate_path_stat_cache()
        changed = False
        try:
            if prune_missing_public_items(self.settings, force=True):
                changed = True
            if prune_missing_virtual_items(self.settings, force=True):
                changed = True
            if changed:
                save_settings(self.settings)
                self.refresh_fences(refresh_public=True)
        except OSError:
            pass

    def _purge_temp_desktop_refs(self) -> bool:
        """Drop Office/email temp paths from pins and public floats (runtime)."""
        try:
            from src.desktop_scanner import purge_temp_from_settings

            return bool(purge_temp_from_settings(self.settings))
        except Exception:
            return False

    def _run_watched_organize(self) -> None:
        """Debounced desktop watcher: optional rule auto-pin, then float refresh.

        When ``auto_organize_watch`` is on (Fences Rules-like), newly created /
        renamed desktop paths that match software/document rules are pinned.
        Otherwise they stay as current-page floats until 一键整理 / manual drag.
        """
        pending_raw = list(getattr(self, "_pending_watch_paths", None) or [])
        self._pending_watch_paths = []
        purged = self._purge_temp_desktop_refs()
        ingested_public = False
        for raw in pending_raw:
            try:
                if self._try_ingest_desktop_file_as_public(str(raw)):
                    ingested_public = True
            except Exception:
                continue
        if ingested_public:
            try:
                save_settings(self.settings)
            except OSError:
                pass
        elif purged:
            try:
                save_settings(self.settings)
            except OSError:
                pass
        if bool(self.settings.get("hide_shell_icons")):
            try:
                from src.public_desktop import ensure_desktop_loose_floats

                if ensure_desktop_loose_floats(self.settings):
                    try:
                        save_settings(self.settings)
                    except OSError:
                        pass
            except Exception:
                pass
        if pending_raw and bool(self.settings.get("auto_organize_watch", False)):
            try:
                from src.organizer import try_auto_pin_desktop_paths

                pinned, pin_pages = try_auto_pin_desktop_paths(
                    self.settings, pending_raw
                )
                if pinned:
                    try:
                        save_settings(self.settings)
                    except OSError:
                        pass
                    get_logger().info(
                        "auto_organize_watch: pinned %s path(s)", len(pinned)
                    )
                if pin_pages:
                    self._reveal_organize_float_pages(pin_pages)
            except Exception:
                get_logger().exception("auto_organize_watch: pin failed")
        # refresh_public runs background loose sync for unmatched Save As / drops.
        self.refresh_fences(refresh_public=True)
        self.window.refresh_data_if_visible()
