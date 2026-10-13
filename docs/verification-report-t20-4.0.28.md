# t21 第 4 轮定向回归验证报告（partial · 用户要求停止测试后固化）

- 任务：t21 = 第 4 轮定向回归验证（kind=verification，round 4），attempt_id `c2f3d41f-7812-438d-814e-c8d6039c70f5`
- 验证对象：当前工作区字节（t10 P0 修复 + t14 绑定修复 + t16 F-t12-01 修复 + t19 重写的闸门）
- 归档口径：**本报告只记录已实际执行并留有日志的事实**。凡未执行项一律标「未执行（用户停止测试）」，不凭印象补结论。
- 证据根目录：`logs\v4028\t21\`

---

## 1. 结论摘要

| 项 | 结果 |
| --- | --- |
| 基线字节核对 | **通过**：`src/ui/fence_widget.py` 168331b / SHA256 `3CFEFFE843DC2AEAE2E196B45841A7326F877CF1DD19CD5BA331F150FBB2E463`；`scripts/selftest_fence_narrow_layout.py` 10587b / SHA256 `2255ECF84F17A9AA469140F29EFE288168F71D7A997EA8BD41931700267C2A24`（与派单冻结值逐字节一致） |
| 5 条 verify 命令 | **5/5 退出码 0**（§2） |
| 闸门双态证据 | **两态齐备且有控制组**：修复版 rc=0（PASS，逐级 cell_w/clipped）；注入回退版 rc=1（FAIL，width=200→cell_w 210/clipped 56、width=160→cell_w 210/clipped 96）（§3） |
| F-t12-01 修复确认 | **确认修复**：≤230px 各级 cell_w 154（200px）/114（160px），clipped=0；对照修复前 210 / 56~96（§4） |
| 残余观察 | 3 条低危 observation，均不构成本轮缺陷、不改变判定（§5） |
| 环境前置 | 缺 `dist\DeskTidy.exe`、缺 ffmpeg、UAC 人工、多屏夹具口径 —— 单独归档，不计缺陷（§7） |
| 未执行项 | `selftest_all.bat`、t5 UX 报告三态复测等（§8），**未执行（用户停止测试）** |
| 门禁判定 | **verdict=pass**（F-t12-01 修复有效 + 闸门双态可复现 + 基线零改动；P0 类实测全通过） |

---

## 2. 契约 verify 命令复跑（本 attempt 实测）

| # | 命令 | 退出码 | 关键输出 | 日志 |
| --- | --- | --- | --- | --- |
| 1 | `.venv\Scripts\python.exe -m compileall -q src` | **0** | 无输出（0 字节日志） | `logs\v4028\t21\verify_compileall.log` |
| 2 | `.venv\Scripts\python.exe scripts\selftest_smoke.py` | **0** | `=== SUMMARY === passed=42 failed=0` | `logs\v4028\t21\verify_smoke.log` |
| 3 | `.venv\Scripts\python.exe scripts\selftest_fence_narrow_layout.py` | **0** | `selftest_fence_narrow_layout: PASS`（逐级表见 §3.1） | `logs\v4028\t21\verify_narrow_a_rc0.log` |
| 4 | `.venv\Scripts\python.exe scripts\selftest_multimon_chrome.py` | **0** | 4 条 OK，含 `OK  page buttons built with pet on and off (G11)` | `logs\v4028\t21\verify_multimon.log` |
| 5 | `.venv\Scripts\python.exe logs\v4028\t12\probe_t12_p0.py` | **0** | `probe_t12_p0: total=18 fail=0`（G11 4 项 / G2 7 项 / G4 3 项 / G8 4 项） | `logs\v4028\t21\verify_probe_p0.log` |

执行环境：`.venv\Scripts\python.exe`（CPython 3.12.10 + PyQt6 6.11.0，无 PySide6），`QT_QPA_PLATFORM=offscreen`、`DESKTIDY_SELFTEST=1`、`PYTHONUTF8=1`、`PYTHONIOENCODING=utf-8`。

---

## 3. 闸门双态对照（本 attempt 自产，未引用 t19/t20 自报结果）

### 3.1 修复版（当前冻结字节）— rc=0

命令：`.venv\Scripts\python.exe scripts\selftest_fence_narrow_layout.py` → 退出码 **0**
日志：`logs\v4028\t21\verify_narrow_a_rc0.log`

```
 width   vp_w             cell_w  rows  clipped  verdict
   640       12              [118]     4       12  OK
   512        0              [113]     5        0  OK
   416        0              [120]     6        0  OK
   320        0              [135]     9        0  OK
   256        0              [210]    18        0  OK
   200        0              [154]    18        0  OK
   160        0              [114]    18        0  OK

