# -*- coding: utf-8 -*-
"""QA-DT-ORG-001: organize never remounts already-pinned fence icons."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
NATIVE = ROOT / "native" / "src"


def _read(name: str) -> str:
    return (NATIVE / name).read_text(encoding="utf-8", errors="ignore")


def _fn(src: str, sig: str, span: int = 1800) -> str:
    i = src.find(sig)
    assert i >= 0, sig
    return src[i : i + span]


def test_should_skip_virtual_skips_pinned():
    org = _read("organize.cpp")
    body = _fn(org, "bool ShouldSkipVirtual(")
    assert "FindPin(pins, it.path)" in body.replace(" ", "") or "FindPin(pins, it.path)" in body
    pin_block = body[body.find("FindPin") : body.find("IsOrganizeSuppressed")]
    assert "return true" in pin_block


def test_apply_and_preview_call_same_skip():
    org = _read("organize.cpp")
    apply = _fn(org, "int OrganizeApplyDefaultPins(")
    preview = _fn(org, "std::vector<std::wstring> OrganizePreviewVirtualPins(")
    assert "ShouldSkipVirtual" in apply
    assert "ShouldSkipVirtual" in preview
    skip_i = apply.find("ShouldSkipVirtual")
    set_i = apply.find("SetPin(")
    assert skip_i >= 0 and set_i > skip_i


def test_unpinned_still_reaches_resolve():
    org = _read("organize.cpp")
    apply = _fn(org, "int OrganizeApplyDefaultPins(", 2400)
    assert "ResolveFenceByKind" in apply or "ResolveFenceId" in apply
    assert "SetPin(pins" in apply or "SetPin(pins," in apply


def test_reassign_confirm_does_not_claim_pinned_move():
    app = _read("app.cpp")
    body = _fn(app, "void App::OrganizeReassign()", 1200)
    assert "已钉进分区的图标不会被挪动" in body
    assert "也会按规则重新归位" not in body
    assert "公共浮层" in body


def test_heal_is_noop():
    org = _read("organize.cpp")
    body = _fn(org, "int HealMisplacedSystemPins(", 600)
    assert "return 0" in body
    # Must not rewrite pin fence ids anymore
    assert 'kv.second = "system_docs"' not in body
    assert 'kv.second = "system_common"' not in body


def test_organize_done_never_retargets_live_pins():
    app = _read("app.cpp")
    body = _fn(app, "void App::OnOrganizeDone(", 2800)
    assert "HealMisplacedSystemPins(" not in body
    # Merge path: already-pinned → continue (no SetPin retarget)
    assert "FindPin(pins_, kv.first)" in body
    assert "already pinned" in body or "continue;  // already pinned" in body
    # Boot: no heal call (comment mentioning the name is OK)
    boot = app[app.find("system_page2") : app.find("system_page2") + 600]
    assert "HealMisplacedSystemPins(" not in boot
    assert "HealMisplacedSystemPins(&" not in boot
