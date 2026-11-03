---
id: QA-DT-PUB-003
title: 公共浮层框选批量拖拽契约
module: DeskTidy/公共浮层
priority: P0
type: code-test
env:
  profile: local
  role: 本机用户
source: public_host BeginOleDrag / ole_drag ShellDoDragFiles cancel_desktop_self_drop
requirement: 框选多选后 shell 拖拽；松手回桌面/浮层取消；取消不重写整批坐标
traces: [AC-PUB-20, AC-PUB-21]
design: [ep, reverse]
version: 1
status: active
automation: python -m pytest "testcases/DeskTidy/公共浮层/test_QA_DT_PUB_003.py" -q
last_run: PASS
last_run_at: 2026-11-03 08:55
---
## 测什么

公共浮层框选后应走系统拖拽；拖回桌面或浮层自身时取消；取消后不把图标叠到一点。

---

## 原逻辑

1. **自拖落桌面** — 同目录 CF_HDROP 触发「源相同」或图标乱叠
2. **取消后重写坐标** — 松手取消后整批 placement 被改写到鼠标附近

---

## 改后逻辑

1. **自拖取消** — 公共浮层发起拖拽时开启「取消桌面自落」
2. **落回浮层当取消** — 松手点仍在浮层/框选带上视为取消
3. **不在拖后强写几何** — 注释约束：DoDragDrop 后不激进改写 placement

---

## 逻辑改动后涉及测试范围

1. **自拖取消开关** — BeginOleDrag 传取消桌面自落（→ `### 1.`）
2. **落回浮层取消** — 命中浮层窗口则 dropped=false（→ `### 2.`）
3. **逆向：分区拖出** — 分区内文件拖拽不强制开桌面自落取消（→ `### 3.`）

---

## 测试步骤与预期

### 1. 自拖取消开关

1. **检查公共浮层拖拽入口**  
   > 预期：调用系统拖文件且开启取消桌面自落  
   > 实测情况：符合预期

### 2. 落回浮层取消

1. **检查松手命中浮层时**  
   > 预期：视为未成功投放，不按落地处理  
   > 实测情况：符合预期

### 3. 逆向：分区拖出

1. **检查分区拖文件**  
   > 预期：分区路径不默认开「取消桌面自落」  
   > 实测情况：符合预期

---

## 最终结果

- **结果**：PASS
- **时间**：2026-11-03 08:55
- **说明**：automation exit 0；…                                                                      [100%] | 3 passed in 0.03s

