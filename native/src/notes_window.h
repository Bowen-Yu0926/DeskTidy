#pragma once

#include <Windows.h>
#include <commdlg.h>

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
  void SearchLibrary();
  void PreviewMarkdown();
  void SetFontSize(int pt);
  void InsertTemplate();
  void ShowFind(bool replace);
  void HandleFindMsg(LPFINDREPLACEW fr);
  std::wstring CurrentPlainText() const;

  HWND hwnd_ = nullptr;
  HWND edit_ = nullptr;
  HFONT font_ = nullptr;
  int font_pt_ = 15;
  std::wstring path_;
  FINDREPLACEW find_{};
  wchar_t find_what_[256]{};
  wchar_t replace_with_[256]{};
  HWND find_dlg_ = nullptr;
  UINT find_msg_ = 0;
};
