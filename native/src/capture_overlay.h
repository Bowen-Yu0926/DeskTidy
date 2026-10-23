#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Frozen-frame capture overlay with WeChat-style annotation toolbar (P0-5/P0-6).
// Select region → annotate (rect/ellipse/arrow/brush/mosaic/text/emoji) →
// copy / save / pin. Esc cancels with no file.
class CaptureOverlay {
 public:
  using DoneFn = std::function<void(const std::wstring& png_path, bool pin)>;

  bool Begin(HINSTANCE instance, DoneFn on_done, bool auto_pin = false);
  void Cancel();

 private:
  enum class FinishAction { Copy, Save, Pin };
  enum class Mode { Selecting, Annotating };
  enum class Tool {
    None,
    Rect,
    Ellipse,
    Arrow,
    Brush,
    Mosaic,
    Text,
    Emoji
  };

  struct Annotation {
    enum Kind {
      kRect,
      kEllipse,
      kArrow,
      kBrush,
      kMosaic,
      kText,
      kEmoji
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
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT HandleToolbar(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void PaintToolbar(HDC hdc);
  void DrawAnnotations(HDC hdc);
  void DrawOneAnnotation(HDC hdc, const Annotation& a, bool draft);
  bool FinishSelection(FinishAction action);
  RECT CurrentSelectionRect() const;
  void UpdateHover(POINT client);
  void CommitSelection();
  void EnterAnnotateMode();
  void LeaveAnnotateMode();
  void PlaceToolbar();
  void CreateToolbar();
  void DestroyToolbar();
  int HitToolbar(int x, int y) const;
  void OnToolbarCommand(int id);
  void UndoAnnotation();
  void BeginDraft(Tool tool, POINT pt);
  void UpdateDraft(POINT pt);
  void CommitDraft();
  HBITMAP CompositeAnnotatedCrop(const RECT& sel) const;
  COLORREF SampleFrozen(int client_x, int client_y) const;

  bool FreezeVirtualDesktop();
  void ReleaseFrozenFrame();
  HBITMAP CropSelectionBitmap(const RECT& sel_client) const;

  HWND hwnd_ = nullptr;
  HWND toolbar_ = nullptr;
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

  Mode mode_ = Mode::Selecting;
  Tool tool_ = Tool::None;
  COLORREF stroke_color_ = RGB(250, 81, 81);
  int stroke_width_ = 4;
  int color_index_ = 0;
  int width_index_ = 1;  // kWidths[1] == 4
  int emoji_index_ = 0;
  RECT sel_{};
  bool sel_ok_ = false;
  std::vector<Annotation> annotations_;
  Annotation draft_{};
  bool has_draft_ = false;
  bool drawing_ = false;
};
