---
id: QA-DT-PF-003
title: 图层缓冲复用、扫描去壳、保活不全量重绘
module: DeskTidy/桌面整理性能
priority: P0
type: code-test
env:
  profile: local
  role: 本机用户
source: fence_window EnsureLayerBuffer / public_host EnsurePresentBuffer / desktop_items ScanDesktopItems / app kTimerRestore
requirement: Present 复用 DIB；桌面扫描少打壳；4s 保活不对健康分区全量 blit
traces: [AC-PF-10, AC-PF-11, AC-PF-12]
design: [ep, reverse]
version: 1
status: active
automation: python -m pytest "testcases/DeskTidy/桌面整理性能/test_QA_DT_PF_003.py" -q
last_run: PASS
last_run_at: 2026-11-03 17:17
---
## 测什么

分区/公共浮层绘制应复用图层缓冲；桌面扫描不应给每个普通文件打壳显示名；周期性保活只在脏层或桌面 chrome 异常时重绘。

---

## 原逻辑

1. **每帧建 DIB** — Present 每次 CreateDIBSection/DeleteObject
2. **扫描打壳** — 每个桌面项 SHGetFileInfo 显示名 + O(n²) 去重
3. **4 秒全量 blit** — 保活定时器对所有可见分区 RefreshLayer

---

## 改后逻辑

1. **缓冲复用** — 同尺寸复用 mem DC + DIB
2. **扫描收敛** — 仅快捷方式打壳；路径去重用哈希集
3. **保活收敛** — 仅脏层 / chrome 不健康 / 压在图标层下时重绘

---

## 逻辑改动后涉及测试范围

1. **分区缓冲** — Ensure/ReleaseLayerBuffer（→ `### 1.`）
2. **公共缓冲** — EnsurePresentBuffer（→ `### 2.`）
3. **扫描** — 打壳范围与去重（→ `### 3.`）
4. **保活** — 条件重绘（→ `### 4.`）

---

## 测试步骤与预期

### 1. 分区缓冲

1. **检查 Present 复用缓冲**  
   > 预期：存在 EnsureLayerBuffer，Present 主路径不在每次末尾 DeleteObject(dib)  
   > 实测情况：符合预期

### 2. 公共缓冲

1. **检查公共 Present 复用**  
   > 预期：EnsurePresentBuffer 存在且 Present 使用之  
   > 实测情况：符合预期

### 3. 扫描

1. **检查打壳与去重**  
   > 预期：普通文件不强制 SHGetFileInfo；去重用 unordered_set  
   > 实测情况：符合预期

### 4. 保活

1. **检查 4 秒保活条件**  
   > 预期：RefreshLayer 受 LayerDirty / unhealthy / AboveDefView 门控  
   > 实测情况：符合预期

---

## 最终结果

- **结果**：PASS
- **时间**：2026-11-03 17:17
- **说明**：automation exit 0；…                                                                    [100%] | 5 passed in 0.04s

