#include "fence_style.h"

#include <Windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")

namespace {

// 精选 8 款市面主流桌面整理高颜值磨砂配色，色相鲜明、辨识度极高、通透微光
constexpr FenceStylePreset kPresets[] = {
    // 1. 经典黑曜暗晶（纯净透光磨砂灰黑，沉稳高贵，不挑壁纸）
    {"obsidian", L"曜石黑", RGB(24, 30, 42), RGB(147, 197, 253),
     RGB(255, 255, 255), 148, 13, false},

    // 2. 深海湛蓝（科技深海宝石蓝，色泽深邃澄澈）
    {"navy", L"深海蓝", RGB(24, 58, 112), RGB(56, 189, 248),
     RGB(255, 255, 255), 150, 13, false},

    // 3. 通透云母白（极简明净透白，微透桌面纹理）
    {"mica", L"云母白", RGB(246, 248, 252), RGB(37, 99, 235),
     RGB(30, 41, 59), 165, 13, true},

    // 4. 清新极光绿（生机盎然的深翡翠薄荷色）
    {"aurora", L"极光绿", RGB(18, 68, 52), RGB(52, 211, 153),
     RGB(255, 255, 255), 150, 13, false},

    // 5. 温暖落霞金（温润高级的落霞轻奢琥珀金）
    {"sunset", L"落霞金", RGB(76, 42, 24), RGB(251, 146, 60),
     RGB(255, 255, 255), 150, 13, false},

    // 6. 幻夜紫晶（神秘梦幻的深紫罗兰宝石色）
    {"midnight", L"幻夜紫", RGB(56, 28, 86), RGB(192, 132, 252),
     RGB(255, 255, 255), 150, 13, false},

    // 7. 暮色嫣红（浪漫轻奢深玫红，红玛瑙质感）
    {"rose", L"暮色红", RGB(76, 24, 42), RGB(251, 113, 133),
     RGB(255, 255, 255), 150, 13, false},

    // 8. 静谧黛灰（极简高级商务金属冷灰）
    {"slate", L"静谧灰", RGB(42, 54, 70), RGB(148, 163, 184),
     RGB(255, 255, 255), 148, 13, false},
};

}  // namespace

// Chameleon helpers are defined further down (they need GDI+ and the wallpaper
// sampler, which stay local to this TU). Declared here so the style lookup can
// use them without a second translation unit.
namespace {
FenceStylePreset& ChameleonPreset();
}  // namespace

const FenceStylePreset& DefaultFenceStyle() { return kPresets[1]; } // 默认 navy (深海蓝)

std::string NormalizeFenceStyleId(const std::string& id) {
  // Chameleon is a valid style id but has no static entry in kPresets — check
  // it before the fallback, or it would be normalised away to navy.
  if (id == kChameleonStyleId) {
    return id;
  }
  for (const auto& p : kPresets) {
    if (id == p.id) {
      return id;
    }
  }
  // 历史与扩展旧预设向前兼容映射至 8 款新预设
  if (id == "cyberpunk" || id == "sky") return "navy";
  if (id == "graphite" || id == "dark" || id == "black") return "obsidian";
  if (id == "latte" || id == "peach") return "sunset";
  if (id == "matcha" || id == "mint") return "aurora";
  if (id == "mist" || id == "light" || id == "white") return "mica";
  if (id == "lavender" || id == "purple") return "midnight";
  if (id == "red" || id == "crimson") return "rose";
  if (id == "gray" || id == "grey") return "slate";

  return DefaultFenceStyle().id;
}

const FenceStylePreset* FenceStyleById(const std::string& id) {
  const std::string key = NormalizeFenceStyleId(id);
  if (key == kChameleonStyleId) {
    // Resolve lazily: the palette needs GDI+ and a wallpaper read, which must
    // not happen during static init or on a thread without GDI+ started.
    if (!ChameleonPaletteReady()) {
      RefreshChameleonPalette();
    }
    return &ChameleonPreset();
  }
  for (const auto& p : kPresets) {
    if (key == p.id) {
      return &p;
    }
  }
  return &DefaultFenceStyle();
}

std::vector<const FenceStylePreset*> AllFenceStyles() {
  std::vector<const FenceStylePreset*> out;
  for (const auto& p : kPresets) {
    out.push_back(&p);
  }
  // Chameleon rides along as the last entry so the fence menu / settings chips
  // offer it without a second code path.
  if (!ChameleonPaletteReady()) {
    RefreshChameleonPalette();
  }
  out.push_back(&ChameleonPreset());
  return out;
}

