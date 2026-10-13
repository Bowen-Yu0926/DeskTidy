# v4.0.28 测试基线与新增用例

> 依据：docs/FULL_TEST_CASES.md（46 条，冻结不改）、README.md v4.0.28、git 最近 19 提交。

## 一、本轮范围（冻结）

| 范围 | 内容 |
|------|------|
| P1 全量 | docs/FULL_TEST_CASES.md 中所有 P1 条目（TC-000~TC-171），执行 `python scripts/run_full_test_plan_x3.py` ×3 |
| P2 相关 | TC-005 打包活体冒烟；以及本次新增用例 V028-01~V028-22 |
| 排除 | TC-004 性能压测（P3）、仅历史功能且无提交改动的 P3 条目 |

## 二、基线用例执行矩阵

| 用例编号 | 模块 | 步骤 | 期望 | 异常或边界输入 | 执行脚本 |
|---|---|---|---|---|---|
| TC-000~TC-004 | 基础设施 | 跑冒烟/系统/性能脚本 | import 成功、版本 4.0.28 一致、锁契约成立 | 版本文件不一致应失败 | selftest_smoke.py / selftest_system.py / selftest_perf_stress.py |
| TC-010~TC-019 | 桌面分区 | 钉选/整理/右键/快捷键/门户/命名空间 | 功能与基线一致 | 空分区、重复钉选 | selftest_virtual.py、selftest_chunked.py、selftest_recent.py 等 |
| TC-020~TC-022 | 分页 | 切页无闪屏、布局迁移、新建默认当前页 | 无闪屏、迁移成功 | 异分辨率屏幕切换 | selftest_page_switch_flash.py / selftest_smoke.py |
| TC-030~TC-033 | 公共区域 | 宿主布局、拖放、多选、拖放矩阵 | 全部落位正确 | 同名文件、跨屏拖放 | selftest_public_host.py / selftest_public_drag.py / selftest_drop_matrix.py |
| TC-040/050/051/060/061 | 快照/截图/录屏 | API 与热键契约 | 与基线一致 | 模态下热键 | selftest_recent.py / selftest_smoke.py / selftest_recording_indicator.py |
| TC-070/080/090/100/110 | 搜索/记事本/纪要/计算器/待办 | 各模块主流程 | 与基线一致 | 空结果、大文件 | selftest_file_search.py 等 |
| TC-120~TC-123 | 桌面宠物 | 资源加载、卫衣动作、丢垃圾、拖文件 | 姿态映射正确 | 动画帧缺失 | selftest_desktop_pet.py |
| TC-130/140/150/151/152 | 壁纸/分页文件夹/设置/热键/右键菜单 | 主流程 | 与基线一致 | 配置损坏 JSON | selftest_system.py / selftest_recent.py |
| TC-160/170/171 | 多屏/综合回归 | 多屏 work area、大回归、8/12 会话 | 无回归 | 拔插显示器 | selftest_multimon_chrome.py / selftest_recent.py / selftest_session_aug12.py |
| TC-005 | 打包活体 | 启动 dist/DeskTidy.exe | 进程存活、organize_mode=virtual | 缺少 FFmpeg/fd | selftest_live_e2e.py |

## 三、新增功能用例（v4.0.26 → v4.0.28）

### A. 更新检查器 src/update_checker.py（840d7e8, 74bc237）

| 用例编号 | 模块 | 步骤 | 期望 | 异常或边界输入 | 执行脚本 |
|---|---|---|---|---|---|
| V028-01 | 更新检查器/版本解析 | 调用 parse_version 解析 tag 名 | 正确解析 x.y.z | "v4.0.28"、"4.0.28"、空串、None | selftest_update_checker.py |
| V028-02 | 更新检查器/Release 检查 | 模拟 GitHub API 返回 release JSON | 解析 tag_name/资产链接，判定有新版本 | 缺 tag_name、缺 assets、非法 JSON | selftest_update_checker.py |
| V028-03 | 更新检查器/已是最新 | 当前版本 == 最新版本 | 返回 up_to_date，不弹提示 | 本地版本高于远端 | selftest_update_checker.py |
| V028-04 | 更新检查器/pre-release 比较 | 远端最新为 pre-release | 与稳定版按语义版本正确比较，不误报升级 | "4.1.0-rc.1" vs "4.0.28" | selftest_update_checker.py |
| V028-05 | 更新检查器/API 配额 | 返回 403 rate limit | 静默降级，不弹窗、不刷屏 | 403+body 含 message 限流文案 | selftest_update_checker.py |
| V028-06 | 更新检查器/网络异常 | 超时或连接失败 | 静默失败，无阻塞 UI | timeout、DNS 失败 | selftest_update_checker.py |
| V028-07 | 更新检查器/单飞与泄漏 | 连续触发检查 | 只启动一个线程；线程启动失败时 inflight 复位 | Thread.start 抛异常 | selftest_update_checker.py |
| V028-08 | 更新检查器/缓存 | 自动检查命中缓存；手动检查强制实时 | 自动用缓存、手动绕缓存 | 缓存过期时间边界 | selftest_update_checker.py |
| V028-09 | 更新检查器/死父窗口 | 父窗口已销毁时 ask_yes_no | 不抛异常，安全返回 | 父 HWND 无效 | selftest_update_checker.py |
| V028-10 | 更新检查器/入口 | 设置页、托盘菜单、帮助菜单、关于对话框 | 四处入口均触发检查并正确展示结果 | 入口缺失需失败 | 人工 |
| V028-11 | 更新检查器/默认配置 | 读取默认设置键（enabled/interval/check_time） | 键齐全且默认值正确 | 配置缺键回退默认 | selftest_update_checker.py |

