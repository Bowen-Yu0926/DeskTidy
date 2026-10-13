# t4 修复独立评审报告（review-t4-4.0.28.md，round 1）

- 评审者：开发B（独立于 t4 执行者开发A）。评审对象：t4 attempt 2。
- 方式：git diff 逐文件核对 + 独立复跑 + 自构造边界反例；评审只读，未修改 src/、scripts/。
- **Verdict：pass**（无 blocker/high/medium 遗留；2 条 low 观察项不阻塞，见 §5）。

## 1. 越界核对

`git status --porcelain` 修改文件 = 11 个（scripts/selftest_all.bat、scripts/selftest_multimon_chrome.py、scripts/selftest_system.py、src/desknote_launch.py、src/elevated_io.py、src/icon_utils.py、src/notepad.py、src/ole_file_drag.py、src/settings.py、src/ui/notepad_window.py、src/update_checker.py），与 t4 声明 changedPaths 完全一致；新增 requirements-test.txt、docs/fix-log-4.0.28.md（inScope）。`git diff --stat -- docs/FULL_TEST_CASES.md` 为空、`git status --porcelain docs/FULL_TEST_CASES.md` 为空 → **零改动，无越界**。

## 2. 独立复跑（本机实跑，退出码）

| 命令 | 结果 | 退出码 |
|---|---|---|
| `.venv\Scripts\python.exe scripts\selftest_smoke.py` | passed=42 failed=0 | 0 |
| `.venv\Scripts\python.exe scripts\selftest_update_checker.py` | 通过 12 失败 0 | 0 |
| `.venv\Scripts\python.exe scripts\selftest_settings_bugs.py` | passed=6 failed=0 | 0 |
| `.venv\Scripts\python.exe scripts\selftest_public_drag.py` | 通过 40 失败 0 | 0 |
| `.venv\Scripts\python.exe scripts\selftest_multimon_chrome.py` | 通过 3 失败 0 | 0 |
| `.venv\Scripts\python.exe scripts\selftest_desktop_pet.py` | OK | 0 |
| `.venv\Scripts\python.exe -m compileall -q src` | 无输出 | 0 |
| `cmd /c scripts\selftest_all.bat` | 11 步骤，失败 4（全部 dist/ffmpeg 环境前置：virtual、recent、system、live_e2e；逻辑步骤 3b/3c/3d/3e/3e2/3f 全 OK） | 1（仅环境前置） |

## 3. 逐条独立验证（不只信自证）

