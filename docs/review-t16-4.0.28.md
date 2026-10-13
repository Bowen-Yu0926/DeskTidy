# t17 评审报告 — t16 F-t12-01 修复（round 3）

- 评审人：开发B（attempt 73ae82e3-c57c-45d2-94ca-b5bb2c58fd08）
- 日期：2026-10-13
- **verdict: needs_revision**（修复本体有效且无回归，但 t16 新增的回归闸门 `scripts/selftest_fence_narrow_layout.py` 被证明是恒真断言，不构成有效防复发闸门）

## 1. 修复本体有效性（PASS）

独立复现（probe_t12_q3.py，18 单元格、WIDTHS=[640,512,416,320,256,200,160]）：

| 状态 | cell_w 序列 | clipped 序列 |
|---|---|---|
| 修复后（工作区 t16 代码） | 118,113,120,135,210,154,114 | 12,0,0,0,0,0,0 |
| 否定测试（回退单行条件） | 118,113,120,135,210,**210,210** | 12,0,0,0,0,**56,96** |

- 修复后 cell_w 不再卡 210，clipped 全 0（首档 12px 为 fence 框架装饰，非裁切）。
- negated code 下 cell_w 卡 210、clipped 56/96，与 t12 §4.2 记录的缺陷形态一致。
- probe_t12_q3.py 能有效区分修复前/后（`cell_fits_viewport_all`：True vs False）。
- 边界用例（logs/v4028/t16/probe_t17_boundary.py 与 probe_t17_boundary_real.py，真实事件路径、不手动清缓存）：
  - A：640→160→640 回环，第 3 步 cell_w 118→115，证明 label_width 缓存被真实重算；cell_w 全程 ≤ vp_w。
  - B：800→160 直接跳变，cell_w=106 ≤ vp_w，无残留裁切。
  - boundary_all_cell_fits=True，rc=0。

## 2. 位点核对（PASS）

`src/ui/fence_widget.py` 全部四处缓存位点存在且同步维护：
- :585-587 init 三缓存归零（含 `_last_label_width`）；
- :697-705 zoom 菜单置零（含 prev_cols 判断）；
- :1890-1895 提前 return 条件 = icon_size AND max_cols AND label_width 三者全等（fast 分支 :1896 仅比 icon_size，未改，属原语义）；
- :2026-2028 refresh 下发路径写回三个缓存；
- :4041-4043 resize 结束置零三缓存后 refresh；
- :4166-4168 配置热更新 zoom_changed 时置零 icon_size/label_width。
- 三缓存均在 gate 中被读取，无只写死代码。:1587-1593 横向滚动条策略仍为 AlwaysOff，未越界。

## 3. 否定测试（FAIL — 本轮 needs_revision 依据）

- 方法：单行删除 `and label_width == self._last_label_width` 得到 pre-t16 逻辑副本（logs/v4028/t16/negtest/fence_widget.py），经 sys.modules 预加载为 `src.ui.fence_widget` 后运行 `scripts/selftest_fence_narrow_layout.py`（runner: logs/v4028/t16/negtest/run_neg.py）。预加载有效性已证实：negated cell_w=[119]×8 vs t16 版 [115,119]×8，二者确实执行了不同模块。
- 结果：**negated rc=0 PASS**。闸门在自身测试路径（单列、钉选 8 个 txt）上即使缓存缺陷存在也 cell_w=119 ≤ viewport，所有断言恒真。
- 脚本 docstring 第 15-16 行声称「pre-t16 修复前 cell_w 卡 210、断言 1 失败」——该声明在该脚本的可执行路径上**为假**。缺陷只在特定 label 配置（如 t12 Q3 的 18 格宽分区）复现。
- 结论：该脚本不是有效闸门（captain 判据「若脚本恒真，判 needs_revision」）。有效判别者应改用 probe_t12_q3.py 口径（多列宽分区收缩序列 + cell_fits_viewport_all），或将闸门用例改造成能触发卡 210 的配置。

## 4. verify 命令（全部 rc=0，证据 logs/v4028/t16/verify/）

| # | 命令 | 退出码 |
|---|---|---|
| 1 | `python -m compileall src scripts` | 0 |
| 2 | `python scripts/selftest_smoke.py` | 0（42/42） |
| 3 | `python scripts/selftest_fence_narrow_layout.py` | 0 |
| 4 | `python logs/v4028/t12/probe_t12_p0.py` | 0（18/18） |
| 5 | `python scripts/selftest_multimon_chrome.py` | 0（4/4） |

## 5. 工作区还原与未越界

- 否定测试后 `src/ui/fence_widget.py` 已逐字节还原：SHA256 `3CFEFFE843DC2AEAE2E196B45841A7326F877CF1DD19CD5BA331F150FBB2E463` 与备份 `logs/v4028/t16/fence_widget.py.t16-orig` 相等（备份仅为评审期临时文件，位于 logs/ 不入库）。
- `git diff --stat -- src/ui/fence_widget.py` = `35 insertions(+), 1 deletion(-)`（t10 G2 代码 + t16 三条件 return），与 t16 落地后预期一致。
- 未越界核对：src/ui/fence_icon_item.py 的 diff 仅含 t10 G4/G8 代码（无 t16 内容）；scripts/selftest_smoke.py 与 docs/FULL_TEST_CASES.md 零 diff（Measure-Object Lines=0）；未执行 selftest_all.bat。
- negtest 副本仅存在于 logs/v4028/t16/negtest/，不在 src/ 或 scripts/。

## 6. findings

| 编号 | 文件:行号 | 问题 | 触发条件 | 影响 | 建议修法 | 严重度 | 置信度 |
|---|---|---|---|---|---|---|---|
| F-t16-R1 | scripts/selftest_fence_narrow_layout.py:15-16, 全文 | 回归闸门恒真：其单列钉选路径上 pre-t16 缺陷（label_width 缓存失真）仍产出 cell_w=119≤viewport，否定测试 rc=0；docstring 声称的「卡 210 失败」在该路径不可达 | 对本脚本输入（8 个窄文件名、单列收缩）运行修复前代码 | F-t12-01 若回归，此闸门无法拦截，t16 的防复发承诺落空 | 用例改为能复现卡 210 的配置（参照 probe_t12_q3.py 的 18 格宽分区 + WIDTHS 序列 + `cell_fits_viewport_all` 判据），或将 probe_t12_q3.py 固化为 scripts/ 下正式闸门并重跑否定测试至 rc=1 | high | 高（双模式实测） |
| F-t16-R2 | logs/v4028/t12/probe_t12_q3.py（口径观察，非 src 缺陷） | `host_fits_viewport_w_all` 判据在 offscreen 下恒 False（host 宽 640 vs viewport 604，12px 为框架装饰），导致修复后代码 rc=1，弱化探针作为 CI 判据的可用性 | 任意 offscreen 运行 | 探针 rc 与真实缺陷不再一一对应（本双模式下仍能通过 cell_fits 区分） | 探针将框架装饰余量（实测 12px）纳入容差，或拆分 host_fits 与 cell_fits 两个独立判据 | low | 中 |

## 7. 结论

- 修复代码本身：有效、位点齐全、无回归（§1/§2/§4/§5）。
- 但 t16 的验收契约要求「新增脚本是有效闸门」，而否定测试证明其恒真 → **needs_revision**，requiredFix 见 F-t16-R1。修复 F-t16-R1 后无需重跑全部 verify，只需：新闸门对 t16 代码 rc=0、对 negated 副本 rc=1 即可放行。
