---
trigger: always_on
---

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
