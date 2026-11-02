---
id: QA-DT-MS-001
title: 关键业务提示走 Mist 而非原生硬弹
module: DeskTidy/Mist提示
priority: P0
type: code-test
env:
  profile: local
  role: 本机用户
source: mist_msg MistAlert MistMessage / tools_misc wallpaper / help_center / capture_overlay
requirement: 壁纸、帮助、截图失败等用户可见提示应统一 Mist 风格
traces: [AC-MS-01, AC-MS-02]
design: [ep, reverse]
version: 1
status: active
automation: python -m pytest "testcases/DeskTidy/Mist提示/test_QA_DT_MS_001.py" -q
last_run: PASS
last_run_at: 2026-10-29 15:27
---
## 测什么

壁纸失败、帮助缺失、截图保存失败等路径应使用统一 Mist 提示；这些路径不应再直接弹出系统原生对话框。

---

## 原逻辑

> 改前实际表现。与「改后逻辑」同序号对照。

1. **风格不一** — 部分路径仍用系统原生对话框，观感割裂
2. **关键失败提示** — 壁纸/帮助/截图失败提示入口不统一

---

## 改后逻辑

> 改后实际表现。序号与「原逻辑」一一对应。

1. **风格不一** — 上述关键路径改为 Mist 提示/确认
2. **关键失败提示** — 壁纸、帮助、截图失败均调用 Mist 提示

---

## 逻辑改动后涉及测试范围

1. **主路径 Mist** — 壁纸/帮助/截图失败含 Mist 提示（→ `### 1.`）
2. **逆向原生弹窗** — 同文件对应失败段不应再出现系统原生对话框调用（→ `### 2.`）

---

## 测试步骤与预期

### 1. 主路径 Mist

1. **检查壁纸、帮助、截图失败提示**  
   > 预期：均调用统一 Mist 提示  
   > 实测情况：符合预期

### 2. 逆向原生弹窗

1. **检查上述文件失败提示附近**  
   > 预期：不以系统原生对话框作为这些失败提示的主路径  
   > 实测情况：符合预期

---

## 最终结果

- **结果**：PASS
- **时间**：2026-10-29 15:27
- **说明**：automation exit 0；..                                                                       [100%] | 2 passed in 0.05s

