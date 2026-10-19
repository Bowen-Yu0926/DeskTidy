#pragma once

#include <Windows.h>

#include <string>
#include <vector>

class TodoWindow {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }

 private:
  struct Item {
    bool done = false;
    std::wstring text;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Load();
  void Save();
  void RefreshList();
  void AddItem();
  void RemoveSelected();
  void ToggleSelected();

  HWND hwnd_ = nullptr;
  HWND list_ = nullptr;
  HWND btn_add_ = nullptr;
  HWND btn_del_ = nullptr;
  std::vector<Item> items_;
};
