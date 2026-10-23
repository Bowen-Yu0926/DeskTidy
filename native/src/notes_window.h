#pragma once

#include "notes_library.h"
#include "notes_webview.h"
#include "paths_util.h"

#include <Windows.h>

#include <string>
#include <vector>

class NotesWindow {
 public:
  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  // Maximize, raise, and focus (matches Python DeskNote.present).
  void Present();
  // Open the window and load the given files (first replaces the current tab,
  // the rest open as new tabs). Empty list behaves like Show().
  void OpenPaths(const std::vector<std::wstring>& paths);
  // Apply UI theme palette (content/card/text) — same ids as settings ui_theme.
  void ApplyUiTheme(const std::string& ui_theme);
  // When true, WM_CLOSE destroys the window and posts quit (standalone DeskNote).
  void SetQuitOnClose(bool quit) { quit_on_close_ = quit; }
  HWND Hwnd() const { return hwnd_; }
  // Global shortcuts (work even when focus is not on the editor).
  bool TranslateAccel(MSG* msg) const;
  bool IsOpen() const;
  // Prompt (save/discard/cancel) for any unsaved edits across tabs.
  // Returns false if the caller should abort the close/exit.
  bool ConfirmCloseAll();

 private:
  struct TabState {
    bool wysiwyg = false;
    bool wrap = true;  // Per-tab word wrap (Python page.wrap).
    LONG caret = 0;
  };

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK TabsProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK SidebarProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK StatusProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Load();
  // Re-read current path from disk (prompts if dirty).
  void ReloadFromDisk();
  void PromptGotoLine();
  void Save();
  void SaveAs();
  // Leave preview/WYSIWYG and show the plain editor (Python Ctrl+E).
  void ShowEditMode();
  // Full-width Markdown preview (Python Ctrl+Shift+V / right strip).
  void ShowPreviewMode();
  void ToggleMdViewMode();
  void UpdateModeStrip();
  void RenameSidebarSelected();
  // Commit inline / modal rename: stem only; keeps original suffix.
  bool ApplySidebarRename(const std::wstring& path,
                          const std::wstring& new_stem);
  void DeleteSidebarSelected();
  // Access-denied save → UAC relaunch handoff. Returns true when handed off.
  bool OfferElevatedSave(const std::wstring& path, const std::wstring& text);
  void BuildAccelerators();
  bool PathLooksMarkdown() const;
  // Wrap current selection with markdown markers (**.md / .markdown).
  void WrapSelectionMarkdown(const wchar_t* left, const wchar_t* right);
  void ApplyBold();
  void ApplyItalic();
  void ApplyUnderline();
  void InsertMdLink();
  void InsertMdCodeFence();
  void InsertMdHeading(int level);
  // File picker → assets/ + markdown image snippet (Python insert image).
  void InsertImageFromFile();
  // Copy image path into note assets/; returns false on failure.
  bool InsertImageFile(const std::wstring& src_path);
  void RefreshCurrentTabLabel();
  void ToggleAlwaysOnTop();
  void ToggleSyncScroll();
  void OpenLibrary();
  void SearchLibrary();
  // Jump caret to 0-based line (P1-19 search hit).
  void GotoLine(int line0);
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
  // Image → assets, or HTML → Markdown (md only); false = use default paste.
  bool SmartPaste();
  void PersistViewSettings();
  void LoadViewSettings();
  void ApplyViewMenuChecks();
  // Gray out Markdown-only commands when the active file is not .md.
  void SyncMdMenuState();
  void SetSaveEncoding(TextFileEncoding enc);
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
  // P1-22: inline library search / new / pin.
  void RunSidebarSearch();
  void OpenSidebarResult(int index);
  // Select + scroll the sidebar tree to match the active tab path.
  void HighlightSidebarPath(const std::wstring& path);
  void TogglePinPath(const std::wstring& path);
  bool PathIsPinned(const std::wstring& path) const;
  // Keep pinned open tabs on the left (Python pin-to-front).
  void ApplyPinnedTabOrder();
  static std::wstring NormalizePathKey(const std::wstring& path);
  bool ConfirmSaveCurrent();
  void MarkDirty();
  void RefreshTitle();
  void ApplyChrome();
  void ApplyEditorFont();
  // Returns true if a match was selected. Updates status_out when non-null.
  bool RunFindOnce(bool down, bool wrap, bool match_case, bool whole,
                   std::wstring* status_out);
  bool RunReplaceOnce(bool match_case, bool whole, bool wrap,
                      std::wstring* status_out);
  int RunReplaceAll(bool match_case, bool whole);
  std::wstring CurrentPlainText() const;
  void NewTab();
  void CloseTab();
  void CloseTabAt(int index);
  void ActivateNextTab(bool forward);
  void HandleTabContextMenu(int index, POINT screen_pt);
  void ReorderTab(int from, int to);
  // Replace in current tab; when all_tabs, walk every open tab (P2-26).
  int RunReplaceAllTabs(bool match_case, bool whole);
  std::wstring MakeNewNotePath();
  void SwitchToTab(int index, bool save_current);
  void RebuildTabControl();
  void PersistTabs();
  void LoadTabSession();
  std::wstring TabLabel(const std::wstring& path) const;
  void AdoptPath(const std::wstring& path);
  // Activate existing tab or open in a new tab (Python _activate_or_open_path).
  void OpenOrActivatePath(const std::wstring& path);
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
  void ToggleWordWrap();
  void ApplyWordWrap();
  void UpdateStatusBar();
  int TabIndexFromPoint(POINT client) const;

