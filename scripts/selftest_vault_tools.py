"""Selftest: vault usability tools (password generator / strength / clipboard).

Offline, no network, no GUI. The QTimer clipboard-clear path is covered by a
pure predicate + static/import checks only (no QApplication is started).

Run (workdir=E:\\Desktidy):
    $env:DESKTIDY_SELFTEST=1; python scripts/selftest_vault_tools.py
"""

from __future__ import annotations

import inspect
import string
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


def main() -> int:
    from src import account_vault as av

    # --- generate_password: length + clamp ---
    for ln in (4, 8, 16, 32, 128):
        assert len(av.generate_password(ln)) == ln, (ln,)
    assert len(av.generate_password(1)) == 4
    assert len(av.generate_password(9999)) == 128

    # --- per-class toggles ---
    digits_only = av.generate_password(64, upper=False, lower=False, digits=True, symbols=False)
    assert digits_only and all(c in string.digits for c in digits_only)

    upper_only = av.generate_password(64, upper=True, lower=False, digits=False, symbols=False)
    assert upper_only and all(c in string.ascii_uppercase for c in upper_only)

    lower_only = av.generate_password(64, upper=False, lower=True, digits=False, symbols=False)
    assert lower_only and all(c in string.ascii_lowercase for c in lower_only)

    symbols_only = av.generate_password(64, upper=False, lower=False, digits=False, symbols=True)
    assert symbols_only and all(c in av._SAFE_SYMBOLS for c in symbols_only)
    assert any(not c.isalnum() for c in symbols_only), "symbols=True must contain a symbol"

    # --- safe symbol pool: no quotes/backslash/backtick/pipe/whitespace ---
    forbidden = set("'\"\\`") | set(string.whitespace)
    for _ in range(100):
        p_sym = av.generate_password(
            128, upper=True, lower=True, digits=True, symbols=True, avoid_ambiguous=True
        )
        assert not (set(p_sym) & forbidden), repr(p_sym)
        assert all(c in (set(string.ascii_letters) | set(string.digits) | set(av._SAFE_SYMBOLS)) for c in p_sym)
    # avoid_ambiguous=False still stays inside the safe symbol set (no quotes etc.)
    p_raw = av.generate_password(128, upper=False, lower=False, digits=False, symbols=True, avoid_ambiguous=False)
    assert p_raw and all(c in av._SAFE_SYMBOLS for c in p_raw)
    assert not (set(p_raw) & forbidden)

    # --- avoid_ambiguous ---
    ambiguous = set("0O1lI|")
    p_avoid = av.generate_password(64, avoid_ambiguous=True)
    assert not (set(p_avoid) & ambiguous), p_avoid
    found: set[str] = set()
    for _ in range(200):
        found |= set(av.generate_password(64, avoid_ambiguous=False))
    assert found & ambiguous, "without avoid_ambiguous, ambiguous chars must appear"

    # --- all off -> ValueError ---
    try:
        av.generate_password(16, upper=False, lower=False, digits=False, symbols=False)
        raise AssertionError("expected ValueError")
    except ValueError:
        pass

    # --- each enabled class appears at least once ---
    for _ in range(50):
        p = av.generate_password(12, upper=True, lower=True, digits=True, symbols=True)
        assert any(c.islower() for c in p)
        assert any(c.isupper() for c in p)
        assert any(c.isdigit() for c in p)
        assert any(not c.isalnum() for c in p)

    # --- randomness: no immediate repeat ---
    seen = {av.generate_password(24) for _ in range(200)}
    assert len(seen) > 1

    # --- password_strength ---
    assert av.password_strength("") == (0, "很弱")
    assert av.password_strength("abc") == (0, "很弱")
    assert av.password_strength("abcdefgh") == (1, "弱")
    assert av.password_strength("abcABC123") == (3, "强")
    assert av.password_strength("Ab1!") == (2, "中")
    assert av.password_strength("Ab1!cdEfGhIjKl") == (4, "很强")
    # monotonic sanity
    assert av.password_strength("abcdefgh")[0] < av.password_strength("Ab1!cdEfGhIjKl")[0]

    # --- should_clear_clipboard ---
    assert av.should_clear_clipboard("secret", "secret", 30) is True
    assert av.should_clear_clipboard("other", "secret", 30) is False
    assert av.should_clear_clipboard("secret", "secret", 0) is False
    assert av.should_clear_clipboard("secret", "secret", -5) is False
    assert av.should_clear_clipboard("", "", 30) is False
    assert av.should_clear_clipboard("secret", "secret", "30") is True

    # --- clipboard_clear_seconds setting ---
    s: dict = {"account_vault": {}}
    assert av.account_vault_clipboard_clear_seconds(s) == av.DEFAULT_CLIPBOARD_CLEAR_SECONDS
    assert av.DEFAULT_CLIPBOARD_CLEAR_SECONDS == 30
    s["account_vault"]["clipboard_clear_seconds"] = 60
    assert av.account_vault_clipboard_clear_seconds(s) == 60
    s["account_vault"]["clipboard_clear_seconds"] = 0
    assert av.account_vault_clipboard_clear_seconds(s) == 0
    s["account_vault"]["clipboard_clear_seconds"] = "bogus"
    assert av.account_vault_clipboard_clear_seconds(s) == av.DEFAULT_CLIPBOARD_CLEAR_SECONDS
    assert av.account_vault_settings({})["clipboard_clear_seconds"] == 30

    # --- GUI static/import checks (no QApplication started) ---
    from src.ui.account_vault_widget import CredentialEditDialog

    assert hasattr(CredentialEditDialog, "_generate_password")
    assert hasattr(CredentialEditDialog, "_update_strength")
    assert hasattr(CredentialEditDialog, "_toggle_generator")
    init_src = inspect.getsource(CredentialEditDialog.__init__)
    assert "gen_btn" in init_src and "pass_strength_lbl" in init_src
    assert "gen_len_combo" in init_src and "gen_ambig_cb" in init_src
    gen_src = inspect.getsource(CredentialEditDialog._generate_password)
    assert "generate_password" in gen_src
    strength_src = inspect.getsource(CredentialEditDialog._update_strength)
    assert "password_strength" in strength_src

    from src.ui.account_vault_widget import AccountVaultWidget

    copy_src = inspect.getsource(AccountVaultWidget._on_copy)
    assert "_schedule_clipboard_clear" in copy_src
    sched_src = inspect.getsource(AccountVaultWidget._schedule_clipboard_clear)
    assert "should_clear_clipboard" in sched_src
    assert "QTimer.singleShot" in sched_src

    print("selftest_vault_tools: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
