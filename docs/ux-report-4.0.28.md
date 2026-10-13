# DeskTidy v4.0.28 用户体验交互测试报告（t5 第二部分）

- 执行者：测试工程师B（AgentTeams t5，attempt 4；复用 attempt 2/3 已落盘证据 + 本轮补跑 M-04b/M-09/M-02b 探针）
- 版本：v4.0.28；解释器 `.venv\Scripts\python.exe`（CPython 3.12.10 / PyQt6 6.11.0）
- 环境变量：`PYTHONIOENCODING=utf-8`、`PYTHONUTF8=1`、`DESKTIDY_SELFTEST=1`
- 判据来源：`docs/ux-checklist-4.0.28.md`（UX-F-01~20、UX-P-01~09）、`docs/ux-decisions-4.0.28.md`（Q1~Q4、G1~G12）
- 证据根目录：`logs\v4028\t5\`（截图 `logs\v4028\t5\shots\`）
- 约束遵守：未修改 `src/`、`scripts/`、`docs/FULL_TEST_CASES.md`

> 范围说明（按队长 t5 收口口径）：本报告只对「已有证据可判定」的条目下结论；证据不足的一律标「未覆盖 + 原因 + 人工步骤」，不凭推测填结论。无法自动化项（真实外部应用拖放、UAC 提权、回收站浮标字形、活体会话）列为人工项。修复后的全量 29 项复核由 t12 承担。

## 1. 结论摘要

| 口径 | 计数 |
|---|---|
| 清单条目总数 | 29（UX-F 20 + UX-P 9） |
| 通过（判据自动化满足） | **6**（F-07、F-08、F-13、P-01、P-08、P-09） |
| 部分通过（主判据满足，副判据未覆盖） | **12**（F-01、F-02、F-03、F-04、F-05、F-09、F-16、F-17、F-19、P-04、P-06、P-07） |
| 判据未满足（有代码级证据，属产品问题） | **2**（F-10 批量部分失败静默、F-18 删除无确认） |
| 因环境前置无法判定 | **1**（F-15 活体空板右键，需运行中的 DeskTidy 会话） |
| 未覆盖（需人工/活体会话） | **8**（F-06、F-11、F-12、F-14、F-20、P-02、P-03、P-05） |

- 参与取证的自动化脚本 16 个：14 个 `ux_*`（13 退出码 0 / 1 退出码 1）+ `selftest_public_drag.py`（0）+ `selftest_full_features.py`（1，环境前置聚合）。
- 关键帧截图 5 张（wait/play/sleep/trash/default），像素级非空校验通过（alpha 覆盖率 6%~32%）。
- 体验问题分级：**阻塞使用 1 条**（E-01 删除无确认 + 批量部分失败静默）、明显不适 2 条（E-02、E-05）、打磨项 4 条。

## 2. 执行方式与限制

执行方式标记：`S` = 现有自测脚本；`P` = 本轮可量化探针（`logs\v4028\t5\probe_ux_metrics.py`、`probe_ux_metrics2.py`）；`M` = 人工项（已给步骤，未执行的不填结论）。

环境限制（影响哪些判据必须留待人工）：
1. **无真实屏幕合成/无字体目录**：`QFontDatabase: Cannot find font directory .../Qt6/lib/fonts`，Qt offscreen 渲染下定时器与重绘节流（详见 §5 M-09 归因），因此「动画帧率/流畅度」类判据本轮不可判定。
2. **无运行中的 DeskTidy 活体会话**：所有依赖 `public host HWND` 的脚本直接前置失败。
3. **无 `dist\DeskTidy.exe`、无 ffmpeg**：影响打包版与录屏相关判据。
4. 探针在自建 QApplication 中运行，不驱动真实鼠标/拖放，故所有交互路径以「代码契约 + 断言」取证，视觉反馈（高亮、指示线、闪烁）无法自动判定。

## 3. 文件交互 UX-F-01~20

| 用例 | 判据要点 | 方式 | 实际结果（含验证到的断言名） | 结论 | 证据（退出码） |
|---|---|---|---|---|---|
| UX-F-01 资源管理器→分区拖入 | 高亮落点；松手 ≤200ms 出图标；取消无残留 | S | `通过 6 失败 0`：`public host Explorer desktop`、`pet sprite-only OLE`、`custom drag bridges`、`post-drop fence interactive`、`pet drop zone live`、`public host drop live`。**视觉高亮**与**中途取消幽灵图残留**未自动验证（G1→t12） | 部分 | `ux_selftest_drop_matrix.log`(0)、`selftest_public_drag.log`(0)、`ux_selftest_public_host.log`(0) |
| UX-F-02 落点判定（边缘/中心/缝隙） | 落区唯一；不双写 | S | drop_matrix 6 项含 `post-drop fence interactive`（落地后分区可交互）；缝隙/边缘的**落区视觉提示**未自动验证 | 部分 | `ux_selftest_drop_matrix.log`(0) |
| UX-F-03 释放后图标出现时机 | 松手即出现；只读/无权限有 toast | S | `拖拽避开 Explorer OLE 死锁`、`拖拽时隐藏源图标不重影`、`拖到外部门窗不挪位消失` 通过；**只读/无权限 toast** 未覆盖 | 部分 | `selftest_public_drag.log`(0) |
| UX-F-04 分区内拖拽重排 | 插入指示线；顺序即时生效；拖出留原位 | S | `selftest_public_drag.py::test_same_fence_reorder_no_flash` 通过（区内重排无闪屏）；**插入指示线**未覆盖（G3 = N/A，t8 判定本轮不要求） | 部分 | `selftest_public_drag.log`(0) |
| UX-F-05 自动换行 | 任意宽度无重叠/溢出；最小宽度不塌陷 | S | layout_cards `通过 2 失败 0`（`编辑分区透明度滑条`、`双栏分页列表/分区图标`）+ page_switch_flash `通过 15 失败 0`（含 `首次创建分区用保存尺寸`、`首次进入分页网格用保存宽度`）；**「自动换行」本身无任何自动化断言**——在两个脚本与全部 UX 日志中检索 `换行/折行/wrap` 均为 0 命中，本轮无「已实现」证据（Q3 列为 P0，G2/Q3→t12）；窄分区极端宽度边界值亦未覆盖 | 部分 | `ux_selftest_desktop_layout_cards.log`(0)、`ux_selftest_page_switch_flash.log`(0) |
| UX-F-06 拖到屏幕边缘 | 边缘 8px 内操作 10 次无崩溃 | M | 未执行（需真实 OS 鼠标拖拽与活体会话） | 未覆盖 | — |
| UX-F-07 框选 | 框选矩形可见；松手进多选态；空框不误删 | S | `通过 4 失败 0`：`Ctrl/Shift 选中与多路径拖拽`、`刷新保护多选拖拽路径`、`多选移动到分区`、`多选删除与多PIDL菜单`；另 `public_host` 的 `plate refresh + marquee abort` 覆盖框选中止 | 通过 | `ux_selftest_public_multiselect.log`(0)、`ux_selftest_public_host.log`(0) |
| UX-F-08 Ctrl 点选 | 每次切换 <100ms；可取消选中 | S | 同上脚本的 `Ctrl/Shift 选中与多路径拖拽` 断言通过（未做耗时量化，判据按功能满足计） | 通过 | `ux_selftest_public_multiselect.log`(0) |
| UX-F-09 Shift 连选 | 区间全选；锚点消失降级不崩 | S | 同一断言覆盖区间选择；`刷新保护多选拖拽路径` 覆盖刷新期间的选择保护；**「锚点图标已被删除」场景**未覆盖 | 部分 | `ux_selftest_public_multiselect.log`(0) |
| UX-F-10 批量删除/移动 | 一次生效；部分失败逐项提示 | S + P | 批量路径断言通过（`多选删除与多PIDL菜单`），但**部分失败被静默跳过**：`src/ui/fence_icon_item.py` 的 `except Exception: continue`（探针 M-06 实测 `静默跳过=True`，`src/ui/fence_icon_item.py:4868-4872`）→ 判据「逐项提示」未满足（Q4 要求 toast 汇总失败项，G4→t12） | **否** | `ux_selftest_public_multiselect.log`(0)、§5 M-06 |
| UX-F-11 双击打开默认程序 | ≤300ms 启动默认程序；失败有提示 | M | 未执行（需真实默认程序与外部窗口） | 未覆盖 | — |
| UX-F-12 多选双击 | 语义一致（Q1：打开全部选中项并保持多选态） | M | 未执行（本轮未验证实现是否已按 Q1 落地；清单修订后语义已冻结） | 未覆盖 | — |
| UX-F-13 右键菜单·分区背景 | 仅背景相关项；Esc 无副作用 | S | `通过 5 失败 0`：`CreateViewObject builds background menu`、`API + FenceWidget wiring`、`menu host does not destroy borrowed HWND`、`menu host TOOLWINDOW + chrome FG`、`portal paste branch` | 通过 | `ux_selftest_fence_bg_menu.log`(0) |
| UX-F-14 右键菜单·文件图标 | 含打开/重命名/删除/属性；反映选中数量 | M | 未执行。静态事实：图标菜单由系统 Shell 文件菜单承载（`src/shell_file_menu.py`、`src/ui/fence_icon_item.py:5050 after_shell_file_menu`），**「删除 N 个项目」数量文案未在代码中生成**，需实测确认 | 未覆盖 | — |
| UX-F-15 右键菜单·空白桌面 | 桌面级菜单；无选中不出现多选项 | S | 分项：`ux_selftest_folder_rmb_ghost.log` → `OK contracts`、`OK call_on_main_thread`（2 项 OK，退出码 0）；`ux_selftest_live_empty_rmb.log` → `FAIL  no DeskTidy public host HWND (is DeskTidy running?)`（退出码 1）。`public_host` 侧 `empty plate RMB passes to DefView`、`request_desktop_background_menu posts WM_CONTEXTMENU` 通过 | **否（环境前置）** | `ux_selftest_folder_rmb_ghost.log`(0)、`ux_selftest_live_empty_rmb.log`(1)、`ux_selftest_public_host.log`(0) |
| UX-F-16 键盘导航/重命名/删除 | 焦点框可见；F2 就地重命名 Enter/Esc；Delete 走回收站 | S | `通过 12 失败 0`：`快捷键与就地重命名契约`、`重命名提交与钉选改写`、`就地重命名 UI 提交收尾`、`LL 键盘钩子契约`、`剪贴板宿主不借用分区 HWND`、`CF_HDROP 剪切复制`、`dispatch 复制剪切路径`、`剪切粘贴后清理公共浮标`、`复制锚点与选中闸门契约`、`空白宿主板仍用分区选中复制` 等；**焦点框可见性**与 **Ctrl+A 全选后批量删除**未覆盖 | 部分 | `ux_selftest_fence_keys.log`(0) |
| UX-F-17 OLE 拖出到外部应用 | 每次拖出只复制一次；取消无副作用 | S | `selftest_public_drag.py` 全绿（含 OLE 多文件移交、重复写入、`拖到外部门窗不挪位消失`）；**第三方真实应用链路**未覆盖 | 部分 | `selftest_public_drag.log`(0) |
| UX-F-18 删除到回收站 + 可撤销 | 删除前有确认提示；可从回收站还原回原位置 | S + P | **未满足**：探针 M-05 实测 `src/ui/fence_icon_item.py:4847-4886`（`delete_selected_fence_items`）全函数无 `QMessageBox` 等确认调用（`源码含确认调用=False`），直接 `delete_to_trash`；回收站还原回原分区路径未验证（G8→t12） | **否** | §5 M-05 |
| UX-F-19 搜索唤起与输入反馈 | 搜索框 ≤200ms；输入到结果 <300ms；空态可读 | S | `通过 14 失败 0`：`默认配置与热键`、`locator 找到内置 fd`、`能搜到临时文件`、`已打开文件夹纳入搜索且全局深度足够`、`根目录去重与取消安全`、`app/扩展/托盘/安装脚本接线`、`数字键解析兼容 PyQt6`、`desktop 根与缓存失效`、`监视器删除通知`、`force prune 空跑不掐死节流` 等；**响应耗时未量化**、**空态 UI 文案**未覆盖 | 部分 | `ux_selftest_file_search.log`(0) |
| UX-F-20 搜索结果定位与高亮 | 滚动可见 + 闪烁高亮 ≥1s + 自动翻页 | M | 未执行（G9→t12） | 未覆盖 | — |

## 4. 宠物交互 UX-P-01~09

| 用例 | 判据要点 | 方式 | 实际结果 | 结论 | 证据 |
|---|---|---|---|---|---|
| UX-P-01 显示/隐藏宠物 | ≤200ms 出现/消失并写设置；资源缺失降级不崩 | S | `selftest_desktop_pet: OK`（设置默认值 `enabled/visible/show_page_bubbles=True`、`character=hoodie`、`size_scale=1.0`、姿态资源齐备断言全过）；出现/消失耗时未量化 | 通过 | `v_desktop_pet.log`(0)、`ux_selftest_desktop_pet.log`(0) |
| UX-P-02 拖动宠物 | 跟手、无残影；重启后位置保持 | M | 未执行。静态事实：拖动阈值与吸附阈值存在（`_DRAG_THRESHOLD=6`、`_SNAP_THRESHOLD=24`，`src/ui/pet_widget.py:87-88`） | 未覆盖 | — |
| UX-P-03 贴边行为 | 吸附 ≤100ms；不越出屏幕 | M | 未执行。静态事实：`_SNAP_THRESHOLD=24` 用于落点吸附（`src/ui/pet_widget.py` `_end_drag` 附近） | 未覆盖 | — |
| UX-P-04 单击 | 有明确反馈；不误触分区整理 | S | OK（单击动作面板/姿态入口断言通过）；**「单击不触发一键整理」**未覆盖 | 部分 | `ux_selftest_desktop_pet.log`(0) |
| UX-P-05 双击 | 双击专属姿态；间隔 ≥300ms 不误判单/双击 | M | 未执行 | 未覆盖 | — |
| UX-P-06 气泡与分页栏托管关系 | 芯片可见可点、不与宠物姿态重叠；关闭宠物后分页按钮回归常规位置 | S + P | 宠物托管断言通过；探针实测芯片 `n=3`、`min chip=44x26`（≥24px 可点）、两两不重叠 `overlaps=[]`、与身体矩形重叠 `[0,0,0]`（M-04b，body=0,41,180x220；芯片 (19,4)/(68,9)/(117,5) 均 44x26）。**「关闭宠物后分页按钮回归」**属 G11，t12 交互实测 | 部分 | §5 M-03/M-04b、`ux_selftest_desktop_pet.log`(0) |
| UX-P-07 动画状态机 wait/play/sleep/trash | 切换自然不卡帧；脚底锚点不漂移；切换 ≤300ms | S + P | 姿态映射与丢垃圾动画断言通过；tick 成本实测极低（`avg=0.0004ms p95=0.0004 max=0.0041 n=600`），**但探针环境下 cel 帧位未推进**：M-02 四姿态 `cel_fps=0.00 帧变化=0/300`、M-02b `distinct_frames=1`、M-09 真实定时器 `8.0/s`、`max_gap=311.5ms`（详见 §5 归因）。「不卡帧/切换 ≤300ms」本轮不可判定 | 部分 | §5 M-01/M-02/M-02b/M-09、`ux_selftest_desktop_pet.log`(0) |
| UX-P-08 设置开关即时生效 | ≤200ms 生效；保存失败不半应用 | S + P | `通过 6 失败 0`：`startup does not force page indicator on`、`hosted save prefers live bridge`、`storage origins atomic write and record` 等；20 次快速写盘耗时 611.1ms、最终状态一致（`visible=False bubbles=False`） | 通过 | `v_settings_bugs.log`(0)、§5 M-07 |
| UX-P-09 层级与遮挡顺序 | 菜单 > 浮标栏 > 分区 > 宠物；前台外部应用时不抬升 | S | overlay `通过 7 失败 0`：`raise owner contracts`、`common callers gated`、`skip raise when not explorer FG`、`raise when explorer FG`、`refuse detached HWND_TOP`、`attached HWND_TOP`、`page chrome foreign FG`；public host `通过 11 失败 0`（含 `host mask excludes taskbar gap`、`hosted icons share one toplevel`） | 通过 | `ux_selftest_overlay_raise.log`(0)、`ux_selftest_public_host.log`(0) |

## 5. 可量化判据实测数据

探针 1：`logs\v4028\t5\probe_ux_metrics.py` → `v_probe_ux_metrics.log`（`checks=10 pass=7 fail=3`，退出码 1）
探针 2：`logs\v4028\t5\probe_ux_metrics2.py` → `v_probe_ux_metrics2.log`（`checks=4 pass=1 fail=3`，退出码 1，7.1s）

| 编号 | 判据 | 实测值 | 判定 |
|---|---|---|---|
| M-01 | 宠物 tick 成本（30Hz 预算 33ms） | `avg=0.0004ms p95=0.0004ms max=0.0041ms n=600`；600 次 tick 无异常 | **PASS** |
| M-01b | 600 tick 内 `frame_index` 变化 | `1 个不同帧位`（20s 内未换帧） | 观察项（见 M-02） |
| M-02 | 四姿态 cel 帧率（10s 窗口） | wait/play/sleep/trash 均 `cel_fps=0.00 帧变化=0/300`，tick 成本 0.10~0.11ms | **未判定**（环境限制，见下） |
| M-02b | widget 帧位推进（300 次采样） | `distinct_frames=1 值=[0]` | **FAIL**（环境/探针口径，见下） |
| M-03 | 分页芯片布局与命中区域 | `n=3 left_pad=0 band_h=41 total_w=180`；`min chip=44x26px`（判据 ≥24px）；两两重叠 `overlaps=[]` | **PASS** |
| M-04 | 芯片与宠物身体不重叠（原始探针） | `FAIL overlap_areas=[1144,1144,1144]`（该版本用了错误的 body 原点） | **探针口径问题**，由 M-04b 取代 |
| M-04b | 芯片与宠物身体不重叠（`_body_origin()` 口径） | `body=0,41,180x220`；芯片 (19,4)/(68,9)/(117,5) 各 44x26；`overlap_areas=[0,0,0]` | **PASS** |
| M-05 | 删除前确认对话框 | `src/ui/fence_icon_item.py:4847-4886` 无确认调用（`源码含确认调用=False`） | **FAIL（真实产品问题）** |
| M-06 | 批量删除部分失败逐项提示 | `public 删除路径含确认=False`；`源码含 except Exception: continue 静默跳过=True`（`:4868-4872`） | **FAIL（真实产品问题）** |
| M-07 | 20 次快速写盘（设置竞态） | 耗时 `611.1ms`（debounce=0.2s）；最终状态一致 | **PASS** |
| M-08 | 5 张关键帧非空白（alpha 覆盖率 >1%） | default 200x266 **30%**、play 180x267 **32%**、sleep 354x289 **19%**、trash 524x487 **6%**、wait 459x267 **13%** | **PASS** |
| M-09 | 真实定时器动画节奏 | 3s 内 `tick=24`（**8.0/s**，期望 ≈30/s）、`paints=25`（8.3/s）、`max_gap=311.5ms` | **FAIL**（环境限制，见下） |

**M-02/M-02b/M-09 的归因（不据此判产品卡顿）**：探针运行在自建 `QApplication` + 无字体目录（`QFontDatabase: Cannot find font directory`）+ 无真实合成的环境中，重绘与定时器被明显节流（painting 仅 8.3/s、帧间隔 311ms），且 `DesktopPetWidget._anim.frame` 在探针采样口径下恒为 0。因此：
- 「动画流畅度 / 帧率 / 切姿态耗时 ≤300ms」在本环境**不可判定**，判为部分通过（tick 计算成本已证极低，无掉帧风险的直接证据）；
- 列为**人工复核观察项 E-05**（§8），不得据此写「已通过」也不得写「产品卡顿」。

## 6. 环境前置阻塞项

| 阻塞项 | 影响条目/脚本 | 表现 | 证据 | 处置 |
|---|---|---|---|---|
| 无运行中的 DeskTidy 活体会话 | UX-F-15、G11/G12 相关实测 | `FAIL  no DeskTidy public host HWND (is DeskTidy running?)`，退出码 1 | `ux_selftest_live_empty_rmb.log` | 需活体会话；P0 缺口交互实测归 t12 |
| 无 `dist\DeskTidy.exe`（未构建） | 打包版相关判据（非 UX 条目，但影响 A1） | `selftest_system` 76/78、`selftest_live_e2e` 前置失败 | `v_*`、`selftest_system.log` | 本轮禁令：不构建 |
| 无 ffmpeg | UX-F 无关，影响 A1 与录屏 | `selftest_recent` 83/84、`selftest_full_features` 内同断言 | `selftest_recent.log`、`ux_selftest_full_features.log` | 本轮禁令：不安装 |
| 单显示器 1536x834 | 多屏夹具类断言 | 1920x1032 硬编码断言不可满足 | `selftest_system.log` | 需多屏/可伪造夹具 |
| 无字体目录/无合成（Qt offscreen） | 动画帧率、重绘节流类判据 | `cel_fps=0.00`、真实定时器 8.0/s、`max_gap=311.5ms` | `v_probe_ux_metrics.log`、`v_probe_ux_metrics2.log` | 需真实屏幕人工复核（E-05） |
| HKCU `.txt` ProgId 状态 | `selftest_desknote.py`（回归侧，非 UX 条目） | `1 failure(s) - main/installer: txtfile` | `ux_selftest_full_features.log` | 待标准 .txt 关联环境复核，见回归报告 §3.5 |

## 7. 未覆盖项与原因（人工步骤）

| 未覆盖条目 | 原因 | 人工步骤（未执行，未填结论） |
|---|---|---|
| UX-F-06 边缘 8px 拖拽 | 需真实 OS 鼠标拖拽 | 启动 DeskTidy → 从资源管理器拖文件至屏幕最左/右/上边缘悬停 10 次 → 记录是否异常、落区是否明确 |
| UX-F-11 双击打开 / UX-F-12 多选双击 | 需真实默认程序窗口与焦点判定 | 双击 .txt/.png/.docx 各 1 次记录启动耗时与是否抢焦点；选中 3 项后双击其一，核对是否按 Q1「打开全部并保持多选态」 |
| UX-F-14 图标右键菜单（含数量文案） | 菜单由系统 Shell 承载 | 选中 3 个图标右键 → 截图菜单项文案，核对是否出现「删除 3 个项目」等数量文案 |
| UX-F-20 搜索结果定位高亮 | 属 G9（t12） | 搜索「报」→ 双击结果 → 观察图标是否滚动可见并闪烁 ≥1s、分区是否自动翻页 |
| UX-P-02 / UX-P-03 / UX-P-05 拖动、贴边、双击 | 需屏幕活体交互 | 拖宠物至四角与屏幕中央记录吸附与越界；间隔 400ms 与 150ms 各试记录单/双击语义 |
| 宠物动画帧率与切姿态耗时（UX-P-07 副判据） | 无合成环境（§5 归因） | 在真实桌面运行宠物 30s，记录帧率与最大帧间隔、四姿态切换耗时 |
| 真实第三方应用 OLE 拖出 | 需外部应用 | 从分区拖文件到聊天软件输入框，核对只产生一次复制 |
| 回收站浮标字形 / 删除后可还原回原分区（G8） | 需活体桌面宿主 | 删除文件 → 打开回收站还原 → 记录是否回到原分区原位置、浮标字形是否正常 |
| 关闭宠物后分页按钮回归（G11） | 需活体交互 | 关闭宠物开关 → 观察分页按钮是否回归常规位置且可点 |

## 8. 体验问题分级

| 编号 | 问题 | 分级 | 证据 | 复现步骤 | 期望 |
|---|---|---|---|---|---|
| E-01 | 删除文件/分区**无确认对话框**，且批量删除中单个失败被静默跳过（`except Exception: continue`） | **阻塞使用** | `src/ui/fence_icon_item.py:4847-4886`（无 `QMessageBox`）、`:4868-4872`（静默 continue）；探针 M-05/M-06 双 FAIL | 选中 ≥1 个文件按 Delete / 删除到回收站 | 删除前出现含名称的确认提示；部分失败逐项提示并提供可撤销汇总（Q4） |
| E-02 | 活体空板右键菜单无法在源码检出环境验证（脚本要求运行中的宿主窗口），当前无自动化保障 | 明显不适 | `ux_selftest_live_empty_rmb.log`(1)：`FAIL no DeskTidy public host HWND` | 运行 `scripts\selftest_live_empty_rmb.py` | 以活体会话运行该脚本并通过（t12） |
| E-05 | 宠物动画帧率/流畅度在无合成环境不可判定（实测 tick 成本极低但帧位未推进、重绘 8.3/s、帧间隔 311ms） | 明显不适（证据不足，需复核） | `v_probe_ux_metrics.log` M-01/M-02、`v_probe_ux_metrics2.log` M-02b/M-09 | 在真实桌面观察宠物动画与姿态切换 | 补齐真实屏幕下的帧率/切换耗时数据，确认无卡顿 |
| E-03 | 探针 `scripts/probe_v4028_boundary.py` 的 BND-A1 期望值仍是修复前口径，产生 1 条过期 DEV（BND-A12 已 OK） | 打磨项 | `v_boundary.log`：`BND-A1 '4.0.28.'->(4, 0, 28) expected (4, 0, 28, 0)` | 运行边界探针 | 更新探针期望值为 `(4, 0, 28)`（属 `scripts/`，本轮禁改） |
| E-04 | 交互视觉反馈类判据（拖放高亮、插入指示线、搜索定位闪烁、焦点框）无自动化保障 | 打磨项 | UX-F-01/02/04/16/20 的「部分/未覆盖」列 | — | 增加可视化断言或纳入 t12 人工清单 |
| E-06 | 无 `dist` + 无 ffmpeg 导致 A1 无法零失败、打包版与录屏路径无端到端覆盖 | 打磨项 | `full_plan_r3_attempt2.log`、`selftest_system.log`、`selftest_recent.log` | 运行 A1 三轮计划 | 在具备 dist/ffmpeg 的机器上重跑 A1 |
| E-07 | `docs/ux-decisions-4.0.28.md` 判定的 P0 缺口（G1/G2/G4/G8/G11/Q3 换行）本轮无一条取得「已修复」证据 | 打磨项（阻断项归 t12） | 本报告 §3/§4 的「部分/未覆盖」列 | — | 修复后由 t12 全量 29 项复核 |

## 9. 与 attempt 3 草稿的核对（以日志为准）

| # | 草稿表述 | 日志事实 | 处置 |
|---|---|---|---|
| C-1 | UX-P-06「与身体矩形重叠面积 0」（引用 M-03/M-04） | 原始 M-04 实为 `FAIL overlap_areas=[1144,1144,1144]`（错误 body 原点）；补跑 M-04b 得 `[0,0,0]` PASS，body=0,41,180x220 | 结论保留、证据更正为 M-04b；M-04 原始 FAIL 如实列出（§5） |
| C-2 | UX-P-07 判「通过：无掉帧」 | M-01 PASS，但 M-02 `帧变化=0/300`、M-02b `distinct_frames=1`、M-09 `8.0/s` 且 `max_gap=311.5ms` | 降级为「部分」，新增 E-05 人工复核项 |
| C-3 | UX-F-13 断言名「menu host TOOLWINDOW + chrome FG」等 3 项 | 实测 5 项为 `CreateViewObject builds background menu`、`API + FenceWidget wiring`、`menu host does not destroy borrowed HWND`、`menu host TOOLWINDOW + chrome FG`、`portal paste branch` | 按日志更正断言名 |
| C-4 | 未提 `ux_selftest_full_features` 归因 | `汇总: pass=22 fail=4`，4 项均环境前置（virtual/recent/desknote/system） | 补入回归报告 §3.5 #7 |
| C-5 | UX-F-05 判「通过（自动换行）」 | 全部 UX 日志与两个相关脚本中 `换行/折行/wrap` **0 命中**，判据无自动化证据 | 降级为「部分」，并在 E-07 中列为 P0 待 t12 |
| C-6 | §1.2 通过/部分计数 | 因 C-2、C-5 的降级，通过由 7 降为 6、部分由 11 升为 12 | 摘要已同步更正 |

## 10. 证据索引（第二部分）

| 证据 | 路径 | 要点 |
|---|---|---|
| 文件交互脚本（`ux_*`） | `logs\v4028\t5\ux_selftest_{drop_matrix,folder_rmb_ghost,live_empty_rmb,desktop_layout_cards,file_search,public_multiselect,public_host,portal,fence_bg_menu,fence_keys}.log` | 10 脚本，9 绿 1 红（live_empty_rmb） |
| 宠物/层级脚本（`ux_*`） | `logs\v4028\t5\ux_selftest_{desktop_pet,page_switch_flash,overlay_raise,settings_bugs}.log` | 4 脚本全绿 |
| 回归侧复用日志 | `logs\v4028\t5\selftest_public_drag.log`(0)、`v_desktop_pet.log`(0)、`v_settings_bugs.log`(0) | 退出码 0 |
| 聚合脚本 | `logs\v4028\t5\ux_selftest_full_features.log` | pass=22 fail=4（环境前置） |
| 量化探针 | `logs\v4028\t5\probe_ux_metrics.py` + `v_probe_ux_metrics.log`（checks=10 pass=7 fail=3）；`probe_ux_metrics2.py` + `v_probe_ux_metrics2.log`（checks=4 pass=1 fail=3） | M-01~M-09 |
| 关键帧截图 | `logs\v4028\t5\shots\pet_{default,wait,play,sleep,trash}.png` | 5 张，由 `probe_ux_keyframes.py` 生成（`v_probe_ux_keyframes.log`：`SUMMARY frames=5`），M-08 像素级非空校验通过 |
| 退出码汇总 | `logs\v4028\t5\attempt2_exitcodes.csv` | 全部脚本退出码与耗时 |
