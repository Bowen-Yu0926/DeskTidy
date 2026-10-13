# DeskTidy v4.0.28 测试执行与缺陷报告（任务 t2）

- 执行人：测试工程师（DeskTidy-Dev / desktidy-dev）
- 任务：t2「v4.0.28 自动化套件与边界异常测试执行」，attempt 2 = `9e4662d0-0f40-48f9-a94b-5e6b4bc94c2f`
- 仓库：`E:\opencode\DeskTidy`，git HEAD `9ba2434`（Fix settings races and stop pet page chips from overlapping.），`src/_version.py` = `4.0.28`
- 解释器：`.venv\Scripts\python.exe`（CPython 3.12.10，PyQt6 / pywin32 312 / Send2Trash / watchdog 就绪）
- 环境：主屏可用区 `1536x834`（geometry `1536x864`），指纹 `0,0,1536x864@355x199mm@1.25`；`dist\` 不存在；系统无 `ffmpeg`
- 基线：`docs\V4_0_28_TEST_BASELINE.md`（P1 全量 + P2 TC-005 + 新增 V028-01~25）

## 0. 结论摘要

| 维度 | 结果 |
|---|---|
| 定向脚本矩阵 | 17 个脚本：11 个退出码 0，6 个退出码 1 |
| P1 全量计划（`--rounds 1 --no-write-md`） | 47 条用例中 24 条全过；22 条 P1 未通过（全部落在这 5 个脚本：virtual / recent / desktop_pet / multimon_chrome / system）+ TC-005(P2) 未运行 |
| 边界/异常输入探针 | 23 项检查：18 OK / 4 DEVIATION / 1 FAIL（1 项 FAIL 与 2 项 DEVIATION 经复核为**探针自身**问题） |
| 产品缺陷（确认，可复现） | 1 项（低）：尾点版本号被判为升级；另 2 项低危观察项（预发布策略口径、参数内嵌引号未转义） |
| 环境/构建前置阻塞 | 3 项（高）：缺打包产物、缺测试依赖声明、缺 ffmpeg |
| 用例自身问题 | 2 项（低）：多屏几何硬编码 1920、分页栏用例未禁用默认桌面宠物 |
| 口径/覆盖偏差 | 3 项（低）：A1「P1 46 条」口径、V028-24 映射、`selftest_all.bat` fail-fast |
| 超时阻塞 | 无。最慢脚本 `selftest_recent.py` 22.3s，全量计划 57.2s，远低于 10 分钟门限 |

**一句话结论**：本轮没有发现 blocker 级产品缺陷；`v4.0.28` 的门禁失败**主因是运行环境前置**（未打包、依赖未声明、无 ffmpeg）与**两条用例的环境硬编码假设**，产品代码层面仅确认 1 条低危缺陷（畸形版本号尾点）。

## 1. 执行矩阵（通过清单，含退出码）

命令模板（仓库根目录，显式使用 venv 解释器，环境变量 `PYTHONIOENCODING=utf-8`、`DESKTIDY_SELFTEST=1`）：

```powershell
cd E:\opencode\DeskTidy
$env:PYTHONIOENCODING="utf-8"; $env:DESKTIDY_SELFTEST="1"
.venv\Scripts\python.exe scripts\<脚本>.py
```

| 脚本 | 退出码 | 脚本内断言 | 用例编号 | 证据日志 |
|---|---|---|---|---|
| selftest_smoke.py | 0 | 通过 42 / 失败 0 | TC-000, TC-011, TC-019, TC-021, TC-050 | logs\v4028\r2_selftest_smoke.log |
| selftest_public_drag.py | 0 | 通过 40 / 失败 0 | TC-031（+V028-16/17/19） | logs\v4028\r2_selftest_public_drag.log |
| selftest_public_host.py | 0 | 通过 11 / 失败 0 | TC-030 | logs\v4028\r2_selftest_public_host.log |
| selftest_public_multiselect.py | 0 | 通过 4 / 失败 0 | TC-032 | logs\v4028\r2_selftest_public_multiselect.log |
| selftest_portal.py | 0 | 通过 6 / 失败 0 | TC-017 | logs\v4028\r2_selftest_portal.log |
| selftest_fence_bg_menu.py | 0 | 通过 5 / 失败 0 | TC-014 | logs\v4028\r2_selftest_fence_bg_menu.log |
| selftest_fence_keys.py | 0 | 通过 12 / 失败 0 | TC-015 | logs\v4028\r2_selftest_fence_keys.log |
| selftest_update_checker.py | 0 | 通过 12 / 失败 0 | TC-153(V028-01~09,11) | logs\v4028\r2_selftest_update_checker.log |
| selftest_settings_bugs.py | 0 | 全通过 | V028-20, V028-21 | logs\v4028\r2_selftest_settings_bugs.log |
| selftest_page_switch_flash.py | 0 | 通过 15 / 失败 0 | TC-020 | logs\v4028\r2_selftest_page_switch_flash.log |
| selftest_overlay_raise.py | 0 | 通过 7 / 失败 0 | V028-23 | logs\v4028\r2_selftest_overlay_raise.log |
| selftest_chunked.py / session_aug12 / drop_matrix / namespace_native / fence_style / file_search / calculator / todos / recording_indicator / perf_stress | 0 | 计划内全通过 | TC-013, TC-171, TC-033, TC-018, TC-015b, TC-070, TC-100, TC-110, TC-060, TC-003(+TC-004 P3) | logs\v4028\r2_full_plan_r1.log |

全量计划（P1 主体，含上表脚本）：

```powershell
.venv\Scripts\python.exe scripts\run_full_test_plan_x3.py --rounds 1 --no-write-md
# 用例数 47 / 自动化脚本 25 / 脚本执行 24 次 / 耗时 57.2s / 用例通过 24/47 → 退出码 1
```

> `--no-write-md` 是刻意使用：默认模式会重写 `docs\FULL_TEST_CASES.md`，与验收要点 A6「不修改该文件」冲突。已确认该文件未被改动（`git status` 未列出）。
> 批量退出码矩阵：logs\v4028\r2_batch1.csv、logs\v4028\r2_batch2.csv、logs\v4028\batch_matrix.csv。

## 2. 失败清单

### BUG-01 打包产物缺失导致 3 个脚本失败（环境/构建前置，high）

| 项 | 内容 |
|---|---|
| 用例编号 | TC-010；TC-001/TC-002/TC-016/TC-022/TC-061/TC-150/TC-151（脚本内 2 项）；TC-005(P2) |
| 严重度 | high |
| 复现命令 | `.venv\Scripts\python.exe scripts\selftest_virtual.py`；`.venv\Scripts\python.exe scripts\selftest_system.py`；`.venv\Scripts\python.exe scripts\selftest_live_e2e.py` |
| 实际输出 | selftest_virtual：`FAIL  DeskTidy.exe 存在:` + `AssertionError`，`通过 15  失败 1`；selftest_system：`FAIL  dist/DeskTidy.exe 存在: 缺少 E:\opencode\DeskTidy\dist\DeskTidy.exe`、`FAIL  启动后分区窗口宽度正常`，`通过 72  失败 6`；selftest_live_e2e：`FAIL  missing E:\opencode\DeskTidy\dist\DeskTidy.exe`（`=== LIVE B) 启动打包版 ===`） |
| 期望结果 | 发布门禁应存在打包产物（`dist\DeskTidy\DeskTidy.exe` 或 `dist\DeskTidy.exe`），或在源码运行模式下显式跳过打包类检查 |
| 退出码 | virtual=1，system=1，live_e2e=1 |
| 证据文件 | logs\v4028\r2_selftest_virtual.log、logs\v4028\r2_selftest_system.log、logs\v4028\r3_selftest_live_e2e.log、logs\v4028\selftest_all.stdout.log（`Test-Path dist` = False） |
| 归因 | 环境/构建前置（非产品代码缺陷） |

### BUG-02 测试依赖未声明：numpy / Pillow / psutil（环境前置，high）

| 项 | 内容 |
|---|---|
| 用例编号 | TC-120~TC-123（宠物 4 条 P1）；V028-22；TC-001/TC-061 相关 2 项系统检查；TC-005(P2) |
| 严重度 | high |
| 复现命令 | `.venv\Scripts\python.exe scripts\selftest_desktop_pet.py`；`.venv\Scripts\python.exe scripts\selftest_system.py` |
| 实际输出 | `ModuleNotFoundError: No module named 'numpy'`（scripts\selftest_desktop_pet.py:98 `import numpy as _np`，退出码 1，11.6s）；`ModuleNotFoundError: No module named 'PIL'`（system 项「应用图标资产透明且铺满」）；`ModuleNotFoundError: No module named 'psutil'`（system 2 项 + live_e2e 启动阶段） |
| 期望结果 | 测试依赖可用；`requirements.txt` 目前仅声明 python-docx / PyQt6 / PyQt6-WebEngine / markdown / pywin32 / send2trash / watchdog，**未声明 numpy、Pillow、psutil**，建议补 `requirements-test.txt` |
| 退出码 | desktop_pet=1；system=1（其中 4 项由此引起：PIL×1、psutil×2 另见 BUG-01） |
| 证据文件 | logs\v4028\r2_selftest_desktop_pet.log、logs\v4028\r2_selftest_system.log、logs\v4028\r2_selftest_system_pilshim.log |
| 反证（已解除） | 注入 PIL 12.3.0 + numpy 2.3.5 后 `selftest_desktop_pet: OK`、退出码 0（logs\v4028\r3_selftest_desktop_pet_shim.log）；psutil 7.2.2 装入 venv 后 live_e2e 越过 psutil 阶段，仅剩 BUG-01（logs\v4028\r3_selftest_live_e2e.log） |
| 归因 | 环境前置（依赖声明缺失） |

### BUG-03 无 ffmpeg（系统无、仓库无捆绑），连带 9 条 P1 用例失败（环境/构建前置，high）

| 项 | 内容 |
|---|---|
| 用例编号 | TC-012, TC-040, TC-051, TC-080, TC-090, TC-130, TC-140, TC-152, TC-170（+V028-18、V028-24） |
| 严重度 | high |
| 复现命令 | `.venv\Scripts\python.exe scripts\selftest_recent.py` |
| 实际输出 | `FAIL  录屏孤儿进程可接管结束: bundled/system ffmpeg required for this test`，`通过 83  失败 1  耗时 18.7s`，`AssertionError: bundled/system ffmpeg required for this test` |
| 期望结果 | 系统 PATH 存在 ffmpeg，或打包产物内含捆绑 ffmpeg（`scripts\dist_paths.py` 解析捆绑目录） |
| 退出码 | 1 |
| 证据文件 | logs\v4028\r2_selftest_recent.log；`Get-Command ffmpeg` 未找到、仓库深度 3 内无 `ffmpeg*.exe` |
| 归因 | 环境/构建前置（该检查项的名称也表明期望“捆绑”产物） |

### BUG-04 产品候选（低）：畸形版本号尾点被判为新版本

| 项 | 内容 |
|---|---|
| 用例编号 | V028-04（pre-release/版本比较边界） |
| 严重度 | low |
| 复现命令 | `.venv\Scripts\python.exe scripts\probe_v4028_boundary.py`（检查项 BND-A12） |
| 实际输出 | `DEVIATION BND-A12 4.0.28. 尾点误报升级: is_newer('4.0.28.','4.0.28')=True parse=(4, 0, 28, 0)` |
| 期望结果 | 非法/畸形 tag（尾点、空段）不应被判定为“有新版本”，避免无意义升级提示 |
| 退出码 | 探针整体退出码 1（内含 4 项 DEVIATION + 1 项 FAIL，见第 4 节） |
| 证据文件 | logs\v4028\r2_boundary.log（第 12 行）；代码位置 `src\update_checker.py:53-69`（`parse_version` 按 `.` 切分、非数字段计 0，尾点产生额外段） |
| 归因 | 产品缺陷候选（低；需上游 tag 畸形才触发） |

### BUG-05 用例缺陷（低）：selftest_system.py 多屏几何断言硬编码 1920 主屏

| 项 | 内容 |
|---|---|
| 用例编号 | TC-001, TC-002, TC-016, TC-022, TC-150, TC-151（脚本内组 4) 分区布局/多屏钳制） |
| 严重度 | low |
| 复现命令 | `.venv\Scripts\python.exe scripts\selftest_system.py` |
| 实际输出 | `FAIL  笔记本→显示器等比例且并排`（scripts\selftest_system.py:578 `assert a["width"] == int(round(0.838 * 1920))`）；`FAIL  拔掉外接屏按多屏布局比例落到本机: {'x': 50, 'y': 50, 'width': 400, 'height': 300, 'collapsed': False}`（scripts\selftest_system.py:691 `assert g1["width"] >= 1500`） |
| 期望结果 | 用例应在真实主屏尺寸下成立，或同时 patch 钳制所依赖的屏幕解析函数 |
| 退出码 | 1（`通过 72  失败 6`） |
| 证据文件 | logs\v4028\r2_selftest_system.log、根因实验 logs\v4028\probe_unplug_clamp.log / logs\v4028\probe_unplug_clamp.py |
| 根因机制 | 用例只 patch 了 `fl.primary_desktop_rect`（selftest_system.py:574、:686），但钳制路径 `src\fence_layout.py:701-707 _clamp_geometry` 使用 `_target_screen_rect`（`src\fence_layout.py:88-125`，遍历**真实**屏幕）解析可用区，实测 `fl._target_screen_rect(0,0,1609,315) = QRect(0, 0, 1536, 834)`，故 1609 被钳到 1536 → 断言失败 |
| 反证 | 把 `_target_screen_rect` 置为 no-op（等价于主屏足够大）后上述两项 **PASS**，而同一文件的 `assert geom["width"] <= primary.width()`（scripts\selftest_system.py:880「跨屏全宽钳制到单屏」）反而 **FAIL** → 同一文件内“按 1920 缩放”与“钳到真实主屏”两套前置假设互相矛盾 |
| 归因 | 用例自身（环境硬编码），非产品缺陷 |

### BUG-06 用例缺陷（低）：分页栏兜底用例未禁用默认桌面宠物

| 项 | 内容 |
|---|---|
| 用例编号 | TC-160（多屏 work area 与 chrome）；基线将该脚本关联 V028-24 |
| 严重度 | low |
| 复现命令 | `.venv\Scripts\python.exe scripts\selftest_multimon_chrome.py` |
| 实际输出 | `FAIL  page bar salvages orphan saved pos` + `AssertionError`（scripts\selftest_multimon_chrome.py:122 `assert rect_visible_on_any_screen(w.geometry())`），`通过 2  失败 1` |
| 期望结果 | 该用例应验证“孤儿保存位置可被兜底到工作区”；判定应基于实际承载分页按钮的部件 |
| 退出码 | 1 |
| 证据文件 | logs\v4028\r2_selftest_multimon_chrome.log、根因实验 logs\v4028\probe_page_indicator_rootcause.log（脚本 logs\v4028\probe_page_indicator_rootcause.py） |
| 根因机制 | 用例 settings（scripts\selftest_multimon_chrome.py:112-117）未提供 `desktop_pet` 键 → `src\desktop_pet.py:240-263` 默认 `enabled=True/visible=True/show_page_bubbles=True` → `desktop_pet_hosts_float_bar()` 为 True → `src\ui\page_indicator.py:810-813` 不创建分页按钮 → 部件尺寸 `0x20`、`rect_visible_on_any_screen` 因宽度 0 返回 False |
| 反证（兜底逻辑本身正常） | 实测：`desktop_pet={'enabled':False}` → `buttons=1 size=104x100 geo_visible=True saved={'x':1424,'y':367}`；`desktop_pet={'enabled':True,'show_page_bubbles':False}` → 同上；仅默认（宠物托管浮标栏）时 `buttons=0 size=0x20 geo_visible=False` |
| 归因 | 用例自身（缺前置）为主；「默认由宠物托管浮标栏」是否符合产品预期需产品确认 |

### BUG-07 覆盖口径偏差（低）：V028-24 的脚本映射与实际不符

| 项 | 内容 |
|---|---|
| 用例编号 | V028-24（记事本关闭后重同步） |
| 严重度 | low |
| 复现命令 | `Select-String -Path scripts\selftest_multimon_chrome.py -Pattern 'run\('` |
| 实际输出 | `selftest_multimon_chrome.py` 仅有 3 项检查（work_screen+orphan snap / chrome placement / page bar orphan pos），**无**记事本关闭重同步项 |
| 期望结果 | 基线表 V028-24 应指向真实执行脚本 |
| 退出码 | 不适用（口径问题） |
| 证据文件 | scripts\selftest_multimon_chrome.py:130-132；实际覆盖在 scripts\selftest_recent.py:8219（`notepad close must re-sync public floats the may_show gate left hidden`），该检查在本轮 **通过** |
| 归因 | 基线/覆盖口径 |

### BUG-08 验收口径（低）：A1「P1 基线 46 条」与脚本化计划不一致

| 项 | 内容 |
|---|---|
| 用例编号 | A1 验收要点 |
| 严重度 | low |
| 复现命令 | `.venv\Scripts\python.exe -c "from scripts.full_test_plan import ALL_TEST_CASES as A; print(len(A))"`（→47） |
| 实际输出 | 脚本化计划共 47 条 = 44 条 P1 + 2 条 P2（TC-005、TC-153）+ 1 条 P3（TC-004）；`docs\FULL_TEST_CASES.md` 共 46 行 TC（优先级列 44 P1 / 1 P2 / 1 P3） |
| 期望结果 | A1 的「46 条」需明确指 md 全量行数还是 P1 子集；本次计划运行还执行了 P3 的 TC-004（通过），与基线「排除 TC-004」的表述不符 |
| 退出码 | 不适用（口径问题） |
| 证据文件 | logs\v4028\r2_full_plan_r1.log（用例数 47 / 用例通过 24/47） |
| 归因 | 验收口径 |

### BUG-09 套件基础设施（低）：selftest_all.bat fail-fast 且不使用 venv 解释器

| 项 | 内容 |
|---|---|
| 用例编号 | 套件入口（A1 执行路径） |
| 严重度 | low |
| 复现命令 | `cmd /c scripts\selftest_all.bat`（仓库根目录） |
| 实际输出 | 仅执行第 1 步（`selftest_virtual.py`）即退出：`通过 15  失败 1`，`BATCH_EXIT=1`；后续 11 个脚本与 `cleanup_selftest_artifacts.py` **未执行**（logs\v4028\selftest_all.stdout.log 只到 `1/5`） |
| 期望结果 | 套件应汇总所有步骤退出码后再决定整体退出码，便于一次拿到完整矩阵；且应显式使用 `.venv\Scripts\python.exe`（当前脚本调用裸 `python`，会落到系统解释器 `C:\Users\18141\AppData\Local\Programs\Python\Python312\python.exe`，环境不一致） |
| 退出码 | 1 |
| 证据文件 | scripts\selftest_all.bat（每步 `if errorlevel 1 exit /b 1`；`python scripts\<x>.py`）；logs\v4028\selftest_all.stdout.log |
| 说明 | 本报告改用等价矩阵（逐脚本单独执行并记录退出码）完成任务要求的“逐步记录退出码” |
| 归因 | 套件/测试基础设施 |

## 3. 风险观察项（低，需产品/队长确认口径）

### OBS-01 预发布版本比较策略（BND-A10）

- 实测：`current=4.0.28`，远端 `4.1.0-rc.1` → `status=update-available`、`latest='4.1.0-rc.1'`，比较前剥离 `-rc.1` 后缀。
- 语义上 `4.1.0-rc.1 > 4.0.28` 成立，故不构成“误报升级”；但基线 V028-04 期望「不误报升级」，需产品明确**是否向稳定版用户提示 pre-release**，并据此决定是否加白名单。
- 证据：logs\v4028\r2_boundary.log（BND-A10）。

### OBS-02 提权保存参数内嵌引号未转义（BND-B5）

- 实测：`params='"E:\opencode\DeskTidy\desknote_main.py" --elevated-save "C:\tmp\we"ird.txt"'`，内嵌 `"` 未转义（构造处 `src\elevated_io.py:63-83`）。
- 可达性：Win32 文件名不允许 `"`，实际不可达；属防御性建议。
- 证据：logs\v4028\r2_boundary.log（BND-B5）。

