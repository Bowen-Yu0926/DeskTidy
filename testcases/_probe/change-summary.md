# 变更测试说明 — 整理不改已钉选 + 右键级联移动 — 2026-11-03

## 一、本次改动内容

- 整理合并结果**不再改写已有钉选**；启动/收尾停用 `HealMisplacedSystemPins`
- 右键图标「移动到分页」改为分页 → 分区级联（去掉并列「移动到分区」）
- 对齐：已在分区的不参与整理；移动入口与 Fences/市面产品一致

## 二、涉及到改动的业务模块

| 业务模块 | 为何受影响 |
|----------|------------|
| 桌面整理 | 钉选跳过 / 合并不改钉 / 疗愈停用 |
| 桌面分区图标 | 右键移动级联 |
| 公共浮层 | 右键移动级联 |

## 三、基于上述模块生成的测试用例

| 用例编号 | 用例名称 | 测什么 | 文件 |
|----------|----------|--------|------|
| QA-DT-ORG-001 | 整理跳过已钉选 | 跳过 + 合并不改钉 + 疗愈停用 | `testcases/DeskTidy/桌面整理/QA-DT-ORG-001.md` |
| QA-DT-FE-003 | 右键分页再选分区 | 单一入口 + 级联 | `testcases/DeskTidy/桌面分区图标/QA-DT-FE-003.md` |
| QA-DT-PUB-002 | 公共浮层移动级联 | ShowMoveToPageFenceMenu | `testcases/DeskTidy/公共浮层/QA-DT-PUB-002.md` |

## 四、执行

```bash
python -m pytest "testcases/DeskTidy/桌面整理/test_QA_DT_ORG_001.py" "testcases/DeskTidy/桌面分区图标/test_QA_DT_FE_003.py" "testcases/DeskTidy/公共浮层/test_QA_DT_PUB_002.py" -q
```
