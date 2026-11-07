#!/usr/bin/env python3
"""Process the 8 AI-generated 白衣剑仙 frames into pet sprites.

- rembg background removal
- resize to 376px tall (width proportional)
- name xianjian_play_0..7 by 衣袂飘动 order
- fill other poses with the standing frame (placeholder until user generates them)
"""

from __future__ import annotations

import glob
from pathlib import Path

from PIL import Image

# 时间戳 -> play 序号（按衣袂飘动程度：轻微→明显→回落 循环）
ORDER = {
    "4429": 0,  # 静态站立，衣袂轻微
    "8305": 1,  # 3/4侧站清冷
    "4210": 2,  # 颔首浅笑
    "5581": 3,  # 衣袂轻微-中度
    "2743": 4,  # 衣袂中等
    "8186": 5,  # 衣袂翻卷（高潮）
    "2308": 6,  # 浅笑，衣袂轻扬（回落）
    "8680": 7,  # 平静，衣袂轻扬（循环）
}

TARGET_H = 376


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    from rembg import remove

    files = sorted(glob.glob(str(Path.home() / "Downloads" / "jimeng-2026-11-05-*-基于*白衣剑仙*.png")))
    if len(files) < 8:
        raise SystemExit(f"expected 8 frames, found {len(files)}")

    # map 时间戳 -> path
    by_ts = {}
    for f in files:
        name = Path(f).name
        ts = name.split("-")[4]  # jimeng-2026-11-05-4429-...
        by_ts[ts] = f

    dsts = [
        root / "native" / "assets" / "pets",
        root / "assets" / "pets",
        root / "native" / "build" / "Release" / "assets" / "pets",
    ]
    for d in dsts:
        d.mkdir(parents=True, exist_ok=True)

    # 1) 抠图 + 缩放到统一高度
    cut_frames = {}
    for ts, idx in ORDER.items():
        if ts not in by_ts:
            raise SystemExit(f"missing frame ts={ts}")
        src = Image.open(by_ts[ts]).convert("RGB")
        print(f"  removing bg: {ts} -> play_{idx}")
        cut = remove(src).convert("RGBA")
        bbox = cut.split()[-1].getbbox()
        if bbox:
            cut = cut.crop(bbox)
        ratio = TARGET_H / cut.height
        w = max(1, int(cut.width * ratio))
        cut = cut.resize((w, TARGET_H), Image.LANCZOS)
        cut_frames[idx] = cut

    # 2) 统一画布：最大宽度 × 高度，脚底对齐底部、水平居中
    max_w = max(f.width for f in cut_frames.values())
    canvas_w = max_w
    frames = {}
    for idx, f in cut_frames.items():
        canvas = Image.new("RGBA", (canvas_w, TARGET_H), (0, 0, 0, 0))
        ox = (canvas_w - f.width) // 2
        canvas.paste(f, (ox, 0), f)  # 脚底已对齐（内容裁剪到脚底）
        frames[idx] = canvas

    # 写入 play 帧
    for idx in range(8):
        for d in dsts:
            frames[idx].save(d / f"xianjian_play_{idx}.png", "PNG", optimize=True)
    print(f"  wrote xianjian_play_0..7 (canvas {canvas_w}x{TARGET_H})")

    # 其他姿态先用站立帧(play_0)占位
    stand = frames[0]
    placeholder = {
        "idle": 4, "walk": 4, "sleep": 4, "wait": 4, "hold": 6,
        "fall": 3, "trash": 12,
    }
    for pose, n in placeholder.items():
        for i in range(n):
            for d in dsts:
                stand.save(d / f"xianjian_{pose}_{i}.png", "PNG", optimize=True)
    for d in dsts:
        stand.save(d / "xianjian_stand.png", "PNG", optimize=True)
    print("  wrote placeholder poses (idle/walk/... = standing frame)")

    print(f"Done: xianjian_* ({stand.size[0]}x{stand.size[1]})")


if __name__ == "__main__":
    main()
