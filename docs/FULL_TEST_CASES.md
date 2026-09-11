# DeskTidy 全功能测试用例

> 自动生成自 `scripts/full_test_plan.py`。执行三轮自动化：`python scripts/run_full_test_plan_x3.py`

**用例总数**：46  **自动化脚本数**：24

## 执行说明

| 步骤 | 命令 | 说明 |
|------|------|------|
| 1 | `python scripts/run_full_test_plan_x3.py` | 按本用例表跑自动化 ×3 轮 |
| 2 | `python scripts/run_full_test_plan_x3.py --with-live` | 含打包版活体 E2E |
| 3 | `python scripts/full_test_plan.py --write-md` | 更新本文档 |

## 用例清单

| 编号 | 模块 | 用例 | 优先级 | 自动化脚本 |
|------|------|------|--------|------------|
| TC-000 | 基础设施 | 模块导入与 QApplication 冒烟 | P1 | `selftest_smoke.py` |
| TC-001 | 基础设施 | 版本号与默认配置一致性 | P1 | `selftest_system.py` |
| TC-002 | 基础设施 | 单实例锁与退出确认契约 | P1 | `selftest_system.py` |
| TC-003 | 基础设施 | 打包产物 onedir 与 FFmpeg/fd 捆绑 | P1 | `selftest_perf_stress.py` |
| TC-004 | 基础设施 | 性能：DefView 缓存 / overlay 修复 / 热键轮询 | P3 | `selftest_perf_stress.py` |
| TC-005 | 基础设施 | 打包版活体启动冒烟 | P2 | `selftest_live_e2e.py` |
| TC-010 | 桌面分区 | 虚拟钉选 / 移区 / 取消钉选 | P1 | `selftest_virtual.py` |
| TC-011 | 桌面分区 | FenceWidget 构造与图标选中高亮 | P1 | `selftest_smoke.py` |
| TC-012 | 桌面分区 | 一键整理与白名单 | P1 | `selftest_recent.py` |
| TC-013 | 桌面分区 | 分块整理与会话回归 | P1 | `selftest_chunked.py` |
| TC-014 | 桌面分区 | 分区空白右键（Explorer 菜单） | P1 | `selftest_fence_bg_menu.py` |
| TC-015 | 桌面分区 | 分区快捷键与 Alt 缩放过滤 | P1 | `selftest_fence_keys.py` |
| TC-015b | 桌面分区 | 分区外观预设预览与应用 | P1 | `selftest_fence_style.py` |
| TC-016 | 桌面分区 | Win+D 层级 / overlay 栈 / 非抢焦点 | P1 | `selftest_system.py` |
| TC-017 | 桌面分区 | 文件夹门户拖放 | P1 | `selftest_portal.py` |
| TC-018 | 桌面分区 | 系统图标原生命名空间 | P1 | `selftest_namespace_native.py` |
| TC-019 | 桌面分区 | 还原不重复 | P1 | `selftest_smoke.py` |
| TC-020 | 分页 | 分页切换无闪屏 | P1 | `selftest_page_switch_flash.py` |
| TC-021 | 分页 | 布局指纹迁移与屏幕适配 | P1 | `selftest_smoke.py` |
| TC-022 | 分页 | 新建分区默认落在当前页 | P1 | `selftest_system.py` |
| TC-030 | 公共区域 | 公共浮标宿主与布局 | P1 | `selftest_public_host.py` |
| TC-031 | 公共区域 | 公共区拖放（自定义拖放桥） | P1 | `selftest_public_drag.py` |
| TC-032 | 公共区域 | 公共区多选 | P1 | `selftest_public_multiselect.py` |
| TC-033 | 公共区域 | 拖放矩阵（分区/公共/桌面组合） | P1 | `selftest_drop_matrix.py` |
| TC-040 | 布局快照 | 快照保存/预览/应用 API | P1 | `selftest_recent.py` |
| TC-050 | 区域截图 | 截图冻结 UI 与模态对话框挂起 | P1 | `selftest_smoke.py` |
| TC-051 | 区域截图 | 截图热键与贴图契约 | P1 | `selftest_recent.py` |
| TC-060 | 桌面录屏 | 录屏指示器与 REC 浮层 | P1 | `selftest_recording_indicator.py` |
| TC-061 | 桌面录屏 | FFmpeg 捆绑验证 | P1 | `selftest_system.py` |
| TC-070 | 文件搜索 | fd 搜索框与结果列表 | P1 | `selftest_file_search.py` |
| TC-080 | 记事本 | 多标签打开与查找对话框 | P1 | `selftest_recent.py` |
| TC-090 | 会议纪要 | Word 纪要生成路径 | P1 | `selftest_recent.py` |
| TC-100 | 计算器 | 唤起/收起系统计算器 | P1 | `selftest_calculator.py` |
| TC-110 | 桌面待办 | 待办增删勾选与持久化 | P1 | `selftest_todos.py` |
| TC-120 | 桌面宠物 | 宠物资源加载与姿态映射 | P1 | `selftest_desktop_pet.py` |
| TC-121 | 桌面宠物 | 卫衣少年：玩手机/睡觉分动作与脚底锚点 | P1 | `selftest_desktop_pet.py` |
| TC-122 | 桌面宠物 | 丢垃圾动画溶解与即时回收 | P1 | `selftest_desktop_pet.py` |
| TC-123 | 桌面宠物 | 拖文件到宠物与 shaped mask | P1 | `selftest_desktop_pet.py` |
| TC-130 | 壁纸 | 壁纸库应用与还原 API | P1 | `selftest_recent.py` |
| TC-140 | 分页文件夹 | 浮标栏文件夹快捷方式 | P1 | `selftest_recent.py` |
| TC-150 | 设置与备份 | 主题/开机启动/导入导出 | P1 | `selftest_system.py` |
| TC-151 | 设置与备份 | 全局热键注册与注销 | P1 | `selftest_system.py` |
| TC-152 | 设置与备份 | Shell 右键菜单注册 | P1 | `selftest_recent.py` |
| TC-160 | 多显示器 | 多屏 work area 与 chrome | P1 | `selftest_multimon_chrome.py` |
| TC-170 | 综合回归 | 近期功能大回归（白名单/公共/整理/剪贴板等） | P1 | `selftest_recent.py` |
| TC-171 | 综合回归 | 8/12 会话专项回归 | P1 | `selftest_session_aug12.py` |

