#pragma once

#include <Windows.h>

#include <string>
#include <vector>

struct FenceStylePreset {
  const char* id;
  const wchar_t* name;
  COLORREF background;
  COLORREF accent;
  COLORREF title;   // title + caption ink
  BYTE alpha;       // panel fill alpha 0-255 (wallpaper shows through)
  int radius;       // corner radius (px @ 96 DPI)
  bool light;       // light panel → dark ink / darker border
};

const FenceStylePreset* FenceStyleById(const std::string& id);
const FenceStylePreset& DefaultFenceStyle();
std::vector<const FenceStylePreset*> AllFenceStyles();
std::string NormalizeFenceStyleId(const std::string& id);

// ---------------------------------------------------------------------------
// Chameleon: derive a fence palette from the current desktop wallpaper.
//
// Fences 6 blends fences into the wallpaper's palette. The presets above are
// static, so chameleon is resolved at paint time: sample the wallpaper, pick
// its dominant hue, and synthesise a panel colour + accent that sit naturally
// on it. `FenceStyleById("chameleon")` returns a process-cached result, so
// callers need no special casing.
//
// Returns a dark panel with a light accent when the wallpaper is dark, and a
// light panel with a saturated accent when it is light — the same contrast
// contract the hand-picked presets follow.
// ---------------------------------------------------------------------------
constexpr char kChameleonStyleId[] = "chameleon";

// Recompute the cached chameleon palette from the current wallpaper. Call
// after a wallpaper change so the next paint picks up the new colours.
void RefreshChameleonPalette();
// True when the chameleon palette has been resolved at least once.
bool ChameleonPaletteReady();
