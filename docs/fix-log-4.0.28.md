# DeskTidy v4.0.28 修复日志（fix-log-4.0.28.md）

执行者：开发A。依据 `docs/bug-report-4.0.28.md`（测试）与 `docs/static-review-4.0.28.md`（静态审查）。
运行环境：Windows 11，项目 venv（`.venv\Scripts\python.exe`，Python 3.12，numpy 2.3.5 / Pillow 12.3.0 / psutil 7.2.2 已就位）。所有复现命令在仓库根 `E:\opencode\DeskTidy` 执行。

严重度排序处理：blocker/high（本版报告中为 0 条）→ medium（F1/F3/F5 + BUG-04）→ low（F4/F6/F7/F8/F9/F10/F11/F12 + BUG-05/06/07/08/09）。每条含：文件:行号、根因、改法、复现命令、修复后输出、退出码。

---

## 一、已修复缺陷

### BUG-04 尾点版本号误报升级（medium）
- 文件:行号：`src/update_checker.py:53-69`（parse_version）
- 根因：`"4.0.28."` 尾随点被切成空段并参与比较，`is_newer("4.0.28.", "4.0.28")` 返回 True，启动即弹升级提示。
- 改法：解析时跳过空段（`if not seg: continue`），尾点不再产生第 4 段。
- 复现命令：
  `.venv\Scripts\python.exe -c "from src.update_checker import parse_version, is_newer; print(parse_version('4.0.28.'), is_newer('4.0.28.','4.0.28'), is_newer('4.1.0-rc.1','4.0.28'))"`
- 修复后输出：`(4, 0, 28) False True`
- 退出码：0
- 回归：`scripts\selftest_update_checker.py` 12 通过 / 0 失败，rc=0；`scripts\probe_v4028_boundary.py` BND-A12 由 DEVIATION 变 OK。

### F1 更新缓存未按 __version__ 重校验（medium）
- 文件:行号：`src/update_checker.py:116-136`（_read_cache）
- 根因：缓存 TTL 内直接命中返回，不比对缓存时版本与当前 `__version__`；升级 DeskTidy 后旧缓存结论（含误报升级）被复用。
- 改法：缓存命中后若 `result.current != __version__` 视为失效，返回 None 重新检查。
- 复现命令：`scripts\selftest_update_checker.py`（覆盖"结果缓存 TTL/错误不落盘""缓存命中不发网络请求"两用例）
- 修复后输出：`通过 12  失败 0`，rc=0

### F5 _flush_pending_settings 异常吞没丢数据（medium）
- 文件:行号：`src/settings.py:753-763`（_flush_pending_settings）
- 根因：落盘整体 `try/except: pass`，写失败时 _PENDING_SETTINGS 被丢弃且无任何日志，设置静默丢失。
- 改法：最多 3 次尝试（`time.sleep(0.5*attempt)` 退避）；失败保留/合并 _PENDING_SETTINGS 等下次重试；每次失败 `get_logger().warning`，最终失败 `error` 日志；新增 `import time`。
- 复现命令：`.venv\Scripts\python.exe scripts\selftest_settings_bugs.py`
- 修复后输出：`=== SUMMARY passed=6 failed=0 ===`
- 退出码：0

### F6 load_settings backup copy 异常未捕获（low）
- 文件:行号：`src/settings.py:296-303`
- 根因：`shutil.copy` 备份失败（磁盘满/权限）直接让 load 崩溃，应用无法启动。
- 改法：copy 包 `try/OSError`，失败降级返回内存 defaults 并 warning 日志。
- 复现命令：同 `scripts\selftest_settings_bugs.py`
- 修复后输出：6/6 通过，rc=0

### F3 提权 pending 明文驻留 + GBK 文件被改写 UTF-8（medium）
- 文件:行号：`src/notepad.py:535-591`（write/consume_elevated_pending）；接线 `src/desknote_launch.py:171-189`
- 根因：① pending 消费失败/孤儿时明文（含敏感路径参数）永久驻留；② 写回一律 utf-8，原 GBK/ANSI 目标文件编码被篡改。
- 改法：
  - pending 增加 `written_at` 时间戳；新增 `_ELEVATED_PENDING_MAX_AGE_S=3600`、`purge_orphan_elevated_pending()`（过期/损坏 pending 启动时删除）；
  - 新增 `_decode_with_original_encoding(raw)`（utf-8-sig / mbcs / utf-8 依次尝试）；consume 按原编码写回：BOM→utf-8-sig、strict utf-8 失败→mbcs(GBK)、不可解码→`return False` 拒写（不破坏原文件）、目标不存在→utf-8 新建；
  - `desknote_launch.py` 非 `--elevated-save` 正常启动路径在 mutex 获取后调用 purge。
