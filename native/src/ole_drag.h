#pragma once

#include <Windows.h>
#include <oleidl.h>

#include <functional>
#include <string>
#include <vector>

// Start Explorer-class file drag (shell IDataObject + DoDragDrop).
// Returns true if a drop completed (DRAGDROP_S_DROP).
bool ShellDoDragFiles(const std::vector<std::wstring>& paths);

// Start a pin-transfer drag: the IDataObject offers both CF_HDROP (so a drop
// onto Explorer / the Recycle Bin keeps normal shell move/copy semantics) and
// the private "DeskTidyPinTransfer" format (payload = src_fence_id + '\n' +
// path1 + '\n' + path2 …, UTF-16, NUL-terminated). Returns true when the drop
// was consumed as a pin transfer (a DeskTidy fence accepted it); false on
// cancel or an external CF_HDROP drop.
bool ShellDoDragPins(const std::vector<std::wstring>& paths,
                     const std::wstring& src_fence_id);

// Drop-hint state reported on each drag-loop move (drives the floating hint).
struct PinDragHint {
  bool over_target = false;  // cursor over a pin-capable fence
  bool invalid = false;      // over a target that rejected the transfer
};
using PinDragMoveFn = std::function<void(POINT cursor, const PinDragHint&)>;
// Install a per-move callback invoked from IDropSource::QueryContinueDrag.
void SetPinDragMoveFn(PinDragMoveFn fn);

// IDropTarget that forwards dropped filesystem paths to a callback.
class PathDropTarget : public IDropTarget {
 public:
  using DropFn = std::function<void(const std::vector<std::wstring>&)>;
  // Pin-transfer handler. `commit` is false during DragEnter/DragOver (query
  // effect only, no side effects) and true on Drop (perform the transfer).
  // Return the DROPEFFECT to advertise (DROPEFFECT_NONE rejects the target).
  using PinDropFn = std::function<DWORD(
      const std::wstring& src_fence_id, const std::vector<std::wstring>& paths,
      bool commit)>;

  explicit PathDropTarget(DropFn on_drop, PinDropFn on_pin_drop = {});
  void SetWindow(HWND hwnd) { hwnd_ = hwnd; }
  // Optional highlight callback (true on accepted DragEnter, false on
  // DragLeave/Drop) — reused for the fence drop-highlight overlay.
  void SetHighlightFn(std::function<void(bool)> fn) {
    highlight_fn_ = std::move(fn);
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

  LONG ref_ = 1;
  HWND hwnd_ = nullptr;
  DropFn on_drop_;
  PinDropFn on_pin_drop_;
  std::function<void(bool)> highlight_fn_;
  bool accept_ = false;
  bool pin_mode_ = false;
};

bool RegisterPathDropTarget(HWND hwnd, PathDropTarget* target);
void RevokePathDropTarget(HWND hwnd);
