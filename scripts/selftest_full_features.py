"""DeskTidy 全功能自测 — 单入口一次跑完所有自动化套件。

覆盖范围（与 ``selftest_all_x3.CORE_SUITE`` 一致）：
  桌面分区 / 分页 / 公共浮标 / 拖放 / 文件夹门户 / 快捷键剪贴板 /
  截图录屏 / 文件搜索 / 记事本 / 宠物 / 待办 / 计算器 / 多显示器 /
  性能压力 / 系统整体 / 拖放矩阵 等。

用法：
  python scripts/selftest_full_features.py
  python scripts/selftest_full_features.py --with-live   # 含打包版 live E2E
  scripts/selftest_full_features.bat

全部通过后可用 ``scripts/build_installer.bat`` 打包。
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

FEATURE_AREAS: list[tuple[str, str]] = [
    ("启动冒烟", "selftest_smoke.py"),
    ("虚拟分区", "selftest_virtual.py"),
    ("分块整理", "selftest_chunked.py"),
    ("会话回归", "selftest_session_aug12.py"),
    ("近期功能", "selftest_recent.py"),
    ("分页切换", "selftest_page_switch_flash.py"),
    ("桌面布局卡片", "selftest_desktop_layout_cards.py"),
    ("公共区拖放", "selftest_public_drag.py"),
    ("公共区宿主", "selftest_public_host.py"),
    ("公共区多选", "selftest_public_multiselect.py"),
    ("拖放矩阵", "selftest_drop_matrix.py"),
    ("系统图标原生", "selftest_namespace_native.py"),
    ("文件夹门户", "selftest_portal.py"),
    ("分区壳菜单", "selftest_fence_bg_menu.py"),
    ("分区快捷键", "selftest_fence_keys.py"),
    ("分区外观", "selftest_fence_style.py"),
    ("文件搜索", "selftest_file_search.py"),
    ("计算器", "selftest_calculator.py"),
    ("桌面待办", "selftest_todos.py"),
    ("桌面宠物", "selftest_desktop_pet.py"),
    ("多显示器", "selftest_multimon_chrome.py"),
    ("录屏指示", "selftest_recording_indicator.py"),
    ("性能压力", "selftest_perf_stress.py"),
    ("系统整体", "selftest_system.py"),
    ("打包活体", "selftest_live_e2e.py"),
]


def main(argv: list[str] | None = None) -> int:
    from scripts.selftest_all_x3 import main as run_all

    args = list(argv if argv is not None else sys.argv[1:])
    if "--with-live" in args:
        args = [a for a in args if a != "--with-live"]
    else:
        if "--no-live" not in args:
            args = ["--no-live", *args]
    if "--rounds" not in args:
        args = ["--rounds", "1", *args]

    print("DeskTidy 全功能自测（单轮）")
    print("功能面：")
    for label, script in FEATURE_AREAS:
        live = " [可选]" if script == "selftest_live_e2e.py" else ""
        print(f"  · {label:<12} {script}{live}")
    print("-" * 60)

    code = run_all(args)
    if code == 0:
        print("\n全功能自测通过，可以打包。")
    return code


if __name__ == "__main__":
    raise SystemExit(main())