### OBS-03 边界输入不可构造：>260 超长路径（BND-C4 FAIL 的复核结论）

- 探针报 `FAIL BND-C4 超长路径(>260): raised FileNotFoundError: [WinError 206]`，复核确认**抛出点在探针自身的构造代码**（scripts\probe_v4028_boundary.py:421-425 的 `deep.mkdir()` / `write_text()`），`src\shell_clipboard.py:254-283 _normalize_paths` 未被触达。
- 结论：本机未启用长路径策略，该输入**不可构造**（不构成产品缺陷）；如需覆盖，应在启用长路径的机器或用 `\\?\` 前缀构造。
- 证据：logs\v4028\r2_boundary.log（第 25/34 行）、logs\v4028\probe_longpath_rootcause.py。

### OBS-04 探针自身误报：BND-B1（已澄清，非缺陷）

- 探针因 `src\elevated_io.py:9-13` 文档字符串中出现 “powershell” 而误判；实际调用为 `ShellExecuteW(None, 'runas', ...)`（`src\elevated_io.py:44-60`），且断言所需的 `runas` 存在。
- 结论：提权调用方式 **符合** V028-13 期望（ShellExecuteW + runas 动词）。UAC 拒绝阈值 `rc > 32` 亦符合 MSDN（返回值 >32 即成功），实测映射 `{0:False, 5:False, 31:False, 32:False, 33:True, 42:True}` 正确。

## 4. 边界与异常输入实测结论（探针 23 项）

命令：`.venv\Scripts\python.exe scripts\probe_v4028_boundary.py`
证据：logs\v4028\r2_boundary.log（汇总 `checks=23 ok=18 deviation=4 error=1`）

| 检查项 | 输入 | 结论 |
|---|---|---|
| BND-A1 | parse_version 8 组边界输入（v4.0.28 / 4.0.28 / 空串 / None 等） | OK |
| BND-A2 | 无网络（DNS 失败） | OK：`status=error error='<urlopen error getaddrinfo failed>'`，静默失败 |
| BND-A3 | 请求超时 | OK：`status=error error='timed out'` |
| BND-A4 | HTTP 403 rate limit | OK：`error='HTTP 403'`，重试仍 error，`error_cached=False`（错误不写缓存），`inflight_after_worker=False`（单飞复位） |
| BND-A5 | HTTP 500 | OK：`error='HTTP 500'` |
| BND-A6 | 畸形 JSON | OK：`error='invalid JSON from release feed'` |
| BND-A7 | JSON 数组载荷 | OK：`error='unexpected release feed payload'` |
| BND-A8 | `tag_name=null` | OK：`error='release feed missing tag'` |
| BND-A9 | 合法 JSON 但非对象 | OK：`error='unexpected release feed payload'` |
| BND-A10 | 远端 pre-release `4.1.0-rc.1` | DEVIATION → OBS-01（策略待确认） |
| BND-A11 | 当前版本带预发布后缀 `4.0.28-rc.1`/`-beta.3` | OK：`not newer than 4.0.28` 且 `newer than 4.0.27` |
| BND-A12 | `is_newer('4.0.28.','4.0.28')` | DEVIATION → **BUG-04（产品低危）** |
| BND-B1 | runas 动词校验 | DEVIATION → OBS-04（探针误报，实际符合） |
| BND-B2 | UAC 拒绝返回码阈值 | OK：`rc>32` 判失败/成功映射正确 |
| BND-B3 | ShellExecuteW 抛 OSError | OK：吞掉异常返回 False |
| BND-B4 | 含空格/中文路径引用 | OK：`quote_count=4`，参数正确加引号 |
| BND-B5 | 路径内嵌双引号 | DEVIATION → OBS-02（现实不可达） |
| BND-C1 | OLE 空文件列表 | OK：`do_file_ole_drag([])=0`，`_shell_data_object([])=None` |
| BND-C2 | 全部路径不存在 | OK：`_normalize_paths=[]`，`do_file_ole_drag=0` |
| BND-C3 | 同一文件 4 种写法去重 | OK：归一为 1 条路径（大小写/分隔符归一） |
| BND-C4 | >260 超长路径 | FAIL → OBS-03（探针构造不可达，非产品缺陷） |
| BND-C5 | 存在+缺失混合 | OK：仅保留真实路径 |
| BND-C6 | DROPEFFECT→Qt 名称映射 0/1/2/4 | OK |

## 5. 验收要点核对（对照基线第四节）

| 编号 | 要点 | 状态 | 依据 |
|---|---|---|---|
| A1 | P1 基线 ×3 轮零失败 | **未通过** | 单轮即 24/47；失败全部指向 BUG-01/02/03/05/06 与 BUG-08 口径；本轮未跑 3 轮（前置未解除前重复无意义） |
| A2 | V028-01~11 更新检查器自动化全绿 | **通过** | selftest_update_checker.py 退出码 0（通过 12 / 失败 0）+ 探针 A1~A9、A11 |
| A3 | V028-12~15 提权保存人工验证 | **未执行（人工）** | 自动化侧已核对调用方式与拒绝阈值（BND-B1~B5、OBS-02） |
| A4 | V028-16~18 OLE 多文件移交/去重/无泄漏 | **部分通过** | selftest_public_drag.py 退出码 0（V028-16/17）；V028-18 归属 selftest_recent.py，因 BUG-03 未判通过；探针 C1~C3/C5/C6 OK |
| A5 | V028-19~25 交互层级 | **部分通过** | V028-19/20/21/23 通过（public_drag/settings_bugs/overlay_raise 退出码 0）；V028-22 受 BUG-02 阻塞；V028-24 实际在 selftest_recent.py 通过（映射见 BUG-07）；V028-25 人工 |
| A6 | 不修改 docs\FULL_TEST_CASES.md | **通过** | 本轮始终使用 `--no-write-md`；`git status --porcelain` 未列出该文件 |

## 6. 未完成/待补项

1. **依赖就绪后补跑（队长接管安装）**：`selftest_desktop_pet.py`（numpy，必要时 Pillow）与 `selftest_live_e2e.py`（psutil 已装好，仍需 dist）。按队长指示本轮标注为「等待依赖，未复跑」；其中宠物脚本已用 PIL/numpy 影子环境取得一次 `OK`（退出码 0）作为依赖归因证据。
2. **人工用例**：V028-10（四入口）、V028-12~15（提权保存 / UAC 拒否 / payload 顺序）、V028-25（回收站浮标字形）需人工执行。
3. **打包前置**：`dist\` 缺失使 TC-010/TC-005/TC-061 相关与打包类检查无法判定，需先出构建产物或明确「源码模式跳过」。
4. **x3 轮**：前置解除后按 A1 要求执行 `run_full_test_plan_x3.py --rounds 3 --no-write-md`。

## 7. 探测脚本与证据索引

- 边界/异常探针（23 项，可复跑）：`scripts\probe_v4028_boundary.py` → logs\v4028\r2_boundary.log
- 根因实验脚本（均在 logs\v4028\）：
  - `probe_unplug_clamp.py` → probe_unplug_clamp.log（BUG-05 钳制机制）
  - `probe_page_indicator_rootcause.py` → probe_page_indicator_rootcause.log（BUG-06 宠物托管浮标栏）
  - `probe_longpath.py` / `probe_longpath2.py` / `probe_longpath4.py` / `probe_longpath_rootcause.py`（OBS-03 长路径 MAX_PATH）
  - `probe_clamp.py`、`probe_fence_layout.py`、`probe_unplug.py`、`probe_pageind*.py`、`probe_page_indicator*.py`（探索过程留档）
- 套件日志：logs\v4028\r2_selftest_*.log（脚本日志）、logs\v4028\r2_full_plan_r1.log（P1 计划）、logs\v4028\selftest_all.stdout.log（bat 入口）、logs\v4028\r2_batch*.csv（退出码矩阵）、logs\v4028\r3_*.log（依赖影子环境复跑）
- 用例脚本本体：`scripts\selftest_*.py`
- 收尾清理：`.venv\Scripts\python.exe scripts\cleanup_selftest_artifacts.py` → 退出码 0，`Removed 0 selftest artifact(s), closed 0 explorer window(s)`（无残留；logs\v4028 下 51 个证据日志未被清理，已确认保留），日志 logs\v4028\r3_cleanup.log

## 8. 给下游的可执行建议（按优先级）

1. 解除三个环境前置：出打包产物（或显式跳过打包检查）、补 `requirements-test.txt`（numpy/Pillow/psutil）、安装/捆绑 ffmpeg。解除后重跑矩阵，预期 22 条 P1 中至少 19 条转绿（BUG-01/02/03 相关）。
2. 修用例：`selftest_system.py` 改为同时 patch `_target_screen_rect` 或按真实主屏宽度推导期望；`selftest_multimon_chrome.py:112` 增加 `desktop_pet={'enabled': False}`（或按产品约定改为断言宠物托管形态）。
3. 产品侧（低危）：`src\update_checker.py` 的 `parse_version` 忽略空段/尾点，避免畸形 tag 误报升级。
4. 套件侧：`scripts\selftest_all.bat` 改为汇总退出码 + 使用 venv 解释器；基线表修正 V028-24 的脚本映射；统一 A1 的用例计数口径。
