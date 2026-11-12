#pragma once

#include <Windows.h>
#include <oleidl.h>

#include <functional>
#include <string>
#include <vector>

// Start Explorer-class file drag (shell IDataObject + DoDragDrop).
// Returns true if a drop completed (DRAGDROP_S_DROP).
// cancel_desktop_self_drop: public-float sources already live on Desktop —
// releasing over desktop/public cancels OLE (Fences) instead of same-folder
// CF_HDROP. Portal/folder drags leave this false so Desktop can still receive.
bool ShellDoDragFiles(const std::vector<std::wstring>& paths,
                      bool cancel_desktop_self_drop = false);

// Outcome of a pin drag (Fences-class: share to apps keeps the pin).
enum class PinDragResult {
  KeptInFence,       // Esc / drop COPY|LINK to WeChat·Explorer·etc.
  TransferredFence,  // another DeskTidy fence consumed DeskTidyPinTransfer
  Unpinned,          // released on desktop/public, or MOVE away (Recycle/folder)
};

// Start a pin-transfer drag: CF_HDROP (Explorer / apps / Recycle) plus private
// "DeskTidyPinTransfer". Releasing over the real desktop / public float host
// cancels OLE so Explorer never gets same-folder CF_HDROP (「源相同」) — that
// path returns Unpinned. Dropping on WeChat (COPY) returns KeptInFence.
PinDragResult ShellDoDragPins(const std::vector<std::wstring>& paths,
                              const std::wstring& src_fence_id);

// Drop-hint state reported on each drag-loop move (drives the floating hint).
struct PinDragHint {
  bool over_target = false;   // cursor over a pin-capable fence
  bool invalid = false;       // over a target that rejected the transfer
  bool over_desktop = false;  // real desktop / public float → unpin on release
};
// True when cursor is over Progman/WorkerW/public float (pin drag unpin zone).
bool PinDragOverDesktopUnpinZone(POINT pt);
using PinDragMoveFn = std::function<void(POINT cursor, const PinDragHint&)>;
// Install a per-move callback invoked from IDropSource::QueryContinueDrag.
void SetPinDragMoveFn(PinDragMoveFn fn);

// How CF_HDROP effects are advertised to the OLE source (Explorer).
// VirtualPin never MOVE (pin-only must not delete the source). LINK is still
// allowed for Alt / Ctrl+Shift (Desktop .lnk then pin).
enum class FileDropSemantics {
  Physical,    // Explorer: same-volume MOVE, cross-volume COPY (+ Ctrl/Shift)
  VirtualPin,  // DeskTidy plain fence / notes: COPY, or LINK on Alt/Ctrl+Shift
  PreferMove,  // Pet trash / recycle-like sinks
};

// IDropTarget that forwards dropped filesystem paths to a callback.
class PathDropTarget : public IDropTarget {
 public:
  // Return the OLE effect actually performed. DROPEFFECT_NONE if the sink
  // rejected / no-oped (so Explorer does not delete sources on a fake MOVE).
  using DropFn = std::function<DWORD(const std::vector<std::wstring>& paths,
                                     DWORD proposed_effect)>;
  // Pin-transfer handler. `commit` is false during DragEnter/DragOver (query
  // effect only, no side effects) and true on Drop (perform the transfer).
  // Return the DROPEFFECT to advertise (DROPEFFECT_NONE rejects the target).
  using PinDropFn = std::function<DWORD(
      const std::wstring& src_fence_id, const std::vector<std::wstring>& paths,
      bool commit)>;

  explicit PathDropTarget(DropFn on_drop, PinDropFn on_pin_drop = {});
  void SetWindow(HWND hwnd) { hwnd_ = hwnd; }
  void SetFileDropSemantics(FileDropSemantics s) { semantics_ = s; }
  void SetDestDir(std::wstring dir) { dest_dir_ = std::move(dir); }
  // Live providers (fence portal can change after RegisterDragDrop).
  void SetFileDropSemanticsProvider(
      std::function<FileDropSemantics()> fn) {
    semantics_fn_ = std::move(fn);
  }
  void SetDestDirProvider(std::function<std::wstring()> fn) {
    dest_fn_ = std::move(fn);
  }
  // Optional highlight callback (true on accepted DragEnter, false on
  // DragLeave/Drop) — legacy whole-target flash; prefer SetDragOverFn.
  void SetHighlightFn(std::function<void(bool)> fn) {
    highlight_fn_ = std::move(fn);
  }
  // Live cursor feedback while dragging over this target (insert bar /
  // recycle-icon hover). `active` is false on Leave/Drop.
  void SetDragOverFn(std::function<void(POINTL screen_pt, bool active)> fn) {
    drag_over_fn_ = std::move(fn);
  }

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // IDropTarget
  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD key_state,
                                      POINTL pt, DWORD* effect) override;
  HRESULT STDMETHODCALLTYPE DragOver(DWORD key_state, POINTL pt,
                                     DWORD* effect) override;
  HRESULT STDMETHODCALLTYPE DragLeave() override;
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD key_state, POINTL pt,
                                 DWORD* effect) override;

 private:
  bool HasFileDrop(IDataObject* data) const;
  std::vector<std::wstring> ExtractPaths(IDataObject* data) const;
  bool HasPinTransfer(IDataObject* data) const;
  bool ExtractPinTransfer(IDataObject* data, std::wstring* src,
                          std::vector<std::wstring>* paths) const;
  FileDropSemantics ActiveSemantics() const;
  std::wstring ActiveDestDir() const;
  DWORD ResolveFileEffect(DWORD key_state, DWORD allowed,
                          const std::wstring& sample_src) const;
  void NotifyDragOver(POINTL pt, bool active);

  LONG ref_ = 1;
  HWND hwnd_ = nullptr;
  DropFn on_drop_;
  PinDropFn on_pin_drop_;
  std::function<void(bool)> highlight_fn_;
  std::function<void(POINTL, bool)> drag_over_fn_;
  std::function<FileDropSemantics()> semantics_fn_;
  std::function<std::wstring()> dest_fn_;
  FileDropSemantics semantics_ = FileDropSemantics::Physical;
  std::wstring dest_dir_;
  std::wstring sample_src_;
  std::wstring pin_src_;
  std::vector<std::wstring> pin_paths_;
  bool accept_ = false;
  bool pin_mode_ = false;
};

bool RegisterPathDropTarget(HWND hwnd, PathDropTarget* target);
void RevokePathDropTarget(HWND hwnd);

// Fences / Explorer Ctrl+C·X: shell IDataObject on the OLE clipboard with
// Preferred DropEffect (COPY vs MOVE). Prefer this over raw Win32 CF_HDROP —
// Win11 Explorer paste often requires a real Shell IDList Array.
bool ShellOleSetClipboardFiles(const std::vector<std::wstring>& paths,
                               bool cut);