- **BUG-04**（独立命令，非 t4 原命令）：`parse_version('4.0.28.')==(4,0,28) == parse_version('4.0.28')`，`is_newer('4.0.28.','4.0.28') is False` → ✅。实现为 parse_version 跳过空段（update_checker.py:63-66），语义正确（`4..28` 同样跳空段）。
- **F1 缓存按版本重校验**（自构造反例，替换 `_cache_path` 指向临时文件）：current='4.0.27' 的 TTL 内缓存 → `_read_cache` 返回 None（过期重查）；current=__version__ → 命中 HIT。断言 `r1 is None and r2 is not None` 通过 → ✅（update_checker.py:140-146）。
- **F3 GBK 写回**（自构造临时文件）：目标写入 `你好世界`.encode('gbk') 后 consume → 写回字节 `\xd0\xc2\xc4\xda\xc8\xdd` = '新内容'.encode('gbk')，**编码未漂移** → ✅。新文件路径：目标不存在 → UTF-8 新建 ✅；不可解码字节 → consume 返回 False 拒写，原文件未破坏 ✅（notepad.py consume_elevated_pending）。pending 带 written_at（:544）、purge_orphan_elevated_pending（:591-608）+ desknote_launch.py:182-189 正常启动路径 purge，逻辑走查无误。
- **F5 防抖落盘重试/保留**（monkeypatch `_save_settings_now` 真实 API）：第 1 次抛 OSError → 重试成功，attempts=2、pending 清空 ✅；持续失败 → 3 次退避后 `_PENDING_SETTINGS` 保留 `{'k':2}` 供 atexit ✅（settings.py:769-819）。sleep 在锁外（:808-809），无死锁风险。
- **F10 预取线程上限**（CountingThread 统计 200 并发调用）：worker 线程 spawn 恰好 1 次 → ✅（icon_utils.py:536-580，in-flight 标志 + 三处复位：todo 空/start 失败/_worker finally）。
- **F4**（notepad_window.py:2285-2300）：reacquire 非 OWNED → warning + `page._elevating=True` 跳过 close 自动保存 + `QApplication.quit()` + return False，无双开继续路径；该分支需真实互斥体竞争，无法低成本单测，代码走查 + compileall 通过，判定合理。
- **F6**（settings.py:303-317）：DEFAULT_SETTINGS copy 失败 → warning + 返回内存 defaults，不再启动崩溃 ✅。
- **F7/F8/F9/F12**（ole_file_drag.py）：finally 先 `del _alive`/`data=None`/gc.collect 后 OleUninitialize（:385-398）✅；OleInitialize hr<0 → CoInitialize 兜底（:308-314）✅；看门狗注入前 `GetAsyncKeyState(VK_LBUTTON) & 0x8000` 检查（:273-276）✅；空父 PIDL `if not parent_t: continue` 走 ShellItemArray（:136-140）✅。全部由 smoke 42/42 与 public_drag 40/40 覆盖。
- **F11**（elevated_io.py:74-79）：相对路径 `Path(target).resolve()` 后再拼参数 ✅（System32 CWD 错位消除）。
- **BUG-05**（selftest_system.py:567-600、:691-710）：两处补 patch `fl._target_screen_rect`，finally 恢复；`_span_clamp`（:887-899）保留真实屏断言并加注释说明不矛盾，逻辑自洽。本机实跑 76/78，仅剩 2 条 dist 缺失（环境前置）。
- **BUG-06**（selftest_multimon_chrome.py:112-117）：settings 增加 `"desktop_pet": {"enabled": False}`，3/3 通过。
- **BUG-09**（selftest_all.bat 全文走查 + 实跑）：venv 解释器 `set "PY=%~dp0..\.venv\Scripts\python.exe"` 缺则回退 python ✅；:run 子程序逐步记录 [OK]/[FAIL] 与 RC、FAILTOTAL/STEPRUN 汇总、cleanup 纳入统计、结尾统一 exit /b ✅；输出全 ASCII（cp936 控制台不再乱码）✅。无新增 fail 路径（引用的 selftest_portal.py / cleanup_selftest_artifacts.py 均存在）。
- **BUG-07/08**：文档口径修正，diff 与 fix-log 描述一致。
- **未修项确认**：F2（COM 按线程）、BUG-01（dist）、BUG-03（ffmpeg）、probe 4 项 DEVIATION——均已在 fix-log 列出理由，评审认可（见 §4/§5）。

## 4. 高风险改动专项

- **F2 判定不修——理由成立**：原审查即标注 medium 置信；COM 初始化模型改动需触碰全部 GUI 图标路径调用方，回归面大；本版无真实复现实例，且 smoke 42/42、public_drag 40/40（含 COM 扫描、PIDL、拖拽收尾用例）全绿。接受「留待有复现缺陷时专项处理」。
- **F3 编码分支**：BOM→utf-8-sig / strict-utf8 失败→mbcs / 不可解码→拒写 / 缺文件→utf-8 新建，优先级正确；已实测 GBK 往返与拒写路径。
- **F10 并发**：实测 200 并发仅 1 spawn；标志复位覆盖 todo 空、start 异常、worker finally 三路径，无泄漏。

## 5. Findings（low，不阻塞 pass）

- **R1 (low)** `src/notepad.py:609-621` consume_elevated_pending 中存在冗余死代码：先基于 `decoded == raw.decode('utf-8', errors='replace')` 计算一版 `enc`，随即被后面的 BOM/strict-utf8 分支完全覆盖，首版计算结果永不生效。行为正确（实测 GBK 往返通过），仅增加误读风险。requiredFix（可留待后续小改）：删除首个 enc 计算块，只保留 BOM→utf-8-sig / strict→utf-8 / 否则 mbcs 的判定链。
- **R2 (low)** `scripts/selftest_all.bat` 失败计 4 条均为环境前置（dist×3、ffmpeg×1），退出码恒为 1 直到 t5 补齐前置；建议 t5 完成后复跑确认 ALL TEST SUITES PASSED。非 t4 缺陷，仅交接提示。

## 6. 队长补充点核销

- pet_anim.py:697 惰性 `import numpy as np`：**已兜底**——import 位于 `try:` 内，`except Exception:` 回退纯 `img.pixel` 逐像素扫描（pet_anim.py:696-717），发布环境缺 numpy 不会 ImportError，判定合理。
- F2 不修理由：成立（§4）。
- 三个脚本改动：未引入新 fail 路径（§3 BUG-09 条 + R2）。

## 7. 结论

t4 修复正确、完整、无越界、无回归；环境前置项归属 t5。Verdict：**pass**。
