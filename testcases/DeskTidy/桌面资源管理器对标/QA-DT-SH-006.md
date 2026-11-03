---
id: QA-DT-SH-006
title: 切页分区立刻显隐（无空白、无叠影）
module: DeskTidy/桌面资源管理器对标
priority: P0
type: code-test
env:
  profile: local
  role: 本机用户
source: app SetPage HideOffPageFences / fence_window Show PresentLayered
requirement: 切页后当前页分区立即可见；离页分区硬隐藏，不在应用窗口下叠影
traces: [AC-SH-30, AC-SH-31]
design: [ep, reverse]
version: 1
status: active
automation: python -m pytest "testcases/DeskTidy/桌面资源管理器对标/test_QA_DT_SH_006.py" -q
last_run: PASS
last_run_at: 2026-11-03 08:55
---
## 测什么

切换桌面分页时：当前页分区应马上画出内容；离开的分页分区应真正隐藏，不能半透明叠在其它程序背后，也不能要再点一下桌面才出现。

---

## 原逻辑

1. **软停靠跳过刷新** — 离页分区仍「可见但压在桌面图标层下」，切回时只抬层不重绘 → 空白直到点桌面
2. **叠影** — 应用盖住桌面图标层后，两页分区都会透出来

---

## 改后逻辑

1. **到页强制显示并刷新** — 切页对当前页分区执行显示 + 图层刷新
2. **离页硬隐藏** — 非当前页分区调用隐藏；落座路径也会先清离页，避免被再次显示

---

## 逻辑改动后涉及测试范围

1. **切页到页路径** — SetPage 显示并刷新（→ `### 1.`）
2. **离页硬隐藏** — HideOffPage 使用真正隐藏（→ `### 2.`）
3. **逆向：埋层仍 Present** — 分区 Show 在埋于桌面层下时仍重绘（→ `### 3.`）

---

## 测试步骤与预期

### 1. 切页到页路径

1. **检查切页对当前页分区**  
   > 预期：隐藏离页后，对当前页调用显示与图层刷新，且切页路径不走离屏预热竞态  
   > 实测情况：符合预期（FAIL）

### 2. 离页硬隐藏

1. **检查离页隐藏实现**  
   > 预期：对不应显示的分区执行真正隐藏  
   > 实测情况：符合预期

### 3. 逆向：埋层仍 Present

1. **检查分区显示在埋层时重绘**  
   > 预期：已附着但压在桌面图标层下时仍强制刷新图层  
   > 实测情况：符合预期

---

## 最终结果

- **结果**：PASS
- **时间**：2026-11-03 08:55
- **说明**：automation exit 0；…                                                                    [100%] | 5 passed in 1.41s

