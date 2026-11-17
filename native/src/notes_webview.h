#pragma once

#include <Windows.h>

#include <functional>
#include <string>

// Prefetch the DeskNote WebView2 environment (persistent profile) so the first
// note open does not pay a full Edge cold start. Safe to call multiple times.
void NotesWarmWebViewEnv();

// Hosts a WebView2 surface for the notes window. Two kinds are supported:
//   kWysiwyg — the ToastUI markdown editor (edits markdown in place).
//   kViewer  — the offline markdown preview (read-only render + PDF export).
class NotesWebView {
 public:
  enum class Kind { kWysiwyg, kViewer, kVaultAccounts, kLibrary };

  bool Create(HWND parent);
  void Destroy();
  bool Available() const { return available_; }
  bool Ready() const { return ready_; }
  // True after title chrome painted (chromeReady) or Editor set (contentReady).
  // Safe to cover RichEdit staging — body Toast may still be loading.
  bool ContentReady() const { return content_ready_; }
  Kind GetKind() const { return kind_; }
  void Show();
  void Hide();
  void Resize();
  void SetKind(Kind kind) { kind_ = kind; }
  // Navigate to vault_accounts.html and push masked entry JSON.
  void ShowVaultAccounts(const std::wstring& entries_json);
  // Left-rail unified library (online + local), notes_library.html.
  void ShowLibrary(const std::wstring& state_json);
  void PushLibraryState(const std::wstring& state_json);
  // Return to Toast WYSIWYG shell (after leaving the vault HTML view).
  void ShowWysiwygEditor();
  // Preview/editor chrome theme: "light" or "dark" (P2-30).
  void SetTheme(const std::wstring& theme) { theme_ = theme; }
  const std::wstring& Theme() const { return theme_; }

  // Fire-and-forget script (Shift+Enter soft break, etc.).
  void EvalScript(const std::wstring& js);
  // Insert an image node at the caret (WYSIWYG). `url` stays relative in the
  // saved markdown; the page resolves it against the note folder.
  void InsertImage(const std::wstring& url, const std::wstring& alt);
  // WYSIWYG editor (kWysiwyg). `base_dir` is the note's folder: Toast loads
  // from https://app.desknote/, so `assets/x.png` would resolve inside the
  // shell folder and 404 — the page prefixes relative srcs with this dir.
  void SetMarkdown(const std::wstring& md,
                   const std::wstring& base_dir = std::wstring());
  // Last known markdown (updated by SetMarkdown / Toast `changed`) — no IPC.
  const std::wstring& CachedMarkdown() const { return pending_md_; }
  // Host-side title compose without a Toast round-trip.
  void SetCachedMarkdown(const std::wstring& md) { pending_md_ = md; }
  // Round-trip get (slow). Prefer CachedMarkdown when the cache is authoritative.
  std::wstring PullMarkdown(DWORD timeout_ms = 1500);
  // After a successful Save: clear Toast mdDirty + cancel debounced `changed`
  // so the tab glyph can flip disc → check (Typora / VS Code).
  void AckClean();

  // Preview viewer (kViewer).
  // `base_dir` is the note's directory, used to resolve relative images.
  void SetPreview(const std::wstring& md, const std::wstring& base_dir);
  void SetScrollRatio(double ratio);
  double RequestScrollRatio(DWORD timeout_ms = 800);
  // Export the current page to PDF via ICoreWebView2_7. Returns false and fills
  // `err` on failure.
  bool PrintToPdf(const std::wstring& path, std::wstring* err);

  HWND Host() const { return host_; }

