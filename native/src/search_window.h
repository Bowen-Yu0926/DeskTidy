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

  HWND hwnd_ = nullptr;
  HWND edit_ = nullptr;
  HWND list_ = nullptr;
  HWND btn_ = nullptr;
  std::vector<std::wstring> results_;
};
