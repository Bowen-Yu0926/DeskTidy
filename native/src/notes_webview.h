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
  enum class Kind { kWysiwyg, kViewer, kLibrary };

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
  // Move Win32 + WebView2 controller + ProseMirror DOM focus to the editor.
  void FocusEditor();
  void SetKind(Kind kind) { kind_ = kind; }
  // Left-rail unified library (online + local), notes_library.html.
  void ShowLibrary(const std::wstring& state_json);
  void PushLibraryState(const std::wstring& state_json);
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
  void SetCachedMarkdown(const std::wstring& md,
                         const std::wstring& base_dir = std::wstring());
  // Round-trip get (slow). Prefer CachedMarkdown when the cache is authoritative.
  // expected_first_line: when non-empty, a pulled body whose first line differs
  // is from a stale document still rendered in the WebView; it is discarded.
  std::wstring PullMarkdown(DWORD timeout_ms = 1500,
                            const std::wstring& expected_first_line = std::wstring());
  // After a successful Save: clear Toast mdDirty + cancel debounced `changed`
  // so the tab glyph can flip disc → check (Typora / VS Code).
  void AckClean();
  // Map the note's folder to https://note.assets/ so relative image srcs
  // (`assets/x.png`) load from the page's own origin instead of 404ing inside
  // the shell folder. Returns the URL prefix ("" when `dir` is unusable).
  std::wstring SetAssetBaseDir(const std::wstring& dir);

  // Preview viewer (kViewer).
  // `base_dir` is the note's directory, used to resolve relative images.
  void SetPreview(const std::wstring& md, const std::wstring& base_dir);
  // Wait until the preview finished rendering the last SetPreview (mermaid
  // diagrams + images). Returns false on timeout — the caller should then fall
  // back to a plain delay rather than exporting a half-painted document.
  bool WaitRendered(DWORD timeout_ms = 5000);
  void SetScrollRatio(double ratio);
  double RequestScrollRatio(DWORD timeout_ms = 800);
  // Export the current page to PDF via ICoreWebView2_7. Returns false and fills
  // `err` on failure.
  bool PrintToPdf(const std::wstring& path, std::wstring* err);
  // Send execCommand to webview (copy, cut, paste, undo, redo, selectAll)
  void ExecCommand(const std::wstring& cmd);

  HWND Host() const { return host_; }

  // Context menu requested from webview.
  std::function<void(POINT screen_pt, bool has_selection)> on_context_menu;
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
  // WYSIWYG image paste / upload: decode dataUrl and save into <NoteBaseDir>/assets/,
  // returning the relative path ("assets/img_...png").
  std::function<std::wstring(const std::wstring& name, const std::wstring& mime,
                             const std::wstring& data_url)>
      on_upload_image;
  // Unified library rail actions (JSON from notes_library.html).
  std::function<void(const std::wstring& json)> on_library_action;

 private:
  void NavigateHome();
  void OnReady();
  void OnWebMessage(const std::wstring& json);
  // True only while a SetMarkdown handshake is legitimately in flight.
  // Falls back to false after 5s so a page that never posts contentReady
  // cannot suppress dirty/changed posts forever.
  bool SetHandshakeActive() const;
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
  // Deadline for the set handshake: contentReady normally clears applying_set_,
  // but a page navigation / crashed bundle never posts it. Without a timeout
  // every later `changed`/`dirty` post is discarded and edits are silently lost.
  ULONGLONG applying_set_deadline_ = 0;
  // After AckClean / Save: drop Toast dirty/changed so the tab check sticks.
  ULONGLONG quiet_dirty_until_ = 0;
  std::wstring theme_ = L"light";
  std::wstring pending_md_;
  std::wstring pending_set_;
  std::wstring pending_base_;
  // Folder currently mapped to https://note.assets/ (empty = none).
  std::wstring asset_base_dir_;
  std::wstring mapped_asset_base_dir_;
  std::wstring pending_library_json_;
  void PushLibraryData();
  bool got_scroll_ = false;
  double scroll_ratio_ = 0.0;
  // Render handshake: SetPreview bumps render_token_ and the page reports back
  // with the same token once mermaid + images settled.
  unsigned render_token_ = 0;
  unsigned render_done_token_ = 0;
  bool print_done_ = false;
  bool print_ok_ = false;
  void* controller_ = nullptr;  // ICoreWebView2Controller*
  void* webview_ = nullptr;     // ICoreWebView2*
  int last_ww_height_ = -1;     // Skip redundant Toast setHeight scripts.
};
