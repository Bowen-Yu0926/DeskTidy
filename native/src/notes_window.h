#pragma once

#include <Windows.h>

#include <string>

class NotesWindow {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  HWND Hwnd() const { return hwnd_; }
  bool IsOpen() const;

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Load();
  void Save();
  void SaveAs();
  void OpenLibrary();
  void PreviewMarkdown();
  void SetFontSize(int pt);
  void InsertTemplate();
  std::wstring CurrentPlainText() const;

  HWND hwnd_ = nullptr;
  HWND edit_ = nullptr;
  HFONT font_ = nullptr;
  int font_pt_ = 15;
  std::wstring path_;
};