## 用例明细

### 基础设施

#### TC-000 模块导入与 QApplication 冒烟

- **优先级**：P1
- **自动化**：`selftest_smoke.py`
- **步骤**：导入 app/settings/fence/organizer/win_shell 等核心模块；构造 QApplication
- **预期**：全部 import 成功，无异常

#### TC-001 版本号与默认配置一致性

- **优先级**：P1
- **自动化**：`selftest_system.py`
- **步骤**：读取 VERSION、_version.py、default_settings.json
- **预期**：版本一致；默认配置可 load

#### TC-002 单实例锁与退出确认契约

- **优先级**：P1
- **自动化**：`selftest_system.py`
- **步骤**：检查 instance_lock API 与退出对话框 TOPMOST 契约
- **预期**：API 可调用；退出前先隐藏 overlay

#### TC-003 打包产物 onedir 与 FFmpeg/fd 捆绑

- **优先级**：P1
- **自动化**：`selftest_perf_stress.py`
- **步骤**：检查 dist/DeskTidy 布局、ffmpeg.exe、fd.exe
- **预期**：onedir 结构正确；录屏/搜索依赖存在

#### TC-004 性能：DefView 缓存 / overlay 修复 / 热键轮询

- **优先级**：P3
- **自动化**：`selftest_perf_stress.py`
- **步骤**：压测 EnumWindows、40 overlay repair、菜单节流、鼠标钩子
- **预期**：热路径毫秒级；无回归契约破坏

#### TC-005 打包版活体启动冒烟

- **优先级**：P2
- **自动化**：`selftest_live_e2e.py`
- **步骤**：启动 dist/DeskTidy.exe，检查窗口与 virtual-only 配置
- **预期**：进程存活；organize_mode=virtual；主 HWND 可见

### 桌面分区

#### TC-010 虚拟钉选 / 移区 / 取消钉选

- **优先级**：P1
- **自动化**：`selftest_virtual.py`
- **步骤**：pin 到分区；跨区移动；unpin
- **预期**：互斥钉选；顺序保留；unpin 成功

#### TC-011 FenceWidget 构造与图标选中高亮

- **优先级**：P1
- **自动化**：`selftest_smoke.py`
- **步骤**：离屏构造 FenceWidget；单击选中；sendEvent 不丢选中
- **预期**：控件可构造；选中态可见

#### TC-012 一键整理与白名单

- **优先级**：P1
- **自动化**：`selftest_recent.py`
- **步骤**：dry-run organize；白名单匹配；整理跳过白名单路径
- **预期**：不移动文件；白名单生效

#### TC-013 分块整理与会话回归

- **优先级**：P1
- **自动化**：`selftest_chunked.py`
- **步骤**：chunked organize 与 8/12 会话契约
- **预期**：分块逻辑正确；会话 API 稳定

#### TC-014 分区空白右键（Explorer 菜单）

