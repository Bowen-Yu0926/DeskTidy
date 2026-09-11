"""Regenerate hoodie pet cels — locked identity, green screen, dark hair edges.

Gens land in Cursor assets/; import via scripts/import_hoodie_regen_frames.py.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
IDLE_REF = ROOT / "assets" / "pets" / "hoodie_idle_0.png"

IDENTITY = (
    "SAME chibi anime boy as the reference image — copy face, glasses, hoodie, proportions. "
    "PURE WHITE hoodie fabric everywhere (never green/black hoodie). Forest-GREEN chest print "
    "only: world-map + bold PEOPLE + Love Your Hood. Orange drawstrings. Semi-rimless glasses: "
    "thick black upper rim, thin gold lower rim and bridge. Large brown eyes, light skin, "
    "dark grey pants, white sneakers, soft purple oval shadow under feet, 2.5-head chibi, "
    "flat cel-shaded anime lines, NO motion blur, NO watermark text."
)

HAIR = (
    "CRITICAL HAIR EDGES: messy spiky dark charcoal-brown hair matching reference silhouette. "
    "Every hair spike tip and the entire outer hair silhouette MUST be solid dark charcoal "
    "(#1a1614 to #2a2420) — the outermost opaque hair pixels are dark brown/black. "
    "Hair mass is continuous dark fill with NO holes, NO gaps, NO missing spikes against green. "
    "ABSOLUTELY FORBIDDEN: white fringe, light-grey halo, cream/peach rim, chalk shine tips, "
    "silver strokes, pale anti-alias border around hair, white speckles inside hair, "
    "floating white shards near ears or temples, jagged white cutouts in the crown. "
    "Edge anti-alias may ONLY blend dark hair into chroma-key green — never into white or light grey. "
    "Do NOT draw white outlines around hair."
)

BG = (
    "Background MUST be solid flat chroma-key green #00FF00 edge-to-edge — NOT black, NOT white, "
    "NOT grey. Leave at least 12% solid green margin on all four sides. Hair spikes must not "
    "touch canvas edges."
)

IDLE_BEATS = [
    "standing facing camera, right hand gentle wave near shoulder, left hand in hoodie pocket, calm smile, full body feet bottom-center",
    "same stance, right hand wave slightly higher, cheerful, full body feet bottom-center",
    "same as first wave pose, right hand mid-wave, full body feet bottom-center",
    "same stance, right hand wave a bit lower returning, soft smile, full body feet bottom-center",
]

SLEEP_BEATS = [
    "lying prone on stomach looking at smartphone held in both hands, feet kicked up behind, white hoodie, full figure on green, head leftish body horizontal",
    "same prone phone pose, tiny blink/phone tilt variation, feet up, full figure on green",
]

WAIT_BEATS = [
    "sitting reading an open newspaper held with both hands, white hoodie, glasses, newspaper large with soft grey print columns, full body visible on green",
]

HOLD_BEATS = [
    "being pinched/lifted by large fingers gripping hoodie collar from above, body dangling, arms down, surprised small face, full body on green",
    "same collar-lift, body sway slightly left, dangling, full body on green",
    "same collar-lift, body sway slightly right, dangling, full body on green",
]

TRASH_BEATS = [
    "standing holding flat pure-white paper sheet at chest with both hands, looking at paper",
    "folding the white paper in half, hands pressing crease, upright centered",
    "folding white paper into triangle airplane shape at chest, both hands",
    "finished pure-white paper airplane held at chest pointing upper-right, proud small smile",
    "wind-up: RIGHT arm cocked back near RIGHT shoulder with white plane pointing UPPER-RIGHT, left arm forward, feet centered",
    "throw release: right arm extending UPPER-RIGHT, white plane just leaving fingertips flying UPPER-RIGHT, upright full-body",
    "follow-through: right arm extended upper-right, white plane short distance upper-right of hand, BOTH feet visible",
    "white plane farther upper-right at head height, boy watching smile, right hand lowering, feet bottom-center",
    "smaller white plane near upper-right at forehead height, boy looking up-right, hands lowering",
    "tiny white plane in upper-right at head height, boy content, hands near sides",
    "plane gone, idle-like stand, empty hands, gentle smile facing camera",
    "idle match, right hand slight wave, left hand near pocket, calm happy face",
]

PLANE = (
    "Paper prop: simple origami airplane / sheet of pure white paper #FFFFFF with soft light-grey "
    "crease lines only — no logos."
)


def prompt(kind: str, i: int) -> str:
    if kind == "idle":
        beat = IDLE_BEATS[i]
        extra = ""
    elif kind == "sleep":
        beat = SLEEP_BEATS[i]
        extra = ""
    elif kind == "wait":
        beat = WAIT_BEATS[i]
        extra = ""
    elif kind == "hold":
        beat = HOLD_BEATS[i]
        extra = ""
    elif kind == "trash":
        beat = TRASH_BEATS[i]
        extra = PLANE + " "
    else:
        raise ValueError(kind)
    return (
        f"Desktop-pet animation cel ({kind} {i}). {IDENTITY} {HAIR} {BG} {extra}"
        f"FULL BODY head-to-toe visible unless pose is prone/sitting (then entire figure visible). "
        f"Action: {beat}."
    )


def main() -> int:
    print("idle ref", IDLE_REF.is_file())
    for kind, n in (("idle", 4), ("sleep", 2), ("wait", 1), ("hold", 3), ("trash", 12)):
        for i in range(n):
            name = f"hoodie_{kind}_regen_{i}.png"
            print(name, prompt(kind, i)[:100], "...")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
