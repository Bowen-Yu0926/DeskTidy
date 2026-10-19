#include "pet_ledges.h"

#include "display_geom.h"

#include <algorithm>
#include <vector>

std::vector<WindowLedge> ListWindowLedges(HWND exclude, int max_results) {
  struct Ctx {
    HWND exclude = nullptr;
    RECT work{};
    int max_results = 24;
    std::vector<WindowLedge> found;
  } ctx;
  ctx.exclude = exclude;
  ctx.work = PrimaryWorkArea();
  ctx.max_results = max_results;

  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (static_cast<int>(c->found.size()) >= c->max_results) {
          return FALSE;
        }
        if (hwnd == c->exclude || !IsWindowVisible(hwnd)) {
          return TRUE;
        }
        if (GetWindow(hwnd, GW_OWNER)) {
          return TRUE;
        }
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) {
          return TRUE;
        }
        if (ex & WS_EX_NOACTIVATE) {
          return TRUE;
        }
        if (GetWindowTextLengthW(hwnd) <= 0) {
          return TRUE;
        }
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        if (wcsncmp(cls, L"DeskTidyNative", 14) == 0) {
          return TRUE;
        }
        RECT rc{};
        if (!GetWindowRect(hwnd, &rc)) {
          return TRUE;
        }
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        if (w < 220 || h < 80) {
          return TRUE;
        }
        const int ww = c->work.right - c->work.left;
        const int wh = c->work.bottom - c->work.top;
        if (w >= ww * 96 / 100 && h >= wh * 92 / 100) {
          return TRUE;
        }
        if (rc.top < c->work.top + 8 || rc.top > c->work.bottom - 120) {
          return TRUE;
        }
        if (rc.right < c->work.left + 40 || rc.left > c->work.right - 40) {
          return TRUE;
        }
        const int sit_x = (std::max)(static_cast<int>(c->work.left) + 8,
                                     static_cast<int>(rc.left) + 12);
        const int sit_r = (std::min)(static_cast<int>(rc.right) - 12,
                                     static_cast<int>(c->work.right) - 8);
        const int sit_w = sit_r - sit_x;
        if (sit_w < 80) {
          return TRUE;
        }
        WindowLedge ledge;
        ledge.x = sit_x;
        ledge.y = rc.top;
        ledge.width = sit_w;
        ledge.hwnd = hwnd;
        c->found.push_back(ledge);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));

  std::sort(ctx.found.begin(), ctx.found.end(),
            [](const WindowLedge& a, const WindowLedge& b) {
              if (a.y != b.y) {
                return a.y > b.y;
              }
              return a.width > b.width;
            });
  return ctx.found;
}
