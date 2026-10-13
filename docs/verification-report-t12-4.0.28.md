# DeskTidy v4.0.28 — t12 端到端回归与 P0 交互实测验证报告

- 任务：t12（verification）attempt 2，`attempt_id=301800d0-4260-4157-bb1a-2d12c1871d60`
- 验证对象：**当前工作区字节**——t10 P0 修复（G11/G2/G4/G8 + R1）+ t14 的 F-101 修复（`src/ui/dialogs.py` 绑定与兜底）
- 依赖链：t10 → t14 → t15（评审通过）→ 本任务
- 环境：Windows / `.venv`（PyQt6 6.11.0、psutil 7.2.2）；离线、未联网、未安装任何依赖；`QT_QPA_PLATFORM=offscreen` 用于探针
- 证据根目录：`logs\v4028\t12\`
- 约束遵守：未修改 `docs/FULL_TEST_CASES.md`、未修改 `src/` 与 `scripts/` 任何文件、未执行 `pip`

---

## 1. 结论摘要（判定）

| 项目 | 结论 |
| --- | --- |
| 契约 6 条 verify 命令 | **6/6 退出码 0**（§2） |
| P0 四项交互实测（G11/G2/G4/G8） | **18/18 断言通过**，含真实点击生效、失败 toast、批量失败汇总、删除确认三态（§3） |
| Q3 缩窄换行三条必做断言 | **通过**（不重叠、不越出可见区、行数单调不减），重排延迟实测 88–126ms（§4.1） |
| Q3 追加取证 | **发现缺陷 F-t12-01（medium）**：窄分区单列单元保持 210px、横向被裁 56–96px（§4.2） |
| `selftest_all.bat` | **11 步跑完、5 步失败**（缺 dist ×3、缺 ffmpeg ×1、外部终止 ×1），无多屏夹具以外的产品失败（§5.1） |
| t5 报告 21 条「部分/未覆盖/环境前置」复核 | 修复后通过 9 / 未通过 1 / 部分 1 / 仍缺环境前置 1 / 需人工 6 / 未复跑 3（§6） |
| 门禁判定 | **needs_revision**（P0 全通过，但 §4.2 为实测可复现的真实缺陷） |

---

## 2. 契约 verify 命令复跑（本 attempt 实测）

| # | 命令 | 退出码 | 耗时 | 证据日志 |
| --- | --- | --- | --- | --- |
| 1 | `.venv\Scripts\python.exe -m compileall -q src` | 0 | 0.1s | `logs\v4028\t12\verify_compileall.log` |
| 2 | `.venv\Scripts\python.exe scripts\selftest_smoke.py` | 0 | 1.5s | `logs\v4028\t12\verify_selftest_smoke.log`（`passed=42 failed=0`） |
| 3 | `.venv\Scripts\python.exe scripts\selftest_update_checker.py` | 0 | 0.3s | `logs\v4028\t12\verify_selftest_update_checker.log`（全 OK） |
| 4 | `.venv\Scripts\python.exe scripts\selftest_settings_bugs.py` | 0 | 0.4s | `logs\v4028\t12\verify_selftest_settings_bugs.log`（`passed=6 failed=0`） |
| 5 | `.venv\Scripts\python.exe scripts\selftest_multimon_chrome.py` | 0 | 0.3s | `logs\v4028\t12\verify_selftest_multimon_chrome.log`（含 `OK page buttons built with pet on and off (G11)`） |
| 6 | `.venv\Scripts\python.exe scripts\selftest_desktop_pet.py` | 0 | 18.6s | `logs\v4028\t12\verify_selftest_desktop_pet.log`（`selftest_desktop_pet: OK`） |

退出码汇总文件：`logs\v4028\t12\verify_rc.txt`。

**t4 修复项无回退的证据**：`selftest_update_checker`（BUG-04）、`selftest_settings_bugs`（F1/F3/F5）、`selftest_multimon_chrome`（G11 双配置）全部 rc=0；`compileall` rc=0 说明 t14 的 `src/ui/dialogs.py` 绑定修复无语法/导入错误。

---

## 3. P0 四项交互实测（G11 / G2 / G4 / G8）

探针：`logs\v4028\t12\probe_t12_p0.py`（offscreen，真实控件事件 + 真实 `QTest.mouseClick`/`mouseMove`，仅对 toast 与虚拟分区赋值做记录型替身）
结果：`logs\v4028\t12\p0_results.json`，日志 `logs\v4028\t12\probe_p0.log`，退出码 **0**（`total=18 fail=0`）。

| 断言 | 结果 | 证据要点 |
| --- | --- | --- |
| G11-wiring | OK | `page_changed` 真实接到 `app.switch_page`（信号→槽连线存在） |
| G11-pet-off | OK | 宠物关闭：**真实点击**边缘栏分页按钮 → 页码实际改变（`2→4→1`，非仅控件存在） |
| G11-pet-on | OK | 宠物开启：同上真实点击 → 页码实际改变 |
| G11-pet-on-hosts-folders-tools | OK | **宠物开启且只显示 folders/tools 组合**：分页按钮仍建于边缘栏，真实点击生效 |
| G2-none-added | OK | 源文件缺失 → `warn` toast《放置失败》，正文含 `1 项` |
| G2-samename-other-dir | OK | 同名异目录（钉选键为全路径）：判定未加入 → warn toast |
| G2-already-present | OK | 同一路径重复放置：**不报错**（设计如此，钉选去重，避免误报） |
| G2-runtime-error | OK | `assign_paths_to_virtual_fence` 抛 `RuntimeError` → warn toast |
| G2-exception | OK | 抛 `ValueError` → warn toast（异常不静默） |
| G2-success | OK | 成功路径返回 True 且**无 toast**（不误报） |
| G2-detail | OK | toast 正文点名失败项与目标分区 |
| G4-partial | OK | 3 项中 1 项删除失败 → toast 汇总（数量+名称），失败项**图标恢复显示** |
| G4-allfail | OK | 全部失败 → 汇总列出**每一个**失败项，无任何条目被静默丢弃 |
| G4-success | OK | 全部成功 → 无 toast、无假警报 |
| G8-cancel-in-delete | OK | `ask_confirm_delete` 返回 False → 不删除、无 toast |
| G8-default-cancel | OK | 对话框**默认按钮=取消**，且存在明确 Yes 按钮（不会回车即删） |
| G8-drag-recycle | OK | **拖放回收路径不弹确认框**，直接送回收站（与显式删除区分） |
| G8-callsites | OK | `ask_confirm_delete` 仅被 2 个显式删除函数调用（无越界接入拖放路径） |

> G4/G8 说明：t5 报告判为「判据未满足」的 F-10（批量部分失败静默）与 F-18（删除无确认）在本轮**代码级 + 交互级实测均通过**（见 §6）。
> t14 的 F-101 修复（`src/ui/dialogs.py` 改用 PyQt6 绑定 + 异常兜底）由 G8-cancel-in-delete/default-cancel/callsites 三条断言覆盖：确认框可实例化并按 Yes/Cancel 正确返回。

---

## 4. Q3 缩窄换行断言（t13 建议）

探针：`logs\v4028\t12\probe_t12_q3.py`；结果 `logs\v4028\t12\q3_results.json`；日志 `logs\v4028\t12\probe_q3.log`。
场景：18 项虚拟分区，宽度逐级 640→512→416→320→256→200→**MIN_WIDTH=160**（`src/ui/fence_widget.py:539 MIN_WIDTH=160`），每次 resize 后驱动真实事件循环（`QtTest.qWait`，因为重排是 debounce 定时器：`_icon_scale_debounce` 90ms / `_icon_scale_settle` 220ms），再读 `QGridLayout` 实际格位。

### 4.1 三条必做断言：通过

| 断言 | 实测 | 结论 |
| --- | --- | --- |
| 图标不重叠 | 全部宽度 `overlap_pairs=0`（几何两两求交，0 对相交） | 通过 |
| 不越出分区（可见区） | `spill_fence_visible=0`、`spill_host=0`、`footer_overlap_visible=0`（可见部分=格位 ∩ 滚动视口） | 通过 |
| 行数单调不减 | 列数 `5→4→3→2→1→1→1`，行数 `4→5→6→9→18→18→18`，且每档 `rows == ceil(cells/cols)` | 通过 |

重排延迟（resize → 网格采用新列数）实测：**90.4 / 98.6 / 126.3 / 101.5 ms**（首档与末档 ≤0.3ms，因列数未变无需重建），最大值 **126.3ms**，落在 debounce 设计区间内。

### 4.2 追加取证发现：F-t12-01（medium，真实、可复现）

**现象**：把已有分区从较宽拖窄到 ≤ 约 230px 时，单列图标单元宽度**固定保持 210px**（`cell_w` 序列 `118,113,120,135,210,210,210`），而滚动视口在 200px 宽度下仅 164px、在 MIN_WIDTH=160 下仅 124px → 每格右侧 **56px / 96px 被裁掉**，且 `src/ui/fence_widget.py:1591` 明确 `setHorizontalScrollBarPolicy(ScrollBarAlwaysOff)`，横向滚动条不显示（`hscroll_max=56/96` 仅有程序化范围），用户无法把它拖出来。表现为长文件名右侧被**硬裁切**而非省略号收敛。

**根因（实验隔离，非推测）**：`src/ui/fence_widget.py:1884-1885`
```python
if not fast:
    if icon_size == self._last_icon_size and max_cols == self._last_max_cols:
        return