// ---------------------------------------------------------------------------
// Chameleon palette
// ---------------------------------------------------------------------------
namespace {

// Runtime preset backing FenceStyleById("chameleon"). The struct stores
// `const char*` / `const wchar_t*`, so the strings must outlive the result —
// these are function-local statics, not stack buffers.
FenceStylePreset& ChameleonPreset() {
  static FenceStylePreset s_preset = {
      kChameleonStyleId, L"跟随壁纸", RGB(24, 30, 42), RGB(147, 197, 253),
      RGB(255, 255, 255), 150, 13, false};
  return s_preset;
}

bool g_chameleon_ready = false;

// RGB → HSV. Hue in [0,360), saturation/value in [0,1].
void RgbToHsv(int r, int g, int b, double* h, double* s, double* v) {
  const double rf = r / 255.0;
  const double gf = g / 255.0;
  const double bf = b / 255.0;
  const double mx = (std::max)(rf, (std::max)(gf, bf));
  const double mn = (std::min)(rf, (std::min)(gf, bf));
  const double d = mx - mn;
  *v = mx;
  *s = mx <= 0.0 ? 0.0 : d / mx;
  double hue = 0.0;
  if (d > 0.0) {
    if (mx == rf) {
      hue = 60.0 * ((gf - bf) / d);
      if (hue < 0.0) {
        hue += 360.0;
      }
    } else if (mx == gf) {
      hue = 60.0 * (((bf - rf) / d) + 2.0);
    } else {
      hue = 60.0 * (((rf - gf) / d) + 4.0);
    }
  }
  *h = hue;
}

void HsvToRgb(double h, double s, double v, int* r, int* g, int* b) {
  const double c = v * s;
  const double hp = h / 60.0;
  const double x = c * (1.0 - std::fabs(std::fmod(hp, 2.0) - 1.0));
  double r1 = 0, g1 = 0, b1 = 0;
  if (hp < 1) {
    r1 = c; g1 = x;
  } else if (hp < 2) {
    r1 = x; g1 = c;
  } else if (hp < 3) {
    g1 = c; b1 = x;
  } else if (hp < 4) {
    g1 = x; b1 = c;
  } else if (hp < 5) {
    r1 = x; b1 = c;
  } else {
    r1 = c; b1 = x;
  }
  const double m = v - c;
  auto to8 = [&](double f) {
    const int i = static_cast<int>((f + m) * 255.0 + 0.5);
    return (std::min)(255, (std::max)(0, i));
  };
  *r = to8(r1);
  *g = to8(g1);
  *b = to8(b1);
}

// Read the wallpaper and return its dominant hue plus mean luminance.
// Falls back to the solid desktop background colour when the wallpaper is a
// solid fill (or unreadable), so chameleon still tracks the user's colour.
void SampleWallpaperHue(double* hue, double* value, bool* ok) {
  *hue = 210.0;   // navy-ish default
  *value = 0.25;
  *ok = false;

  // Hue histogram: 24 buckets × 15°. Saturation-weighted so a big grey field
  // does not drown the actual coloured content.
  double bucket_w[24] = {};
  double lum_sum = 0.0;
  int samples = 0;

  wchar_t path[MAX_PATH] = {};
  SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
  if (path[0] && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
    Gdiplus::Bitmap bmp(path);
    if (bmp.GetLastStatus() == Gdiplus::Ok && bmp.GetWidth() > 0 &&
        bmp.GetHeight() > 0) {
      const UINT w = bmp.GetWidth();
      const UINT h = bmp.GetHeight();
      // 12×12 sparse grid: enough to find the dominant hue, cheap on 4K.
      const UINT step_x = (std::max)(1u, w / 12);
      const UINT step_y = (std::max)(1u, h / 12);
      for (UINT y = step_y / 2; y < h; y += step_y) {
        for (UINT x = step_x / 2; x < w; x += step_x) {
          Gdiplus::Color c;
          if (bmp.GetPixel(static_cast<INT>(x), static_cast<INT>(y), &c) !=
              Gdiplus::Ok) {
            continue;
          }
          const int r = c.GetR();
          const int g = c.GetG();
          const int b = c.GetB();
          double hh = 0, ss = 0, vv = 0;
          RgbToHsv(r, g, b, &hh, &ss, &vv);
          lum_sum += 0.299 * r + 0.587 * g + 0.114 * b;
          ++samples;
          if (ss < 0.12) {
            continue;  // near-grey carries no usable hue
          }
          int idx = static_cast<int>(hh / 15.0) % 24;
          bucket_w[idx] += ss * vv;
        }
      }
    }
  }
  if (samples > 0) {
    *value = (lum_sum / samples) / 255.0;
    *ok = true;
  }

  int best = -1;
  double best_w = 0.0;
  for (int i = 0; i < 24; ++i) {
    if (bucket_w[i] > best_w) {
      best_w = bucket_w[i];
      best = i;
    }
  }
  if (best >= 0 && best_w > 0.0) {
    *hue = best * 15.0 + 7.5;
    return;
  }
  // No saturated content: keep the neutral default hue but still honour the
  // wallpaper's lightness so a white desktop gets a light fence.
}

}  // namespace

void RefreshChameleonPalette() {
  double hue = 210.0;
  double value = 0.25;
  bool ok = false;
  SampleWallpaperHue(&hue, &value, &ok);
  FenceStylePreset& p = ChameleonPreset();
  if (value >= 0.55) {
    // Light wallpaper → light panel, saturated accent, dark ink.
    p.background = RGB(static_cast<BYTE>((std::min)(255.0, 235.0)),
                       static_cast<BYTE>((std::min)(255.0, 238.0)),
                       static_cast<BYTE>((std::min)(255.0, 245.0)));
    int ar, ag, ab;
    HsvToRgb(hue, 0.78, 0.62, &ar, &ag, &ab);
    p.accent = RGB(ar, ag, ab);
    p.title = RGB(28, 32, 40);
    p.alpha = 165;
    p.light = true;
  } else {
    // Dark wallpaper → deep panel in the wallpaper's hue, bright accent.
    int br, bg, bb;
    HsvToRgb(hue, 0.55, 0.28, &br, &bg, &bb);
    p.background = RGB(br, bg, bb);
    int ar, ag, ab;
    HsvToRgb(hue, 0.62, 0.95, &ar, &ag, &ab);
    p.accent = RGB(ar, ag, ab);
    p.title = RGB(255, 255, 255);
    p.alpha = 150;
    p.light = false;
  }
  g_chameleon_ready = true;
}

bool ChameleonPaletteReady() { return g_chameleon_ready; }
