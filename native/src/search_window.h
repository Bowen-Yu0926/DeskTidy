#pragma once

#include <Windows.h>

#include <string>
#include <vector>

class SearchWindow {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void RunSearch();
  void OpenSelected();
  void RevealSelected();
  int SelectedIndex() const;

  HWND hwnd_ = nullptr;
  HWND edit_ = nullptr;
  HWND list_ = nullptr;
  HWND btn_ = nullptr;
  HWND btn_open_ = nullptr;
  HWND btn_reveal_ = nullptr;
  std::vector<std::wstring> results_;
};