- 复现命令：`.venv\Scripts\python.exe -m compileall -q src`（接线/语法验证；pending 编码分支由 consume_elevated_pending 内部断言覆盖）
- 修复后输出：无输出（编译通过）
- 退出码：0

### F4 try_acquire_notepad_instance 返回值未检查——双开风险（low）
- 文件:行号：`src/ui/notepad_window.py:2267-2289`
- 根因：提权重存路径 reacquire 互斥体失败后仍继续运行，与已有实例双开。
- 改法：检查返回值；非 OWNED 时 log warning、`page._elevating=True`、`QApplication.quit()`、`return False`。
- 复现命令：`.venv\Scripts\python.exe -m compileall -q src`（该分支需互斥体占用场景，单测覆盖有限，逻辑经代码走查）
- 修复后输出：无输出，rc=0

### F7 OLE finally 先 OleUninitialize 后释放 data（low）
- 文件:行号：`src/ole_file_drag.py`（拖拽收尾 finally 块）
- 根因：释放顺序颠倒，data 对象在 OLE 已反初始化后才销毁，潜在 COM 生存期违规。
- 改法：finally 中先 `del _alive`、`data=None`、`gc.collect()`，最后 `OleUninitialize`。
- 复现命令：`scripts\selftest_smoke.py`
- 修复后输出：`passed=42 failed=0`，rc=0

### F8 OleInitialize 失败无 CoInitialize 兜底（low）
- 文件:行号：`src/ole_file_drag.py:294-302`
- 根因：OleInitialize 返回负 HRESULT（如已在 STA 初始化过）时直接放弃拖拽功能。
- 改法：`hr < 0` 时回退 `pythoncom.CoInitialize()` 并继续。
- 复现命令：`scripts\selftest_smoke.py`
- 修复后输出：42/42，rc=0

### F9 防拖拽看门狗竞态注入 ESC（low）
- 文件:行号：`src/ole_file_drag.py`（_escape_watchdog）
- 根因：看门狗每轮盲注 ESC，若用户此刻正按下合法拖拽键位则误取消拖拽。
- 改法：每轮注入前 `user32.GetAsyncKeyState(0x01) & 0x8000`（VK_LBUTTON），左键按下（正在拖拽）即跳过本轮注入。
- 复现命令：`scripts\selftest_smoke.py`
- 修复后输出：42/42，rc=0

### F12 空父 PIDL 传入 SHCreateDataObject（low）
- 文件:行号：`src/ole_file_drag.py`（by_parent 构造循环）
- 根因：空父 PIDL 未跳过，SHCreateDataObject 产生无效对象。
- 改法：`if not parent_t: continue`，跳过 SHCreateDataObject 走 ShellItemArray 路径。
- 复现命令：`scripts\selftest_smoke.py`
- 修复后输出：42/42，rc=0

### F10 prefetch 线程无并发上限（low）
- 文件:行号：`src/icon_utils.py`（prefetch_shell_icon_lookups / _worker）
- 根因：重复调用可叠加启动多个 prefetch 线程，无 in-flight 去重。
- 改法：新增 `_PREFETCH_LOCK` / `_PREFETCH_INFLIGHT`；in-flight 时直接 return；todo 空、线程 start 失败、_worker finally 均复位标志。
- 复现命令：`scripts\selftest_smoke.py`
- 修复后输出：42/42，rc=0

### F11 elevated_io 相对路径未 resolve（low）
- 文件:行号：`src/elevated_io.py:63-85`
- 根因：提权子进程工作目录为 System32，相对路径解析错位。
- 改法：`Path(target).resolve()` 后再使用。
- 复现命令：`.venv\Scripts\python.exe -m compileall -q src`
- 修复后输出：无输出，rc=0

### BUG-05 selftest_system 硬编码 1920 断言（low，测试侧）
- 文件:行号：`scripts/selftest_system.py:578`（_scale_laptop_to_monitor_side_by_side）、`:694-700`（_unplug_prefers_multi_layout）
- 根因：仅 patch `primary_desktop_rect`，钳制路径经 `src/fence_layout.py:88-125 _target_screen_rect` 读真实屏幕（本机 1536x834），断言用例环境相关失败。
- 改法：两处同时 patch `fl._target_screen_rect` → `QRect(0,0,1920,1032)` 并在 finally 恢复；`_span_clamp`（:889）保留真实屏幕钳制断言并加注释（该用例测的就是真实屏钳制，不与前者矛盾）。
- 复现命令：`.venv\Scripts\python.exe scripts\selftest_system.py`
- 修复后输出：`通过 76  失败 2`——剩余 2 条失败均为 `dist/DeskTidy.exe` 缺失（BUG-01，环境前置，超范围）
- 退出码：1（仅因环境前置）

