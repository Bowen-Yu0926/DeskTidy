---
id: QA-DT-FE-003
title: 右键移动为分页再选分区
module: DeskTidy/桌面分区图标
priority: P0
type: code-test
env:
  profile: local
  role: 本机用户
source: fence_window ShowIconMenu ShowMoveToPageFenceMenu / shell_context_menu TrackMoveToPageThenFenceMenu
requirement: 图标右键只有一个「移动到分页」入口，级联先选分页再选该页分区；不再并列「移动到分区」
traces: [AC-FE-10]
design: [ep, reverse]
version: 1
status: active
automation: python -m pytest "testcases/DeskTidy/桌面分区图标/test_QA_DT_FE_003.py" -q
last_run: pass
last_run_at: 2026-11-03
---
## 测什么

分区图标右键：移动入口只有「移动到分页」，展开后先选分页，再选该页下的目标分区。

---

## 原逻辑

1. **两个平级项** — 「移动到分页」与「移动到分区」并列
2. **分区列表无页** — 移动到分区不区分分页

---

## 改后逻辑

1. **单一入口** — 仅「移动到分页」
2. **级联选分区** — 分页弹出菜单下再列该页分区

---

## 逻辑改动后涉及测试范围

1. **菜单单一入口**（→ `### 1.`）
2. **级联实现**（→ `### 2.`）
3. **共享级联助手**（→ `### 3.`）

---

## 测试步骤与预期

### 1. 菜单单一入口

1. **检查分区图标右键附加项**  
   > 预期：含移动到分页；不含并列的移动到分区  
   > 实测情况：通过

### 2. 级联实现

1. **检查移动菜单实现**  
   > 预期：调用分页→分区级联，并回调整到目标分区  
   > 实测情况：通过

### 3. 共享级联助手

1. **检查级联菜单构建**  
   > 预期：按页弹出子菜单；空页有占位  
   > 实测情况：通过

---

## 最终结果

- **结果**：通过
- **时间**：2026-11-03
- **说明**：pytest FE-003 + PUB-002 共 8 项通过；Release 已重启
