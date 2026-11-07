// Verifies fence geometry survives load-heal (market: Fences keeps user sizes)
// and display-adaptivity helpers behave across work-area sizes.
#include "display_geom.h"
#include "layout.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_fails = 0;

void Expect(bool ok, const char* msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  } else {
    std::printf("OK: %s\n", msg);
  }
}

std::vector<FenceLayout> UserResizedSystemRow() {
  std::vector<FenceLayout> fences;
  FenceLayout common;
  common.id = "system_common";
  common.title = L"常用";
  common.page = 0;
  common.x = 14;
  common.y = 0;
  common.w = 1631;
  common.h = 573;
  FencePageGeometry cg;
  cg.x = 14;
  cg.y = 0;
  cg.w = 1631;
  cg.h = 573;
  common.page_geometries[0] = cg;
  fences.push_back(common);

  FenceLayout side;
  side.id = "system_software_side";
  side.title = L"其他";
  side.page = 0;
  side.x = 1645;
  side.y = 0;
  side.w = 275;
  side.h = 573;
  FencePageGeometry sg;
  sg.x = 1645;
  sg.y = 0;
  sg.w = 275;
  sg.h = 573;
  side.page_geometries[0] = sg;
  fences.push_back(side);
  return fences;
}

}  // namespace

int main() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // 1) Automatic heal must NOT reset intentional narrow/wide user sizes.
  {
    auto fences = UserResizedSystemRow();
    HealFenceLayouts(&fences);
    Expect(fences[0].w == 1631 && fences[0].h == 573,
           "heal keeps custom common size");
    Expect(fences[1].w == 275 && fences[1].h == 573,
           "heal keeps custom side size");
  }

  // 2) Divergent page_geometries (user intent) promote over stale top-level.
  {
    auto fences = UserResizedSystemRow();
    fences[0].x = 0;
    fences[0].w = 1280;
    fences[1].x = 1284;
    fences[1].w = 636;
    PromoteHomePageGeometries(&fences);
    Expect(fences[0].w == 1631 && fences[0].x == 14,
           "promote restores common from page_geometries");
    Expect(fences[1].w == 275 && fences[1].x == 1645,
           "promote restores side from page_geometries");
  }

  // 3) Explicit proportion restore still available for "修复布局".
  {
    auto fences = UserResizedSystemRow();
    // Far from ~6:1 (常用 swallowed the row) — must trigger restore.
    fences[0].w = 1800;
    fences[0].page_geometries[0].w = 1800;
    fences[1].w = 80;
    fences[1].x = 1820;
    fences[1].page_geometries[0].w = 80;
    fences[1].page_geometries[0].x = 1820;
    const int n = RestoreSystemFenceProportions(&fences);
    Expect(n > 0, "explicit restore detects skewed row");
    Expect(fences[0].w != 1800 || fences[1].w != 80,
           "explicit restore changes skewed sizes");
  }

  // 4) Cross-display absolute page_geometries must not stomp donor top-level.
  {
    auto fences = UserResizedSystemRow();
    fences[0].x = 40;
    fences[0].y = 40;
    fences[0].w = 400;
    fences[0].h = 300;
    fences[0].page_geometries[0].w = 8000;  // larger than any work area
    fences[0].page_geometries[0].h = 5000;
    PromoteHomePageGeometries(&fences);
    Expect(fences[0].w == 400 && fences[0].h == 300,
           "promote skips oversized page_geometries");
  }

  // 5) Runaway / taller-than-work height is healed automatically.
  {
    auto fences = UserResizedSystemRow();
    fences[0].h = 9000;
    fences[0].page_geometries[0].h = 9000;
    fences[1].h = 9000;
    fences[1].page_geometries[0].h = 9000;
    HealFenceLayouts(&fences);
    Expect(fences[0].h < 2000 && fences[1].h < 2000,
           "heal shrinks runaway fullscreen heights");
  }

  // 6) Intentional tall fence (~80% work) must NOT be auto-shrunk.
  {
    const RECT work = PrimaryWorkArea();
    const int wh = (std::max)(1, static_cast<int>(work.bottom - work.top));
    auto fences = UserResizedSystemRow();
    const int tall = wh * 80 / 100;
    fences[0].h = tall;
    fences[0].page_geometries[0].h = tall;
    fences[0].w = (std::min)(fences[0].w,
                              static_cast<int>(work.right - work.left) - 40);
    fences[0].page_geometries[0].w = fences[0].w;
    HealFenceLayouts(&fences);
    Expect(fences[0].h >= tall - 8 && fences[0].h <= tall + 8,
           "heal keeps intentional ~80% height");
  }

  // 7) DefaultLayouts: work row ~6:1 + content_min; docs fills work area.
  {
    const auto defaults = DefaultLayouts();
    Expect(defaults.size() >= 3, "DefaultLayouts has work+docs fences");
    const RECT work = PrimaryWorkArea();
    const int ww = (std::max)(1, static_cast<int>(work.right - work.left));
    const int wh = (std::max)(1, static_cast<int>(work.bottom - work.top));
    const int content_min = MinFenceHeightForIconRows(2);
    const FenceLayout* common = nullptr;
    const FenceLayout* side = nullptr;
    const FenceLayout* docs = nullptr;
    for (const auto& f : defaults) {
      if (f.id == "system_common") {
        common = &f;
      } else if (f.id == "system_software_side") {
        side = &f;
      } else if (f.id == "system_docs") {
        docs = &f;
      }
    }
    Expect(common && side && docs, "system fences present");
    if (common && side) {
      Expect(common->h >= content_min || common->h >= wh / 2,
             "common height meets content_min or half-work");
      Expect(common->h <= wh / 2 + 2, "work fences capped at half work");
      Expect(side->h <= wh / 2 + 2, "side fence capped at half work");
      // Balanced width (common >= side, both fitting work row).
      Expect(common->w >= side->w, "work row common >= side");
      Expect(common->w + side->w <= ww + 8, "work row fits work width");
    }
    if (docs) {
      Expect(docs->w * 100 >= ww * 95, "docs fills work width");
      Expect(docs->h * 100 >= wh * 95, "docs fills work height");
    }
  }

  // 8) FingerprintIsTransient: portrait / 1280-wide must NOT kill profiles.
  {
    Expect(!FingerprintIsTransient("0,0,1080x1920@150x260mm@1"),
           "portrait secondary is not transient");
    Expect(!FingerprintIsTransient("0,0,1280x800@277x156mm@1"),
           "1280-wide laptop is not transient");
    Expect(FingerprintIsTransient("0,0,800x800@100x100mm@1"),
           "tiny square panel is transient");
    Expect(FingerprintIsTransient("0,0,1920x200@520x50mm@1"),
           "hotplug stub short height is transient");
  }

  // 9) Dialog scaling grows with DPI and clamps to work.
  {
    const RECT work = PrimaryWorkArea();
    POINT mid{(work.left + work.right) / 2, (work.top + work.bottom) / 2};
    int w = 440;
    int h = 260;
    ScaleDialogSizeForPoint(mid, &w, &h);
    Expect(w >= 200 && h >= 120, "scaled dialog has sane mins");
    Expect(w <= (work.right - work.left) && h <= (work.bottom - work.top),
           "scaled dialog fits work area");
    Expect(ScalePxForDpi(100, 96) == 100, "ScalePxForDpi identity @96");
    Expect(ScalePxForDpi(100, 144) == 150, "ScalePxForDpi 150%");
  }

  // 10) Shared fence min constants are coherent.
  {
    Expect(kFenceMinW == 160 && kFenceMinH == 120, "fence min constants");
    Expect(kFenceExpandedFloor >= kFenceMinH, "expanded floor >= min h");
    Expect(kFenceDefaultW >= kFenceMinW && kFenceDefaultH >= kFenceMinH,
           "defaults above mins");
  }

  if (g_fails) {
    std::fprintf(stderr, "%d assertion(s) failed\n", g_fails);
    return 1;
  }
  std::printf("all layout geom tests passed\n");
  return 0;
}