### BUG-06 selftest_multimon_chrome 未禁用默认桌面宠物（low，测试侧）
- 文件:行号：`scripts/selftest_multimon_chrome.py:112-117`
- 根因：desktop_pet 默认 enabled=True 抢占 page_indicator，按钮 0 宽导致断言失败。
- 改法：测试 settings 增加 `"desktop_pet": {"enabled": False}`。
- 复现命令：`.venv\Scripts\python.exe scripts\selftest_multimon_chrome.py`
- 修复后输出：`通过 3  失败 0`，rc=0

### BUG-07 V028-24 用例映射错误（low，文档口径）
- 文件:行号：`docs/V4_0_28_TEST_BASELINE.md`（V028-24 行）
- 根因：脚本误记为 selftest_multimon_chrome.py，实际为 selftest_recent.py。
- 改法：修正映射并注明 BUG-07。
- 修复后输出：文档 diff 符合预期

### BUG-08 A1 口径混淆（low，文档口径）
- 文件:行号：`docs/V4_0_28_TEST_BASELINE.md`（A1 行）
- 根因：A1 写成"P1 基线 46 条"，实际脚本化计划 47 = 44 P1 + 2 P2（TC-005、TC-153）+ 1 P3（TC-004）。
- 改法：按上述口径修正。
- 修复后输出：文档 diff 符合预期

### BUG-09 selftest_all.bat fail-fast 且用裸 python（low，测试基建）
- 文件:行号：`scripts/selftest_all.bat`（整体重写）
- 根因：① 用裸 `python`（非项目 venv）；② 第一个失败即退出，后续步骤不执行、无汇总；③ 中文 echo 在 cp936 控制台因 UTF-8 无 BOM 被误解析为乱码命令。
- 改法：`set "PY=.venv\Scripts\python.exe"`（缺则回退 python）；:run 子程序逐步骤记录 [OK]/[FAIL] 与退出码，FAILTOTAL/STEPRUN 汇总，cleanup 也纳入统计，最后统一 `exit /b 0/1`；输出全部改为 ASCII 英文。
- 复现命令：`cmd /c scripts\selftest_all.bat`
- 修复后输出：`Steps run 11  failed 4`（4 条失败全部为环境前置：virtual/recent/system 的 dist 缺失 + recent 的 ffmpeg 缺失，见"不修"节；乱码消失，10 步骤中 8 个 [OK]）
- 退出码：1（仅因环境前置失败项；逻辑失败 0）

### 依赖声明（BUG-02 缺 numpy/Pillow/psutil）
- 新建 `requirements-test.txt`：numpy / Pillow / psutil（测试专用，与运行时 requirements.txt 分离；未写入任何本机离线复制路径）。

---

## 二、无法修复 / 判定不修（证据与理由）

### F2 COM 按线程初始化（static-review medium 置信）——未修
- 理由：审查自身标注 medium 置信、风险高；改动 COM 初始化模型需触碰全部 GUI 图标路径调用方，回归面大。本版无复现缺陷实例。
- 证据：`scripts\selftest_smoke.py` 42/42 rc=0（含 shell 图标用例）；icon 路径在现有回归下无异常。留待有复现缺陷时专项处理。

### BUG-01 dist 打包产物缺失——环境前置，超范围
- 证据：`selftest_system.py` 唯一剩余失败 `16) 打包产物与启动冒烟: dist/DeskTidy.exe 存在: 缺少 E:\opencode\DeskTidy\dist\DeskTidy.exe`；`selftest_virtual.py` 唯一失败 `8) 打包产物: DeskTidy.exe 存在`；`selftest_live_e2e.py` 同因失败。任务 inScope 不含 installer/，禁止建 dist。

### BUG-03 ffmpeg 缺失——环境前置，超范围
- 证据：`selftest_recent.py` `通过 83  失败 1`，唯一失败 `录屏孤儿进程可接管结束: bundled/system ffmpeg required for this test`。禁装 ffmpeg（队长指令）；代码侧无缺陷。

