"""Voyage (少女) trash throw — locked character + ONE trash-can design.

Gen files: C:/Users/.../assets/voyage_trash_gen_{0..7}.png
Idle ref: assets/pets/voyage_idle_0.png

Every prompt reuses IDENTITY + BIN + BG verbatim. Change only the beat.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
IDLE_REF = ROOT / "assets" / "pets" / "voyage_idle_0.png"
N = 8

IDENTITY = (
    "SAME girl as the idle reference ONLY — copy face/hair/hat/outfit exactly. "
    "Wide-brim cream straw hat, shoulder-length straight brown hair, large brown eyes, "
    "soft blush, small smile. White shirt with thin vertical blue stripes. Open ORANGE "
    "utility/life-vest with black straps and buckles (same orange as idle — never grey "
    "vest, never solid black vest). Purple sunglasses hanging on the shirt neckline. "
    "Thin gold necklace with tiny crescent moon. Dark blue denim shorts. White sneakers "
    "with blue accents and white socks. Soft purple oval shadow under BOTH feet. "
    "2.5-head chibi proportions, flat cel-shaded anime line art matching idle — "
    "NO photorealism, NO motion blur, NO frame-number watermark."
)

BIN = (
    "LOCKED PROP — same trash can on EVERY frame, identical silhouette/colors: "
    "cylindrical galvanized SILVER metal bin with soft vertical ribs, open top, "
    "black plastic bag liner folded over the rim as a soft bunched cuff, NO lid, "
    "NO recycle arrows, NO logos, NO printed TRASH text, NO green mesh, NO square bin. "
    "Draw the bin in the SAME flat cel-shaded anime style as the girl (clean dark "
    "outlines, simple grey metal flats + soft highlight — NEVER photoreal metal, "
    "NEVER 3D render, NEVER photo texture). Bin stands on the LEFT; girl on the RIGHT."
)

BG = (
    "Background MUST be solid flat chroma key green #00FF00 edge-to-edge — NOT black, "
    "NOT white, NOT grey. Leave clear green margin on all four sides."
)

FULL_BODY = (
    "FULL BODY head-to-toe visible. Feet near bottom-center. Do NOT crop at waist. "
    "Leave at least 12% solid green empty margin on TOP, LEFT, and RIGHT — hat brim "
    "and bin must NOT touch or clip any canvas edge."
)

PAPER = (
    "Trash prop is a dark charcoal crumpled paper ball (or flat sheet early on) — "
    "simple anime shapes, not photoreal."
)

BEATS: list[str] = [
    "standing facing camera beside the locked bin, holding a flat dark paper sheet at chest with both hands, looking at paper",
    "same stance, starting to crumple the dark paper with both hands at chest",
    "paper half-crumpled into a rough ball at chest, focused expression, upright",
    "fully crumpled dark paper ball held at chest ready to throw, small determined smile",
    "wind-up: right arm cocked back near right shoulder holding crumpled ball, left arm forward for balance, looking at bin",
    "throw release: right arm extending toward bin, crumpled ball just leaving hand flying LEFT toward bin opening, short motion arcs only on the ball",
    "follow-through: ball dropping into bin opening, girl arms forward after throw, watching bin",
    "done: empty hands, cheerful smile facing camera, right hand small wave, tiny yellow sparkles only above bin opening — bin design unchanged",
]


def prompt_for_frame(i: int) -> str:
    return (
        f"Animation cel {i + 1}/{N} girl crumple-and-toss-to-bin sequence. "
        f"{IDENTITY} {BIN} {BG} {FULL_BODY} {PAPER} "
        f"Action: {BEATS[i]}."
    )


def main() -> int:
    missing = [i for i in range(N) if not (SRC_DIR / f"voyage_trash_gen_{i}.png").is_file()]
    print(f"storyboard N={N}, idle={IDLE_REF.is_file()}, missing={missing}")
    for i in range(min(2, N)):
        print(f"[{i}] {prompt_for_frame(i)[:180]}...")
    return 0 if not missing else 1


if __name__ == "__main__":
    raise SystemExit(main())
