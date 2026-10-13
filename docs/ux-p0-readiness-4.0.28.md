# v4.0.28 六个 P0 用户体验缺口现状核查与修复方案（只读核查）

- 基线：v4.0.28 当前工作区（2026-10-13 核查）；未修改 src/、scripts/、docs/FULL_TEST_CASES.md。
- 输入依据：docs/ux-decisions-4.0.28.md（Q1~Q4、G1~G12 判定）、docs/ux-checklist-4.0.28.md（UX-F/UX-P）、docs/bug-report-4.0.28.md（BUG-06）、docs/static-review-4.0.28.md（F1~F12）。
- 每项给出：结论 / 证据（文件:行号）/ 触发路径 / 最小修复方案 / 风险 / 验证方式 / 成本。

## 汇总表

| 项 | 结论 | 成本 | 建议顺序 |
|---|---|---|---|
| G1 拖放取消幽灵残留 | 已实现（清理路径完备，仅在进程被杀时可能残留） | 小（可选：加保险日志） | 5（可不做） |
| G2 只读/无权限写入失败无提示 | 缺失（虚拟分区路径静默失败） | 小 | 2 |
| G4 批量部分失败反馈与撤销 | 部分实现（失败静默跳过，无汇总、无撤销） | 中 | 3 |
| G8 删除确认与还原入口 | 确认=缺失；还原=仅系统回收站，无应用内还原 | 中 | 4 |
| G11 分页按钮单点依赖宠物 | 已实现但不可靠（BUG-06：宠物托管时边缘栏 0 按钮，页切换只存在于宠物气泡） | 中 | 1 |
| Q3 分区内自动换行 | 已实现（行优先网格+宽度自适应列数），需回归验证而非修复 | 小（仅回归） | 6（仅验证） |

建议实施顺序：G11 → G2 → G4 → G8 → Q3（仅回归）→ G1（可选保险日志）。耦合关系见 §7。

## 1) G1 拖放取消后幽灵图/预览残留

**结论：已实现，清理生命周期完备。** 幽灵 QLabel 生命周期：创建 `src/ui/fence_icon_item.py:1605-1617`（objectName `desktidyPublicDragGhost`，Tool|Frameless|StaysOnTop|DoesNotAcceptFocus，WA_TransparentForMouseEvents），显示 `_show_drag_ghost :3077-3086`（`src/win_shell.py:1426-1429` arm 永久 WS_EX_NOACTIVATE|TRANSPARENT|TOOLWINDOW），45s 超时保险 `:1637-1641`；`finally :1656-1668` hide+setParent(None)+deleteLater（含 RuntimeError 守卫）。取消路径 `:1671-1672` 在幽灵拆除后返回 IgnoreAction。会话收尾：`src/ui/fence_icon_item.py:1247-1262 _end_overlay_drag_session` → `src/app.py:1672-1741 _end_overlay_drag`：销毁 unpin catchers、`_overlay_drag_active=False`、`_clear_ole_handoff_overlays`（:1685）、`_shell_attach_quiet_until = now+1.2s`（:1689-1692）；quiet_end 分支（:1712-1726）恢复围栏 Z 序 + `_remap_hidden_overlay_hwnds` + `ensure_live_fences_interactive`；完整分支（:1727+）QTimer 0ms `_heal_overlays_after_shell_menu` + 120ms `_flush_public_refresh_after_drag` + 延迟前台同步。源隐藏/还原：`_conceal_widgets_for_custom_drag :3013-3023` / `_reveal_widgets_after_custom_drag :3026-3032`（finally :1901-1903），`_drop_concealed_for_paths :3045` 防回收/移动后的格子复现。

- 触发路径：拖放被 ESC / 无效目标取消 → QDrag.exec 返回 IgnoreAction → finally 链。
- 残留风险：仅当进程被强杀或 45s 超时在取消前触发（`win_shell.py:1246` 注释表明 ghost 窗口为永久 skip 样式，Windows 自身会在进程退出时回收 HWND）。
- 最小修复：可不做；如需保险，在 `_end_overlay_drag` 里加一条 ghost-HWND 扫描日志（`EnumWindows` 匹配 `desktidyPublicDragGhost`），不改行为。
- 风险：无（纯日志）。验证：`python scripts/live_gui_click_test.py`（拖拽取消分支截图）；人工 ESC 拖拽。
- 成本：小（可选）。

## 2) G2 只读介质/无权限写入失败 toast

**结论：缺失。** toast 基础设施已存在：`src/ui/toast.py:66-95 show_toast`（单例、info/success/warn、anchor）。但虚拟分区放置失败是静默的：`src/ui/fence_widget.py:2792-2884 _apply_virtual_drop_paths` 捕获所有异常仅 `get_logger().exception("virtual drop apply failed")`（:2877-2884）返回 False，无 toast；调用方 `src/ui/fence_icon_item.py:1518-1532 _apply_fence_virtual_drop` 同样静默返回 False。对照：粘贴失败 toast 已有（fence_icon_item :2197、:6748、:6826；无法复制/剪切 :6726-6808；文件夹移动 :2413-2437、:4323-4518），即真实只读介质移动路径有提示，**虚拟分区（settings 写入/引脚路径）失败无提示**。

