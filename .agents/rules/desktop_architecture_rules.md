---
trigger: always_on
---

# 桌面系统架构与视窗层级规范 (Desktop Band & Taskbar Architecture Rules)

1. **桌面视窗层级永不置顶 (Never WS_EX_TOPMOST on Desktop Overlays)**:
   - 桌面增强覆盖层（包括分区 FenceWindow、公共浮层 PublicHost、桌面宠物 PetWindow、Dock、桌面挂件等）归属于桌面层（Desktop Band），生命周期与层级必须通过 `SoftSeatToOwner` 挂载给 `WorkerW` / `Progman` 宿主。
   - 绝对不允许赋予 `WS_EX_TOPMOST` 或在切屏/恢复时调用 `HWND_TOPMOST`。严禁与系统任务栏（`Shell_TrayWnd`，系统置顶层）争抢 Z-Order，确保任务栏及其图标、开始菜单永远处于桌面覆盖层之上且完全可点击。

2. **物理几何严格受限于工作区 (Strict WorkArea Boundary Invariant)**:
   - 任何桌面覆盖层（特别是公共浮层容器 `PublicHost`）的窗口矩形必须严格限制在当前显示器的有效工作区（`work` area，即避开任务栏后的物理坐标）之内。
   - 绝不允许因 padding 或浮层边距计算导致窗口边界侵入或延伸至任务栏区域（`y >= work.bottom`）。

3. **非图标空白区域安全穿透 (Hit-Test Passthrough Safety)**:
   - 覆盖层窗口在响应 `WM_NCHITTEST` 时，若鼠标坐标落在工作区之外（如任务栏区域）或未命中有效图标/交互元素，必须向底层穿透（返回 `HTTRANSPARENT`），坚决禁止截断或拦截系统任务栏、系统托盘以及底层窗口的鼠标点击事件。
