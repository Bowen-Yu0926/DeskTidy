---
id: QA-DT-SH-004
title: 文件夹↔桌面与拖到其他软件对齐 Fences/Explorer
module: DeskTidy/桌面资源管理器对标
priority: P0
type: code-test
env:
  profile: local
  role: 本机用户
source: PathDropTarget::ResolveFileEffect / OnDesktopDropPaths / ShellDoDragFiles / CreateShellDataObject
requirement: 整理模式下跨窗口拖放效果与落地应与资源管理器/Fences 一致
traces: [AC-SH-10, AC-SH-11, AC-SH-12]
design: [ep, decision, reverse]
version: 1
status: active
automation: python -m pytest "testcases/DeskTidy/桌面资源管理器对标/test_QA_DT_SH_004.py" -q
last_run: PASS
last_run_at: 2026-11-02 13:31
---
## 测什么

1. **文件夹 → 桌面**：同卷默认移动；Ctrl 复制；已在桌面的源忽略（自拖）
2. **桌面 → 文件夹 / 其他软件**：浮动层发起 shell `IDataObject` 拖拽；松手在桌面/公共区取消 OLE，不自拖同目录
3. **普通分区钉选**：对外回报 COPY，不因拖入分区删掉源文件

---

## 原逻辑

1. **进桌面** — `OnDesktopDropPaths` 一律 `move=false`，同卷也只复制；`PathDropTarget::Drop` 恒报 `DROPEFFECT_COPY`
2. **出自拖** — 浮动层拖回桌面仍可能 CF_HDROP 落地，闪一下或叠图标
3. **进普通分区** — 若误报 MOVE，资源管理器会删源，与「只钉选」产品语义冲突

---

## 改后逻辑

1. **进桌面** — Physical + 同卷 MOVE / Ctrl COPY；自拖跳过；OLE 效果与落地一致
2. **拖出** — `ShellDoDragFiles(..., cancel_desktop_self_drop=true)`；目标为文件夹/微信等照常 DROP
3. **普通分区** — `FileDropSemantics::VirtualPin` 恒 COPY；门户走 Physical

---

## 测试步骤与预期

### 1. 源码契约

1. **核对拖放效果矩阵与桌面落地入口**  
   > 预期：存在 `FileDropSemantics`、`ResolveFileEffect`、`cancel_desktop_self_drop`、`OnDesktopDropPaths` 同卷/`VK_CONTROL` 分支、`CreateShellDataObject`/`SHCreateItem`  
   > 实测情况：符合预期（pytest）

### 2. 同卷移动语义（镜像落地）

1. **临时目录建文件，按「同卷移动到桌面」语义执行**  
   > 预期：桌面出现该文件；源路径不再存在  
   > 实测情况：符合预期（pytest）

2. **清理落地文件**  
   > 预期：无残留  
   > 实测情况：符合预期（pytest）

### 3. 逆向：桌面自拖不落地

1. **源已在桌面目录时跳过 MoveOrCopy**  
   > 预期：不新增「(2)」副本  
   > 实测情况：符合预期（pytest）

### 4. 手工冒烟（可选，记结果）

| # | 步骤 | 期望 |
|---|------|------|
| A | 资源管理器文件夹拖文件到整理桌面空白 | 同盘移动；浮动层出现 |
| B | 按住 Ctrl 再拖进桌面 | 复制；源仍在 |
| C | 浮动图标拖进已打开的文件夹窗口 | 文件离开桌面进入该夹 |
| D | 浮动图标拖到记事本/微信等 | 对方能收；桌面文件仍在（COPY） |
| E | 文件夹文件拖进普通分区 | 钉入；源文件仍在原文件夹 |

---

## 最终结果

- **结果**：PASS
- **时间**：2026-11-02 13:31
- **说明**：automation exit 0；…                                                                   [100%] | 6 passed in 0.10s

