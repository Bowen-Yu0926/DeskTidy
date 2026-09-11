"""Desktop sticky todo list."""

from __future__ import annotations

import json
import time
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

from src.settings import APP_DIR, ensure_app_dir

TODOS_FILE = APP_DIR / "todos.json"
_todos_file_override: Path | None = None


def todos_file_path() -> Path:
    return _todos_file_override or TODOS_FILE


@contextmanager
def temporary_todos_file(path: Path) -> Iterator[Path]:
    """Redirect todo I/O to *path* (selftests must not touch user data)."""
    global _todos_file_override
    prev = _todos_file_override
    _todos_file_override = Path(path)
    try:
        yield _todos_file_override
    finally:
        _todos_file_override = prev


def desktop_todos_settings(settings: dict[str, Any] | None) -> dict[str, Any]:
    if settings is None:
        return {"enabled": False}
    raw = settings.get("desktop_todos")
    if not isinstance(raw, dict):
        raw = {"enabled": False}
        settings["desktop_todos"] = raw
    raw.setdefault("enabled", False)
    return raw


def desktop_todos_enabled(settings: dict[str, Any] | None) -> bool:
    return bool(desktop_todos_settings(settings).get("enabled", False))


def _new_item(text: str = "", *, done: bool = False) -> dict[str, Any]:
    return {
        "id": uuid.uuid4().hex[:12],
        "text": str(text or "").strip(),
        "done": bool(done),
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }


def normalize_item(raw: Any) -> dict[str, Any] | None:
    if not isinstance(raw, dict):
        return None
    text = str(raw.get("text") or "").strip()
    item_id = str(raw.get("id") or "").strip() or uuid.uuid4().hex[:12]
    return {
        "id": item_id,
        "text": text,
        "done": bool(raw.get("done", False)),
        "created_at": str(raw.get("created_at") or ""),
    }


def load_todos() -> list[dict[str, Any]]:
    path = todos_file_path()
    try:
        if not path.is_file():
            return []
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, TypeError):
        return []
    items = data.get("items") if isinstance(data, dict) else data
    if not isinstance(items, list):
        return []
    out: list[dict[str, Any]] = []
    for raw in items:
        item = normalize_item(raw)
        if item is not None:
            out.append(item)
    return out


def save_todos(items: list[dict[str, Any]]) -> None:
    import os
    import tempfile
    import time

    ensure_app_dir()
    path = todos_file_path()
    clean: list[dict[str, Any]] = []
    for raw in items:
        item = normalize_item(raw)
        if item is not None:
            clean.append(item)
    payload = {"version": 1, "items": clean}
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(
        prefix=f"{path.stem}_",
        suffix=".tmp",
        dir=str(path.parent),
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2)
        last_err: OSError | None = None
        for attempt in range(6):
            try:
                os.replace(tmp_name, path)
                return
            except OSError as exc:
                last_err = exc
                if attempt >= 5:
                    break
                time.sleep(0.04 * (attempt + 1))
        if last_err is not None:
            raise last_err
    except Exception:
        try:
            os.unlink(tmp_name)
        except OSError:
            pass
        raise


def add_todo(text: str, items: list[dict[str, Any]] | None = None) -> list[dict[str, Any]]:
    current = list(items) if items is not None else load_todos()
    current.append(_new_item(text))
    save_todos(current)
    return current


def toggle_todo(item_id: str, items: list[dict[str, Any]] | None = None) -> list[dict[str, Any]]:
    current = list(items) if items is not None else load_todos()
    for item in current:
        if item.get("id") == item_id:
            item["done"] = not bool(item.get("done"))
            break
    save_todos(current)
    return current


def delete_todo(item_id: str, items: list[dict[str, Any]] | None = None) -> list[dict[str, Any]]:
    current = list(items) if items is not None else load_todos()
    current = [i for i in current if i.get("id") != item_id]
    save_todos(current)
    return current


def update_todo_text(
    item_id: str, text: str, items: list[dict[str, Any]] | None = None
) -> list[dict[str, Any]]:
    current = list(items) if items is not None else load_todos()
    cleaned = str(text or "").strip()
    for item in current:
        if item.get("id") == item_id:
            item["text"] = cleaned
            break
    # Drop empty unfinished drafts that the user cleared.
    current = [i for i in current if i.get("text") or i.get("done")]
    save_todos(current)
    return current