### B. 提权保存 src/elevated_io.py（b202c7f, 5c6da5a, fec4ef8）

| 用例编号 | 模块 | 步骤 | 期望 | 异常或边界输入 | 执行脚本 |
|---|---|---|---|---|---|
| V028-12 | 提权保存/hosts 写入 | 触发受保护文件保存（hosts） | 先应用提权写入，再做互斥检查，成功返回 | hosts 被其他程序占用 | 人工 |
| V028-13 | 提权保存/ShellExecuteW | 校验以 ShellExecuteW(runas) 调用，非 powershell | 参数含 "runas"，动词正确 | 路径含空格/中文 | 人工 |
| V028-14 | 提权保存/UAC 拒绝 | 在 UAC 弹窗点「否」 | 丢弃 payload，不写半截文件，原文件不变 | 返回码拒绝分支 | 人工 |
| V028-15 | 提权保存/payload 顺序 | 提权写入先于互斥检查执行 | 顺序不得颠倒（避免死锁） | 互斥锁被占 | 人工 |

### C. OLE 多文件拖拽 src/ole_file_drag.py（34afa69）

| 用例编号 | 模块 | 步骤 | 期望 | 异常或边界输入 | 执行脚本 |
|---|---|---|---|---|---|
| V028-16 | OLE 拖拽/多文件移交 | 从外部拖入 ≥2 个文件到分区 | 全部文件一次移交成功，无丢失 | 含中文名/含空格路径 | selftest_public_drag.py |
| V028-17 | OLE 拖拽/重复写入 | 同一文件在同一次 drop 中出现两次 | 只写入一次，无双写 | 重复路径/大小写差异 | selftest_public_drag.py |
| V028-18 | OLE 拖拽/泄漏防护 | 反复拖放与取消拖放 | 会话/隐藏窗口无累积泄漏 | 拖放中途 Esc 取消 | selftest_recent.py |

### D. 交互与层级修复（bad0286, 604b687, 79fc004, 9ba2434, 49a26df, b1bed11, a8cd44c）

| 用例编号 | 模块 | 步骤 | 期望 | 异常或边界输入 | 执行脚本 |
|---|---|---|---|---|---|
| V028-19 | 桌面分区/拖到回收站 | 将文件/分区拖到回收站浮标 | 进入回收站流程，可还原 | 跨分区拖回收站 | selftest_public_drag.py |
| V028-20 | 桌面分区/宽面板移动 | 移动宽度较大的分区 fence | 移动恢复可用，不卡死 | 面板宽于分区 | selftest_settings_bugs.py |
| V028-21 | 设置/竞态 | 连续快速切换设置项（开关主题/热键） | 设置一致落盘，无崩溃/串值 | 并发写入配置 | selftest_settings_bugs.py |
| V028-22 | 桌面宠物/分页芯片 | 宠物等/睡姿态下分页芯片布局 | 芯片不重叠、不换行抖动 | 姿态变宽 | selftest_desktop_pet.py |
| V028-23 | 层级/F1 与分页热键 | 在其他应用前台按 F1/分页热键 | 桌面 chrome 不被抬到该应用之上 | 外部应用全屏 | selftest_overlay_raise.py |
| V028-24 | 记事本/关闭后重同步 | 打开记事本后关闭 | 公共浮层重新同步，被前景遮挡的图标重新出现 | 前台为非 DeskTidy 应用 | selftest_recent.py（BUG-07 修正映射：原误记为 selftest_multimon_chrome.py） |
| V028-25 | 命名空间图标/回收站浮标 | 查看回收站浮标 | 不绘制空白文档字形（b1bed11） | 多命名空间图标 | 人工 |

## 四、验收要点

| 编号 | 要点 |
|---|---|
| A1 | P1 基线 ×3 轮零失败（BUG-08 口径修正：脚本化计划共 47 条 = 44 P1 + 2 P2（TC-005、TC-153）+ 1 P3（TC-004）；docs/FULL_TEST_CASES.md 共 46 行 TC） |
| A2 | V028-01~11（更新检查器）自动化全绿，含 pre-release、配额、单飞、缓存、死父窗口 |
| A3 | V028-12~15（提权保存）人工验证：runas 调用、UAC 拒绝丢弃 payload、写入先于互斥 |
| A4 | V028-16~18（OLE）多文件全量移交、无重复写入、无泄漏 |
| A5 | V028-19~25（交互层级）拖到回收站/宽面板移动/设置竞态/芯片不重叠/热键不抬 chrome/记事本关闭重同步 |
| A6 | 不修改 docs/FULL_TEST_CASES.md |