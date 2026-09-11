"""DeskTidy 全功能自测总控：现有可自动化套件串行跑 3 遍。

覆盖现有 scripts/selftest_*.py 中稳定可重复的功能面（契约 / 伪对象 /
局部 UI / 系统自测）。live E2E 仅在 dist/DeskTidy.exe 存在时执行。

用法：
  python scripts/selftest_all_x3.py
  python scripts/selftest_all_x3.py --rounds 3
  python scripts/selftest_all_x3.py --fail-fast
  scripts\\selftest_all_x3.bat
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from scripts.dist_paths import packaged_exe

SCRIPTS = ROOT / "scripts"
EXE = packaged_exe(ROOT)

# Light → heavy. Mirrors selftest_all.bat plus remaining stable suites.
CORE_SUITE: list[str] = [
    "selftest_smoke.py",
    "selftest_virtual.py",
    "selftest_chunked.py",
    "selftest_session_aug12.py",
    "selftest_recent.py",
    "selftest_page_switch_flash.py",
    "selftest_desktop_layout_cards.py",
    "selftest_public_drag.py",
    "selftest_public_host.py",
    "selftest_public_multiselect.py",
    "selftest_drop_matrix.py",
    "selftest_namespace_native.py",
    "selftest_portal.py",
    "selftest_fence_bg_menu.py",
    "selftest_fence_keys.py",
    "selftest_fence_style.py",
    "selftest_file_search.py",
    "selftest_calculator.py",
    "selftest_todos.py",
    "selftest_desktop_pet.py",
    "selftest_desknote.py",
    "selftest_multimon_chrome.py",
    "selftest_recording_indicator.py",
    "selftest_perf_stress.py",
    "selftest_system.py",
]

OPTIONAL_LIVE = "selftest_live_e2e.py"

# Soft ceilings so a hung Qt widget cannot block the whole triple run forever.
DEFAULT_TIMEOUT_S = {
    "selftest_smoke.py": 180,
    "selftest_virtual.py": 180,
    "selftest_chunked.py": 180,
    "selftest_session_aug12.py": 180,
    "selftest_recent.py": 300,
    "selftest_page_switch_flash.py": 180,
    "selftest_desktop_layout_cards.py": 120,
    "selftest_public_drag.py": 300,
    "selftest_public_host.py": 180,
    "selftest_public_multiselect.py": 180,
    "selftest_drop_matrix.py": 120,
    "selftest_namespace_native.py": 60,
    "selftest_portal.py": 180,
    "selftest_fence_bg_menu.py": 180,
    "selftest_fence_keys.py": 240,
    "selftest_fence_style.py": 120,
    "selftest_file_search.py": 240,
    "selftest_calculator.py": 120,
    "selftest_todos.py": 120,
    "selftest_desktop_pet.py": 180,
    "selftest_desknote.py": 120,
    "selftest_multimon_chrome.py": 180,
    "selftest_recording_indicator.py": 180,
    "selftest_perf_stress.py": 300,
    "selftest_system.py": 600,
    "selftest_live_e2e.py": 300,
}


@dataclass
class CaseResult:
    round_idx: int
    script: str
    status: str  # pass | fail | skip | timeout
    elapsed_s: float
    exit_code: int | None = None
    detail: str = ""


@dataclass
class RunReport:
    results: list[CaseResult] = field(default_factory=list)

    def counts(self) -> dict[str, int]:
        out = {"pass": 0, "fail": 0, "skip": 0, "timeout": 0}
        for r in self.results:
            out[r.status] = out.get(r.status, 0) + 1
        return out


def _kill_stray_desktidy() -> None:
    """Best-effort: stop leftover packaged processes between rounds."""
    if os.name != "nt":
        return
    try:
        subprocess.run(
            ["taskkill", "/F", "/IM", "DeskTidy.exe"],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
    except Exception:
        pass


def _tail(text: str, *, max_chars: int = 1200) -> str:
    text = (text or "").strip()
    if len(text) <= max_chars:
        return text
    return "…\n" + text[-max_chars:]


def _build_suite(*, include_live: bool) -> list[tuple[str, str]]:
    """Return (script_name, reason_if_skip_else_empty)."""
    suite: list[tuple[str, str]] = []
    for name in CORE_SUITE:
        path = SCRIPTS / name
        if not path.is_file():
            suite.append((name, f"missing {path}"))
        else:
            suite.append((name, ""))
    if include_live:
        path = SCRIPTS / OPTIONAL_LIVE
        if not path.is_file():
            suite.append((OPTIONAL_LIVE, f"missing {path}"))
        elif not EXE.is_file():
            suite.append((OPTIONAL_LIVE, f"skip live E2E (no {EXE.name})"))
        else:
            suite.append((OPTIONAL_LIVE, ""))
    return suite


def _run_one(script: str, *, timeout_s: int) -> CaseResult:
    # Placeholder filled by caller for round_idx.
    started = time.perf_counter()
    env = os.environ.copy()
    env.setdefault("PYTHONIOENCODING", "utf-8")
    env.setdefault("PYTHONUTF8", "1")
    env.setdefault("DESKTIDY_SELFTEST", "1")
    log_path: Path | None = None
    # 0xC000041D = STATUS_FATAL_USER_CALLBACK_EXCEPTION — intermittent Qt/Win32
    # callback teardown flake (seen on selftest_recent after all asserts passed).
    _FLAKE_EXIT = {3221226525, -1073740771}
    attempts = 2 if script == "selftest_recent.py" else 1
    last: CaseResult | None = None
    for attempt in range(1, attempts + 1):
        try:
            with tempfile.NamedTemporaryFile(
                mode="w+b",
                prefix=f"desktidy_{script.replace('.py', '')}_",
                suffix=".log",
                delete=False,
            ) as logf:
                log_path = Path(logf.name)
            with log_path.open("wb") as log_out:
                proc = subprocess.run(
                    [sys.executable, "-u", str(SCRIPTS / script)],
                    cwd=str(ROOT),
                    stdout=log_out,
                    stderr=subprocess.STDOUT,
                    timeout=timeout_s,
                    env=env,
                    check=False,
                )
            out = log_path.read_text(encoding="utf-8", errors="replace") if log_path else ""
        except subprocess.TimeoutExpired:
            elapsed = time.perf_counter() - started
            out = ""
            if log_path and log_path.is_file():
                try:
                    out = log_path.read_text(encoding="utf-8", errors="replace")
                except OSError:
                    pass
            last = CaseResult(
                round_idx=0,
                script=script,
                status="timeout",
                elapsed_s=elapsed,
                exit_code=None,
                detail=f"timeout>{timeout_s}s\n{_tail(out)}",
            )
            break
        finally:
            if log_path and log_path.is_file():
                try:
                    log_path.unlink()
                except OSError:
                    pass
                log_path = None
        elapsed = time.perf_counter() - started
        if proc.returncode == 0:
            return CaseResult(
                round_idx=0,
                script=script,
                status="pass",
                elapsed_s=elapsed,
                exit_code=0,
            )
        detail = _tail(out)
        last = CaseResult(
            round_idx=0,
            script=script,
            status="fail",
            elapsed_s=elapsed,
            exit_code=proc.returncode,
            detail=detail,
        )
        # Retry Qt/Win32 callback teardown flakes even when summary was not printed
        # (crash often hits between last OK and「全部通过」).
        if attempt < attempts and int(proc.returncode or 0) in _FLAKE_EXIT:
            print(f"         retry {script} after flake exit={proc.returncode}", flush=True)
            _kill_stray_desktidy()
            continue
        break
    assert last is not None
    return last


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="DeskTidy full selftest xN")
    parser.add_argument("--rounds", type=int, default=3, help="repeat count (default 3)")
    parser.add_argument(
        "--fail-fast",
        action="store_true",
        help="stop on first failing case",
    )
    parser.add_argument(
        "--no-live",
        action="store_true",
        help="never run selftest_live_e2e.py",
    )
    parser.add_argument(
        "--timeout-scale",
        type=float,
        default=1.0,
        help="multiply per-script soft timeouts",
    )
    args = parser.parse_args(argv)
    rounds = max(1, int(args.rounds))
    include_live = not args.no_live
    suite = _build_suite(include_live=include_live)

    print("DeskTidy 全功能自测 ×{}".format(rounds))
    print(f"ROOT = {ROOT}")
    print(f"套件数 = {len(suite)}  fail_fast={args.fail_fast}  live={include_live}")
    print("-" * 60)

    report = RunReport()
    wall0 = time.perf_counter()
    aborted = False

    for r in range(1, rounds + 1):
        print(f"\n######## 第 {r}/{rounds} 轮 ########")
        _kill_stray_desktidy()
        for script, skip_reason in suite:
            if skip_reason:
                status = "skip"
                # Missing scripts are real failures; optional live absence is skip.
                if skip_reason.startswith("missing"):
                    status = "fail"
                cr = CaseResult(
                    round_idx=r,
                    script=script,
                    status=status,
                    elapsed_s=0.0,
                    detail=skip_reason,
                )
                report.results.append(cr)
                tag = "SKIP" if status == "skip" else "FAIL"
                print(f"  [{tag}] R{r} {script}: {skip_reason}")
                if status == "fail" and args.fail_fast:
                    aborted = True
                    break
                continue

            timeout_s = int(
                DEFAULT_TIMEOUT_S.get(script, 300) * max(0.25, float(args.timeout_scale))
            )
            print(f"  … R{r} {script} (timeout={timeout_s}s)", flush=True)
            # Packaged DeskTidy left running (manual use / prior 打包) can race Qt
            # overlays and yield STATUS_FATAL_USER_CALLBACK_EXCEPTION in UI suites.
            _kill_stray_desktidy()
            cr = _run_one(script, timeout_s=timeout_s)
            cr.round_idx = r
            report.results.append(cr)
            mark = {"pass": "PASS", "fail": "FAIL", "timeout": "TIMEOUT"}[cr.status]
            print(f"  [{mark}] R{r} {script}  {cr.elapsed_s:.1f}s  exit={cr.exit_code}")
            if cr.status != "pass":
                if cr.detail:
                    for line in cr.detail.splitlines()[-12:]:
                        print(f"         {line}")
                if args.fail_fast:
                    aborted = True
                    break
        if aborted:
            print("\n--fail-fast：提前结束")
            break

    _kill_stray_desktidy()
    try:
        from scripts.cleanup_selftest_artifacts import report_cleanup

        report_cleanup()
    except Exception as exc:
        print(f"测试残留清理失败: {exc}")
    wall = time.perf_counter() - wall0
    counts = report.counts()

    print("\n" + "=" * 60)
    print(
        f"汇总: pass={counts['pass']}  fail={counts['fail']}  "
        f"timeout={counts['timeout']}  skip={counts['skip']}  "
        f"耗时 {wall:.1f}s"
    )
    bad = [x for x in report.results if x.status in ("fail", "timeout")]
    if bad:
        print("\n失败项:")
        for x in bad:
            print(f"  - R{x.round_idx} {x.script} ({x.status}) exit={x.exit_code}")
            if x.detail:
                first = x.detail.splitlines()[0][:200]
                print(f"      {first}")
        print("\n未全部通过")
        return 1

    print("全部通过（{} 轮 × {} 套件）".format(rounds, len(suite)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
