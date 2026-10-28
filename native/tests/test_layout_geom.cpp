// Verifies fence geometry survives load-heal (market: Fences keeps user sizes).
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
    const int n = RestoreSystemFenceProportions(&fences);
    Expect(n > 0, "explicit restore detects skewed row");
    Expect(fences[0].w != 1631 || fences[1].w != 275,
           "explicit restore changes skewed sizes");
  }

  if (g_fails) {
    std::fprintf(stderr, "%d assertion(s) failed\n", g_fails);
    return 1;
  }
  std::printf("all layout geom tests passed\n");
  return 0;
}
