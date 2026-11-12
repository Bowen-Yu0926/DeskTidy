---
id: QA-DT-ORG-001
title: 一键整理跳过已钉选；重新整理可换区
module: DeskTidy/桌面整理
priority: P0
type: code-test
env:
  profile: local
  role: 本机用户
source: organize ShouldSkipVirtual / app OnOrganizeDone / HealMisplacedSystemPins
requirement: 一键整理只收拾未钉选公共浮层；重新整理按当前规则重挂已钉选；无系统治愈改钉
traces: [AC-ORG-01, AC-ORG-02, AC-ORG-03]
design: [ep, reverse]
version: 4
status: active
automation: python -m pytest "testcases/DeskTidy/桌面整理/test_QA_DT_ORG_001.py" -q
last_run: PASS
last_run_at: 2026-11-11 08:31
---
## 测什么

一键整理：已钉进任意分区的图标保持座位；只把未钉选公共浮层按规则收入分区。  
重新整理：已钉选也可按新规则换区，并维护旧/新区 `pin_order`。启动与整理收尾不得再「治愈」改钉。

---

## 原逻辑

1. **已钉仍可能被重分** — 整理或合并结果会改已有钉选
2. **治愈改钉** — 整理/启动后把常用上的文件挪到文档等
3. **重新整理无效** — `ShouldSkipVirtual` 忽略 `reassign`，确认文案与设置页矛盾

---

## 改后逻辑

1. **一键整理跳过已钉** — `!reassign` 时已钉 skip；合并时已钉直接忽略
2. **重新整理可换区** — `reassign` 不因已钉跳过；确认文案与设置页一致；换区时摘旧 `pin_order`
3. **治愈停用** — 系统钉选治愈恒为无操作；启动/整理收尾不再调用改钉

---

## 逻辑改动后涉及测试范围

1. **一键整理跳过已钉**（→ `### 1.`）
2. **浮层仍可整理**（→ `### 2.`）
3. **预览同源**（→ `### 3.`）
4. **重新整理确认文案**（→ `### 4.`）
5. **治愈停用与合并不改钉**（→ `### 5.`）

---

## 测试步骤与预期

### 1. 一键整理跳过已钉

1. **检查整理跳过规则**  
   > 预期：`!reassign` 时已钉选路径直接跳过  

### 2. 浮层仍可整理

1. **检查未钉路径仍可设钉**  
   > 预期：无钉选时可解析目标并设钉  

### 3. 预览同源

1. **检查预览同源跳过**  
   > 预期：预览同样走 `ShouldSkipVirtual`  

### 4. 重新整理确认文案

1. **检查重新整理说明**  
   > 预期：写明已钉也会按新规则换区  

### 5. 治愈停用与合并不改钉

1. **检查治愈与整理收尾**  
   > 预期：治愈恒返回 0；收尾不调用治愈；非 reassign 合并不改写已钉；reassign 换区会 `RemovePinOrder`  

---

## 最终结果

- **结果**：PASS
- **时间**：2026-11-11 08:31
- **说明**：automation exit 0；6 passed in 0.11s

