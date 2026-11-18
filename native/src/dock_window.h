#pragma once

#include "desktop_items.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Bottom-edge shortcut dock in the desktop band.
class DockWindow {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  // Force re-upload of the layered pixels (Optimus / ULW-loss recovery).
  void RefreshLayer();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  // True only while there is a slot to draw. Callers that seat the dock must
  // check this: AttachOverlayToDesktop(show=true) ShowWindow()s unconditionally,
  // so seating an empty dock resurrects the host after SetItems hid it.
  bool HasItems() const { return !slots_.empty(); }
  void SetItems(std::vector<DesktopItem> items);
  bool FillMissingIcons();
  // P1-10: read dock items from the settings UI field (comma-separated paths,
  // "ext_dock_items") — validates existence, caps at 8, renders.
  void SetItemsFromCsv(const std::wstring& csv);
  // P1-10: read dock items straight from app_prefs (dock_items), cap 8, render.
  void ReloadFromPrefs();
  // edge: "bottom" | "left" | "right"
  void SetEdge(const std::wstring& edge);
  void SetOpacity(float opacity);  // 0.4 – 1.0
  void AttachToDesktopLayer();
  // Re-measure chrome for the dock's current monitor DPI (WM_DPICHANGED).
  void RelayoutForDpi();
  void SetOnPathReplaced(
      std::function<void(const std::wstring&, const std::wstring&)> fn) {
    on_path_replaced_ = std::move(fn);
  }
  void SetOnRemoved(std::function<void(const std::wstring&)> fn) {
    on_removed_ = std::move(fn);
  }
  void FinishInlineRename(bool commit);

 private:
  struct Slot {
    DesktopItem item;
    HICON icon = nullptr;
    RECT bounds{};
  };
  // Design sheet @ 96 DPI; LayoutSlots fills live px for the host monitor.
  struct Metrics {
    int btn = 44;
    int icon = 32;
    int gap = 8;
    int mx = 10;
    int my = 6;
    int clear = 48;  // bottom edge clearance above work bottom
    int radius = 16;
    int glow = 4;
    int dpi = 96;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void Present();
  void LayoutSlots();
  void ApplyVisibility();
  int HitTest(int x, int y) const;
  Metrics HostMetrics() const;
  RECT HostWorkArea() const;
  void RebuildIcons();

  void ShowSlotMenu(int index, POINT screen);
  void BeginInlineRename(int index);
  void EnsureTooltip();
  void UpdateHoverTip(int index);
  void ApplyPaths(const std::vector<std::wstring>& paths);
  // Reused ULW back-buffer — avoid CreateDIBSection churn each Present.
  void ReleaseLayerBuffer();
  bool EnsureLayerBuffer(int w, int h);

  HWND hwnd_ = nullptr;
  HWND tip_ = nullptr;
  HWND rename_edit_ = nullptr;
  int rename_index_ = -1;
  WNDPROC rename_proc_ = nullptr;
  bool want_visible_ = false;
  std::wstring edge_ = L"bottom";
  BYTE fill_alpha_ = 217;  // ≈ 0.85
  int hover_index_ = -1;
  int press_index_ = -1;
  Metrics metrics_{};
  std::vector<Slot> slots_;
  std::function<void(const std::wstring&, const std::wstring&)> on_path_replaced_;
  std::function<void(const std::wstring&)> on_removed_;
  HDC layer_dc_ = nullptr;
  HBITMAP layer_dib_ = nullptr;
  HGDIOBJ layer_old_ = nullptr;
  void* layer_bits_ = nullptr;
  int layer_buf_w_ = 0;
  int layer_buf_h_ = 0;
};

// P1-10: split a comma-separated path list (settings "ext_dock_items" input),
// trimming whitespace and dropping empties. Used by App to persist dock_items.
std::vector<std::wstring> SplitDockCsv(const std::wstring& csv);
