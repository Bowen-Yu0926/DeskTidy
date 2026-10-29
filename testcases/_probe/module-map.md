# Module map — DeskTidy

| 系统 | id 前缀 | 前端目录 | app_path | local port | 备注 |
| DeskTidy | DT | native | — | — | Win32 原生桌面整理 |

| 模块 | 缩写 | 路由 | 菜单 | 后端入口 | 冒烟用例 |
| 桌面资源管理器对标 | SH | — | 桌面/分区/浮动层 | public_host / fence_window / shell_clipboard / app OnDesktopPaste | QA-DT-SH-001 |
| 桌面分区图标 | FE | — | 分区钉选/拖拽重排/跨区 | fence_window / ole_drag / app TransferPins | QA-DT-FE-001 |
| 桌面整理性能 | PF | — | 分区绘制/拖拽/粘贴热路径 | fence_window / public_host / shell_clipboard | QA-DT-PF-001 |
| 扩展工具 | TL | — | 计算器 / 桌面待办 / 壁纸 | tools_misc OpenCalculator WallpaperMenu / todo_window / app OpenTodos | QA-DT-TL-001 |
