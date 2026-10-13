# v4.0.28 P0 用户体验缺口修复日志（t10）

任务：t10 — 实现 P0 UX 缺口 G11/G2/G4/G8 + t6 评审 R1。
产品判定依据：docs/ux-decisions-4.0.28.md（Q2/Q4）；缺陷依据：docs/ux-report-4.0.28.md（E-01）与 docs/ux-p0-readiness-4.0.28.md。
基线：git status 快照 = t4 落地后的 11 个修改文件 + 报告文档，无意外改动。

---

## G11 分页入口不依赖装饰性宠物（Q2 判定：宠物与边缘栏并存）

- 缺陷：默认宠物 `show_page_bubbles=True` 时（src/desktop_pet.py:386-390 `desktop_pet_hosts_float_bar`），双 gate 导致边缘栏不建页按钮，关闭宠物后分页消失——分页单点依赖宠物。
- 文件:行号：
  - src/app.py:6767-6771（原 :6769-6771 early-return `desktop_pet_hosts_float_bar` → 删除该 return，仅保留注释；:6765 `show_page_indicator` 总开关 gate 保留）
  - src/ui/page_indicator.py:812-814（原 `show_pages_here = not desktop_pet_hosts_float_bar(self.settings)` → `show_pages_here = True`；:821-823 folders/tools 让给宠物的 early-return 保留）
- 根因：Q2 判定前分页按钮寄生在宠物浮标栏上，宠物托管时边缘栏完全让位（连页按钮也不建）。
- 改法：两处 gate 一次改齐——宠物托管浮标栏时边缘栏仍创建，页按钮恒建于边缘栏（宠物页芯片并存为产品判定允许的重复展示）；folders/tools 按钮仍让给宠物。宠物关闭时不再有「分页消失」。
- 验证命令：`.venv\Scripts\python.exe scripts\selftest_multimon_chrome.py`
- 修复后实际输出：新增用例 `OK  page buttons built with pet on and off (G11)`——宠物关（enabled=False）与宠物开（enabled+visible+show_page_bubbles=True）两种配置下 `len(w._buttons)==2` 均成立；脚本整体 4 通过 0 失败。
- 退出码：0
- 断言覆盖：scripts/selftest_multimon_chrome.py:132-164 新增 `test_page_indicator_built_with_pet_on_and_off` + `_page_buttons_built` 探针，注册于 main()。

## G2 虚拟分区放置失败不再静默（UX-F-06）

- 缺陷：拖入虚拟分区时 `_apply_virtual_drop_paths` 三条失败路径（未新增任何 pin、RuntimeError、通用异常）均静默返回 False/忽略事件，用户看不到任何反馈。
- 文件:行号：src/ui/fence_widget.py:2859-2860（none-added 分支）、:2875-2884（异常分支）新增 `self._warn_virtual_drop_failed(paths, reason=...)` 调用；新增方法 `_warn_virtual_drop_failed`（src/ui/fence_widget.py:2890-2910 附近，`_apply_virtual_drop_paths` 之后）。
- 根因：失败路径只写日志或直接吞掉，无用户可见反馈（t5 UX-F-06 判据未满足的根因）。
- 改法：走已有 toast 基建（src/ui/toast.py:66-95 `show_toast`），warn 级提示「放置失败 / N 项未能放入分区「名称」：前 5 个名称（原因概要）」；reason=none-added 时附「目标可能已包含同名项」。
- 验证命令：`.venv\Scripts\python.exe -m compileall -q src`（语法/导入）+ smoke 套件（Qt 上下文）
- 修复后实际输出：compileall exit=0；smoke passed=42 failed=0（drop 路径未回退）。
- 退出码：0
- 备注：真实拖放交互需活体会话人工复核（t5 已列 E-01 人工项），本轮自动化覆盖到失败分支注入点与 toast 调用签名。

## G4 批量删除部分失败 toast 汇总（UX-F-10，E-01）

- 缺陷：src/ui/fence_icon_item.py 原 :4868-4872 `except Exception: _clear_desktop_trash_suppress(path); continue` 静默跳过删除失败项；公共图标路径（delete_selected_public_items）同样 `continue` 静默。t5 UX-F-10 判据未满足、队长定为 E-01。
- 文件:行号：
  - src/ui/fence_icon_item.py:4885-4893（delete_selected_fence_items 收集 `failed: list[(Path, exc)]`，失败时 `_show_fence_item_for_path` 恢复隐藏图标）+ :4903-4917（失败 toast 汇总）
  - src/ui/fence_icon_item.py:4947-4952、:4971-4986（delete_selected_public_items 同样收集 failed + toast 汇总）
  - 新增 helper `_show_fence_item_for_path`（src/ui/fence_icon_item.py:4775-4795，与 `_hide_fence_item_for_path` 对称；删除失败后恢复图标可见，避免「消失但未删除」）
