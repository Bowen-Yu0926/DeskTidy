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

# 桌面系统架构与视窗层级规范 (Desktop Band & Taskbar Architecture Rules)

1. **桌面视窗层级永不置顶 (Never WS_EX_TOPMOST on Desktop Overlays)**:
   - 桌面增强覆盖层（包括分区 FenceWindow、公共浮层 PublicHost、桌面宠物 PetWindow、Dock、桌面挂件等）归属于桌面层（Desktop Band），生命周期与层级必须通过 `SoftSeatToOwner` 挂载给 `WorkerW` / `Progman` 宿主。
   - 绝对不允许赋予 `WS_EX_TOPMOST` 或在切屏/恢复时调用 `HWND_TOPMOST`。严禁与系统任务栏（`Shell_TrayWnd`，系统置顶层）争抢 Z-Order，确保任务栏及其图标、开始菜单永远处于桌面覆盖层之上且完全可点击。
2. **物理几何严格受限于工作区 (Strict WorkArea Boundary Invariant)**:
   - 任何桌面覆盖层（特别是公共浮层容器 `PublicHost`）的窗口矩形必须严格限制在当前显示器的有效工作区（`work` area，即避开任务栏后的物理坐标）之内。
   - 绝不允许因 padding 或浮层边距计算导致窗口边界侵入或延伸至任务栏区域（`y >= work.bottom`）。
3. **非图标空白区域安全穿透 (Hit-Test Passthrough Safety)**:
   - 覆盖层窗口在响应 `WM_NCHITTEST` 时，若鼠标坐标落在工作区之外（如任务栏区域）或未命中有效图标/交互元素，必须向底层穿透（返回 `HTTRANSPARENT`），坚决禁止截断或拦截系统任务栏、系统托盘以及底层窗口的鼠标点击事件。
