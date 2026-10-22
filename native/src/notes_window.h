#pragma once

#include "notes_webview.h"

#include <Windows.h>

#include <string>
#include <vector>

class NotesWindow {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  // Open the window and load the given files (first replaces the current tab,
  // the rest open as new tabs). Empty list behaves like Show().
  void OpenPaths(const std::vector<std::wstring>& paths);
  HWND Hwnd() const { return hwnd_; }
  bool IsOpen() const;
  // Prompt (save/discard/cancel) for any unsaved edits across tabs.
  // Returns false if the caller should abort the close/exit.
  bool ConfirmCloseAll();

 private:
  struct TabState {
    bool wysiwyg = false;
    LONG caret = 0;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Load();
  void Save();
  void SaveAs();
  void OpenLibrary();
  void SearchLibrary();
  void PreviewMarkdown();
  void ExportMarkdown();
  void ExportHtml();
  void ExportPdf();
  void FindAgain(bool down);
  void ToggleOutline();
  void RebuildOutline();
  void JumpToOutline();
  void ToggleFocus();
  void ToggleTypewriter();
  void CenterCaret();
  void LayoutClient();
  void ExportWithPandoc();
  void OpenWithShell();
  void PasteHtmlAsMarkdown();
  void ToggleToast();
  void ToggleWysiwyg();
  void SyncFromWysiwyg();
  void SetFontSize(int pt);
  void InsertTemplate();
  void ShowFind(bool replace);
  void OpenFileDialog();
  void OpenNotesFolder();
  void ShowHelp();
  void ToggleSidebar();
  void RebuildSidebar();
  void HandleSidebarContextMenu(const POINT& pt);
  void OpenSelectedSidebarItem();
  std::wstring SidebarSelectedPath();
  void Present();
  bool ConfirmSaveCurrent();
  void MarkDirty();
  // Returns true if a match was selected. Updates status_out when non-null.
  bool RunFindOnce(bool down, bool wrap, bool match_case, bool whole,
                   std::wstring* status_out);
  bool RunReplaceOnce(bool match_case, bool whole, bool wrap,
                      std::wstring* status_out);
  int RunReplaceAll(bool match_case, bool whole);
  std::wstring CurrentPlainText() const;
  void NewTab();
  void CloseTab();
  std::wstring MakeNewNotePath();
  void SwitchToTab(int index, bool save_current);
  void RebuildTabControl();
  void PersistTabs();
  void LoadTabSession();
  std::wstring TabLabel(const std::wstring& path) const;
  void AdoptPath(const std::wstring& path);
  // Markdown live preview (M1) + PDF export (M3) + image paste (M2).
  void TogglePreview();
  void UpdatePreview();
  void SchedulePreviewRefresh();
  void SyncEditToPreview();
  void SyncPreviewToEdit(double ratio);
  double EditScrollRatio();
  void SetEditScrollRatio(double ratio);
  bool PasteImageToMarkdown();
  std::wstring NoteBaseDir() const;
  void EnsureTabStates();
  void ApplyWysiwygVisibility();

  HWND hwnd_ = nullptr;
  HWND tabs_ = nullptr;
  HWND edit_ = nullptr;
  HWND outline_list_ = nullptr;
  HWND sidebar_ = nullptr;
  HMENU menu_ = nullptr;
  HFONT font_ = nullptr;
  int font_pt_ = 15;
  std::wstring path_;
  std::vector<std::wstring> tab_paths_;
  std::vector<std::wstring> sidebar_paths_;
  std::vector<TabState> tab_states_;
  int tab_index_ = 0;
  wchar_t find_what_[256]{};
  wchar_t replace_with_[256]{};
  bool find_match_case_ = false;
  bool find_whole_word_ = false;
  bool find_wrap_ = true;
  bool toast_ = false;
  bool wysiwyg_ = false;
  bool outline_ = false;
  bool focus_ = false;
  bool typewriter_ = false;
  bool sidebar_visible_ = false;
  bool dirty_ = false;
  bool loading_ = false;
  bool preview_ = false;
  bool syncing_scroll_ = false;
  NotesWebView web_;
  NotesWebView preview_web_;
  WNDPROC edit_proc_ = nullptr;
};
