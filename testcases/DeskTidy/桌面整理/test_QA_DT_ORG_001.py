# -*- coding: utf-8 -*-
"""QA-DT-ORG-001: organize skips pinned fence icons (public floats only)."""

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
    # Pinned → return true (skip). Empty pin must not skip solely via FindPin*.
    assert "return true" in body
    pin_block = body[body.find("FindPin") : body.find("IsOrganizeSuppressed")]
    assert "return true" in pin_block
    # reassign param must not reopen pinned items
    assert "reassign" in body or "/*reassign*/" in body.replace(" ", "")


def test_apply_and_preview_call_same_skip():
    org = _read("organize.cpp")
    apply = _fn(org, "int OrganizeApplyDefaultPins(")
    preview = _fn(org, "std::vector<std::wstring> OrganizePreviewVirtualPins(")
    assert "ShouldSkipVirtual" in apply
    assert "ShouldSkipVirtual" in preview
    # Apply: skip before SetPin
    skip_i = apply.find("ShouldSkipVirtual")
    set_i = apply.find("SetPin(")
    assert skip_i >= 0 and set_i > skip_i


def test_unpinned_still_reaches_resolve():
    org = _read("organize.cpp")
    apply = _fn(org, "int OrganizeApplyDefaultPins(", 2400)
    # After skip, must still resolve a fence for loose items
    assert "ResolveFenceByKind" in apply or "ResolveFenceId" in apply
    assert "SetPin(pins" in apply or "SetPin(pins," in apply


def test_reassign_confirm_does_not_claim_pinned_move():
    """回归：重新整理确认框不得再声称会挪动已钉分区图标。"""
    app = _read("app.cpp")
    body = _fn(app, "void App::OrganizeReassign()", 1200)
    assert "已钉进分区的图标不会被挪动" in body
    assert "也会按规则重新归位" not in body
    assert "公共浮层" in body