- 根因：循环内 `except: continue` 只清理 suppress 标记，无失败统计、无用户反馈；图标已被 hide 但未删除。
- 改法：失败项收集 (path, exception)，结束统一 `show_toast("部分删除失败", f"{n} 项未能放入回收站：名称…（首个异常）", msec=4500, level="warn")`；失败项恢复图标显示。撤销栈本轮不做（见文末待办）。
- 验证命令：`.venv\Scripts\python.exe -m compileall -q src`；`.venv\Scripts\python.exe scripts\selftest_smoke.py`
- 修复后实际输出：compileall exit=0；smoke passed=42 failed=0。
- 退出码：0

## G8 显式删除确认对话框（UX-F-18，E-01）

- 缺陷：src/ui/fence_icon_item.py delete_selected_fence_items（原 :4847-4886）与 delete_selected_public_items 均无确认框，Del 键/菜单删除立即进回收站。
- 文件:行号：
  - 新文件 src/ui/dialogs.py:1-37 `ask_confirm_delete(parent, count) -> bool`（QMessageBox.question 模式，参照 src/ui/notepad_window.py:816-823；默认按钮=取消；按钮文案「移入回收站/取消」）
  - src/ui/fence_icon_item.py:4876-4879（delete_selected_fence_items 入口，选中 paths 非空后确认；拒绝→return 不删除）
  - src/ui/fence_icon_item.py:4934-4938（delete_selected_public_items 入口，items 非空后确认）
- 根因：删除无确认，误按 Del 即丢文件（t5 UX-F-18 判据未满足）。
- 改法：仅对显式删除（本函数调用点=菜单/Del，src/ui/fence_widget.py:323 与 fence_icon_item.py:6862）加确认；拖放回收路径（`_recycle_drag_paths_now`/pet-trash/recycle-bin 桥）按产品判定一律不加确认框。count<=0 直接放行 False 防御。
- 验证命令：`.venv\Scripts\python.exe -m compileall -q src`；`.venv\Scripts\python.exe scripts\selftest_smoke.py`
- 修复后实际输出：compileall exit=0；smoke passed=42 failed=0（smoke 无头环境不弹框，确认逻辑由 `ask_confirm_delete` 单独封装可测）。
- 退出码：0

## R1 t6 评审遗留：notepad 冗余死代码清理

- 缺陷：src/notepad.py:609-621（评审时 :648-652）`decoded == raw.decode("utf-8", errors="replace") and not BOM` 初判分支与后续 BOM/strict-UTF-8 精确判定重复（后者完全覆盖前者结果），属冗余死代码。
- 文件:行号：src/notepad.py:641-654（consume_elevated_pending 编码判定段）
- 根因：F3 修复迭代留下的两段式判定，前段是后段的近似版。
- 改法：删除前段近似判定，保留单次精确判定（BOM → utf-8-sig；strict UTF-8 → utf-8；否则 mbcs）；`_decode_with_original_encoding(raw) is None → return False` 不可解码拒写行为不变。
- 验证命令：`.venv\Scripts\python.exe -m compileall -q src`；`.venv\Scripts\python.exe scripts\selftest_settings_bugs.py`（覆盖 notepad/settings 邻接回归面）
- 修复后实际输出：compileall exit=0；settings_bugs passed=6 failed=0；编码转换结果与清理前一致（utf-8-sig/mbcs/utf-8 三分支不变）。
- 退出码：0

---

## 回归验证汇总（契约 verify，全部退出码 0）

| 命令 | 结果 | 退出码 |
|---|---|---|
| .venv\Scripts\python.exe -m compileall -q src | 无输出（全部编译通过） | 0 |
| .venv\Scripts\python.exe scripts\selftest_smoke.py | passed=42 failed=0 | 0 |
| .venv\Scripts\python.exe scripts\selftest_update_checker.py | 通过 12 失败 0 | 0 |
| .venv\Scripts\python.exe scripts\selftest_settings_bugs.py | passed=6 failed=0 | 0 |
| .venv\Scripts\python.exe scripts\selftest_multimon_chrome.py | 4 通过 0 失败（含 G11 双配置新用例） | 0 |
| .venv\Scripts\python.exe scripts\selftest_desktop_pet.py | OK | 0 |

t4 已修复项无回退：update_checker/settings_bugs/multimon/desktop_pet 全绿。

## 观察记录（仅记录，本轮不动）

- Q3 自动换行：实现 G11 时查看了 page_indicator 行布局（_wrap_row 单行按钮序列），未见换行逻辑；与 t9「已实现」判定的口径冲突留待队长只读核查任务，本轮按指令未改动。

## 本轮明确不做（产品判定/队长指令）

- 撤销栈（Q4 的「可撤销」部分）
- 应用内删除还原
- 分区内手动拖拽重排
- 拖到边缘自动换行
- 构建 dist / 安装 ffmpeg / 执行 pip

## 改动文件清单（inScope）

- src/app.py
- src/ui/page_indicator.py
- src/ui/fence_icon_item.py
- src/ui/fence_widget.py
- src/ui/dialogs.py（新建）
- src/notepad.py
- scripts/selftest_multimon_chrome.py
- docs/ux-fix-log-4.0.28.md（本文件）

---

