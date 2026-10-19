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
  void SetPageNames(const std::vector<std::wstring>& names);
  void SetToggles(bool dock_on, bool pet_on, bool autostart_on);
  void SetOrganizePrefs(bool startup_on, bool watch_on);
  void SetOrganizePreview(const std::vector<std::wstring>& names);
  void SetHandlers(VoidFn on_add, IndexFn on_remove, VoidFn on_seat,
                   RenameFn on_rename, PageFn on_page, VoidFn on_org_virtual,
                   VoidFn on_org_physical, BoolFn on_dock, BoolFn on_pet,
                   VoidFn on_edit_rules, BoolFn on_autostart,
                   VoidFn on_whitelist = {}, VoidFn on_page_folders = {},
                   VoidFn on_help = {}, BoolFn on_org_startup = {},
                   BoolFn on_org_watch = {}, VoidFn on_record_cfg = {},
                   VoidFn on_org_preview = {});

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  int SelectedIndex() const;
  void ShowTab(int index);

  HWND hwnd_ = nullptr;
  HWND tabs_ = nullptr;
  HWND list_ = nullptr;
  HWND edit_title_ = nullptr;
  HWND combo_page_ = nullptr;
  HWND chk_dock_ = nullptr;
  HWND chk_pet_ = nullptr;
  HWND chk_autostart_ = nullptr;
  HWND lbl_hint_ = nullptr;
  HWND btn_add_ = nullptr;
  HWND btn_remove_ = nullptr;
  HWND btn_seat_ = nullptr;
  HWND btn_apply_ = nullptr;
  HWND btn_org_v_ = nullptr;
  HWND btn_org_p_ = nullptr;
  HWND btn_rules_ = nullptr;
  HWND btn_whitelist_ = nullptr;
  HWND chk_org_startup_ = nullptr;
  HWND chk_org_watch_ = nullptr;
  HWND lbl_org_preview_ = nullptr;
  HWND list_org_preview_ = nullptr;
  HWND btn_org_preview_ = nullptr;
  HWND btn_folders_ = nullptr;
  HWND btn_record_cfg_ = nullptr;
  HWND btn_help_ = nullptr;
  HWND lbl_title_ = nullptr;
  HWND lbl_page_ = nullptr;
  HWND lbl_list_ = nullptr;
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
  BoolFn on_autostart_;
  VoidFn on_whitelist_;
  VoidFn on_page_folders_;
  VoidFn on_help_;
  BoolFn on_org_startup_;
  BoolFn on_org_watch_;
  VoidFn on_record_cfg_;
  VoidFn on_org_preview_;
  std::vector<std::wstring> page_names_;
};
