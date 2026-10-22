#pragma once

#include <Windows.h>

#include <functional>

// Desktop float 「账」— click opens vault; drag to reposition (Python AccountVaultLauncher).
class VaultLauncher {
 public:
  using VoidFn = std::function<void()>;
  using MovedFn = std::function<void(int x, int y)>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  void SetPosition(int x, int y);  // screen coords; -1,-1 = default corner
  void GetPosition(int* x, int* y) const;
  void SetOnActivate(VoidFn fn) { on_activate_ = std::move(fn); }
  void SetOnMoved(MovedFn fn) { on_moved_ = std::move(fn); }

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Present();
  void ApplySize();
  int BuoySize() const;
  POINT ClampPos(int x, int y) const;

  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  bool visible_ = false;
  bool dragging_ = false;
  bool hover_ = false;
  POINT press_{};
  POINT drag_offset_{};
  VoidFn on_activate_;
  MovedFn on_moved_;
};