- 触发路径：拖文件入分区 → `_apply_virtual_drop_paths` 抛 OSError/权限错 → 仅记日志。
- 最小修复：在 `_apply_fence_virtual_drop` 失败分支（返回 False 前）调 `show_toast("放入分区失败", f"「{fence.name}」写入失败：{err}", level="warn", anchor=fence)`；`_apply_virtual_drop_paths` 保持返回 False 语义不变。
- 风险：低；注意 toast 单例会顶掉并发提示，失败路径低频，可接受。
- 验证：`python scripts/selftest_drop_matrix.py`（现有 wiring 不回归）+ 人工：目录 ACL 拒绝后拖入分区截图 toast；无现成脚本覆盖 toast，需在验收脚本中新增断言或标人工。
- 成本：小。

## 3) G4 批量操作部分失败反馈与撤销

**结论：部分实现（失败静默跳过）。** 回收路径：`src/ui/fence_icon_item.py:4005-4043 _recycle_drag_paths_now` 循环 `delete_to_trash`，逐项 try/except continue，只返回成功 done 列表；`:4046 _finalize_recycled_drag_paths` 清引脚，失败项无任何汇总。`delete_selected_fence_items :4847-4886` 同样逐路径 try/except continue，recycled 列表后无失败 toast、无撤销栈。宠物侧：`src/ui/pet_widget.py:1750 _recycle_paths_now`、`:1827 _cleanup_pins_after_trash` 同构。Q4 标准为「toast 汇总失败项 + 可撤销、禁止静默跳过」，当前两条都不满足。

- 触发路径：多选拖入回收站/删除键，其中个别文件被占用（WinError 32）或权限拒绝。
- 最小修复：在 `_recycle_drag_paths_now` 与 `delete_selected_fence_items` 收集 `failed: list[Path]`，结束时若有失败调 `show_toast("部分删除失败", f"{len(failed)} 项未能放入回收站", level="warn")`。撤销栈本轮可不做（Q4 撤销为 P0 内可选增强，先补可见性）。
- 风险：低；注意失败列表可能含已部分成功的路径（send2trash 半失败），toast 文案不要暗示全失败。
- 验证：人工锁定一个文件后批量删除；或临时以只读目录构造失败。无现有脚本，验收标人工或新增最小脚本。
- 成本：中（含撤销则大；仅 toast 汇总为小，建议拆两步：先 toast=小，撤销栈另立任务）。

## 4) G8 删除到回收站的确认提示与还原入口

**结论：确认=缺失；还原=依赖系统回收站，应用内还原入口缺失。** 删除链路无任何确认对话框：`src/ui/fence_icon_item.py:6834-6839` dispatch_desktop_icon_chord 'delete' → 直接 `delete_selected_fence_items`；Delete 键 `src/ui/fence_widget.py:2514-2525` 同样直删。对照项目内已有确认模式：`src/ui/notepad_window.py:816-823`、`:2398-2405` QMessageBox.question「此操作不可撤销」；`desktop_layout_widget` ask_yes_no :1339/:1417。还原：删除走 `win_shell.delete_to_trash :1883-1889`（send2trash，含 .lnk CLSID 处理），文件在真实回收站（系统可还原），但全库 grep「还原」仅命中桌面快照还原，无应用内回收站浮标/还原入口。

- 触发路径：选中图标按 Delete / 右键删除。
- 最小修复：在 `delete_selected_fence_items` 入口对非空 recycled 结果弹 `QMessageBox.question`（复用 notepad 模式，文案注明「已移入回收站，可在系统回收站还原」），记住「不再询问」可写入 settings；还原入口本轮先不做（依赖回收站枚举，成本大）。
- 风险：确认框会打断批量拖放回收路径——`_recycle_drag_paths_now` 是拖放语义，**不要**对其加确认，只对键盘/右键显式删除加。
- 验证：`python scripts/selftest_drop_matrix.py` 不回归 + 人工按 Delete 弹框截图。
- 成本：中（确认=小~中；应用内还原=大，建议另立任务）。

## 5) G11 分页按钮回归（宠物托管浮标栏单点依赖）

