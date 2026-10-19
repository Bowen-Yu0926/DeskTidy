#pragma once

#include <Windows.h>
#include <oleidl.h>

#include <functional>
#include <string>
#include <vector>

// Start Explorer-class file drag (shell IDataObject + DoDragDrop).
// Returns true if a drop completed (DRAGDROP_S_DROP).
bool ShellDoDragFiles(const std::vector<std::wstring>& paths);

// IDropTarget that forwards dropped filesystem paths to a callback.
class PathDropTarget : public IDropTarget {
 public:
  using DropFn = std::function<void(const std::vector<std::wstring>&)>;

  explicit PathDropTarget(DropFn on_drop);
  void SetWindow(HWND hwnd) { hwnd_ = hwnd; }

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

  LONG ref_ = 1;
  HWND hwnd_ = nullptr;
  DropFn on_drop_;
  bool accept_ = false;
};

bool RegisterPathDropTarget(HWND hwnd, PathDropTarget* target);
void RevokePathDropTarget(HWND hwnd);
