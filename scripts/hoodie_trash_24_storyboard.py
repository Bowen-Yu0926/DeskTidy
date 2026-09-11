"""12-frame paper-plane throw — locked identity / green / white plane.

Source:
  C:/Users/baoxin.yu/.cursor/projects/e-soft-AIproject/assets/hoodie_trash_gen_{0..11}.png

Every prompt must reuse IDENTITY + BG + PLANE verbatim. Prefer neighbor refs.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC_DIR = Path(r"C:\Users\baoxin.yu\.cursor\projects\e-soft-AIproject\assets")
IDLE_REF = ROOT / "assets" / "pets" / "hoodie_idle_0.png"
N = 12

# --- locked blocks (do not paraphrase per frame) ---
IDENTITY = (
    "SAME character as the idle reference ONLY — copy face/hair/glasses/hoodie exactly. "
    "CRITICAL hoodie: body fabric is PURE WHITE everywhere (hood, sleeves, torso) — NEVER a "
    "green hoodie, NEVER a black hoodie, NEVER two-tone green/black. Hood INNER lining must be "
    "white or light grey — NEVER chroma-key green #00FF00 on any clothing (green only on BACKGROUND). "
    "Forest-GREEN ink ONLY on "
    "the chest print: world-map + bold 'PEOPLE' + 'Love Your Hood' (print stays green like idle "
    "— NEVER white print, NEVER black print). Orange drawstrings. Hair MUST match idle reference "
    "EXACTLY: same messy spiky silhouette and SAME dark black-brown palette — deep charcoal base "
    "with ONLY subtle low-contrast warm-brown shading (highlights stay dark, never tan/caramel/"
    "blonde crown caps). NO white/grey/pale tips, NO chalk-white hair shine strokes, "
    "NO lighter hair than idle, NO flat jet-black "
    "helmet. Copy idle hair color frame-to-frame — do not invent a new hair grade. "
    "Semi-rimless glasses: thick black upper rim, thin "
    "gold lower rim and bridge. Large brown eyes, light skin, dark grey pants, white sneakers, "
    "soft purple oval shadow under BOTH feet, 2.5-head chibi proportions, flat cel-shaded anime "
    "lines matching reference, NO motion blur, NO frame-number text watermark. "
    "CRITICAL SILHOUETTE: match the phone-wait reference line quality — thick crisp "
    "solid BLACK ink outline, razor-sharp cel edges like hoodie_wait (playing phone). "
    "White hoodie outer edge stays CLEAN WHITE (no grey chalk rim, no dirty halo). "
    "NO soft blur, NO fuzzy fringe, NO grey halo, NO white halo, NO chalk outline, "
    "NO semi-transparent ghost outline, NO light-grey rim around dark hair, "
    "NO watercolor bleed into the green background, NO double-exposure afterimage. "
    "Hair spikes must end in opaque black tips with a clean black line against pure green — "
    "zero white or mint pixels on the hair outline."
)

BG = (
    "Background MUST be solid flat chroma key green #00FF00 edge-to-edge — NOT black, "
    "NOT white, NOT grey. Leave clear green margin on all four sides."
)

PLANE = (
    "Paper prop MUST be the SAME simple origami airplane every frame: pure white paper "
    "#FFFFFF with only soft light-grey crease lines, no logos, no purple tint, no metal "
    "look. When holding a sheet before folding, use the same pure white paper."
)

FULL_BODY = (
    "FULL BODY head-to-toe visible. Feet exactly at bottom-center. Torso near horizontal "
    "center. Do NOT crop at waist. Do NOT lean off-canvas. CRITICAL composition: leave at "
    "least 18% solid green empty margin on TOP, LEFT, and RIGHT — hair spikes and paper "
    "plane must NOT touch or clip any canvas edge. "
    "CRITICAL HAIR: copy idle hair silhouette EXACTLY — dense layered spikes, COMPLETE "
    "pointed tips on crown LEFT and RIGHT, full volume over ears, NO flat-cropped helmet "
    "top, NO missing chunks, NO holes in the hair mass, NO bald patches. Hair must look "
    "identical in completeness to the idle reference. "
    "CRITICAL HAIR SHARPNESS: crisp solid opaque dark spikes with clean pointed tips — "
    "NO soft blur, NO fuzzy fringe, NO motion blur on hair, NO chalk dots, NO salt-and-pepper "
    "noise, NO transparent holes in bangs or crown, bangs form a continuous clean dark edge "
    "on the forehead (no speckled gap between hair and skin). "
    "CRITICAL FACE: forehead skin under bangs must be CLEAN smooth peach — ZERO black "
    "smudges, ZERO ink blotches, ZERO dirt flecks between bangs and glasses; no floating "
    "dark flecks or watermark scraps above the hair crown."
)

BEATS: list[str] = [
    "standing straight facing camera, holding flat pure-white paper sheet at chest with both hands, looking at paper",
    "same stance, folding the white paper in half, hands pressing crease, upright centered",
    "folding white paper into triangle airplane shape at chest, both hands, upright centered",
    "finished pure-white paper airplane held at chest pointing upper-right, proud small smile, centered",
    "wind-up: RIGHT arm cocked back near RIGHT shoulder with white plane pointing UPPER-RIGHT (nose to top-RIGHT), left arm forward for balance, feet centered — throw will go RIGHT only, never left",
    "throw release: right arm extending UPPER-RIGHT, white plane just leaving fingertips flying UPPER-RIGHT (nose top-RIGHT), body upright full-body, feet centered — NEVER throw left",
    "follow-through FULL BODY: right arm extended upper-right, white plane a short distance upper-right of hand, looking at plane, BOTH legs and feet visible at bottom-center",
    "FULL BODY: white plane farther upper-right at head height (nose still upper-RIGHT, "
    "plane near ear/shoulder height — NOT high in the sky), boy watching smile, right hand "
    "lowering, feet bottom-center — plane stays on the RIGHT half",
    "FULL BODY: smaller white plane near upper-right at about forehead height (clear of "
    "canvas edges, NOT sky-high), short grey dotted trail behind plane only, boy looking "
    "up-right, hands lowering, feet bottom-center",
    "FULL BODY: tiny white plane in upper-right quadrant at head height with clear green "
    "margin from edges, boy content, hands near sides, feet bottom-center",
    "FULL BODY: plane gone, idle-like stand, empty hands, gentle smile facing camera, feet bottom-center",
    "FULL BODY final: idle match, right hand slight wave, left hand near pocket, calm happy face, feet bottom-center",
]


def prompt_for_frame(i: int) -> str:
    return (
        f"Animation cel {i + 1}/{N} paper-airplane throw sequence. "
        f"{IDENTITY} {BG} {PLANE} {FULL_BODY} "
        f"Action: {BEATS[i]}."
    )


def main() -> int:
    missing = [i for i in range(N) if not (SRC_DIR / f"hoodie_trash_gen_{i}.png").is_file()]
    print(f"storyboard N={N}, idle={IDLE_REF.is_file()}, missing={missing}")
    for i in range(min(2, N)):
        print(f"[{i}] {prompt_for_frame(i)[:160]}...")
    return 0 if not missing else 1


if __name__ == "__main__":
    raise SystemExit(main())
