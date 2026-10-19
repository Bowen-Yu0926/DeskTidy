#pragma once

#include <Windows.h>

#include <string>
#include <vector>

class VaultWindow {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }

 private:
  struct Entry {
    std::wstring title;
    std::wstring user;
    std::wstring pass;
    std::wstring url;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Load();
  void Save();
  void RefreshList();
  void AddFromFields();
  void ExportBackup();
  void ImportBackup();

  HWND hwnd_ = nullptr;
  HWND list_ = nullptr;
  HWND edit_title_ = nullptr;
  HWND edit_user_ = nullptr;
  HWND edit_pass_ = nullptr;
  HWND edit_url_ = nullptr;
  std::vector<Entry> entries_;
};
