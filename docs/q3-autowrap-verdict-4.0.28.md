# Q3 自动换行实现状态裁定（q3-autowrap-verdict-4.0.28.md）

## 结论

**A) 自动换行确有实现**——`src/ui/fence_widget.py` 的图标网格本身就是行优先自动换行布局（`QGridLayout` + 宽度自适应列数 `max_cols`，列满即换行），且 resize 路径已接线（resizeEvent → `_update_icon_scale` → `_refresh_impl` 重排）。t5 实测 0 命中的原因是**判据口径错位**：t5 用「自动化断言/日志中检索 `换行/折行/wrap` 关键字」作为「已实现」判据，但代码里换行不是以关键字形式存在的独立功能——它是 `_fit_grid_columns` 计算列数 + `refresh` 逐项 addWidget 换行的固有行为，源码与日志中自然无 `wrap` 字样；且既有脚本从未构造「把分区缩窄到强制折行」的用例。所以「日志 0 命中」只证明「缺自动化断言」，不证明「未实现」。t9 的静态判定成立；t5 降级理由（无自动化证据）在流程上也成立，但结论应更正为「已实现、缺回归断言」而非「部分/可能缺失」。

## 证据链（可复现静态证据）

1. **列数由分区宽度自适应**：`src/ui/fence_widget.py:1804-1834 _fit_grid_columns(usable, icon_size, ...)` —— `max_cols = max(1, (usable + h_spacing) // (ideal_cell + h_spacing))`（:1818），随后 while 循环逐级减列直到 `total <= usable`（:1819-1833）；1 列也放不下时收缩 label（:1828-1832）。即列数随宽度单调收缩，这就是「分区变窄→列数变少→换行」的机制本体。
2. **可用宽度来自真实分区宽度**：`src/ui/fence_widget.py:1761-1794 _available_items_width()` —— 取 `self.width()`（live 宽度优先于保存 cfg_w，:1780-1783），经 scroll viewport 校正（:1784-1793）。分区被拖窄 → usable 变小 → max_cols 变小。
3. **行优先填充、列满换行**：`src/ui/fence_widget.py:2028` `row, col = 0, 0`；`:2062-2064 addWidget(item, row, col)`；`:2065-2069 col += 1; if col >= max_cols: col = 0; row += 1` —— 严格的行优先网格换行。上限 60 项（:2070-2071），超出直接 break（不溢出）。
4. **网格容器参数**：`src/ui/fence_widget.py:872` items_layout = QGridLayout（spacing 4、margins 2,2,8,4、AlignHCenter|AlignTop）——QGridLayout 本身保证单元格不重叠。
5. **resize 已接线**：`src/ui/fence_widget.py:4020-4027 resizeEvent` → 非折叠、非 `_resize_mode` 时调 `_update_icon_scale()`；`:1874-1893` max_cols 变化时直接 `self.refresh()` 重排（:1889-1893）；`:1918/1969` refresh → `_refresh_impl` 以新 max_cols 重建网格（:1980、:2017、:2028 起）。拖动缩窄分区松手即触发重排（拖动过程中 `_resize_mode` 延迟重排，防止重排风暴，:4024-4026 注释）。
6. **列表模式恒 1 列**：`src/ui/fence_widget.py:1846-1847`，无换行需求场景单列放行。

综合：`分区宽度 → _available_items_width → _fit_grid_columns → max_cols → refresh 逐行 addWidget(col>=max_cols 换行)` 链条每一环都有明确 file:line，是活代码而非死代码（resizeEvent 与 debounce 双入口都会触发）。

## t5 为什么 0 命中（口径解释）

- t5（docs/ux-report-4.0.28.md:45、:145 C-5）的判据是「两个脚本与全部 UX 日志中检索 `换行/折行/wrap` 0 命中」→ 降级为「部分」。该检索逻辑只对「换行作为独立命名功能」成立；此处换行是布局算法的隐式行为，源码里函数名是 `_fit_grid_columns` / `_icon_layout_metrics` / `_refresh_impl`，不含 wrap/换行关键字，0 命中是必然结果。
- 既有脚本（selftest_desktop_layout_cards、page_switch_flash）只断言「列数/宽度/几何」，从未构造「宽度逐级缩至 1 列且断言不重叠不越界」的用例（t5 自己也指出「窄分区极端宽度边界值亦未覆盖」）。因此缺的不是实现，是**针对 Q3 判定标准的回归断言**。
- 产品判定标准（docs/ux-decisions-4.0.28.md Q3：「分区宽度逐级缩至能容纳 1 列，任意宽度下图标不重叠、不越出分区底边」）按上述证据链在静态层面满足：单元格由 QGridLayout 保证不重叠（机制 4）、列数由 usable 收缩保证不越界（机制 1/2）、60 项上限防溢出（机制 3）。

## 修复建议（不属本任务执行，供 t10/t12 参考）

不需要实现代码，需要补**回归验证**：新增一条最小用例（可加入 scripts/ 现有 UX 脚本或 live_gui_click_test 流程）——创建分区后把宽度从 600px 逐级缩到 MIN_WIDTH（如 160px），每级断言：(a) 每个 FenceIconItem 几何互不重叠；(b) 所有 item 右边距 ≤ 分区宽度；(c) 网格行数随宽度减小单调不减。成本小（约 30-50 行脚本，复用 selftest 框架的 offscreen widget + resize() 调用）。风险：无 src 改动。

## 交叉核对声明

本报告只读完成：未修改 src/、scripts/；仅新增本文档。t9 结论维持；t5 的「部分」应按本报告更正为「已实现，缺自动化断言」。
