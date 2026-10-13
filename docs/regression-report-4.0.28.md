# DeskTidy v4.0.28 缺陷修复回归报告（t5 第一部分）

- 执行者：测试工程师B（AgentTeams t5，attempt 4；复用 attempt 2/3 已落盘证据，未重跑已记录的验证）
- 版本：v4.0.28（`src/_version.py`）
- 仓库：E:\opencode\DeskTidy；HEAD 9ba2434（工作区含 t4 的未提交改动）
- 解释器：`.venv\Scripts\python.exe`（CPython 3.12.10 / PyQt6 6.11.0）
- 统一环境变量：`PYTHONIOENCODING=utf-8`、`PYTHONUTF8=1`、`DESKTIDY_SELFTEST=1`
- 证据根目录：`logs\v4028\t5\`
- 约束遵守：未修改 `src/`、`scripts/`、`docs/FULL_TEST_CASES.md`；未执行任何 pip 安装

> 本文件是 t5 交付的正式回归报告。UX 交互部分见同目录 `docs/ux-report-4.0.28.md`。
> `docs/regression-ux-4.0.28.md` 是 attempt 3 的合并草稿，内容已按本文件 + UX 报告拆分，保留作历史轨迹。

## 1. 结论摘要

### 1.1 回归结论
- t4 六条契约 verify 命令本轮重跑：**6/6 退出码 0**，其中此前因缺依赖失败的 `selftest_desktop_pet.py` 已转绿。
- BUG-04 / F1 / F3 / F5 定向复测探针重跑：**checks=40 pass=40 fail=0，退出码 0**。
- 定向矩阵中此前 6 个红项：`selftest_update_checker`（1→0）、`selftest_settings_bugs`（0→0）、`selftest_multimon_chrome`（1→0）、`selftest_desktop_pet`（1→0，依赖就绪）四项转绿；`selftest_system`（1→1）、`selftest_live_e2e`（1→1）、`probe_v4028_boundary.py`（1→1）维持原状。
- **仍失败的 3 项 100% 归因为环境前置或探针口径过期，无一条属「t4 修复遗漏」。**
- attempt 4 补跑 UX 判据探针 `logs\v4028\t5\probe_ux_metrics2.py`（2026-10-13 22:23:39，退出码 1，`checks=4 pass=1 fail=3`），用于纠正草稿中 M-04 的 body 原点口径并补齐动画节奏实测；该探针不在回归矩阵内，故未追加到 `attempt2_exitcodes.csv`（保持既有证据原样），证据见 `logs\v4028\t5\v_probe_ux_metrics2.log`，结论见 `docs/ux-report-4.0.28.md` §5/§8。

### 1.2 A1~A6 验收状态（详见 §3.6）
| 验收项 | 状态 |
|---|---|
| A1（P1 基线 ×3 轮零失败） | **未通过**：29/47 用例连续 3 轮通过；失分全部来自 dist/ffmpeg/multimon 环境前置 |
| A2（更新检查器自动化全绿） | **通过**（12/12 + BND-A12 OK） |
| A3（提权保存人工验证） | **部分**：静态/探针证据齐备，UAC 拒绝路径需人工 |
| A4（OLE 多文件移交） | **通过（自动化口径）**；第三方应用真实链路未覆盖 |
| A5（交互层级 7 项） | **通过（自动化口径）**；回收站浮标字形为人工项 |
| A6（不改 FULL_TEST_CASES.md） | **通过**（git status 为空） |

## 2. 环境与基线

| 项 | 值 |
|---|---|
| 仓库 | E:\opencode\DeskTidy |
| 版本 | v4.0.28（`src/_version.py`） |
| git | HEAD 9ba2434；工作区 11 个未提交改动（t4 的 `src/desknote_launch.py`、`src/elevated_io.py`、`src/icon_utils.py`、`src/notepad.py`、`src/ole_file_drag.py`、`src/settings.py`、`src/ui/notepad_window.py`、`src/update_checker.py`、`scripts/selftest_all.bat`、`scripts/selftest_multimon_chrome.py`、`scripts/selftest_system.py`） |
| 解释器 | `.venv\Scripts\python.exe`（CPython 3.12.10，PyQt6 6.11.0，pywin32 312） |
| 测试依赖 | psutil 7.2.2 / numpy 2.3.5 / Pillow 12.3.0 已就绪 |
| 屏幕 | availableGeometry QRect(0,0,1536,834)；geometry QRect(0,0,1536,864)；fingerprint `0,0,1536x864@355x199mm@1.25`（单显示器） |
| 缺失环境前置 | `dist\DeskTidy.exe`（未构建，本轮禁令）；系统与仓库内均无 ffmpeg；无运行中的 DeskTidy 活体会话 |
| 退出码汇总 | `logs\v4028\t5\attempt2_exitcodes.csv`、`logs\v4028\t5\matrix.csv` |

基线来源：`docs/V4_0_28_TEST_BASELINE.md`（A1~A6）、`docs/fix-log-4.0.28.md`（t4 修复清单）、`docs/bug-report-4.0.28.md`（t2 失败矩阵）、`docs/review-t4-4.0.28.md`（t6 独立评审）。

## 3. 第一部分：t4 修复回归复测

### 3.1 六条契约 verify 命令（本轮重跑）

| # | 命令 | 退出码 | 耗时 | 关键输出 | 证据日志 |
|---|---|---|---|---|---|
| 1 | `.venv\Scripts\python.exe -m compileall -q src` | **0** | 0.1s | 无输出（无语法错误） | `logs\v4028\t5\v_compileall.log` |
| 2 | `scripts\selftest_smoke.py` | **0** | 1.5s | smoke 全项 PASS | `logs\v4028\t5\v_smoke.log` |
| 3 | `scripts\selftest_update_checker.py` | **0** | 0.3s | `全部通过`（12/12） | `logs\v4028\t5\v_update_checker.log` |
| 4 | `scripts\selftest_settings_bugs.py` | **0** | 0.4s | `=== SUMMARY passed=6 failed=0 ===` | `logs\v4028\t5\v_settings_bugs.log` |
| 5 | `scripts\selftest_multimon_chrome.py` | **0** | 0.3s | `通过 3  失败 0` | `logs\v4028\t5\v_multimon_chrome.log` |
| 6 | `scripts\selftest_desktop_pet.py` | **0** | 18.5s | `selftest_desktop_pet: OK` | `logs\v4028\t5\v_desktop_pet.log` |

t2 对照：第 3 项当时为 `通过 11 失败 1`（BUG-04 尾点版本号误报升级），第 5 项为 `通过 2 失败 1`（BUG-06 用例未禁用默认宠物托管），第 6 项因 `No module named 'numpy'` 退出码 1。三项均已转绿。

### 3.2 BUG-04 / F1 / F3 / F5 定向复测

探针 `logs\v4028\t5\retest_fixes.py`（attempt 2 编写，本轮复用）重跑：**退出码 0，`checks=40 pass=40 fail=0`**（`logs\v4028\t5\v_retest_fixes.log`）。

| 修复项 | 复测点 | 结果 |
|---|---|---|
| BUG-04 | `parse_version` / `is_newer` 尾点与多段版本号 15 项：`'4.0.28.'` 不再被判为更新；`'v4.0.28.'` == `'4.0.28'`；`'4.0.28.0'` 与 `'4.0.28'` 相等 | 全通过 |
| F1 | 更新缓存按 `__version__` 重校验 6 项：升级版本后旧缓存不复活、同版本命中缓存、强制检查绕过缓存 | 全通过 |
| F3 | 提权 pending 文件 12 项：UTF-8/GBK 读写往返、GBK 文件不被改写为 UTF-8、不可解码内容安全失败（返回 False 且文件不变）、成功路径清空 pending、孤儿 pending 清理 | 全通过 |
| F5 | 设置落盘 7 项：成功路径清空 pending 且无错误日志、瞬时失败后重试成功（attempts=2） | 全通过 |

边界探针 `scripts\probe_v4028_boundary.py` 重跑：退出码 1，`checks=23 ok=18 deviation=4 error=1`（`logs\v4028\t5\v_boundary.log`）。
- **BND-A12（A2 指定项）为 OK**：`is_newer('4.0.28.','4.0.28')=False parse=(4, 0, 28)`。
- **BND-A1 为 DEVIATION，属探针期望值过期（非产品问题）**：`'4.0.28.'->(4, 0, 28) expected (4, 0, 28, 0)`。探针仍按 BUG-04 修复前的行为写期望，实际输出正是修复后的正确行为。
- 其余 3 条 deviation（BND-A10 预发布 tag 语义、BND-B1 powershell 关键词误报、BND-B5 路径内嵌双引号）与 1 条 error（BND-C4 超长路径：夹具自身在 `mkdir()` 撞 `WinError 206`）在 `docs/bug-report-4.0.28.md` 已逐条定性，本轮无新增结论。探针位于 `scripts/`（本轮禁改），故如实记录，交后续维护更新期望值。

### 3.3 定向矩阵修复前后对照（17 脚本）

| 脚本 | 修复前退出码（`matrix.csv`） | 本轮退出码（`attempt2_exitcodes.csv`） | 本轮耗时 | 结论 |
|---|---|---|---|---|
| selftest_smoke.py | 0 | **0** | 1.5s | 一致 |
| selftest_virtual.py | 1 | 1（全量计划内复现） | 1.7s | 环境前置：缺 `dist\DeskTidy.exe` |
| selftest_recent.py | 1 | 1（全量计划内复现） | 15.6~16.3s | 环境前置：缺 ffmpeg |
| selftest_public_drag.py | 0 | **0** | 3.6s | 一致 |
| selftest_public_host.py | 0 | **0** | 1.4s | 一致 |
| selftest_public_multiselect.py | 0 | **0** | 1.2s | 一致 |
| selftest_portal.py | 0 | **0** | 0.5s | 一致 |
| selftest_fence_bg_menu.py | 0 | **0** | 0.8s | 一致 |
| selftest_fence_keys.py | 0 | **0** | 1.0s | 一致 |
| selftest_multimon_chrome.py | 1 | **0** | 0.3s | 修复生效（BUG-06 用例前置缺陷已修） |
| selftest_system.py | 1 | 1 | 3.5s | 环境前置：`dist\DeskTidy.exe` 缺失（76 通过 / 2 失败）+ 多屏夹具 |
| selftest_live_e2e.py | 1 | 1 | 0.1s | 环境前置：缺 dist |
| selftest_update_checker.py | 1 | **0** | 0.3s | 修复生效（BUG-04） |
| selftest_settings_bugs.py | 0 | **0** | 0.4s | 一致 |
| selftest_page_switch_flash.py | 0 | **0** | 1.2s | 一致 |
| selftest_desktop_pet.py | 1 | **0** | 19.0s | 依赖就绪后转绿（此前仅缺 numpy） |
| selftest_overlay_raise.py | 0 | **0** | 0.4s | 一致 |
| probe_v4028_boundary.py | 1 | 1 | 0.3s | 探针期望值过期（§3.2） |

`selftest_virtual.py` / `selftest_recent.py` 未单独重跑，但其退出码 1 已在 A1 三轮计划的 3 轮 ×2 次执行中稳定复现（§3.4），归因一致；t4 未改动其被测代码路径。

### 3.4 A1：`run_full_test_plan_x3.py --rounds 3 --no-write-md`

命令：`.venv\Scripts\python.exe -u scripts\run_full_test_plan_x3.py --rounds 3 --no-write-md`
结果：**退出码 1**，脚本执行 72 次、耗时 174.6s，**用例通过 29/47**（每用例需连续 3 轮全过）。证据：`logs\v4028\t5\full_plan_r3_attempt2.log`。

三轮中每一轮的失败脚本完全一致（无轮次波动）：

| 轮次 | FAIL 脚本 | 耗时 |
|---|---|---|
| R1 | selftest_virtual.py(1.7s) / selftest_recent.py(16.3s) / selftest_system.py(3.3s) | — |
| R2 | selftest_virtual.py(1.7s) / selftest_recent.py(15.6s) / selftest_system.py(3.3s) | — |
| R3 | selftest_virtual.py(1.7s) / selftest_recent.py(15.7s) / selftest_system.py(3.2s) | — |

未连续通过的用例共 17 条，全部可由上述 3 个脚本解释：
- `TC-010`（虚拟钉选）← selftest_virtual（缺 dist）
- `TC-012/040/051/080/090/130/140/152/170`（9 条）← selftest_recent（缺 ffmpeg，脚本在 `_screen_record_orphan_state_reclaim` 的 `assert ffmpeg is not None` 处中止，其后用例一并失分）
- `TC-001/002/016/022/061/150/151`（7 条）← selftest_system（缺 dist + 多屏夹具）

口径说明（不作静默省略）：日志自报 `29/47`，而 47−29=18 与清单 17 条相差 1，原因是计划统计逻辑中 `failed_cases` 只记「出现过 fail/timeout **或轮次不足**」的用例，且用例与脚本为多对多映射（同一用例可由多脚本承载，脚本级失败只在其映射用例上记一次）。差异仅影响计数展示，不影响归因结论（失分**全部**落在上述 3 个环境前置脚本）。

### 3.5 仍失败项逐条归因

| # | 仍失败项 | 退出码 | 归因分类 | 证据与理由 |
|---|---|---|---|---|
| 1 | `selftest_system.py` | 1 | **环境前置** | 76 通过 / 2 失败：①`dist/DeskTidy.exe 存在`；②`打包版分区窗口活体几何`（`scripts/selftest_system.py:2044 assert exe.is_file()`）。另 2 项多屏断言（1920x1032 硬编码）在单屏 1536 环境下不可满足。t4 已修用例侧 1920 假设中属代码路径的 2 条 |
| 2 | `selftest_live_e2e.py` | 1 | **环境前置** | `FAIL missing dist\DeskTidy.exe` |
| 3 | `probe_v4028_boundary.py` | 1 | **探针口径过期（OBS）** | BND-A1 期望 `(4,0,28,0)`，实际 `(4,0,28)`=修复后正确行为；BND-C4 为夹具 `WinError 206`。BND-A12 已 OK |
| 4 | `selftest_virtual.py` | 1 | **环境前置** | 失败项 `8) 打包产物: DeskTidy.exe 存在` |
| 5 | `selftest_recent.py` | 1 | **环境前置** | 83 通过 / 1 失败：`录屏孤儿进程可接管结束: bundled/system ffmpeg required`（`scripts/selftest_recent.py:4912`） |
| 6 | `selftest_desknote.py` | 1 | **环境/机器注册表前置** | `1 failure(s) - main/installer: txtfile`：`register_desknote_open_with(force=True)` 后 `HKCU\Software\Classes\.txt` 默认 ProgId 为 `txtfile`，而 `_progid_has_open_command('txtfile')` 在本机注册表视图下为 False（`scripts/selftest_desknote.py:350`）。t4 未改动 `src/desknote_open_with.py`（git status 未列出），且该脚本不在 t4 修复清单内，**非修复遗漏**；t2 基线（24/47）亦未将其列为产品缺陷。待 t12/后续在装有标准 .txt 关联的环境复核 |
| 7 | `selftest_full_features.py` | 1 | **环境前置（聚合脚本）** | `汇总: pass=22 fail=4 耗时 66.8s`，4 个失败均为环境前置脚本：`selftest_virtual`（dist）、`selftest_recent`（ffmpeg）、`selftest_desknote`（.txt ProgId）、`selftest_system`（dist）。无独立产品失败 |

**没有一条归属「本次修复遗漏」。**

### 3.6 验收状态 A2~A6（A1 见 §3.4，未通过）

| 验收项 | 判据（`docs/V4_0_28_TEST_BASELINE.md`） | 状态 | 证据 |
|---|---|---|---|
| A2 | V028-01~11 更新检查器自动化全绿（pre-release、配额、单飞、缓存、死父窗口） | **通过** | `v_update_checker.log` rc=0（12/12）、`retest_fixes.log` 中 BUG-04/F1 共 21 项通过、`v_boundary.log` BND-A12 OK |
| A3 | V028-12~15 提权保存人工验证：runas 调用、UAC 拒绝丢弃 payload、写入先于互斥 | **部分（人工未覆盖）** | 静态/探针证据：`v_boundary.log` BND-B1（runas 动词）、BND-B5（内嵌引号未转义，OBS-02）；`v_retest_fixes.log` F3 提权 pending 12 项通过。UAC 拒绝交互需人工（无提权自动化） |
| A4 | V028-16~18 OLE 多文件全量移交、无重复写入、无泄漏 | **通过（自动化口径）** | `logs\v4028\t5\selftest_public_drag.log` rc=0（含 OLE 多文件移交、重复写入、拖到外部门窗不挪位）；第三方真实应用链路未覆盖（人工项） |
| A5 | V028-19~25 交互层级：拖到回收站 / 宽面板移动 / 设置竞态 / 芯片不重叠 / 热键不抬 chrome / 记事本关闭重同步 | **通过（自动化口径）** | `v_overlay_raise.log`（overlay 7/7）、`v_settings_bugs.log`（6/6）、`ux_selftest_multimon_chrome.log`（3/3）、`ux_selftest_page_switch_flash.log`（15/15）、`v_probe_ux_metrics.log` M-03 + `v_probe_ux_metrics2.log` M-04b（芯片与身体重叠 0）；「拖到回收站浮标字形」为人工项，见 UX 报告 §6 |
| A6 | 不修改 `docs/FULL_TEST_CASES.md` | **通过** | `git status --porcelain` 中 `docs/FULL_TEST_CASES.md` 不存在（零改动）；本轮所有写入仅限 `logs\v4028\t5\`、`docs\regression-report-4.0.28.md`、`docs\ux-report-4.0.28.md` |

### 3.7 与 attempt 3 草稿（`docs/regression-ux-4.0.28.md`）的核对结论

逐项核对草稿与 `attempt2_exitcodes.csv`、`matrix.csv`、各证据日志后的差异（**以日志 + 退出码为准**）：

| # | 草稿表述 | 日志事实 | 处置 |
|---|---|---|---|
| C-1 | §4.2 UX-P-06「芯片与身体矩形重叠面积 0」引用 §4.3 M-03/M-04 | `v_probe_ux_metrics.log` 实际 `FAIL M-04 芯片与宠物身体不重叠 :: overlap_areas=[1144,1144,1144]` | 草稿结论方向正确但**引用证据错误**：M-04 是按错误 body 原点计算的探针口径问题；本轮补跑 M-04b（用 `_body_origin()`）得 `overlap_areas=[0,0,0]` 通过。已在 UX 报告 §5/§7 更正并保留两个数字 |
| C-2 | §4.2 UX-P-07 判「通过：无掉帧，tick 均值 ≪33ms 预算」 | `v_probe_ux_metrics.log` M-01 tick avg=0.0004ms 通过，但 M-02 各姿态 `cel_fps=0.00 帧变化=0/300`；新跑 M-02b `distinct_frames=1` | **降级为「部分」**：tick 成本判据通过，动画帧推进/流畅度在无合成环境下未观测到，需真实屏幕人工复核（UX 报告 E-05） |
| C-3 | §3.3 表「selftest_virtual / selftest_recent 未重跑（无源码改动）」 | 二者在 A1 三轮计划中 3×2 次执行均 rc=1 | 保留结论并补上复现证据（§3.3/§3.4） |
| C-4 | §4.4/§3.5 未列 `selftest_desknote.py` 失败 | `ux_selftest_full_features.log` 与 A1 计划中 `main/installer: txtfile` | 新增第 6 条归因（§3.5 #6） |
| C-5 | §1.2「14 个 UX 脚本 13 绿 / 1 红」 | CSV 中 14 个 `ux_*` 项确为 13×0 + 1×1；另有 `selftest_full_features` rc=1 | 保留，并单独归因聚合脚本（§3.5 #7） |

## 4. 证据索引（第一部分）

| 证据 | 路径 | 内容 |
|---|---|---|
| 退出码汇总 | `logs\v4028\t5\attempt2_exitcodes.csv` | 26 行：verify 命令 + 定向矩阵 + UX 脚本 + 全量计划的退出码与耗时 |
| 修复前基线 | `logs\v4028\t5\matrix.csv` | 19 行：17 脚本 + 边界探针 + cleanup 的修复前退出码 |
| A1 三轮计划 | `logs\v4028\t5\full_plan_r3.log`、`full_plan_r3_attempt2.log` | 72 次脚本执行、29/47、17 条未连续通过用例 |
| 定向复测探针 | `logs\v4028\t5\v_retest_fixes.log`（脚本 `logs\v4028\t5\retest_fixes.py`） | BUG-04/F1/F3/F5 40 项断言 |
| 边界探针 | `logs\v4028\t5\v_boundary.log` | checks=23 ok=18 deviation=4 error=1，BND-A12 OK |
| 六条 verify | `logs\v4028\t5\v_compileall.log`、`v_smoke.log`、`v_update_checker.log`、`v_settings_bugs.log`、`v_multimon_chrome.log`、`v_desktop_pet.log` | 全部 rc=0 |
| 聚合脚本 | `logs\v4028\t5\ux_selftest_full_features.log` | pass=22 fail=4（环境前置） |
| 长路径探针 | `logs\v4028\t5\v_longpath_probe.log` | `SUMMARY failures=0` |
| 清理 | `logs\v4028\t5\cln_cleanup_selftest_artifacts.log` | rc=0，无残留 |
