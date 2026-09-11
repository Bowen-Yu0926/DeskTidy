"""按 full_test_plan.py 用例表执行自动化测试 ×N 轮。

默认 3 轮；每轮按脚本顺序执行，脚本通过则其映射的全部用例记为通过。

用法：
  python scripts/run_full_test_plan_x3.py
  python scripts/run_full_test_plan_x3.py --rounds 3 --with-live
"""

from __future__ import annotations

import argparse
import sys
import time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.full_test_plan import ALL_TEST_CASES, cases_for_script, scripts_in_plan, write_markdown


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run full test plan ×N")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--fail-fast", action="store_true")
    parser.add_argument("--with-live", action="store_true", help="include selftest_live_e2e.py")
    parser.add_argument("--no-write-md", action="store_true", help="skip regenerating docs/FULL_TEST_CASES.md")
    args = parser.parse_args(argv)

    if not args.no_write_md:
        md = write_markdown()
        print(f"用例文档: {md}")

    from scripts.selftest_all_x3 import DEFAULT_TIMEOUT_S, _build_suite, _kill_stray_desktidy, _run_one
    from scripts.cleanup_selftest_artifacts import report_cleanup

    plan_scripts = scripts_in_plan()
    include_live = bool(args.with_live)
    suite = _build_suite(include_live=include_live)
    suite_scripts = [name for name, _ in suite]

    # 计划内脚本必须都在执行套件中（live 可选）
    missing = [s for s in plan_scripts if s not in suite_scripts and s != "selftest_live_e2e.py"]
    if missing:
        print("ERROR: 用例表引用了不存在的脚本:", ", ".join(missing))
        return 1

    rounds = max(1, int(args.rounds))
    script_to_cases = {s: cases_for_script(s) for s in plan_scripts}

    print("=" * 60)
    print(f"DeskTidy 全功能测试用例 ×{rounds}")
    print(f"用例数: {len(ALL_TEST_CASES)}  自动化脚本: {len(plan_scripts)}  live={include_live}")
    print("=" * 60)

    case_status: dict[str, list[str]] = defaultdict(list)  # case_id -> [pass|fail|skip|timeout] per round
    script_results: list[tuple[int, str, str, float, str]] = []
    wall0 = time.perf_counter()
    aborted = False

    for r in range(1, rounds + 1):
        print(f"\n######## 第 {r}/{rounds} 轮 ########")
        _kill_stray_desktidy()
        for script, skip_reason in suite:
            if script not in plan_scripts:
                continue
            cases = script_to_cases.get(script, [])
            case_ids = [c.case_id for c in cases]

            if skip_reason:
                status = "skip" if not skip_reason.startswith("missing") else "fail"
                for cid in case_ids:
                    case_status[cid].append(status)
                script_results.append((r, script, status, 0.0, skip_reason))
                tag = "SKIP" if status == "skip" else "FAIL"
                print(f"  [{tag}] R{r} {script} ({', '.join(case_ids)}): {skip_reason}")
                if status == "fail" and args.fail_fast:
                    aborted = True
                    break
                continue

            timeout_s = DEFAULT_TIMEOUT_S.get(script, 300)
            print(f"  … R{r} {script} [{', '.join(case_ids)}] timeout={timeout_s}s", flush=True)
            cr = _run_one(script, timeout_s=timeout_s)
            status = cr.status
            script_results.append((r, script, status, cr.elapsed_s, cr.detail))
            mark = {"pass": "PASS", "fail": "FAIL", "timeout": "TIMEOUT"}[status]
            print(f"  [{mark}] R{r} {script}  {cr.elapsed_s:.1f}s  cases={len(case_ids)}")
            if status != "pass" and cr.detail:
                for line in cr.detail.splitlines()[-8:]:
                    print(f"         {line}")
            for cid in case_ids:
                case_status[cid].append(status)
            if status != "pass" and args.fail_fast:
                aborted = True
                break
        if aborted:
            print("\n--fail-fast：提前结束")
            break

    _kill_stray_desktidy()
    try:
        report_cleanup()
    except Exception as exc:
        print(f"测试残留清理失败: {exc}")

    wall = time.perf_counter() - wall0
    passed_cases = sum(
        1
        for cid, sts in case_status.items()
        if len(sts) == rounds and all(s == "pass" for s in sts)
    )
    failed_cases = [
        cid
        for cid, sts in case_status.items()
        if any(s in ("fail", "timeout") for s in sts) or len(sts) < rounds
    ]

    print("\n" + "=" * 60)
    print(f"脚本执行: {len(script_results)} 次  耗时 {wall:.1f}s")
    print(f"用例通过: {passed_cases}/{len(ALL_TEST_CASES)}（每用例需连续 {rounds} 轮全过）")

    if failed_cases:
        print("\n未连续通过的用例:")
        for cid in failed_cases:
            sts = case_status.get(cid, [])
            tc = next(t for t in ALL_TEST_CASES if t.case_id == cid)
            print(f"  - {cid} {tc.title}: {sts}")
        print("\n测试未全部通过")
        return 1

    print(f"\n全部 {len(ALL_TEST_CASES)} 条用例 × {rounds} 轮通过")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
