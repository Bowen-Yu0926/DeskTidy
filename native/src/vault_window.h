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
    std::wstring note;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Load();
  void Save();
  void RefreshList();
  void AddFromFields();
  void UpdateSelected();
  Entry ReadFields() const;
  void ExportBackup();
  void ImportBackup();
  void CloudLogin();
  void CloudSync();
  void GeneratePassword();
  void CopyPassword();
  void CopyUsername();
  void CopyFieldToClipboard(const std::wstring& value, const wchar_t* tip);
  void ClearClipboardIfOurs();

  HWND hwnd_ = nullptr;
  HWND list_ = nullptr;
  HWND edit_title_ = nullptr;
  HWND edit_user_ = nullptr;
  HWND edit_pass_ = nullptr;
  HWND edit_url_ = nullptr;
  HWND edit_note_ = nullptr;
  std::vector<Entry> entries_;
  std::wstring clip_secret_;
  UINT_PTR clip_timer_ = 0;
};