selftest_fence_narrow_layout: PASS
```

（第 2 列表头写作 `vp_w`，实际打印的是 `clipped_px` —— 见 §5 O-02，属列头文案问题，不影响读数。）

### 3.2 注入回退版（我自己从冻结字节派生）— rc=1

- 派生方式：读取 `src\ui\fence_widget.py` **原始字节**（168331b），仅删除 `label_width` 比较那一行死代码/守卫（该行即 F-t12-01 修复所在），写入 `logs\v4028\t21\neg\fence_widget_neg.py`（168293b，含标记常量）；派生脚本 `logs\v4028\t21\make_neg_copy.py`。
- 注入方式：`logs\v4028\t21\run_neg_mine.py` 以 `importlib.util.spec_from_file_location("src.ui.fence_widget", <neg copy>)` 预加载派生模块（stderr 打印 `[NEG-INJECT-runner] mark=True` 证实加载的是我的副本），再 `exec_module` 执行同一闸门脚本并返回其退出码。
- 命令：`.venv\Scripts\python.exe logs\v4028\t21\run_neg_mine.py`（闸门本体即 `scripts\selftest_fence_narrow_layout.py`，**未修改该文件**）→ 退出码 **1**
- 日志：`logs\v4028\t21\neg_gate_runner.log`

```
 width   vp_w             cell_w  rows  clipped  verdict
   640       12              [118]     4       12  OK
   512        0              [113]     5        0  OK
   416        0              [120]     6        0  OK
   320        0              [135]     9        0  OK
   256        0              [210]    18        0  OK
   200       56              [210]    18       56  FAIL
   160       96              [210]    18       96  FAIL

selftest_fence_narrow_layout: FAIL
 - width=200: cell_w 210 > viewport, clipped=56
 - width=160: cell_w 210 > viewport, clipped=96
