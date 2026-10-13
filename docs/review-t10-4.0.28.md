# t10 P0 用户体验缺口修复独立评审报告（t11 round 1）

评审者：开发B（只读，未修改 src/ 与 scripts/）。评审对象：t10（开发A）实现，工作区当前字节。
结论：**needs_revision** —— 1 条 blocker（G8 对话框用了错误的 Qt 绑定 PySide6，运行时 import 必崩）；G11/G2/G4/R1 实现正确且经独立反例验证。

## 门禁裁决依据汇总

| 项 | 独立验证结果 | 结论 |
|---|---|---|
| G11 双 gate | app.py:6769-6771 early-return 已删；page_indicator.py:814 `show_pages_here = True`，:822-824 仅让出 folders/tools（页按钮 :815-821 在此之前已建）。selftest_multimon_chrome 4/4 含 G11 双配置用例 | 通过 |
| G2 | fence_widget.py 三条失败路径（:2860 none-added / :2877 RuntimeError / :2886 Exception）均落 `_warn_virtual_drop_failed`（:2889-2905）→ `show_toast(..., level="warn")`，含分区名与前 5 个文件名 | 通过 |
| G4 | fence_icon_item.py 两个删除循环收集 `failed` + toast 汇总（:4909-4919、:4973-4983）；失败项恢复图标（`_show_fence_item_for_path` :4775-4794）；`_recycle_drag_paths_now`（:4005-4043）与 pet trash（pet_widget.py:1750-1786）保留原有静默跳过——按 Q4 仅约束显式批量删除路径，可接受 | 通过（含反例探针） |
| G8 范围 | 确认框仅接 delete_selected_fence_items（:4888-4891）与 delete_selected_public_items（:4944-4947）；拖放回收路径无确认框；dialogs.py:31 默认按钮=Cancel | 实现正确但 **import 崩溃（blocker）** |
| R1 | notepad.py:608-666 行为等价：BOM→utf-8-sig、strict-UTF-8→utf-8、否则 mbcs、不可解码→拒写（:646-647 return False） | 通过 |
| 越界 | git diff 与 t10 自述 8 文件一致（其余 7 个修改文件为 t4 已有改动）；docs/FULL_TEST_CASES.md 未动 | 通过 |
| t4 回退 | update_checker 12/12、settings_bugs 6/6、multimon 4/4、desktop_pet OK、smoke 42/42、compileall rc=0 | 无回退 |

## 独立验证命令与退出码（.venv\Scripts\python.exe）

| 命令 | 结果 | 退出码 |
|---|---|---|
| python -m compileall -q src | 全部编译通过 | 0 |
| scripts\selftest_smoke.py | passed=42 failed=0 | 0 |
| scripts\selftest_update_checker.py | 通过 12 失败 0 | 0 |
| scripts\selftest_settings_bugs.py | passed=6 failed=0 | 0 |
| scripts\selftest_multimon_chrome.py | 4 通过 0 失败（含 `OK page buttons built with pet on and off (G11)`） | 0 |
| scripts\selftest_desktop_pet.py | OK | 0 |

## 自构造反例

**反例 1（G4 批量部分失败，探针脚本临时运行后已删除）**：offscreen 环境 stub `delete_to_trash` 对 2 项全部抛 `OSError("Access is denied (simulated read-only)")`，调用 `delete_selected_fence_items`（dialogs 模块 stub 为恒确认）。观测：toast 触发 `('部分删除失败', '2 项未能放入回收站：dt_probe_a.txt、dt_probe_b_readonly.txt（Access is denied (simulated read-only)）', 'warn')`（控制台乱码为 GBK 显示问题，内容正确）；失败项图标恢复可见。→ G4 判据满足。

**反例 2（G8 运行时导入）**：`& .venv\Scripts\python.exe -c "import src.ui.dialogs"` →
`ModuleNotFoundError: No module named 'PySide6'`（exit 1）。项目实际绑定 PyQt6（pip list 仅 PyQt6 6.11.0；全库 grep 无其他 PySide6 引用）。反推：真实点击 Del/菜单删除时 :4888 的延迟 import 抛 ModuleNotFoundError，显式删除功能完全不可用——G8 修复以当前字节交付即回归。

## Findings

