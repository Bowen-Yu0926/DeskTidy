#!/usr/bin/env python3
"""生成「白衣剑仙剩余帧」的统一出图清单（给豆包一次性出完）。"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_spec_files import ACTIONS  # noqa: E402

DONE = {"stand"} | {f"play_{i}" for i in range(8)}  # 已出，跳过
SKIP = {"stand"}  # 已出的姿态整体跳过；play 也已完成

HEADER = """# 白衣剑仙 xianjian — 剩余帧出图清单（让豆包一次性出完）

## 一、角色（所有帧统一，不跑形）
- 白发高髻（银白长发、束发髻、散落发丝）
- 白色汉服长袍（交领、宽袖、衣摆及踝）
- 深色腰带 + 棕色斜挎带（左肩斜跨右腰）
- 腰间酒葫芦（棕黄葫芦、黑字「酒」、红流苏）
- 右手持剑（剑尖向下）+ 背剑（左侧斜背）
- 深色长裤 + 深色靴子
- 表情：清冷、平静、淡雅

## 二、输出要求（每帧统一，尽量对齐最终规格）
- **纯白背景 #FFFFFF**，正方形画布（如 2048×2048）
- **全身入镜，水平居中**（中心偏差 ≤3px）
- **脚底对齐底部**（距底 8~10px），角色占高约 90%
- **同一姿态所有帧构图锁定**：大小/位置/角度不变，只变动作
- **相邻帧动作差异要拉开**（不要只差一两个像素，否则动画看不清）
- **循环动画首尾闭合**（最后一帧≈第一帧）
- 命名：`xianjian_<姿态>_<序号>.png`

## 三、已出（跳过，不用再出）
`xianjian_stand.png` + `xianjian_play_0.png` ~ `xianjian_play_7.png`

## 四、待出（共 53 帧）

"""

TIPS = """
## 五、出图建议
- 每个姿态先出一张关键帧（如 idle_0 / walk_0），确认构图一致后再补该姿态其余帧。
- 全部出完后打包给我（或告诉我目录），我跑 `process_pet_frames.py` 自动入库。
"""


def main() -> None:
    lines = [HEADER]
    total = 0
    for pose, a in ACTIONS.items():
        if pose in SKIP:  # stand 已出
            continue
        # play 已出，跳过
        if pose == "play":
            continue
        lines.append(f"### {a['title']}（{a['frames']} 帧，{a['loop']}）")
        lines.append("")
        lines.append("| 序号 | 命名 | 动作 |")
        lines.append("|---|---|---|")
        for i, (idx, act) in enumerate(a["rows"]):
            name = f"xianjian_{pose}_{i}.png"
            lines.append(f"| {idx} | `{name}` | {act} |")
            total += 1
        lines.append("")
    lines.append(TIPS)
    out = Path(__file__).resolve().parent / "pet_frames_todo.md"
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {out} ({total} frames)")


if __name__ == "__main__":
    main()
