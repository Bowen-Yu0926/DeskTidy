---
id: QA-DT-CAP-001
title: 区域截图 Esc 取消与录屏会话存活探测
module: DeskTidy/截图录屏
priority: P0
type: code-test
env:
  profile: local
  role: 本机用户
source: capture_overlay CaptureEscHook / screen_record ScreenRecordSessionLive ExcludeFromCapture
requirement: 截图可用 Esc 取消；录屏控件不进录制画面；无会话时不空转重路径
traces: [AC-CAP-01, AC-CAP-02, AC-CAP-03]
design: [ep, reverse]
version: 1
status: active
automation: python -m pytest "testcases/DeskTidy/截图录屏/test_QA_DT_CAP_001.py" -q
last_run: PASS
last_run_at: 2026-10-29 15:27
---
## 测什么

区域截图应安装键盘钩子以支持 Esc 取消；录屏相关窗口应排除出系统捕获；会话存活探测应轻量，完整结束处理仅在有进程/暂停态时触发。

---

## 原逻辑

> 改前实际表现。与「改后逻辑」同序号对照。

1. **Esc 取消** — 焦点不在截图层时 Esc 无效
2. **录屏自身入画** — 角标/浮层可能被录进视频
3. **会话探测** — 无录屏时也走完整状态检查

---

## 改后逻辑

> 改后实际表现。序号与「原逻辑」一一对应。

1. **Esc 取消** — 截图期间安装低级键盘钩子，Esc 取消选区
2. **录屏自身入画** — 相关窗口设置排除捕获
3. **会话探测** — 提供轻量会话存活判断；宠物定时器先判断再完整检查

---

## 逻辑改动后涉及测试范围

1. **Esc 钩子** — 安装/卸载与 Esc 处理存在（→ `### 1.`）
2. **排除捕获** — 录屏路径含排除捕获（→ `### 2.`）
3. **逆向无会话** — 轻量接口在无句柄且未暂停时为假（→ `### 3.`）

---

## 测试步骤与预期

### 1. Esc 钩子

1. **检查截图覆盖层键盘钩子**  
   > 预期：存在安装/卸载钩子，并对 Esc 做取消处理  
   > 实测情况：符合预期

### 2. 排除捕获

1. **检查录屏排除捕获契约**  
   > 预期：实现含排除捕获相关调用  
   > 实测情况：符合预期

### 3. 逆向无会话

1. **检查会话存活声明与实现**  
   > 预期：提供会话存活接口，且仅依据进程句柄或暂停态  
   > 实测情况：符合预期

---

## 最终结果

- **结果**：PASS
- **时间**：2026-10-29 15:27
- **说明**：automation exit 0；…                                                                      [100%] | 3 passed in 0.05s