### F-101（blocker）src/ui/dialogs.py:10 使用未安装的 Qt 绑定 PySide6
- 问题：`from PySide6.QtWidgets import QMessageBox, QWidget`。代码库统一使用 PyQt6（162 处 import 均为 PyQt6；.venv 无 PySide6，`import src.ui.dialogs` 实测 ModuleNotFoundError）。G8 的 ask_confirm_delete 无法导入，导致 delete_selected_fence_items / delete_selected_public_items 在运行时崩溃（异常发生在函数体 :4888 延迟 import 处）。
- 修复：将 :10 改为 `from PyQt6.QtWidgets import QMessageBox, QWidget`，并把 QMessageBox 枚举访问从 `QMessageBox.Icon.Question` / `QMessageBox.StandardButton.Yes` 改为 PyQt6 等价形式（PyQt6 中为 `QMessageBox.Icon.Question`、`QMessageBox.StandardButton.Yes`，同名枚举可用，仅需实测 exec() 返回值比较改为 `== QMessageBox.StandardButton.Yes`——PyQt6 exec() 返回 StandardButton 枚举，语义一致）。
- 验证建议：offscreen 下直接 `ask_confirm_delete(None, 3)`（需 QApplication）确认不抛 ImportError；再跑 smoke 42/42。

### F-102（low）src/ui/dialogs.py:15-34 未覆盖 Qt 无头/异常环境
- 问题：`ask_confirm_delete` 若 QMessageBox 构造/exec 抛异常（如无显示会话），调用方 delete_selected_fence_items 无兜底，异常向上传播中断删除流程。
- 建议：调用侧（:4890）或函数内 try/except 兜底为返回 False（不删除）并记日志，避免确认环节异常导致 Del 键完全失效。非阻塞，可与 F-101 一并处理。

### F-103（low，观察）G4 拖放回收路径仍为静默跳过
- `_recycle_drag_paths_now`（fence_icon_item.py:4036-4042）与 pet trash（pet_widget.py:1779-1785）失败项无 toast。产品判定 Q4 的措辞针对批量删除汇总，t10 未扩界处理，本轮判定不阻塞；如产品后续要求覆盖拖放回收，需另行立项。

---

## Round 2 复审（t15，2026-10-13，对象 t14 修复）

结论：**pass**。F-101/F-102 闭环，闸门经否定测试证实真实可失败。

### 核对项与独立验证

| 项 | 结果 | 证据 |
|---|---|---|
| F-101 PyQt6 import | 通过 | src/ui/dialogs.py:18 rom PyQt6.QtWidgets import ...；:90 ox.exec() == int(QMessageBox.StandardButton.Yes.value)（PyQt6 exec 返回 int，比较成立）；grep 确认 src/ 无任何 PySide6 import（仅 dialogs.py 闸门自身的正则/文案命中） |
| F-102 异常兜底 | 通过 | dialogs.py:77-98 try/except → get_logger().exception + return False（拒绝删除） |
| 闸门否定测试（关键） | 通过 | monkeypatch Path.rglob/read_text 注入含 rom PySide6... 的伪文件 → qt_binding_self_check() 返回 ['src\fake_bad.py:1: PySide6']；再 monkeypatch __import__ 使 src.ui.toast 抛 ImportError → 返回 ['src.ui.toast: import failed: ...']。两闸门均能失败，非假闸门；扫描路径基于 Path(__file__).parents[2]/src|scripts，真实存在 |
| 确认框两分支实测 | 通过 | scripts/probe_ux_confirm_delete.py（offscreen）：Yes→True、Cancel→False、exec 抛异常→False 不崩、count<=0 无对话框，probe: PASS rc=0 |
| 受保护脚本零 diff | 通过 | git diff --stat 15 个修改文件与 t11 时一致；selftest_smoke/update_checker/settings_bugs/desktop_pet 均无 diff（multimon 的 +39 行为 t4 期 BUG-06/G11 用例，非本轮新增） |
| 6 条 verify 复跑 | 全 rc=0 | compileall=0；smoke 42/42=0；update_checker 12/12=0；settings_bugs 6/6=0；multimon 4/4=0；desktop_pet OK=0 |
| 越界/回退 | 通过 | t14 实改仅 src/ui/dialogs.py；t11 已通过项（G11/G2/G4/R1）代码未变（diff stat 一致）；F-103 未扩界 |
| docs/ux-fix-log-4.0.28.md t14 节 | 一致 | :109-153 自述与实况相符 |

### Findings（均不阻塞）

- F-104（low，观察）：scripts/probe_ux_confirm_delete.py 为非 selftest_* 命名的一次性探针留存于 scripts/。其内容含可复用的闸门调用与分支 stub 实测，建议后续并入 selftest_smoke 或改名 selftest_ux_confirm_delete.py 归入正式套件；本轮不动。
- F-103（沿用 t11 low）：拖放回收路径失败仍静默，产品判定外，未扩界。