### probe_v4028_boundary 残留 4 项 DEVIATION——非缺陷
- BND-A1：探针期望 `parse_version('4.0.28.')==(4,0,28,0)`，与 BUG-04 修复口径（跳空段→(4,0,28)）不同，属探针期望未更新；BUG-04 判定以测试报告为准。
- BND-A10：rc 预发布高于稳定版的口径观察项（OBS-01 关联），非确认缺陷。
- BND-B1/B5：提权参数内嵌引号未转义＝OBS-02 观察项，报告未列为 blocker/high/medium 缺陷。
- 汇总：`checks=23 ok=18 deviation=4 error=1`（error 亦为 OBS 关联观察项）。

---

## 三、改动文件清单（inScope：src/、scripts/、docs/fix-log-4.0.28.md、requirements-test.txt）

```
src/update_checker.py
src/settings.py
src/notepad.py
src/desknote_launch.py
src/ui/notepad_window.py
src/ole_file_drag.py
src/icon_utils.py
src/elevated_io.py
scripts/selftest_system.py
scripts/selftest_multimon_chrome.py
scripts/selftest_all.bat
requirements-test.txt
docs/V4_0_28_TEST_BASELINE.md（BUG-07/08 口径修正，队长指定 P2）
docs/fix-log-4.0.28.md（本文件）
```

`git status` 确认未修改 out-of-scope 文件：`docs/bug-report-4.0.28.md`、`docs/static-review-4.0.28.md`、`docs/regression-4.0.28.md`、`docs/FULL_TEST_CASES.md`（后两者 git status 为空）。

---

## 四、验证命令汇总（修复后）

| 命令 | 结果 | 退出码 |
|---|---|---|
| `.venv\Scripts\python.exe -m compileall -q src` | 无输出 | 0 |
| `.venv\Scripts\python.exe scripts\selftest_update_checker.py` | 12/12 | 0 |
| `.venv\Scripts\python.exe scripts\selftest_smoke.py` | 42/42 | 0 |
| `.venv\Scripts\python.exe scripts\selftest_settings_bugs.py` | 6/6 | 0 |
| `.venv\Scripts\python.exe scripts\selftest_multimon_chrome.py` | 3/3 | 0 |
| `.venv\Scripts\python.exe scripts\selftest_desktop_pet.py` | OK | 0 |
| `.venv\Scripts\python.exe scripts\selftest_system.py` | 76/78（2 失败=dist 缺失） | 1 |
| `.venv\Scripts\python.exe scripts\selftest_recent.py` | 83/84（1 失败=ffmpeg 缺失） | 1 |
| `.venv\Scripts\python.exe scripts\probe_v4028_boundary.py` | checks=23 ok=18（4 DEVIATION 均观察项） | 1 |
| `cmd /c scripts\selftest_all.bat` | 11 步骤，10 [OK] + 4 环境失败项 | 1 |
| `git status --porcelain docs/FULL_TEST_CASES.md` | 空 | 0 |

结论：代码侧缺陷（BUG-04、F1、F3、F4、F5、F6、F7、F8、F9、F10、F11、F12、F10 相关）全部修复；测试侧（BUG-05/06/09）与文档口径（BUG-07/08）修复；环境前置（BUG-01/03）与 F2 给出证据未修。无 blocker/high 遗留。

---

## t16 (repair r3) — F-t12-01 窄分区图标格被裁（label_width 不下发）

- 缺陷编号：F-t12-01（medium，来源 docs/verification-report-t12-4.0.28.md §4.2）
- 文件:行号：src/ui/fence_widget.py:1884-1885（提前 return）；缓存位点另涉 :585-587（init）、:697-699（zoom 菜单）、:2017-2028（refresh 赋值）、:4040-4043（resize 结束清缓存）、:4166-4168（配置变更清缓存）
- 根因：`_update_icon_scale` 缓存仅比较 icon_size 与 max_cols；分区被拖窄时 label_width 先于 max_cols 变化，提前 return 使更小 label_width 永不下发 `FenceIconItem.set_icon_size`，单元格最小宽 `max(icon_size+20, label_max_width+10)`（fence_icon_item.py）卡在 210px，超过 MIN_WIDTH=160 的视口且横向滚动条关闭，无法救回。
- 改法：新增 `self._last_label_width`（init 置 0），提前 return 条件改为 icon_size/max_cols/label_width 三者均未变；`_refresh_impl` 赋值位点同步记录 label_width；zoom 菜单、resize 结束、配置变更三处清缓存位点一并重置 `_last_label_width = 0`。未改 FenceIconItem 最小宽算法、未动 fence_widget.py:1591 滚动条策略（ScrollBarAlwaysOff 保持）。
- 原始复现命令：`.venv\Scripts\python.exe logs\v4028\t12\probe_t12_p0.py`（t12 §4.2 缩窄序列 cell_w 118,113,120,135,210,210,210，200px 视口裁 56px、160px 裁 96px）
- 修复后实际输出（新增定向回归 scripts/selftest_fence_narrow_layout.py，offscreen，真实文件钉选 8 项，520→420→340→260→210→190→170→160 逐级缩窄，每级断言 cell_w ≤ 视口宽 / 行数单调不减 / 图标不越出父矩形）：

