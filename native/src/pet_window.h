#pragma once

#include "ole_drag.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Desktop-band pet. Optional pet.png beside exe / data dir; else drawn sprite.
class PetWindow {
 public:
  using TrashFn = std::function<void(const std::vector<std::wstring>&)>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  void AttachToDesktopLayer();
  void Tick();
  void SetOnTrash(TrashFn fn) { on_trash_ = std::move(fn); }

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void EnsureDropTarget();
  void TryLoadSprite();

  HWND hwnd_ = nullptr;
  int bob_ = 0;
  int base_y_ = 0;
  bool dragging_ = false;
  POINT drag_offset_{};
  PathDropTarget* drop_target_ = nullptr;
  TrashFn on_trash_;
  HBITMAP sprite_ = nullptr;
  int sprite_w_ = 0;
  int sprite_h_ = 0;
};