**结论：已实现但不可靠（BUG-06 实测成立）。** 默认设置下 `desktop_pet.enabled=True, show_page_bubbles=True`（`src/desktop_pet.py:240-263`，:251）→ `desktop_pet_hosts_float_bar :386-390` 为真 → 两处边缘栏分页被抑制：`src/app.py:6767-6771 _setup_page_indicator` 直接 return（整个边缘栏不创建）；`src/ui/page_indicator.py:813 show_pages_here = not desktop_pet_hosts_float_bar(...)`，`:821-823` 宠物托管时 early return（folders/tools 也留给宠物）。页切换芯片只画在宠物精灵上（`src/ui/pet_widget.py:546/:556/:661/:740-751 page_changed`、`:2011 _paint_page_bubbles`），无独立 HWND。BUG-06 实测：默认 → buttons=0（0x20）；`desktop_pet={'enabled':False}` → buttons=1。`desktop_pet_shows_page_bubbles :393-395` 是 hosts_float_bar 的别名（页+工具+文件夹全托管）。设置变更重建可用：`pet_widget._refresh_edge_page_bar :1009-1033` → `app._setup_page_indicator`。

- 触发路径：默认安装 + 多页 + 宠物贴边 → 边缘栏无页按钮，宠物被拖走/遮挡时无法切页（Q2 判定：二者并存、分页不得单点依赖宠物）。
- 最小修复（两处门都要动，缺一不可）：
  1. `src/app.py:6770` 的 `desktop_pet_hosts_float_bar` 早退 gate 改为「宠物托管工具/文件夹但仍创建边缘栏」——即删去该 early-return 或将其降级为只影响工具芯片；
  2. `src/ui/page_indicator.py:813/:821-823`：`show_pages_here` 恒为 True（页按钮始终在边缘栏），`:821-823` 的 early return 收窄为仅 folders/tools 让给宠物。
- 风险：页按钮与宠物页芯片重复展示（Q2 明确允许并存）；`scripts/selftest_multimon_chrome.py:105-130` 用 `desktop_pet={'enabled':False}` 才拿到可见边缘栏（注释 :115-117 即 BUG-06），修复后该测试的 settings 需要随之调整，否则断言对象变化。
- 验证：`python scripts/live_gui_click_test.py`（`page_bar_windows() :179-187` 找边缘栏 HWND，`test_page_indicator_clicks :402-429` 点上带截图）默认设置下应得 buttons≥页数；`python scripts/selftest_desktop_pet.py`（:532-545 hosts_float_bar 语义不回归）。
- 成本：中。
- 耦合：与 `src/desktop_pet.py` show_page_bubbles 默认值、`src/ui/page_indicator.py:813/:821` 创建条件、`src/app.py:6770` gate、`scripts/selftest_multimon_chrome.py:115-118` 强耦合；与 Q2（并存判定）互为前提。

## 6) Q3 分区内自动换行

**结论：已实现，仅需回归验证。** `src/ui/fence_widget.py:872` items_layout=QGridLayout（spacing 4, margins 2,2,8,4, AlignHCenter|AlignTop）；`_fit_grid_columns`（静态）:1804-1834 按可用宽度自适应列数（1 列时收窄 label）；`_icon_layout_metrics :1841-1872`；refresh `:1918` 行优先填充（row,col=0,0 :2028，addWidget :2062-2064，列满换行 :2065-2069，上限 60 :2070-2071）；`_update_icon_scale :1874-1916` 在 max_cols 变化时重刷；resizeEvent :4020-4027 → 非折叠非 `_resize_mode` 时 `_update_icon_scale`（`:596/:602` lambda fast=True/False 防抖）；列表模式恒 1 列 :1846-1847。窄分区修复 bad0286 已合入。唯一延迟重流的 `_resize_mode` 在松手后恢复。

- 验证：`python scripts/live_gui_click_test.py` 窄分区用例 + 人工把分区拖到极窄宽度观察换行不重叠不越界；无需代码改动。
- 成本：小（仅回归）。

## 7) 耦合关系与实施顺序

- **G11 ↔ src/desktop_pet.py（show_page_bubbles 默认值）↔ src/ui/page_indicator.py:813/:821 ↔ src/app.py:6770 ↔ scripts/selftest_multimon_chrome.py:115-118**：同一组 gate，必须一次改齐，否则边缘栏与宠物二选一失效。先做（影响 Q2 验收基线）。
- **G2（toast）是 G4（失败汇总 toast）的基础设施**：G2 先落，G4 复用同一 show_toast 调用约定与文案风格。
- **G4 与 G8 共享删除链路**（`delete_selected_fence_items` / `_recycle_drag_paths_now`）：G8 确认框只加显式删除路径，G4 汇总只加拖放批量路径，二者改同一函数的不同分支，需在同一次回归里一起验证。
- **G1 与 Q3 无代码耦合**：G1 建议不做或仅日志；Q3 仅回归。
- 实施顺序：G11 → G2 → G4（toast 汇总）→ G8（确认框）→ Q3 回归 → G1 保险日志（可选）。G4 撤销栈与 G8 应用内还原成本大，建议另立后续任务，不阻塞本轮 P0。
