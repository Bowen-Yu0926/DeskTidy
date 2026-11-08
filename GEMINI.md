# Antigravity 语言与交互规范 (Chinese Language Rules)

- **交流语言**：始终使用简体中文与用户进行沟通、任务分析、方案设计与结果汇报。
- **代码文档**：新编写的代码注释、文档说明（docstrings）、README 及说明文档优先使用简体中文。
- **任务规划**：所有执行计划、步骤分解（Plan）、风险提示均使用清晰明了的简体中文。
- **Git 规范**：生成的 Git commit 信息和 PR 说明应优先使用中文或中英对照。

# Agent 核心开发规范 (Core Development Rules)

1. **编码前三思**：不确定就提问不瞎猜，有歧义列多种可能，有更简单方案主动说明。
2. **简单至上**：只用最少代码解决问题，不额外加需求外的功能，尽量精简。
3. **手术改变**：只改必须改动的代码，不动原本没问题的其他代码。
4. **目标驱动执行**：明确验证标准，完成后做校验，拒绝模糊交付。
5. **进程生命周期闭环**：每次因编译、构建或排查杀死程序后，测试验证完毕必须通过 `scripts/launch_desktidy.ps1` 重新启动 DeskTidy（独立脱离终端 Job Object 挂载至用户交互桌面），确保用户的桌面环境与程序始终处于可用运行状态。

# 桌面系统架构与视窗层级规范 (Desktop Band & Taskbar Architecture Rules)

1. **桌面视窗层级永不置顶 (Never WS_EX_TOPMOST on Desktop Overlays)**:
   - 桌面增强覆盖层（包括分区 FenceWindow、公共浮层 PublicHost、桌面宠物 PetWindow、Dock、桌面挂件等）归属于桌面层（Desktop Band），生命周期与层级必须通过 `SoftSeatToOwner` 挂载给 `WorkerW` / `Progman` 宿主。
   - 绝对不允许赋予 `WS_EX_TOPMOST` 或在切屏/恢复时调用 `HWND_TOPMOST`。严禁与系统任务栏（`Shell_TrayWnd`，系统置顶层）争抢 Z-Order，确保任务栏及其图标、开始菜单永远处于桌面覆盖层之上且完全可点击。
2. **物理几何严格受限于工作区 (Strict WorkArea Boundary Invariant)**:
   - 任何桌面覆盖层（特别是公共浮层容器 `PublicHost`）的窗口矩形必须严格限制在当前显示器的有效工作区（`work` area，即避开任务栏后的物理坐标）之内。
   - 绝不允许因 padding 或浮层边距计算导致窗口边界侵入或延伸至任务栏区域（`y >= work.bottom`）。
3. **非图标空白区域安全穿透 (Hit-Test Passthrough Safety)**:
   - 覆盖层窗口在响应 `WM_NCHITTEST` 时，若鼠标坐标落在工作区之外（如任务栏区域）或未命中有效图标/交互元素，必须向底层穿透（返回 `HTTRANSPARENT`），坚决禁止截断或拦截系统任务栏、系统托盘以及底层窗口的鼠标点击事件。

# 操作系统兼容性规范 (Windows 10 / Windows 11 Compatibility Rules)

1. **双版本全功能兼容 (Full Win10 & Win11 Parity)**:
   - DeskTidy 所有核心与扩展功能（桌面分区、公共浮层、桌面整理、快捷键、双击切屏、壁纸守护、截图/便签等）必须同时无条件兼容 Windows 10（1809~22H2）和 Windows 11（21H2~24H2）。
   - 严禁依赖特定版本的私有未公开 API；任何系统交互必须优先考虑跨版本一致性与鲁棒性。
2. **桌面 Shell 与任务栏架构适配 (Dual-Shell Architecture Adaptation)**:
   - **桌面宿主双态适配**：兼顾 `Progman` 与 `WorkerW` 双重宿主探测与挂载，完美兼容动态壁纸、幻灯片壁纸及 DWM 桌面重组；
   - **任务栏架构差异兼容**：
     - Windows 10：传统任务栏（`Shell_TrayWnd` -> `MSTaskListWClass`），支持四向停靠与小图标模式；
     - Windows 11：现代 XAML/岛式任务栏（`Shell_TrayWnd` -> `XamlExplorerHostIslandWindow` / `DesktopWindowContentBridge`），默认居中；
     - 两代系统下均必须保证：桌面覆盖层严格在桌面层（`HWND_NOTOPMOST`），绝对不与任务栏争抢置顶层级；物理几何受限于当前显示器可用工作区（WorkArea）；非图标空白区无条件透传鼠标事件，确保两代系统的开始菜单与任务栏 100% 正常响应点击。
3. **分层绘制与 DPI 感知规范 (DPI & Layered Surface Compatibility)**:
   - 全面启用 Per-Monitor DPI Aware V2，统一使用跨版本稳定的 32bpp GDI+ 分层绘制（ULW_ALPHA），杜绝特定系统版本下的窗口黑边、模糊或贴图失效。
