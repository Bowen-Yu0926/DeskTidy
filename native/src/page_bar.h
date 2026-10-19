#pragma once

#include <Windows.h>

#include <functional>

// Desktop-band page switcher + quick tool buttons.
class PageBar {
 public:
  using PageFn = std::function<void(int)>;
  using VoidFn = std::function<void()>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }
  void SetPage(int page);
  void SetPageCount(int count);
  void ApplyStyle(COLORREF background, COLORREF accent, COLORREF title);
  void SetOnPage(PageFn fn) { on_page_ = std::move(fn); }
  void SetOnAddPage(VoidFn fn) { on_add_page_ = std::move(fn); }
  void SetOnRemovePage(VoidFn fn) { on_remove_page_ = std::move(fn); }
  void SetOnRenamePage(VoidFn fn) { on_rename_page_ = std::move(fn); }
  void SetOnNotes(VoidFn fn) { on_notes_ = std::move(fn); }
  void SetOnSearch(VoidFn fn) { on_search_ = std::move(fn); }
  void SetOnCapture(VoidFn fn) { on_capture_ = std::move(fn); }
  void AttachToDesktopLayer();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  int HitTest(int x) const;

  HWND hwnd_ = nullptr;
  int page_ = 0;
  int page_count_ = 2;
  COLORREF bg_ = RGB(24, 26, 32);
  COLORREF accent_ = RGB(70, 130, 200);
  COLORREF title_ = RGB(230, 230, 230);
  PageFn on_page_;
  VoidFn on_add_page_;
  VoidFn on_remove_page_;
  VoidFn on_rename_page_;
  VoidFn on_notes_;
  VoidFn on_search_;
  VoidFn on_capture_;
};