```
`icon_size` 与 `max_cols` 都未变化（1 列、同一缩放）时提前 return，**新的（更小的）`label_width` 永远不会下发**到 `FenceIconItem.set_icon_size`（单元格最小宽度 = `max(icon_size+20, label_max_width+10)`，见 `src/ui/fence_icon_item.py:7678`）。实测 `label_w` 序列 `105,103,110,125,200,144,104` 已随宽度下降，但 `cell_w` 未跟随。

**对照实验（探针 forced 档，仅清零探针侧缓存）**：把 `_last_max_cols`/`_last_icon_size` 置 0 后走完整更新路径，同一场景下 `cell_w` 变为 `115,113,120,135,210,154,114`，`clipped=0`（全档），`clips` 消失。据此确认是该提前 return 造成，而非布局/尺寸策略本身。

**影响面**：用户可达（分区编辑器允许缩到 `MIN_WIDTH=160`，`fence_widget.py:539`）；只影响「**已存在分区被拖窄**」的路径——从零创建窄分区不会触发（无 210 的历史值）。

**建议修法（不在本任务范围，供修复方参考）**：提前 return 条件加入 `label_width == self._last_label_width`（需同时维护该缓存），或列数不变时也下发 `set_icon_size(icon_size, label_width)`。

**严重度**：medium（不阻塞使用；窄分区下载切可见、无滚动条可救；不属环境前置）。

---

## 5. `selftest_all.bat` 与 UX 脚本复跑

### 5.1 `scripts\selftest_all.bat`（`logs\v4028\t12\selftest_all.log`）

批次退出码 **1**，末行 `Steps run 11  failed 5`；cleanup 步骤 `Removed 0 selftest artifact(s)`。逐步骤：

| 步骤 | 脚本 | 退出码/结果 | 归因 |
| --- | --- | --- | --- |
| 1/5 virtual fences | `selftest_virtual.py` | 1 | **环境前置**：`_dist` 缺 dist 产物（`scripts/selftest_virtual.py:456`） |
| 2/5 recent features | `selftest_recent.py` | 1 | **环境前置**：无 ffmpeg（`scripts/selftest_recent.py:4912`） |
| 3/5 public drag | `selftest_public_drag.py` | 0 `[ OK ]` | — |
| 3b/5 public HWND merge | `selftest_public_host.py` | 0 `[ OK ]` | — |
| 3c/5 public multiselect | `selftest_public_multiselect.py` | **-1**（日志原值） | 进程被外部终止、该步输出截断；t5 同期该脚本 rc=0，本轮对该脚本无产品结论，不计入产品缺陷 |
| 3d/5 folder portal | `selftest_portal.py` | 0 `[ OK ]` | — |
| 3e/5 fence blank shell menu | `selftest_fence_bg_menu.py` | 0 `[ OK ]` | — |
| 3e2/5 fence keys and clipboard | `selftest_fence_keys.py` | 0 `[ OK ]` | — |
| 3f/5 multi-monitor chrome | `selftest_multimon_chrome.py` | 0 `[ OK ]` | — |
| 4/5 system selftest | `selftest_system.py` | 1 | **环境前置**：缺 dist（`scripts/selftest_system.py:1891`、`:2044`） |
| 5/5 packaged live e2e | `selftest_live_e2e.py` | 1 | **环境前置**：缺 dist |
| cleanup | `cleanup_selftest_artifacts.py` | 0 | 无残留 |

> 归因汇总（5 个失败步骤）：1/5 与 4/5、5/5 = **缺 dist 打包产物**（`dist\DeskTidy.exe` 不存在）；2/5 = **缺 ffmpeg**（`scripts/selftest_recent.py:4912`）；3c/5 = **-1（外部终止）**。多屏夹具类归因见 §7。
> 队长曾据 `selftest_all.log` 的 mtime（23:00:31）误判该批次为「挂起」，已核实为输出缓冲造成的假象：批次实际跑完 11 步并完整写出 `Steps run 11  failed 5` / `TEST SUITE HAS FAILING STEPS` 与 cleanup 段。本节以日志内容为准，不采用「挂起」归因。

### 5.2 追加 UX 脚本批次（本轮 t12 循环）

批次由队长在 `selftest_full_features.py` 的子步骤处终止，**退出码未捕获（以日志末行结论计）**：

| 脚本 | 结果 | 证据 |
| --- | --- | --- |
| `selftest_drop_matrix.py` | `通过 6 失败 0` | `logs\v4028\t12\ux_selftest_drop_matrix.log` |
| `selftest_folder_rmb_ghost.py` | `OK contracts` / `OK call_on_main_thread` | `logs\v4028\t12\ux_selftest_folder_rmb_ghost.log` |
| `selftest_live_empty_rmb.py` | `FAIL  no DeskTidy public host HWND (is DeskTidy running?)` | `logs\v4028\t12\ux_selftest_live_empty_rmb.log`（环境前置） |
| `selftest_desktop_layout_cards.py` | `通过 2 失败 0` | `logs\v4028\t12\ux_selftest_desktop_layout_cards.log` |
| `selftest_full_features.py` | **未完成**（终止于子步骤 `selftest_virtual.py`，dist 前置） | `logs\v4028\t12\ux_selftest_full_features.log` |
| `selftest_file_search.py` 及之后 | **未执行**（批次终止后未重跑） | — |

未复跑的脚本：`selftest_file_search.py`、`selftest_page_switch_flash.py`、`selftest_overlay_raise.py`、`selftest_public_multiselect.py`（挂起）、`selftest_public_host.py`（本轮 bat 内 OK）、`selftest_public_drag.py`（bat 内 OK）、`selftest_fence_keys.py` / `selftest_fence_bg_menu.py` / `selftest_portal.py`（bat 内 OK）、`selftest_desktop_pet.py`（契约 verify rc=0）。

---

## 6. t5 报告 21 条「部分 12 / 未覆盖 8 / 环境前置 1」修复后复核（逐条三态）

复核对象为 `docs/ux-report-4.0.28.md` §「部分 12 / 未覆盖 8 / 环境前置 1」共 21 条。三态口径：**修复后通过** / **仍缺环境前置** / **需人工**；本轮未能取证的条目单列 **未复跑**，不写成「部分通过」。

| # | 清单项 | t5 状态 | 本轮复核结论 | 证据 / 事实 |
| --- | --- | --- | --- | --- |
| 1 | UX-F-01 资源管理器→分区拖入 | 部分 | **修复后通过**（主判据）；视觉高亮与取消幽灵残留=**需人工** | `3/5 public drag` `[ OK ]`；`ux_selftest_drop_matrix.log` `通过 6 失败 0` |
| 2 | UX-F-02 落点判定 | 部分 | **修复后通过**（主判据）；落区高亮=**需人工** | 同上 |
| 3 | UX-F-03 释放后图标出现时机 | 部分 | **修复后通过**（主判据）；只读/无权限 toast=**需人工** | `3/5 public drag` OK |
| 4 | UX-F-04 区内重排 | 部分 | **修复后通过**（主判据）；插入指示线=**需人工**（G3 记 N/A） | `3/5 public drag` OK（含 same-fence reorder 无闪烁断言） |
| 5 | UX-F-05 自动换行 | 部分 | **未通过**（新增缺陷 F-t12-01，见 §4.2） | Q3 三断言通过，但窄分区单列格裁切 56–96px、横向滚动条被关 |
| 6 | UX-F-09 Shift 连选区间 | 部分 | **未复跑**（时间预算用尽；`selftest_public_multiselect.py` 本轮 -1） | t5 证据仅覆盖区间选择 |
| 7 | UX-F-16 键盘快捷键 | 部分 | **修复后通过**（主判据）；焦点框可见性 / Ctrl+A 批量删除=**需人工** | `3e2/5 fence keys and clipboard` `[ OK ]` |
| 8 | UX-F-17 OLE 拖出到外部应用 | 部分 | **修复后通过**（主判据）；第三方真实应用=**需人工** | `3/5 public drag` OK |
| 9 | UX-F-19 文件搜索 | 部分 | **未复跑**（时间预算用尽，`selftest_file_search.py` 未执行）；耗时量化与空态文案=**需人工** | t5 rc=0 |
| 10 | UX-P-04 宠物单击 | 部分 | **修复后通过**（主判据）；单击不误触发一键整理=**需人工** | `verify_selftest_desktop_pet.log` rc=0 |
| 11 | UX-P-06 气泡与分页栏托管 | 部分 | **修复后通过**：三配置真实点击均生效 | §3 G11（`p0_results.json`） |
| 12 | UX-P-07 动画状态机 | 部分 | **部分**：姿态映射断言通过；帧率/切姿态耗时在无字体、offscreen 环境不可判定 → **需人工** | `verify_selftest_desktop_pet.log` rc=0 |
| 13 | UX-F-06 多选批量操作 | 未覆盖 | **需人工** | 本轮公开主机脚本未提供批量断言 |
| 14 | UX-F-11 删除到回收站 | 未覆盖 | **修复后通过**（确认框路径）+ 还原=**需人工** | §3 G8（`probe_confirm_delete.log`） |
| 15 | UX-F-12 双击打开 | 未覆盖 | **需人工**；代码事实：仅打开被双击项（`src/ui/fence_icon_item.py:7822-7826`、`src/ui/fence_widget.py:2054`） | 本轮未取到真实交互证据 |
| 16 | UX-F-14 右键菜单三态 | 未覆盖 | **需人工**；代码事实：Shell 文件菜单承载，无「删除 N 个项目」数量文案 | `3e/5 fence blank shell menu` `[ OK ]`（仅架构路径） |
| 17 | UX-F-20 误操作不可撤销点 | 未覆盖 | **未复跑**（时间预算用尽）；撤销栈本轮不实现为已知口径 | t5 结论 |
| 18 | UX-P-02 宠物双击 | 未覆盖 | **需人工** | 需真实点击 |
| 19 | UX-P-03 宠物拖动贴边 | 未覆盖 | **需人工** | 需真实拖动 |
| 20 | UX-P-05 宠物设置竞态 | 未覆盖 | **需人工** | 需并发手速场景 |
| 21 | UX-F-15 空白桌面右键 | 环境前置 | **仍缺环境前置** | `ux_selftest_live_empty_rmb.log`：`FAIL  no DeskTidy public host HWND (is DeskTidy running?)`；分项 `ux_selftest_folder_rmb_ghost.log` `OK contracts` |

补充：t5 判为「判据未满足」的 2 条（UX-F-10 批量部分失败静默、UX-F-18 删除无确认）本轮 **修复后通过**，见 §3 G4 / G8。
本表计数（21 条）：修复后通过 **9**、未通过 **1**（F-t12-01 → UX-F-05）、部分 **1**（P-07）、仍缺环境前置 **1**（F-15）、需人工 **6**（F-06/F-12/F-14/P-02/P-03/P-05）、未复跑 **3**（F-09/F-19/F-20）。

---

## 7. 环境前置阻塞项（沿用已知结论，不计入缺陷）

| 阻塞 | 影响命令 | 归因 |
| --- | --- | --- |
| 无 `dist\DeskTidy.exe` 打包产物 | `selftest_system.py`（76/78）、`selftest_live_e2e.py`、`selftest_virtual.py` | 构建前置（BUG-01），非代码回归 |
| 无 ffmpeg | `selftest_recent.py`（83/84） | 环境前置（BUG-03） |
| 单屏 1536x834（无多屏夹具） | 多屏断言 | 夹具缺失 |
| UAC 提权对话框 | 提权保存用例 | 需人工（自动化不可绕过） |
| 无运行中的活体 DeskTidy 会话 | `selftest_live_empty_rmb.py` | 环境前置 |

---

## 8. 门禁判定与交付

- **P0 四项（G11/G2/G4/G8）交互实测：全部通过**（§3，18/18）。
- **Q3 三条必做断言：通过**（§4.1）。
- **发现真实缺陷 1 条（F-t12-01, medium）**：§4.2，代码级根因已隔离并给出对照实验证据。
- 按 t12 契约门禁口径「发现真实缺陷 → verdict=needs_revision + 结构化 findings」，本任务判定 **needs_revision**（任务状态 failed），并将 F-t12-01 作为唯一 blocker-free 的修复项移交修复方。
- 环境前置项（§7）未计入 findings。