```

**符合验收要求**：同一命令下 rc=1，且打印「卡住的 cell_w 210」与「越界像素 56、96」。

### 3.3 控制组（证明注入机制本身无副作用）

- `logs\v4028\t21\pos\fence_widget_pos.py` = 同一文件**未修改副本**（`logs\v4028\t21\make_neg_copy.py` 同族方式生成），经同一预加载机制装配。
- 命令：`.venv\Scripts\python.exe scripts\selftest_fence_narrow_layout.py`（`PYTHONPATH` 指向 pos 目录）→ 退出码 **0**，`selftest_fence_narrow_layout: PASS`
- 日志：`logs\v4028\t21\ctl_pos_gate.log`
- 结论：同一注入机制下「未删减副本通过、删减副本失败」= 失败信号来自被删的那一行，而非装配方式。

### 3.4 启动期预加载变体（observation，不计入判定）

以 `PYTHONPATH` + `sitecustomize.py` 在解释器启动时预加载派生模块（`logs\v4028\t21\neg\sitecustomize.py`）时：派生版闸门以 **`-1073741819`（0xC0000005 access violation）** 崩溃，`faulthandler` 仅给出 `scripts\selftest_fence_narrow_layout.py`, line 271 帧，无表格输出；同机制的**未修改副本控制组仍 rc=0**（`ctl_pos_gate.log`）。见 §5 O-03：属“启动期预加载 + 越界布局”组合下的 Qt 崩溃，与文档记载的运行期预加载recipe不同，未取得可读失败信息，故不作为证据、也不作为缺陷。

---

## 4. F-t12-01 修复确认（独立复现）

对照基线（修复前，t12 report §4.2）：分区缩窄到 ≤230px 时 `cell_w` 卡在 **210**，越界 **56~96px**。

### 4.1 闸门（§3.1）读数

| 宽度 | 修复后 cell_w | 修复后 clipped | 修复前 cell_w / clipped |
| --- | --- | --- | --- |
| 256 | 210 | 0 | 210 / 0（该档本就容得下） |
| **200** | **154** | **0** | 210 / 56 |
| **160** | **114** | **0** | 210 / 96 |

### 4.2 t12 原始探针（`logs\v4028\t12\probe_t12_q3.py`）在冻结字节上的重跑

命令：`.venv\Scripts\python.exe logs\v4028\t12\probe_t12_q3.py`（只读运行，未改该探针）→ 退出码 **1**（见下方说明，唯一来源是 width=640 首帧）
日志：`logs\v4028\t21\q3_frozen_repro.log`

`[persisted]` 逐级（用户拖动分区边缘的真实路径）：

```
width= 640 … label_w=105 cell_w=118 vp_w=604 clipped=12 hscroll=12 | 见 §5 O-01
width= 512 … label_w=103 cell_w=113 vp_w=476 clipped=0
width= 416 … label_w=110 cell_w=120 vp_w=380 clipped=0
width= 320 … label_w=125 cell_w=135 vp_w=284 clipped=0
width= 256 … label_w=200 cell_w=210 vp_w=220 clipped=0
width= 200 … label_w=144 cell_w=154 vp_w=164 clipped=0
width= 160 … label_w=104 cell_w=114 vp_w=124 clipped=0
cols=[5,4,3,2,1,1,1] rows=[4,5,6,9,18,18,18] cell_w=[118,113,120,135,210,154,114] clipped=[12,0,0,0,0,0,0] maxReflow=103.3ms verdict=False
```

- **≤230px 口径（= F-t12-01 缺陷口径）：不再卡 210，clipped=0，无重叠、无越出分区、行数单调不减（4→5→6→9→18→18→18）** → 修复确认。
- 该探针自身 `verdict=False` 的**唯一**来源是首帧 width=640 行的 `clipped=12`（其判定含 `host_fits_viewport_w_all`），与 F-t12-01 的“窄分区 cell_w 卡死”签名不同，已单列 §5 O-01，未据此判缺陷。
- 该探针是 t12 发现 F-t12-01 的**同一工具**，故其复跑即为独立复现（非引用他人结果）。

---

## 5. 残余观察（observation，低危，不计入缺陷计数）

| ID | 观察 | 事实 / 证据 | 判断 |
| --- | --- | --- | --- |
| O-01 | width=640（最宽档、首次测量）host 比视口宽 12px，出现 12px 横向滚动条 | `q3_frozen_repro.log` `[persisted] width=640 … clipped=12 hscroll=12`；闸门同样读数（§3.1 第 1 行 `clipped=12`） | 仅首帧、下一档即归零；闸门 verdict 设计上不含 `clipped_px`；与 F-t12-01 签名（窄档卡死）不同 → 判为测量瞬态/低危抛磨项，不阻塞。追加验证探针 `logs\v4028\t21\probe_t21_settle640.py` 崩于 `-1073741819` 未取得数据（§8） |
| O-02 | 闸门输出第 2 列表头写 `vp_w`，实际打印 `clipped_px`（`scripts\selftest_fence_narrow_layout.py:216` 表头 vs `:252-258` 行打印） | 两态表格均可复现 | 纯列头文案问题，`clipped` 在第 5 列与 FAIL 文案中均正确给出，不影响证据可读性与判定 |
| O-03 | 启动期预加载（sitecustomize）变体下，派生版闸门崩溃 `-1073741819`（access violation），控制组 rc=0 | `logs\v4028\t21\neg_gate_faulthandler.log`、`ctl_pos_gate.log` | 属装配顺序（解释器启动期预加载 + 越界布局）组合效应；文档 recipe（运行期预加载）两态均正常 → 归档为工具观察，不作缺陷 |

---

## 6. 基线零改动与只读核对

| 核对项 | 结果 | 证据 |
| --- | --- | --- |
| `src\ui\fence_widget.py` | 168331b，SHA256 `3CFEFFE843DC2AEAE2E196B45841A7326F877CF1DD19CD5BA331F150FBB2E463` | 与派单冻结值一致 |
| `scripts\selftest_fence_narrow_layout.py` | 10587b，SHA256 `2255ECF84F17A9AA469140F29EFE288168F71D7A997EA8BD41931700267C2A24` | 与派单冻结值一致 |
| `git status --porcelain -- src scripts docs/FULL_TEST_CASES.md` | 19 行：15 `M` + 4 `??` | `M`: `src/app.py`、`src/desknote_launch.py`、`src/elevated_io.py`、`src/icon_utils.py`、`src/notepad.py`、`src/ole_file_drag.py`、`src/settings.py`、`src/ui/fence_icon_item.py`、`src/ui/fence_widget.py`、`src/ui/notepad_window.py`、`src/ui/page_indicator.py`、`src/update_checker.py`、`scripts/selftest_all.bat`、`scripts/selftest_multimon_chrome.py`、`scripts/selftest_system.py`；`??`=`scripts/probe_ux_confirm_delete.py`、`scripts/probe_v4028_boundary.py`、`scripts/selftest_fence_narrow_layout.py`、`src/ui/dialogs.py` —— 均为 t4/t10/t14/t16/t19 各轮已记录改动，**无本轮新增意外改动** |
| `docs/FULL_TEST_CASES.md` | **零改动**（不在上述 19 行内） | 同上 |
| 只读约束 | 未修改 `src/`、`scripts/` 下任何文件；本轮新文件全部位于 `logs\v4028\t21\` 与 `docs\verification-report-t20-4.0.28.md` | 本报告 §3.2/§3.3 的派生副本与预加载器均在 `logs\v4028\t21\` 内 |
| 未跑 `selftest_all.bat` | 遵守队长口径（全量批次对本轮无信息量，且曾造成误判） | §8 |

---

## 7. 环境前置归档（不计缺陷、不据此判 needs_revision）

| 编号 | 环境前置 | 影响 | 本轮处置 |
| --- | --- | --- | --- |
| E-1 | 缺 `dist\DeskTidy.exe` | `selftest_system.py`、`selftest_live_e2e.py`、`selftest_virtual.py`、`live_gui_click_test.py` 不可跑 | 沿用已知结论，本轮未重跑 |
| E-2 | 缺 ffmpeg | `selftest_recent.py` 相关用例 | 沿用已知结论 |
| E-3 | UAC 提权需人工 | 提权保存路径人工项 | 沿用已知结论 |
| E-4 | 多屏夹具口径（本机单屏 1536x834） | 多屏相关断言 | 沿用已知结论 |
| E-5 | Qt 无字体目录（offscreen 下 `QFontDatabase: Cannot find font directory …`） | 与判定无关，仅噪音 | 记录 |

---

## 8. 未执行项清单（**未执行（用户停止测试）**）

1. `scripts\selftest_all.bat` —— **未执行（用户停止测试；且队长明令本轮不跑）**。
2. `docs\ux-report-4.0.28.md` 中「未覆盖 8 / 部分 12 / 环境前置 1」共 21 条的修复后三态复测 —— **未执行（用户停止测试）**；t12 轮已给出同口径三态结论（见 `docs\verification-report-t12-4.0.28.md` §6），本轮未刷新。
3. 追加首帧抛磨项验证探针 `logs\v4028\t21\probe_t21_settle640.py` —— 已运行但 **崩溃（退出码 -1073741819），无数据**，未计入任何结论（对应 O-01）。
4. 环境前置四类（§7）的全量重跑 —— 按队长口径 **不重跑**。
5. UX 清单条款级复跑（宠物交互、文件交互的人工/自动化条款）—— **未执行（用户停止测试）**。

---

## 9. 门禁判定

- 验收口径 1（闸门两态、自产证据）：**通过** —— 修复版 rc=0 + 派生回退版 rc=1 且打印 `cell_w 210 / clipped 56、96`，并有控制组证明机制无副作用（§3）。
- 验收口径 2（F-t12-01 修复独立复现）：**通过** —— ≤230px 各级 cell_w 不再卡 210、clipped=0；对照 210 / 56~96（§4）。
- 验收口径 3（5 条 verify 实跑记录退出码）：**通过** —— 5/5 rc=0（§2）。
- 验收口径 4（基线零改动）：**通过** —— 双哈希匹配、git status 无本轮新增意外改动（§6）。
- 验收口径 5（只读）：**通过**（§6）。
- 验收口径 6（环境前置归档、不计缺陷）：**通过**（§7）。
- 验收口径 7（交付报告）：**通过** —— 本文件。

**verdict = pass**（本报告为 partial：§8 所列项未执行，已如实标注，不影响上述 7 条口径的判定）。
