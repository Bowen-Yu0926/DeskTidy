---
id: QA-DT-SH-005
title: 分区桌面层落座（Progman owned，Win11 raised desktop 可绘）
module: DeskTidy/桌面资源管理器对标
priority: P0
type: code-test
env:
  profile: local
  role: 本机用户
source: desktop_shell SoftSeatToOwner / UpdateOverlayLayered / FenceWindow Present
requirement: 分区可见且在桌面图标层之上、应用窗口之下；不间歇漂出屏幕
traces: [AC-SH-20, AC-SH-21]
design: [ep, reverse]
version: 3
status: active
automation: python -m pytest "testcases/DeskTidy/桌面资源管理器对标/test_QA_DT_SH_005.py" -q
last_run: PASS
last_run_at: 2026-11-03 08:55
---
## 测什么

Win11 24H2+ raised desktop 上，`UpdateLayeredWindow` 分区必须是 **Progman owned 顶层弹窗**，不能 `SetParent` 到 DefView（子窗口能命中但像素不合成 → 看不见）。

---

## 原逻辑

1. DefView `WS_CHILD` 落座 — 窗口在、点得到，画面空白
2. 子窗口 ULW 传屏幕坐标 — 每次 Present 左漂约一个显示器宽

---

## 改后逻辑

1. **SoftSeatToOwner(Progman/WorkerW)** + `HWND_BOTTOM`；软换座不硬 Detach
2. **禁止 DefView 子窗口**（IsAttached 要求无 Parent）
3. **UpdateOverlayLayered**：若仍有 Parent 则 pptDst=NULL 防漂移

---

## 测试步骤与预期

### 1. 源码契约

1. **Attach/Raise 用 SoftSeatToOwner，无 SetParent(defview)**  
   > 预期：通过  
   > 实测情况：—

### 2. 运行时

1. **可见分区 owner=Progman，无 parent，坐标在屏内**  
   > 预期：通过  
   > 实测情况：—

## 最终结果

- **结果**：PASS
- **时间**：2026-11-03 08:55
- **说明**：automation exit 0；…                                                                 [100%] | 8 passed in 0.03s

