# 变更测试说明 — 桌面刷新热路径性能 — 2026-11-03

## 一、本次改动内容

- **同身份跳过分层绘制**：分区图标成员未变且图层未脏时，SetItems 不再强制 UpdateLayeredWindow
- **门户排除用内存偏好**：刷新不再对每个门户反复读 `app_prefs.json`
- **非快捷方式跳过 DISPLAYNAME**：桌外钉选补扫不再对普通文件调 SHGetFileInfo
- **图标格距短缓存**：`QueryIconGridMetrics` 750ms 复用；强制刷新前作废
- **内容刷新按需二次绘制**：仅脏层 / 埋于桌面图标层下时再 RefreshLayer

## 二、涉及到改动的业务模块

| 业务模块 | 为何受影响 |
|----------|------------|
| 桌面整理性能 | 刷新/绘制/格距热路径契约与回归 |
| 桌面分区图标 | SetItems 同身份跳过 Present |
| 公共浮层 | 间接受益于格距缓存 |

## 三、基于上述模块生成的测试用例

| 用例编号 | 用例名称 | 测什么 | 文件 |
|----------|----------|--------|------|
| QA-DT-PF-001 | 拖拽粘贴热路径 | 无扫盘契约回归 | `testcases/DeskTidy/桌面整理性能/QA-DT-PF-001.md` |
| QA-DT-PF-002 | 图标复用与节流 | 复用/粘滞/录屏/trim | `testcases/DeskTidy/桌面整理性能/QA-DT-PF-002.md` |
| QA-DT-PF-003 | 缓冲复用与扫描 | Present 缓冲 / scan TTL | `testcases/DeskTidy/桌面整理性能/QA-DT-PF-003.md` |
| QA-DT-PF-004 | 同身份跳过与缓存 | 本次优化契约 | `testcases/DeskTidy/桌面整理性能/QA-DT-PF-004.md` |

## 四、执行

```bash
python <HOME>/.cursor/skills/executable-testcases/scripts/run_scope.py --summary testcases/_probe/change-summary.md --aggregate
```
