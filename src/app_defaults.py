"""Backfill missing settings keys and heal public-area scope.

Extracted from ``DeskTidyApp.__init__`` (app.py) so the giant class no longer
owns settings bootstrapping. Behaviour, ordering and exception-swallowing are
unchanged from the original inline block.
"""

from __future__ import annotations

from src.public_desktop import clear_ephemeral_public_items
from src.settings import save_settings


def apply_missing_defaults(settings: dict) -> None:
    """Fill missing default keys in-place and heal public-area scope."""
    # Opt-in live pin (Fences Rules-like). Default stays off; do not force-clear
    # a user-enabled preference across restarts.
    if "auto_organize_watch" not in settings:
        settings["auto_organize_watch"] = False
    # Page-rule organizes used to leave docs as free floats on empty pages —
    # migrate them into a real fence so they are findable as partitions.
    # Skip when live auto-pin is off — otherwise every launch re-vacuums
    # page floats the user left unpinned.
    if bool(settings.get("auto_organize_watch", False)):
        from src.fence_rules import migrate_page_local_floats_to_organize_fences

        if migrate_page_local_floats_to_organize_fences(settings):
            save_settings(settings)
    # Drop legacy empty-page mirrors (other pages' pins must stay page-local).
    if clear_ephemeral_public_items(settings):
        save_settings(settings)
    settings["organize_mode"] = "virtual"
    if "hosted_namespace_icons" not in settings:
        settings["hosted_namespace_icons"] = {}
    if "close_behavior_prompted" not in settings:
        settings["close_behavior_prompted"] = False
    if "session_active" not in settings:
        settings["session_active"] = False
    if "legacy_storage_migrated" not in settings:
        settings["legacy_storage_migrated"] = False
    if "public_desktop_items" not in settings or not isinstance(
        settings.get("public_desktop_items"), list
    ):
        settings["public_desktop_items"] = []
    if "enable_public_desktop" not in settings:
        settings["enable_public_desktop"] = False
    if "organize_whitelist" not in settings or not isinstance(
        settings.get("organize_whitelist"), list
    ):
        settings["organize_whitelist"] = []
    # Heal: floats must match public-area toggle (shared ↔ page-local).
    from src.public_desktop import heal_public_area_scope

    page_id = int(settings.get("current_page", 0))
    if heal_public_area_scope(settings, page_id):
        try:
            save_settings(settings)
        except OSError:
            pass