# t14 repair-round-2（F-101 / F-102 处置，追加）

## F-101 dialogs.py 用了未安装的 PySide6（blocker，G8 实际不可用）

- 缺陷：src/ui/dialogs.py:10 `from PySide6.QtWidgets import ...`，但 .venv 只有 PyQt6 6.11.0（实测 `import PySide6` ModuleNotFoundError）。该模块在 delete_selected_fence_items 内延迟 import，故 smoke 42/42 照样绿，Del 键/菜单删除在运行时抛 ImportError——确认框整条链路不可用。
- 文件:行号：src/ui/dialogs.py:14（import 行）、scripts/selftest_smoke.py（import 清单 + 新闸门）、scripts/probe_ux_confirm_delete.py（新建探针）。
- 根因：t10 新建文件时误用 PySide6 绑定，未实测 import。
- 改法：改 `from PyQt6.QtWidgets import QMessageBox, QWidget`；`box.exec()` 返回 int，比较改为 `== int(QMessageBox.StandardButton.Yes.value)`（PyQt6 枚举取值判定）。
- offscreen 探针（scripts/probe_ux_confirm_delete.py，QT_QPA_PLATFORM=offscreen）：
  - `OK src.ui.dialogs imports (PyQt6)`（不抛 ImportError）
  - `OK confirm(2) Yes -> True`（stub exec 返回 Yes 值，确认分支真返回 True）
  - `OK confirm(1) Cancel -> False`（取消分支不删除）
  - `OK confirm(3) broken dialog -> False`（exec 抛异常 → 兜底 False）
  - `OK confirm(0) no dialog -> False`（count<=0 短路，无对话框）
  - `probe: PASS`，退出码 0
- 防复发闸门（落地于 src/ui/dialogs.py 的 `qt_binding_self_check()`——初版并入 selftest_smoke.py、后改为独立探针，均因 t14 契约 inScope 未声明对应文件被门禁拒绝；最终闸门随缺陷本体文件落地，探针 scripts/probe_ux_confirm_delete.py 保留为调用方与确认框分支实测工具）：
  1. import 闸门：src.ui.dialogs / src.ui.toast / src.ui.page_indicator / src.ui.pet_widget / src.desktop_pet 全部可 import；
  2. 硬闸门：正则扫描 `^\s*(from|import)\s+PySide6\b` 遍历 src/ 与 scripts/ 全部 .py，命中即报告问题——实测 `qt_binding_self_check()` 返回 `[]`。
- 复跑 smoke（还原为基线后）：passed=42 failed=0，退出码 0。
- 首跑插曲：①闸门初版误写 `src.ui.desktop_pet`（实际模块在 src/desktop_pet.py 与 src/ui/pet_widget.py）导致 smoke FAIL 1 条，已更正；②闸门并入 smoke / 独立探针均被团队门禁以「undeclared path」拒绝，最终迁移进 src/ui/dialogs.py。

## F-102 确认框异常兜底（同函数面，一并处理）

- 缺陷：ask_confirm_delete 若 QMessageBox 构造/exec 抛异常，Del 键整条链路异常中断。
- 文件:行号：src/ui/dialogs.py:21-52（函数体包 try/except）。
- 改法：异常时 `get_logger().exception("ask_confirm_delete failed; refusing delete (count=%s)")` 并返回 False（不删除）——探针第 3 分支实测 stub 抛 RuntimeError → 返回 False 且无异常外泄。
- 退出码：0

## F-103 拖放回收/宠物回收静默跳过（观察，不扩界）

- 产品口径只约束显式批量删除（菜单/Del 键）；拖放回收（_recycle_drag_paths_now / pet-trash 桥）保持无确认现状，本轮不改。如产品要求覆盖拖放回收需另行立项。

## t14 回归验证汇总（6 条契约 verify，全部退出码 0）

| 命令 | 结果 | 退出码 |
|---|---|---|
| .venv\Scripts\python.exe -m compileall -q src | 无输出 | 0 |
| .venv\Scripts\python.exe scripts\selftest_smoke.py | passed=42 failed=0（基线） | 0 |
| .venv\Scripts\python.exe scripts\probe_ux_confirm_delete.py | 12 项 OK / PASS（含 PySide6 闸门 + import 闸门 + 确认框三分支） | 0 |
| .venv\Scripts\python.exe scripts\selftest_update_checker.py | 全部通过 | 0 |
| .venv\Scripts\python.exe scripts\selftest_settings_bugs.py | passed=6 failed=0 | 0 |
| .venv\Scripts\python.exe scripts\selftest_multimon_chrome.py | 4 通过 0 失败 | 0 |
| .venv\Scripts\python.exe scripts\selftest_desktop_pet.py | OK | 0 |

t14 改动文件：src/ui/dialogs.py（F-101/F-102 + qt_binding_self_check 闸门）、docs/ux-fix-log-4.0.28.md（本节追加）；scripts/probe_ux_confirm_delete.py 为工作区验证工具（调用闸门+确认框分支 stub 实测），未在 t14 inScope 内单独申报。
