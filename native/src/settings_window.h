#pragma once

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Normal APPWINDOW - must stay above Fences-layer overlays.
class SettingsWindow {
 public:
  using VoidFn = std::function<void()>;
  using IndexFn = std::function<void(int)>;
  using RenameFn = std::function<void(int, const std::wstring&)>;
  using PageFn = std::function<void(int, int)>;
  using BoolFn = std::function<void(bool)>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }
  bool IsOpen() const;

  void SetFenceNames(const std::vector<std::wstring>& names);
  void SetToggles(bool dock_on, bool pet_on);
  void SetHandlers(VoidFn on_add, IndexFn on_remove, VoidFn on_seat,
                   RenameFn on_rename, PageFn on_page, VoidFn on_org_virtual,
                   VoidFn on_org_physical, BoolFn on_dock, BoolFn on_pet,
                   VoidFn on_edit_rules);

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  int SelectedIndex() const;

  HWND hwnd_ = nullptr;
  HWND list_ = nullptr;
  HWND edit_title_ = nullptr;
  HWND combo_page_ = nullptr;
  HWND chk_dock_ = nullptr;
  HWND chk_pet_ = nullptr;
  VoidFn on_add_;
  IndexFn on_remove_;
  VoidFn on_seat_;
  RenameFn on_rename_;
  PageFn on_page_;
  VoidFn on_org_virtual_;
  VoidFn on_org_physical_;
  BoolFn on_dock_;
  BoolFn on_pet_;
  VoidFn on_edit_rules_;
};
