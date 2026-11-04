#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Frozen-frame capture overlay with WeChat-style annotation toolbar.
// Select region → annotate (rect/ellipse/arrow/brush/mosaic/text) →
// copy / save / pin. Esc cancels with no file.
class CaptureOverlay {
 public:
  using DoneFn = std::function<void(const std::wstring& png_path, bool pin)>;

  bool Begin(HINSTANCE instance, DoneFn on_done, bool auto_pin = false);
  void Cancel();
  HWND Hwnd() const { return hwnd_; }
  bool IsActive() const { return hwnd_ != nullptr; }

 private:
  enum class FinishAction { Copy, Save, Pin };
  enum class Mode { Selecting, Annotating };
  enum class Tool { None, Rect, Ellipse, Arrow, Brush, Mosaic, Text };
  enum class ResizeEdge {
    None,
    N,
    S,
    E,
    W,
    NE,
    NW,
    SE,
    SW
  };

  struct Annotation {
    enum Kind {
      kRect,
      kEllipse,
      kArrow,
      kBrush,
      kMosaic,
      kText
    } kind = kRect;
    POINT start{};
    POINT end{};
    std::vector<POINT> points;
    COLORREF color = RGB(250, 81, 81);
    int width = 4;
    std::wstring text;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK ToolbarProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK TextEditProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT HandleToolbar(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void PaintContent(HDC hdc);
  void PaintToolbar(HDC hdc);
  void DrawAnnotations(HDC hdc);
  void DrawOneAnnotation(HDC hdc, const Annotation& a, bool draft);
  bool FinishSelection(FinishAction action);
  RECT CurrentSelectionRect() const;
  // Returns true when hover highlight changed (caller invalidates).
  bool UpdateHover(POINT client);
  void CommitSelection();
  void EnterAnnotateMode();
  void LeaveAnnotateMode();
  void PlaceToolbar();
  void CreateToolbar();
  void DestroyToolbar();
  int HitToolbar(int x, int y) const;
  void OnToolbarCommand(int id);
  void UpdateToolbarHover(int id);
  void HideToolbarTip();
  void ShowToolbarTip(int id);
  void UndoAnnotation();
  void BeginDraft(Tool tool, POINT pt);
  bool UpdateDraft(POINT pt);
  void CommitDraft();
  HBITMAP CompositeAnnotatedCrop(const RECT& sel) const;
  // mem: compatible DC with bkg_bmp_ already selected (caller creates it once
  // per mosaic loop instead of per pixel).
  COLORREF SampleFrozen(HDC mem, int client_x, int client_y) const;

  bool FreezeVirtualDesktop();
  void ReleaseFrozenFrame();
  bool BuildDimmedBackground();
  HBITMAP CropSelectionBitmap(const RECT& sel_client) const;

  // Top-level under point, then deepest visible child (WeChat-style tight box).
  HWND HitCaptureTarget(POINT screen) const;
  bool ClientRectOfWindow(HWND target, RECT* out_client) const;

  ResizeEdge HitResizeEdge(POINT pt) const;
  void BeginResize(ResizeEdge edge, POINT pt);
  bool ApplyResize(POINT pt);
  void EndResize();
  void UpdateResizeCursor(POINT pt) const;

  void BeginInlineText(POINT pt);
  void CommitInlineText();
  void CancelInlineText();
  void DestroyInlineText();

  void RequestRepaint();
  void RequestRepaintRects(const RECT* a, const RECT* b);

  HWND hwnd_ = nullptr;
  HWND toolbar_ = nullptr;
  HWND tip_hwnd_ = nullptr;
  int tb_hover_id_ = 0;
  HWND text_edit_ = nullptr;
  WNDPROC text_edit_prev_ = nullptr;
  HINSTANCE instance_ = nullptr;
  bool dragging_ = false;
  POINT start_{};
  POINT end_{};
  RECT hover_{};
  bool hover_ok_ = false;
  HWND hover_hwnd_ = nullptr;
  bool auto_pin_ = false;
  DoneFn on_done_;

  HBITMAP bkg_bmp_ = nullptr;  // pristine freeze
  HBITMAP dim_bmp_ = nullptr;  // freeze + full dim (built once)
  int bkg_w_ = 0;
  int bkg_h_ = 0;
  HBITMAP back_bmp_ = nullptr;  // reused compose buffer
  int back_w_ = 0;
  int back_h_ = 0;

  Mode mode_ = Mode::Selecting;
  Tool tool_ = Tool::None;
  COLORREF stroke_color_ = RGB(250, 81, 81);
  int stroke_width_ = 4;
  int color_index_ = 0;
  int width_index_ = 1;  // kWidths[1] == 4
  RECT sel_{};
  bool sel_ok_ = false;
  std::vector<Annotation> annotations_;
  Annotation draft_{};
  bool has_draft_ = false;
  bool drawing_ = false;

  ResizeEdge resize_edge_ = ResizeEdge::None;
  bool resizing_ = false;
  POINT resize_grab_{};
  RECT resize_origin_{};
  POINT text_anchor_{};
};
