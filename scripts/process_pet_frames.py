#!/usr/bin/env python3
"""通用：把 AI（豆包/即梦）生成的宠物帧处理成立即入库的宠物精灵。

AI 只能给纯白背景 RGB（无法输出真透明底），所以这里做唯一必须的处理：
  1) rembg 抠图（白底 -> 透明）
  2) 裁剪角色内容 bbox
  3) 缩放到统一高度（保持比例）
  4) 统一画布（最大宽度 x 高度，脚底对齐底部、水平居中）
  5) 按命名写入资产目录（源 + build 副本）

用法：
  python scripts/process_pet_frames.py --src "<豆包帧目录>" --char xianjian
  python scripts/process_pet_frames.py --src "<目录>" --char xianjian --height 280

输入文件命名（AI 生成时按此命名）：
  xianjian_stand.png, xianjian_play_0.png ... xianjian_play_7.png
  （也支持任意 xianjian_<pose>_<n>.png / xianjian_<pose>.png）
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image

POSE_FRAME_RE = re.compile(r"^(?P<char>.+?)_(?P<pose>[a-z]+)(?:_(?P<idx>\d+))?$")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", required=True, type=Path, help="AI 生成的帧目录")
    ap.add_argument("--char", required=True, help="角色 id，如 xianjian")
    ap.add_argument("--height", type=int, default=376, help="统一高度（默认 376）")
    ap.add_argument("--skip", action="append", default=[], help="跳过姿态（可多次），如 --skip sleep")
    args = ap.parse_args()

    root = Path(__file__).resolve().parents[1]
    from rembg import remove

    # 收集本角色所有帧，按 pose 分组
    frames_by_pose: dict[str, list[tuple[int, Path]]] = {}
    for p in args.src.glob(f"{args.char}_*.png"):
        m = POSE_FRAME_RE.match(p.stem)
        if not m or m.group("char") != args.char:
            continue
        pose = m.group("pose")
        if pose in args.skip:
            continue
        idx = int(m.group("idx")) if m.group("idx") is not None else -1
        frames_by_pose.setdefault(pose, []).append((idx, p))

    if not frames_by_pose:
        raise SystemExit(f"no {args.char}_*.png under {args.src}")

    # 处理：抠图 -> 裁剪 -> 缩放（统一高度）-> 按姿态统一画布
    # （sleep 含背景树更宽，按姿态分组避免把其它姿态的画布撑大）
    canvased: dict[str, Image.Image] = {}
    for pose, items in frames_by_pose.items():
        pose_processed: dict[str, Image.Image] = {}
        for idx, path in items:
            name = f"{args.char}_{pose}" + (f"_{idx}" if idx >= 0 else "")
            print(f"  remove bg: {path.name}")
            cut = remove(Image.open(path).convert("RGB")).convert("RGBA")
            bbox = cut.split()[-1].getbbox()
            if bbox:
                cut = cut.crop(bbox)
            ratio = args.height / cut.height
            w = max(1, int(cut.width * ratio))
            pose_processed[name] = cut.resize((w, args.height), Image.LANCZOS)
        # 该姿态内统一画布（脚底对齐底部、水平居中）
        max_w = max(f.width for f in pose_processed.values())
        for name, f in pose_processed.items():
            canvas = Image.new("RGBA", (max_w, args.height), (0, 0, 0, 0))
            canvas.paste(f, ((max_w - f.width) // 2, 0), f)
            canvased[name] = canvas

    # 写入资产目录
    dsts = [
        root / "native" / "assets" / "pets",
        root / "assets" / "pets",
        root / "native" / "build" / "Release" / "assets" / "pets",
    ]
    for d in dsts:
        d.mkdir(parents=True, exist_ok=True)
    for name, img in canvased.items():
        for d in dsts:
            img.save(d / f"{name}.png", "PNG", optimize=True)
    print(f"  wrote {len(canvased)} frames (height {args.height}, per-pose canvas)")

    # base 帧（设置界面头像）：优先 stand，否则该 pose 首帧
    base_src = None
    if f"{args.char}_stand" in canvased:
        base_src = canvased[f"{args.char}_stand"]
    elif f"{args.char}_idle" in canvased:
        base_src = canvased[f"{args.char}_idle"]
    if base_src is not None:
        for d in dsts:
            base_src.save(d / f"{args.char}_idle.png", "PNG", optimize=True)
        print(f"  wrote {args.char}_idle.png (base/avatar)")

    print(f"Done: {args.char}_* -> assets/pets")


if __name__ == "__main__":
    main()