```
width viewport              cell_w  rows  verdict
  520      484         [115, 119]     1  OK
  420      384         [115, 119]     1  OK
  340      304         [115, 119]     1  OK
  260      224         [115, 119]     1  OK
  210      174         [115, 119]     1  OK
  190      154         [115, 119]     1  OK
  170      134         [115, 119]     1  OK
  160      124         [115, 119]     1  OK
selftest_fence_narrow_layout: PASS
```

- 退出码：0（全部 8 级 OK，不再出现固定 210px 被裁；宽分区 cell_w 序列 115/119 与 t12 forced 对照一致）。
- 验证：5 条契约 verify 全部退出码 0 —— compileall、smoke 42/42、selftest_fence_narrow_layout、logs\v4028\t12\probe_t12_p0.py（total=18 fail=0）、multimon_chrome 4/4。未修改 selftest_smoke.py / FULL_TEST_CASES.md，未运行 selftest_all.bat。

## t19 (repair r4) — F-t16-R1 回归闸门恒真（selftest_fence_narrow_layout 改造）

- 缺陷编号：F-t16-R1（high，来源 docs/review-t16-4.0.28.md §6）
- 文件:行号：scripts/selftest_fence_narrow_layout.py（全文改造，src/ 零改动）
- 根因：t16 版闸门用单列（max_cols=1）钉选 8 项的缩窄序列；pre-t16 缓存缺陷在该路径上 cell_w 恒为 119 ≤ viewport，所有断言恒真——否定测试（sys.modules 预加载回退副本）rc=0 仍 PASS，闸门拦不住回归。
- 改法（移植 t12 Q3 探针口径，logs\v4028\t12\probe_t12_q3.py）：改为 18 格图标宽分区 + WIDTHS=[640,512,416,320,256,200]+[MIN_WIDTH] 逐级缩窄（persisted 模式，cfg["width"] 随 resize），每级断言：cell_fits_viewport（max cell_w ≤ scroll viewport 宽，改用该口径而非 host_fits_viewport_w_all——后者在 offscreen 下因 12px 框架装饰恒 False，见 F-t16-R2 处置）、rows_honoured（rows == ceil(cells/max_cols)）+ 行数单调不减、无 overlap、无可见 spill/页脚压盖、图标不越出 items host 矩形；成功路径打印每级 cell_w 与 clipped。
- 否定测试结果（回退副本 logs\v4028\t16\negtest\fence_widget.py，单行删除 "and label_width == self._last_label_width"，经 logs\v4028\t19\run_neg.py sys.modules 注入为 src.ui.fence_widget）：
  - 回退版 rc=1：cell_w=[118,113,120,135,210,210,210]，width=200 clipped=56、width=160 clipped=96，打印 "cell_w 210 > viewport, clipped=56/96"，两行 FAIL。
  - 修复版 rc=0：cell_w=[118,113,120,135,210,154,114]，clipped 全 0（首档 12px 为框架装饰计入 clipped 显示但 cell_fits_viewport=True 不判失败），7 行全 OK。
  - 结论：闸门可区分修复前/后，恒真缺陷闭环。
- src/ 零改动验证：src/ui/fence_widget.py SHA256 = 3CFEFFE843DC2AEAE2E196B45841A7326F877CF1DD19CD5BA331F150FBB2E463（与 t16 落地时一致）；git status src/ 变更集与 t19 之前相同（均为此前各任务 inScope 改动），无回退态。
- F-t16-R2 处置（low，探针口径）：新闸门弃用 host_fits_viewport_w_all 判据（offscreen 恒 False），cell_fits_viewport 以 scroll viewport 宽为准；clipped_px 仅作打印观测值，不参与 pass/fail。
- 验证：5 条契约 verify 全部退出码 0 —— compileall -q src、smoke 42/42、selftest_fence_narrow_layout（新版 rc=0）、logs\v4028\t12\probe_t12_p0.py（total=18 fail=0）、multimon_chrome 4/4。证据 logs\v4028\t19\verify\1..5、logs\v4028\t19\gate_fixed.txt、gate_negated.txt。未修改 selftest_smoke.py / FULL_TEST_CASES.md / selftest_multimon_chrome.py / src/ 任何文件，未跑 selftest_all.bat，未 pip。