- **优先级**：P1
- **自动化**：`selftest_fence_bg_menu.py`
- **步骤**：Fence 空白区 shell CreateViewObject 菜单契约
- **预期**：走系统文件夹背景菜单

#### TC-015 分区快捷键与 Alt 缩放过滤

- **优先级**：P1
- **自动化**：`selftest_fence_keys.py`
- **步骤**：热键注册；Fence 不用全局 eventFilter
- **预期**：热键契约正确；缩放注册独立

#### TC-015b 分区外观预设预览与应用

- **优先级**：P1
- **自动化**：`selftest_fence_style.py`
- **步骤**：预设目录；浅色/深色样式生效；设置页预览/应用接线
- **预期**：选预设可预览，应用后写入全部分区

#### TC-016 Win+D 层级 / overlay 栈 / 非抢焦点

- **优先级**：P1
- **自动化**：`selftest_system.py`
- **步骤**：overlay attach；Fence 无 StaysOnBottom；退出恢复图标
- **预期**：层级契约满足；不抢焦点

#### TC-017 文件夹门户拖放

- **优先级**：P1
- **自动化**：`selftest_portal.py`
- **步骤**：门户分区镜像真实文件夹；拖入拖出契约
- **预期**：门户 API 与拖放路径正确

#### TC-018 系统图标原生命名空间

- **优先级**：P1
- **自动化**：`selftest_namespace_native.py`
- **步骤**：namespace native 图标解析
- **预期**：原生图标路径可解析

#### TC-019 还原不重复

- **优先级**：P1
- **自动化**：`selftest_smoke.py`
- **步骤**：restore 跳过已存在项
- **预期**：restored_count 正确

### 分页

#### TC-020 分页切换无闪屏

- **优先级**：P1
- **自动化**：`selftest_page_switch_flash.py`
- **步骤**：切换 current_page；fence relayout
- **预期**：切换后无异常闪烁契约

#### TC-021 布局指纹迁移与屏幕适配

- **优先级**：P1
- **自动化**：`selftest_smoke.py`
- **步骤**：fingerprint；ratio 存储；clamp 越界
- **预期**：多分辨率适配；相对坐标持久化

#### TC-022 新建分区默认落在当前页

- **优先级**：P1
- **自动化**：`selftest_system.py`
- **步骤**：create_fence 与 page 成员关系
- **预期**：新分区归属当前分页

### 公共区域

#### TC-030 公共浮标宿主与布局

- **优先级**：P1
- **自动化**：`selftest_public_host.py`
- **步骤**：public items relayout；hit-test
- **预期**：浮标可布局；宿主 API 正常

#### TC-031 公共区拖放（自定义拖放桥）

- **优先级**：P1
- **自动化**：`selftest_public_drag.py`
- **步骤**：浮标 drag begin/end；pin 同步；OLE 不抢 Explorer
- **预期**：拖放日志契约；pin 与 fence 同步

#### TC-032 公共区多选

- **优先级**：P1
- **自动化**：`selftest_public_multiselect.py`
- **步骤**：多选状态与操作契约
- **预期**：多选逻辑正确

#### TC-033 拖放矩阵（分区/公共/桌面组合）

- **优先级**：P1
- **自动化**：`selftest_drop_matrix.py`
- **步骤**：多场景 mime 与 drop action
- **预期**：矩阵场景全部通过

### 布局快照

#### TC-040 快照保存/预览/应用 API

- **优先级**：P1
- **自动化**：`selftest_recent.py`
- **步骤**：snapshot 读写与 apply
- **预期**：快照 round-trip 正确

### 区域截图

#### TC-050 截图冻结 UI 与模态对话框挂起

- **优先级**：P1
- **自动化**：`selftest_smoke.py`
- **步骤**：prepare_capture_ui / restore_suspended_modals
- **预期**：截图前挂起 ApplicationModal；恢复后对话框回来

#### TC-051 截图热键与贴图契约

- **优先级**：P1
- **自动化**：`selftest_recent.py`
- **步骤**：screenshot / show_screenshot 热键注册
- **预期**：热键可解析；贴图上限契约

### 桌面录屏

#### TC-060 录屏指示器与 REC 浮层

- **优先级**：P1
- **自动化**：`selftest_recording_indicator.py`
- **步骤**：录制中指示器；结束对话框契约
- **预期**：指示器 API 正常

#### TC-061 FFmpeg 捆绑验证

- **优先级**：P1
- **自动化**：`selftest_system.py`
- **步骤**：verify_ffmpeg_bundle h264_mf/ddagrab
- **预期**：编码器与抓取后端可用

### 文件搜索

#### TC-070 fd 搜索框与结果列表

