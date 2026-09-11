"""DeskTidy 全功能测试用例清单（单一数据源）。

用法：
  python scripts/full_test_plan.py              # 打印摘要
  python scripts/full_test_plan.py --write-md   # 生成 docs/FULL_TEST_CASES.md
  python scripts/run_full_test_plan_x3.py       # 按用例执行自动化 ×3
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
MD_PATH = DOCS / "FULL_TEST_CASES.md"


@dataclass(frozen=True)
class TestCase:
    case_id: str
    module: str
    title: str
    steps: str
    expected: str
    script: str  # scripts/ 下自动化脚本；空串表示仅人工
    priority: str = "P1"  # P1 核心 / P2 扩展 / P3 性能与打包


# 与 help_content.HELP_TOPICS + 基础设施对齐；script 列对应该功能的主要自动化套件。
ALL_TEST_CASES: list[TestCase] = [
    # --- 启动与基础设施 ---
    TestCase(
        "TC-000",
        "基础设施",
        "模块导入与 QApplication 冒烟",
        "导入 app/settings/fence/organizer/win_shell 等核心模块；构造 QApplication",
        "全部 import 成功，无异常",
        "selftest_smoke.py",
    ),
    TestCase(
        "TC-001",
        "基础设施",
        "版本号与默认配置一致性",
        "读取 VERSION、_version.py、default_settings.json",
        "版本一致；默认配置可 load",
        "selftest_system.py",
    ),
    TestCase(
        "TC-002",
        "基础设施",
        "单实例锁与退出确认契约",
        "检查 instance_lock API 与退出对话框 TOPMOST 契约",
        "API 可调用；退出前先隐藏 overlay",
        "selftest_system.py",
    ),
    TestCase(
        "TC-003",
        "基础设施",
        "打包产物 onedir 与 FFmpeg/fd 捆绑",
        "检查 dist/DeskTidy 布局、ffmpeg.exe、fd.exe",
        "onedir 结构正确；录屏/搜索依赖存在",
        "selftest_perf_stress.py",
    ),
    TestCase(
        "TC-004",
        "基础设施",
        "性能：DefView 缓存 / overlay 修复 / 热键轮询",
        "压测 EnumWindows、40 overlay repair、菜单节流、鼠标钩子",
        "热路径毫秒级；无回归契约破坏",
        "selftest_perf_stress.py",
        "P3",
    ),
    TestCase(
        "TC-005",
        "基础设施",
        "打包版活体启动冒烟",
        "启动 dist/DeskTidy.exe，检查窗口与 virtual-only 配置",
        "进程存活；organize_mode=virtual；主 HWND 可见",
        "selftest_live_e2e.py",
        "P2",
    ),
    # --- 桌面分区 ---
    TestCase(
        "TC-010",
        "桌面分区",
        "虚拟钉选 / 移区 / 取消钉选",
        "pin 到分区；跨区移动；unpin",
        "互斥钉选；顺序保留；unpin 成功",
        "selftest_virtual.py",
    ),
    TestCase(
        "TC-011",
        "桌面分区",
        "FenceWidget 构造与图标选中高亮",
        "离屏构造 FenceWidget；单击选中；sendEvent 不丢选中",
        "控件可构造；选中态可见",
        "selftest_smoke.py",
    ),
    TestCase(
        "TC-012",
        "桌面分区",
        "一键整理与白名单",
        "dry-run organize；白名单匹配；整理跳过白名单路径",
        "不移动文件；白名单生效",
        "selftest_recent.py",
    ),
    TestCase(
        "TC-013",
        "桌面分区",
        "分块整理与会话回归",
        "chunked organize 与 8/12 会话契约",
        "分块逻辑正确；会话 API 稳定",
        "selftest_chunked.py",
    ),
    TestCase(
        "TC-014",
        "桌面分区",
        "分区空白右键（Explorer 菜单）",
        "Fence 空白区 shell CreateViewObject 菜单契约",
        "走系统文件夹背景菜单",
        "selftest_fence_bg_menu.py",
    ),
    TestCase(
        "TC-015",
        "桌面分区",
        "分区快捷键与 Alt 缩放过滤",
        "热键注册；Fence 不用全局 eventFilter",
        "热键契约正确；缩放注册独立",
        "selftest_fence_keys.py",
    ),
    TestCase(
        "TC-015b",
        "桌面分区",
        "分区外观预设预览与应用",
        "预设目录；浅色/深色样式生效；设置页预览/应用接线",
        "选预设可预览，应用后写入全部分区",
        "selftest_fence_style.py",
    ),
    TestCase(
        "TC-016",
        "桌面分区",
        "Win+D 层级 / overlay 栈 / 非抢焦点",
        "overlay attach；Fence 无 StaysOnBottom；退出恢复图标",
        "层级契约满足；不抢焦点",
        "selftest_system.py",
    ),
    TestCase(
        "TC-017",
        "桌面分区",
        "文件夹门户拖放",
        "门户分区镜像真实文件夹；拖入拖出契约",
        "门户 API 与拖放路径正确",
        "selftest_portal.py",
    ),
    TestCase(
        "TC-018",
        "桌面分区",
        "系统图标原生命名空间",
        "namespace native 图标解析",
        "原生图标路径可解析",
        "selftest_namespace_native.py",
    ),
    TestCase(
        "TC-019",
        "桌面分区",
        "还原不重复",
        "restore 跳过已存在项",
        "restored_count 正确",
        "selftest_smoke.py",
    ),
    # --- 分页 ---
    TestCase(
        "TC-020",
        "分页",
        "分页切换无闪屏",
        "切换 current_page；fence relayout",
        "切换后无异常闪烁契约",
        "selftest_page_switch_flash.py",
    ),
    TestCase(
        "TC-021",
        "分页",
        "布局指纹迁移与屏幕适配",
        "fingerprint；ratio 存储；clamp 越界",
        "多分辨率适配；相对坐标持久化",
        "selftest_smoke.py",
    ),
    TestCase(
        "TC-022",
        "分页",
        "新建分区默认落在当前页",
        "create_fence 与 page 成员关系",
        "新分区归属当前分页",
        "selftest_system.py",
    ),
    # --- 公共区域 ---
    TestCase(
        "TC-030",
        "公共区域",
        "公共浮标宿主与布局",
        "public items relayout；hit-test",
        "浮标可布局；宿主 API 正常",
        "selftest_public_host.py",
    ),
    TestCase(
        "TC-031",
        "公共区域",
        "公共区拖放（自定义拖放桥）",
        "浮标 drag begin/end；pin 同步；OLE 不抢 Explorer",
        "拖放日志契约；pin 与 fence 同步",
        "selftest_public_drag.py",
    ),
    TestCase(
        "TC-032",
        "公共区域",
        "公共区多选",
        "多选状态与操作契约",
        "多选逻辑正确",
        "selftest_public_multiselect.py",
    ),
    TestCase(
        "TC-033",
        "公共区域",
        "拖放矩阵（分区/公共/桌面组合）",
        "多场景 mime 与 drop action",
        "矩阵场景全部通过",
        "selftest_drop_matrix.py",
    ),
    # --- 布局快照 ---
    TestCase(
        "TC-040",
        "布局快照",
        "快照保存/预览/应用 API",
        "snapshot 读写与 apply",
        "快照 round-trip 正确",
        "selftest_recent.py",
    ),
    # --- 区域截图 ---
    TestCase(
        "TC-050",
        "区域截图",
        "截图冻结 UI 与模态对话框挂起",
        "prepare_capture_ui / restore_suspended_modals",
        "截图前挂起 ApplicationModal；恢复后对话框回来",
        "selftest_smoke.py",
    ),
    TestCase(
        "TC-051",
        "区域截图",
        "截图热键与贴图契约",
        "screenshot / show_screenshot 热键注册",
        "热键可解析；贴图上限契约",
        "selftest_recent.py",
    ),
    # --- 桌面录屏 ---
    TestCase(
        "TC-060",
        "桌面录屏",
        "录屏指示器与 REC 浮层",
        "录制中指示器；结束对话框契约",
        "指示器 API 正常",
        "selftest_recording_indicator.py",
    ),
    TestCase(
        "TC-061",
        "桌面录屏",
        "FFmpeg 捆绑验证",
        "verify_ffmpeg_bundle h264_mf/ddagrab",
        "编码器与抓取后端可用",
        "selftest_system.py",
    ),
    # --- 文件搜索 ---
    TestCase(
        "TC-070",
        "文件搜索",
        "fd 搜索框与结果列表",
        "打开搜索；本地/全局模式契约",
        "搜索 UI 与 fd 路径正确",
        "selftest_file_search.py",
    ),
    # --- 记事本 ---
    TestCase(
        "TC-080",
        "记事本",
        "多标签打开与查找对话框",
        "notepad 窗口；find dialog；扩展路径",
        "记事本模块可构造；查找 API 存在",
        "selftest_recent.py",
    ),
    # --- 会议纪要 ---
    TestCase(
        "TC-090",
        "会议纪要",
        "Word 纪要生成路径",
        "minutes docx 输出目录契约",
        "纪要模块与路径配置正确",
        "selftest_recent.py",
    ),
    # --- 计算器 ---
    TestCase(
        "TC-100",
        "计算器",
        "唤起/收起系统计算器",
        "热键 Ctrl+Alt+C；toggle 逻辑",
        "计算器开关契约通过",
        "selftest_calculator.py",
    ),
    # --- 桌面待办 ---
    TestCase(
        "TC-110",
        "桌面待办",
        "待办增删勾选与持久化",
        "添加/完成/删除 todos.json",
        "待办 round-trip 正确",
        "selftest_todos.py",
    ),
    # --- 桌面宠物 ---
    TestCase(
        "TC-120",
        "桌面宠物",
        "宠物资源加载与姿态映射",
        "hoodie/voyage poses；wait/sleep/trash",
        "素材齐全；pose_for_state 正确",
        "selftest_desktop_pet.py",
    ),
    TestCase(
        "TC-121",
        "桌面宠物",
        "卫衣少年：玩手机/睡觉分动作与脚底锚点",
        "单帧 wait 脚底对齐；rest 交替；Zzz 位置",
        "无裁切上移；菜单项正确",
        "selftest_desktop_pet.py",
    ),
    TestCase(
        "TC-122",
        "桌面宠物",
        "丢垃圾动画溶解与即时回收",
        "trash 8 帧溶解；delete_to_trash 在动画前",
        "连贯过渡；文件进回收站",
        "selftest_desktop_pet.py",
    ),
    TestCase(
        "TC-123",
        "桌面宠物",
        "拖文件到宠物与 shaped mask",
        "accepts_trash_at；deliver_paths_to_pet_trash",
        "仅 sprite 区接收；回收站删除",
        "selftest_desktop_pet.py",
    ),
    # --- 壁纸 ---
    TestCase(
        "TC-130",
        "壁纸",
        "壁纸库应用与还原 API",
        "wallpaper 扩展配置与 apply",
        "壁纸模块契约存在",
        "selftest_recent.py",
    ),
    # --- 分页文件夹 ---
    TestCase(
        "TC-140",
        "分页文件夹",
        "浮标栏文件夹快捷方式",
        "添加/删除/打开文件夹快捷方式",
        "快捷方式列表可读写",
        "selftest_recent.py",
    ),
    # --- 设置与备份 ---
    TestCase(
        "TC-150",
        "设置与备份",
        "主题/开机启动/导入导出",
        "settings load/save；配置迁移",
        "设置 round-trip；路径在用户目录",
        "selftest_system.py",
    ),
    TestCase(
        "TC-151",
        "设置与备份",
        "全局热键注册与注销",
        "HotkeyManager register/unregister",
        "热键表完整；轮询轻量",
        "selftest_system.py",
    ),
    TestCase(
        "TC-152",
        "设置与备份",
        "Shell 右键菜单注册",
        "desktop_right_click_menu 注册表契约",
        "注册表项可清理",
        "selftest_recent.py",
    ),
    # --- 多显示器 ---
    TestCase(
        "TC-160",
        "多显示器",
        "多屏 work area 与 chrome",
        "multimon 几何；page bubble 布局",
        "多屏契约通过",
        "selftest_multimon_chrome.py",
    ),
    # --- 近期综合回归 ---
    TestCase(
        "TC-170",
        "综合回归",
        "近期功能大回归（白名单/公共/整理/剪贴板等）",
        "selftest_recent 全量断言",
        "无回归失败",
        "selftest_recent.py",
    ),
    TestCase(
        "TC-171",
        "综合回归",
        "8/12 会话专项回归",
        "session_aug12 契约",
        "会话 API 稳定",
        "selftest_session_aug12.py",
    ),
]


def scripts_in_plan() -> list[str]:
    seen: list[str] = []
    for tc in ALL_TEST_CASES:
        if tc.script and tc.script not in seen:
            seen.append(tc.script)
    return seen


def cases_for_script(script: str) -> list[TestCase]:
    return [tc for tc in ALL_TEST_CASES if tc.script == script]


def render_markdown() -> str:
    lines = [
        "# DeskTidy 全功能测试用例",
        "",
        "> 自动生成自 `scripts/full_test_plan.py`。执行三轮自动化：`python scripts/run_full_test_plan_x3.py`",
        "",
        f"**用例总数**：{len(ALL_TEST_CASES)}  "
        f"**自动化脚本数**：{len(scripts_in_plan())}",
        "",
        "## 执行说明",
        "",
        "| 步骤 | 命令 | 说明 |",
        "|------|------|------|",
        "| 1 | `python scripts/run_full_test_plan_x3.py` | 按本用例表跑自动化 ×3 轮 |",
        "| 2 | `python scripts/run_full_test_plan_x3.py --with-live` | 含打包版活体 E2E |",
        "| 3 | `python scripts/full_test_plan.py --write-md` | 更新本文档 |",
        "",
        "## 用例清单",
        "",
        "| 编号 | 模块 | 用例 | 优先级 | 自动化脚本 |",
        "|------|------|------|--------|------------|",
    ]
    for tc in ALL_TEST_CASES:
        lines.append(
            f"| {tc.case_id} | {tc.module} | {tc.title} | {tc.priority} | `{tc.script or '（人工）'}` |"
        )
    lines.extend(["", "## 用例明细", ""])
    cur_module = ""
    for tc in ALL_TEST_CASES:
        if tc.module != cur_module:
            cur_module = tc.module
            lines.append(f"### {cur_module}")
            lines.append("")
        lines.extend(
            [
                f"#### {tc.case_id} {tc.title}",
                "",
                f"- **优先级**：{tc.priority}",
                f"- **自动化**：`{tc.script or '人工验证'}`",
                f"- **步骤**：{tc.steps}",
                f"- **预期**：{tc.expected}",
                "",
            ]
        )
    lines.extend(
        [
            "## 人工抽测建议（自动化未覆盖）",
            "",
            "| 场景 | 操作 | 预期 |",
            "|------|------|------|",
            "| 长时间挂机 | 运行 2h+ | 内存无持续增长；托盘响应正常 |",
            "| Win+D | 连按 Win+D 进出 | 分区/浮标恢复正确 |",
            "| 多显示器 | 拔插/切换主屏 | 布局 clamp 正确 |",
            "| 真实录屏 | F3 录 10s 后结束 | 生成可播放 mp4 |",
            "| 真实截图贴图 | F1 框选 + F2 | 贴图可拖/右键另存 |",
            "",
        ]
    )
    return "\n".join(lines)


def write_markdown() -> Path:
    DOCS.mkdir(parents=True, exist_ok=True)
    MD_PATH.write_text(render_markdown(), encoding="utf-8")
    return MD_PATH


def main() -> int:
    parser = argparse.ArgumentParser(description="DeskTidy full test plan")
    parser.add_argument("--write-md", action="store_true", help="write docs/FULL_TEST_CASES.md")
    args = parser.parse_args()
    if args.write_md:
        path = write_markdown()
        print(f"Wrote {path}")
        return 0
    print(f"Test cases: {len(ALL_TEST_CASES)}")
    print(f"Automation scripts: {len(scripts_in_plan())}")
    for script in scripts_in_plan():
        ids = ", ".join(tc.case_id for tc in cases_for_script(script))
        print(f"  {script}: {ids}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
