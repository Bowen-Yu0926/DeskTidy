# Review t19 — 闸门改造独立评审（Round 4, t20）

- 评审人: 开发A（独立执行否定测试，不采信 t19 自报 rc）
- 评审对象: `scripts/selftest_fence_narrow_layout.py`（t19 全文重写版）
- 日期: 2026-10-13
- Verdict: **pass**

## 1. 字节核对（src 零改动 + 闸门未被再改）

| 文件 | 期望 SHA256 | 实测 | 结果 |
|---|---|---|---|
| `src/ui/fence_widget.py` (168331b) | `3CFEFFE843DC2AEAE2E196B45841A7326F877CF1DD19CD5BA331F150FBB2E463` | 相同 | PASS |
| `scripts/selftest_fence_narrow_layout.py` | `2255ECF84F17A9AA469140F29EFE288168F71D7A997EA8BD41931700267C2A24`（t19 报告值） | 相同 | PASS |

工作区 `git status` 显示的 src/ 修改均为 t4/t10/t14/t16 已评审的历史改动，t19 未新增 src 变更。无回退残留。

## 2. 闸门口径确认

- `WIDTHS = [640, 512, 416, 320, 256, 200] + [FenceWidget.MIN_WIDTH]`，`N_ITEMS = 18`，tempdir 真实文件 q3_00..17.txt 钉选（`path_present` 过滤要求真实存在）。
- 每级断言（`scripts/selftest_fence_narrow_layout.py:223-231`）：
  1. `cell_fits_viewport`: `cell_w <= vrect.width()`（:157，数值真判）；
  2. `rows_honoured`: `rows == ceil(cells/max_cols)`（:152）且 `rows_from_grid >= prev_rows` 单调不减（:226）；
  3. `overlap_pairs==0`、`spill_fence_visible==0`、`footer_overlap_visible==0`、`outside_parent==0`（:227-230）。
- 失败即 `return 1`（:261-265），非恒 True。

## 3. 独立否定测试（本次评审自建，未复用开发B 产物）

注入方法：读取当前 `src/ui/fence_widget.py` 原始字节，删除唯一一处 `and label_width == self._last_label_width`（确认 needle 恰好出现 1 次），写入 `logs/v4028/t20/fence_widget_neg.py`；runner `logs/v4028/t20/run_neg_t20.py` 在 import 闸门脚本前将其预加载为 `sys.modules["src.ui.fence_widget"]`。

实测（`logs/v4028/t20/negtest.log`）：

```
   200  clipped=56  cell_w=[210]  FAIL
   160  clipped=96  cell_w=[210]  FAIL
selftest_fence_narrow_layout: FAIL
 - width=200: cell_w 210 > viewport, clipped=56
 - width=160: cell_w 210 > viewport, clipped=96
negtest rc= 1   （cmd 退出码 1）
```

同命令下修复版 rc=0（见 §5）——注入真实生效，闸门**能拦截 F-t12-01 回归**。失败信息正确指向卡住的 cell_w=210 与越界像素 56/96。

## 4. F-t16-R2 处置判定（host_fits offscreen 恒 False）

t19 闸门**不断言** `host_fits_viewport_w`（offscreen 下因 12px 框架装饰恒 False），仅以 `STALE_TOLERANCE_PX=2` 观测、`clipped_px` 仅打印（`scripts/selftest_fence_narrow_layout.py:157-160, 139`），改以 scroll viewport 上的 `cell_fits_viewport` 为硬判据。判定：**可接受**——cell-fits-viewport 是正确不变量（缺陷表现正是 cell 超视口裁剪），host 级宽度降为观测不削弱拦截力（否定测试 rc=1 即证）。记 low 观察，不阻塞。

## 5. 独立复跑 verify（全部本评审自行执行，日志在 logs/v4028/t20/）

| 命令 | 退出码 | 证据 |
|---|---|---|
| `.venv\Scripts\python.exe -m compileall -q src` | 0 | verify_compileall.log |
| `.venv\Scripts\python.exe scripts\selftest_smoke.py` | 0 | passed=42 failed=0 |
| `.venv\Scripts\python.exe scripts\selftest_fence_narrow_layout.py` | 0 | cell_w=[118,113,120,135,210,154,114] clipped 0（首档 12px 框架装饰观测值），PASS |
| `.venv\Scripts\python.exe logs\v4028\t12\probe_t12_p0.py <json>` | 0 | total=18 fail=0（注：探针实际位于 logs\v4028\t12\，非 scripts\） |
| `.venv\Scripts\python.exe scripts\selftest_multimon_chrome.py` | 0 | 通过 4 失败 0 |

## 6. 观察项（不阻塞）

1. **low**: `scripts/selftest_fence_narrow_layout.py:253-257` 打印表头 `vp_w` 列实际输出的是 `clipped_px`（列标签与数据不符），且 clipped 列重复打印两次。仅影响日志可读性，不影响判定。建议后续顺手改表头为 `clipped`。
2. **low**: `cell_fits_viewport is not False` 允许 None（视口取不到时跳过判）；实际场景 vrect 恒可得，风险可忽略。
3. **low (F-t16-R2)**: 见 §4，host_fits 观测化处置可接受。

## 结论

改造后的闸门满足验收：多列 18 格宽分区 + WIDTHS 序列 + viewport 级 cell-fit/行数/越界三组数值真判；独立否定注入 rc=1（210px 卡死 + 56/96px 越界正确报出），修复版 rc=0；src/ 零改动。**verdict = pass**，放行 t18 定向回归。