- **优先级**：P1
- **自动化**：`selftest_file_search.py`
- **步骤**：打开搜索；本地/全局模式契约
- **预期**：搜索 UI 与 fd 路径正确

### 记事本

#### TC-080 多标签打开与查找对话框

- **优先级**：P1
- **自动化**：`selftest_recent.py`
- **步骤**：notepad 窗口；find dialog；扩展路径
- **预期**：记事本模块可构造；查找 API 存在

### 会议纪要

#### TC-090 Word 纪要生成路径

- **优先级**：P1
- **自动化**：`selftest_recent.py`
- **步骤**：minutes docx 输出目录契约
- **预期**：纪要模块与路径配置正确

### 计算器

#### TC-100 唤起/收起系统计算器

- **优先级**：P1
- **自动化**：`selftest_calculator.py`
- **步骤**：热键 Ctrl+Alt+C；toggle 逻辑
- **预期**：计算器开关契约通过

### 桌面待办

#### TC-110 待办增删勾选与持久化

- **优先级**：P1
- **自动化**：`selftest_todos.py`
- **步骤**：添加/完成/删除 todos.json
- **预期**：待办 round-trip 正确

### 桌面宠物

#### TC-120 宠物资源加载与姿态映射

- **优先级**：P1
- **自动化**：`selftest_desktop_pet.py`
- **步骤**：hoodie/voyage/duoduo poses；wait/sleep/trash
- **预期**：素材齐全；pose_for_state 正确

#### TC-121 卫衣少年：玩手机/睡觉分动作与脚底锚点

- **优先级**：P1
- **自动化**：`selftest_desktop_pet.py`
- **步骤**：单帧 wait 脚底对齐；rest 交替；Zzz 位置
- **预期**：无裁切上移；菜单项正确

#### TC-122 丢垃圾动画溶解与即时回收

- **优先级**：P1
- **自动化**：`selftest_desktop_pet.py`
- **步骤**：trash 8 帧溶解；delete_to_trash 在动画前
- **预期**：连贯过渡；文件进回收站

#### TC-123 拖文件到宠物与 shaped mask

- **优先级**：P1
- **自动化**：`selftest_desktop_pet.py`
- **步骤**：accepts_trash_at；deliver_paths_to_pet_trash
- **预期**：仅 sprite 区接收；回收站删除

### 壁纸

#### TC-130 壁纸库应用与还原 API

- **优先级**：P1
- **自动化**：`selftest_recent.py`
- **步骤**：wallpaper 扩展配置与 apply
- **预期**：壁纸模块契约存在

### 分页文件夹

#### TC-140 浮标栏文件夹快捷方式

- **优先级**：P1
- **自动化**：`selftest_recent.py`
- **步骤**：添加/删除/打开文件夹快捷方式
- **预期**：快捷方式列表可读写

### 设置与备份

#### TC-150 主题/开机启动/导入导出

- **优先级**：P1
- **自动化**：`selftest_system.py`
- **步骤**：settings load/save；配置迁移
- **预期**：设置 round-trip；路径在用户目录

#### TC-151 全局热键注册与注销

- **优先级**：P1
- **自动化**：`selftest_system.py`
- **步骤**：HotkeyManager register/unregister
- **预期**：热键表完整；轮询轻量

#### TC-152 Shell 右键菜单注册

- **优先级**：P1
- **自动化**：`selftest_recent.py`
- **步骤**：desktop_right_click_menu 注册表契约
- **预期**：注册表项可清理

### 多显示器

#### TC-160 多屏 work area 与 chrome

- **优先级**：P1
- **自动化**：`selftest_multimon_chrome.py`
- **步骤**：multimon 几何；page bubble 布局
- **预期**：多屏契约通过

### 综合回归

#### TC-170 近期功能大回归（白名单/公共/整理/剪贴板等）

- **优先级**：P1
- **自动化**：`selftest_recent.py`
- **步骤**：selftest_recent 全量断言
- **预期**：无回归失败

#### TC-171 8/12 会话专项回归

- **优先级**：P1
- **自动化**：`selftest_session_aug12.py`
- **步骤**：session_aug12 契约
- **预期**：会话 API 稳定

## 人工抽测建议（自动化未覆盖）

| 场景 | 操作 | 预期 |
|------|------|------|
| 长时间挂机 | 运行 2h+ | 内存无持续增长；托盘响应正常 |
| Win+D | 连按 Win+D 进出 | 分区/浮标恢复正确 |
| 多显示器 | 拔插/切换主屏 | 布局 clamp 正确 |
| 真实录屏 | F3 录 10s 后结束 | 生成可播放 mp4 |
| 真实截图贴图 | F1 框选 + F2 | 贴图可拖/右键另存 |
