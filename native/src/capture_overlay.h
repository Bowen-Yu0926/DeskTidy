#pragma once

#include <Windows.h>

#include <functional>
#include <string>

// Frozen-frame capture overlay (Snipaste-style).
//
// On Begin the whole virtual desktop (all monitors, incl. negative coords) is
// captured ONCE into a background bitmap. That frozen frame is drawn as the
// overlay background and dimmed everywhere except the current selection/hover
// window ("挖空"), so there is no live-capture tearing or animation ghosting.
// Confirm copies the selection to the clipboard and saves it; Esc/right-click
// cancels without producing any file.
class CaptureOverlay {
 public:
  using DoneFn = std::function<void(const std::wstring& png_path, bool pin)>;

  // When auto_pin is true, skip the post-capture prompt and always pin.
  bool Begin(HINSTANCE instance, DoneFn on_done, bool auto_pin = false);
  void Cancel();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  bool SaveSelection();
  void UpdateHover(POINT client);

  // Capture the full virtual desktop into bkg_bmp_ (physical pixels).
  bool FreezeVirtualDesktop();
  void ReleaseFrozenFrame();
  // Crop the client-space selection out of the frozen frame (DPI-safe).
  HBITMAP CropSelectionBitmap(const RECT& sel_client) const;

  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  bool dragging_ = false;
  POINT start_{};
  POINT end_{};
  RECT hover_{};
  bool hover_ok_ = false;
  bool auto_pin_ = false;
  DoneFn on_done_;

  HBITMAP bkg_bmp_ = nullptr;
  int bkg_w_ = 0;
  int bkg_h_ = 0;
};