  HWND hwnd_ = nullptr;
  HWND tabs_ = nullptr;
  HWND edit_ = nullptr;
  HWND outline_list_ = nullptr;
  HWND outline_strip_ = nullptr;  // Collapse handle (Python CollapsibleMdPane).
  HWND sidebar_ = nullptr;
  HWND sidebar_search_ = nullptr;
  HWND sidebar_new_ = nullptr;
  HWND sidebar_refresh_ = nullptr;
  HWND sidebar_results_ = nullptr;
  HWND status_ = nullptr;
  HWND mode_strip_ = nullptr;
  HMENU menu_ = nullptr;
  HFONT font_ = nullptr;
  HBRUSH bg_brush_ = nullptr;
  COLORREF content_bg_ = RGB(244, 246, 249);
  COLORREF card_bg_ = RGB(255, 255, 255);
  COLORREF text_color_ = RGB(26, 35, 50);
  COLORREF muted_color_ = RGB(100, 116, 139);
  int font_pt_ = 11;
  std::wstring path_;
  std::string ui_theme_ = "mist";
  // Encoding detected on Load(); Save() writes the same codec (P0-4).
  TextFileEncoding text_enc_ = TextFileEncoding::Utf8;
  std::vector<std::wstring> tab_paths_;
  std::vector<std::wstring> sidebar_paths_;
  std::vector<std::wstring> pinned_paths_;
  std::vector<NoteSearchHit> sidebar_hits_;
  std::vector<TabState> tab_states_;
  int tab_index_ = 0;
  wchar_t find_what_[256]{};
  wchar_t replace_with_[256]{};
  bool find_match_case_ = false;
  bool find_whole_word_ = false;
  bool find_wrap_ = true;
  bool find_all_tabs_ = false;
  bool quit_on_close_ = false;
  bool toast_ = false;
  bool wysiwyg_ = false;
  bool outline_ = false;
  bool focus_ = false;
  bool typewriter_ = false;
  bool sidebar_visible_ = true;  // Match Python library_sidebar default.
  bool dirty_ = false;
  bool loading_ = false;
  bool preview_ = true;  // Match Python md_view_mode default "preview".
  bool sync_scroll_ = true;
  bool syncing_scroll_ = false;
  bool word_wrap_ = true;
  NotesWebView web_;
  NotesWebView preview_web_;
  WNDPROC edit_proc_ = nullptr;
  WNDPROC tabs_proc_ = nullptr;
  WNDPROC sidebar_proc_ = nullptr;
  WNDPROC status_proc_ = nullptr;
  HACCEL accel_ = nullptr;
  bool always_on_top_ = false;
  int tab_drag_from_ = -1;
  bool tab_dragging_ = false;
  POINT tab_drag_origin_{};
};
