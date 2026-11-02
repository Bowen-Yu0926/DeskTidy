# 变更测试说明 — Fences DefView 子窗口落座 — 2026-11-02

## 一、本次改动内容

- `desktop_shell`：对齐 Fences/DeskFrame，分区等 chrome 改为 `SHELLDLL_DefView` 的 `WS_CHILD`（不再 Progman owned popup + HWND_BOTTOM）
- 屏幕坐标经 `SetOverlayScreenPos` / `MoveOverlayScreen` 转父客户区；Peek 仍 Detach + TOPMOST
- `RestoreDesktopChrome` 不做整页 Show；软换座避免闪空
- 落点插入：`InsertPinOrder` 按当前格子顺序插入（manual）

## 二、涉及到改动的业务模块

| 业务模块 | 为何受影响 |
|----------|------------|
| 桌面资源管理器对标 | DefView 落座 / SH-005 契约与运行时冒烟 |
| 桌面分区图标 | fence 附着、拖移、显示保活 |
| 公共浮层 | public_host 同样 Attach 到 DefView |
| 核心浮层冒烟 | pet/dock/page_bar 落座与可见性 |

## 三、基于上述模块生成的测试用例

| 用例编号 | 用例名称 | 测什么 | 文件 |
|----------|----------|--------|------|
| QA-DT-SH-005 | 分区 DefView 子窗口落座 | SoftSeatToDefView / 运行时 parent=DefView | `testcases/DeskTidy/桌面资源管理器对标/QA-DT-SH-005.md` |
| QA-DT-SH-004 | 文件夹↔桌面拖放语义 | 同卷 MOVE / 自拖取消（回归） | `testcases/DeskTidy/桌面资源管理器对标/QA-DT-SH-004.md` |
| QA-DT-SH-001 | 桌面粘贴对标 | 粘贴路径回归 | `testcases/DeskTidy/桌面资源管理器对标/QA-DT-SH-001.md` |
| QA-DT-FE-001 | 分区钉选拖拽 | 分区图标交互回归 | `testcases/DeskTidy/桌面分区图标/QA-DT-FE-001.md` |
| QA-DT-FE-002 | 分区拖拽边界 | 分区拖放边界回归 | `testcases/DeskTidy/桌面分区图标/QA-DT-FE-002.md` |
| QA-DT-PUB-DRAG-001 | 公共浮层多选拖拽 | 浮层 OLE 回归 | `testcases/DeskTidy/公共浮层/QA-DT-PUB-DRAG-001.md` |
| QA-DT-CORE-001 | 核心浮层冒烟 | pet/dock/搜索等冒烟 | `testcases/DeskTidy/核心浮层冒烟/QA-DT-CORE-001.md` |

## 四、执行

```bash
python <HOME>/.cursor/skills/executable-testcases/scripts/run_scope.py --summary testcases/_probe/change-summary.md --aggregate
```