  // Invoked with the document scroll ratio (0..1) whenever the page scrolls.
  std::function<void(double)> on_scroll;
  // WYSIWYG document changed (Toast `changed` message with markdown).
  std::function<void()> on_changed;
  // Immediate dirty signal (no markdown body) — debounce window safe.
  std::function<void()> on_dirty;
  // Page booted empty / resized and wants the host to re-push markdown.
  std::function<void()> on_need_content;
  // Toast applied a setMarkdown/boot — host may swap from staging RichEdit.
  std::function<void()> on_content_ready;
  // Ctrl+F / F3 from WebView (replace Chromium find bar — market note find).
  std::function<void(bool replace)> on_find;
  std::function<void(bool down)> on_find_again;
  // Ctrl+S while focus is in WebView2 (Chromium otherwise eats the accel).
  std::function<void()> on_save;
  // Toast toolbar font face/size changed — persist host prefs.
  std::function<void(const std::wstring& face, int pt)> on_font_prefs;
  // Explorer file drop onto the WebView surface (AllowExternalDrop → file://
  // NavigationStarting cancel). Opens note / inserts image in the host.
  std::function<void(const std::vector<std::wstring>&)> on_external_drop;
  // Vault accounts note: copy a field (dblclick or [复制] link).
  // index >= 0 → by entry index; else match title/user. field empty → pass.
  std::function<void(int index, const std::wstring& field,
                     const std::wstring& title, const std::wstring& user)>
      on_vault_copy;
  // Eye button: host looks up plaintext and calls PushVaultPassReveal.
  std::function<void(int index)> on_vault_reveal;
  // Create/edit save from vault HTML form.
  std::function<void(int index, const std::wstring& title,
                     const std::wstring& user, const std::wstring& pass,
                     const std::wstring& url, const std::wstring& note)>
      on_vault_save;
  // Unified library rail actions (JSON from notes_library.html).
  std::function<void(const std::wstring& json)> on_library_action;
  // Edit dialog needs current fields (password omitted; empty = keep).
  std::function<void(int index)> on_vault_edit_request;
  void PushVaultPassReveal(int index, const std::wstring& pass);
  void PushVaultEditForm(int index, const std::wstring& title,
                         const std::wstring& user, const std::wstring& url,
                         const std::wstring& note, bool has_pass);
  void PushVaultSaveResult(bool ok, bool cloud_ok, bool cloud_attempted,
                           const std::wstring& message,
                           const std::wstring& entries_json);

 private:
  void NavigateHome();
  // Map the note's folder to https://note.assets/ so relative image srcs
  // (`assets/x.png`) load from the page's own origin instead of 404ing inside
  // the shell folder. Returns the URL prefix ("" when `dir` is unusable).
  std::wstring SetAssetBaseDir(const std::wstring& dir);
  void OnReady();
  void OnWebMessage(const std::wstring& json);
  static LRESULT CALLBACK HostProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleHost(UINT msg, WPARAM wparam, LPARAM lparam);

  HWND parent_ = nullptr;
  HWND host_ = nullptr;
  Kind kind_ = Kind::kWysiwyg;
  bool available_ = false;
  bool ready_ = false;
  bool content_ready_ = false;
  bool got_md_ = false;
  bool pulling_md_ = false;  // Ignore late `changed` completing PullMarkdown.
  // After SetMarkdown, ignore in-flight `changed` bodies from the previous
  // document until Toast posts contentReady for the new set.
  bool applying_set_ = false;
  // After AckClean / Save: drop Toast dirty/changed so the tab check sticks.
  ULONGLONG quiet_dirty_until_ = 0;
  std::wstring theme_ = L"light";
  std::wstring pending_md_;
  std::wstring pending_set_;
  std::wstring pending_base_;
  // Folder currently mapped to https://note.assets/ (empty = none).
  std::wstring asset_base_dir_;
  std::wstring pending_vault_json_;
  std::wstring pending_library_json_;
  void PushVaultAccountsData();
  void PushLibraryData();
  bool got_scroll_ = false;
  double scroll_ratio_ = 0.0;
  bool print_done_ = false;
  bool print_ok_ = false;
  void* controller_ = nullptr;  // ICoreWebView2Controller*
  void* webview_ = nullptr;     // ICoreWebView2*
  int last_ww_height_ = -1;     // Skip redundant Toast setHeight scripts.
};
