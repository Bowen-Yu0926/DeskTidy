# Module map — DeskTidy / DeskNote

| 系统 | id 前缀 | 前端目录 | app_path | local port | 备注 |
| DeskTidy | DT | native | — | — | Win32 原生桌面整理 |
| DeskNote | DN | native | deskNote.exe | — | 原生 Markdown 笔记 |

| 模块 | 缩写 | 路由 | 菜单 | 后端入口 | 冒烟用例 |
| 桌面资源管理器对标 | SH | — | 桌面/分区/浮动层 | public_host / fence_window / shell_clipboard / app OnDesktopPaste·OnDesktopDropPaths / ole_drag | QA-DT-SH-001 / QA-DT-SH-004 |
| 公共浮层 | PUB | — | 浮动图标框选拖拽 | public_host BeginOleDrag / ole_drag ShellDoDragFiles | QA-DT-PUB-DRAG-001 |
| 桌面分区图标 | FE | — | 分区钉选/拖拽重排/跨区 | fence_window / ole_drag / app TransferPins | QA-DT-FE-001 |
| 桌面整理性能 | PF | — | 分区绘制/拖拽/粘贴热路径 | fence_window / public_host / shell_clipboard / app prune·trim | QA-DT-PF-001 |
| 扩展工具 | TL | — | 计算器 / 桌面待办 / 壁纸 | tools_misc OpenCalculator WallpaperMenu / todo_window / app OpenTodos | QA-DT-TL-001 |
| 截图录屏 | CAP | — | 区域截图 / 录屏 | capture_overlay / screen_record | QA-DT-CAP-001 |
| Mist 提示 | MS | — | 系统提示/确认 | mist_msg / mist_webview / 各 UI MistAlert | QA-DT-MS-001 |
| 核心浮层冒烟 | CORE | — | 搜索/保险库/待办/宠物 | app OpenSearch OpenVault OpenTodos / pet_window | QA-DT-CORE-001 |
| 所见即所得编排 | WY | — | DeskNote 编辑区 | md_preview/wysiwyg_native.html | QA-DN-WY-001 |
| 页签与侧栏 | TB | — | DeskNote 页签/文件树 | notes_window TabLabel NewTab RebuildSidebar | QA-DN-TB-001 |
