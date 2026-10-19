#include "ole_drag.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include <new>

namespace {

class SimpleDropSource : public IDropSource {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) {
      return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IDropSource) {
      *ppv = static_cast<IDropSource*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return InterlockedIncrement(&ref_);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG v = InterlockedDecrement(&ref_);
    if (v == 0) {
      delete this;
    }
    return static_cast<ULONG>(v);
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape,
                                              DWORD key_state) override {
    if (escape) {
      return DRAGDROP_S_CANCEL;
    }
    if ((key_state & MK_LBUTTON) == 0) {
      return DRAGDROP_S_DROP;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD /*effect*/) override {
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }

 private:
  LONG ref_ = 1;
};

IDataObject* CreateShellDataObject(const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    return nullptr;
  }
  // MVP: shell BindToHandler for the first path (Explorer-compatible).
  IShellItem* item = nullptr;
  if (FAILED(SHCreateItemFromParsingName(paths[0].c_str(), nullptr,
                                         IID_PPV_ARGS(&item))) ||
      !item) {
    return nullptr;
  }
  IDataObject* data = nullptr;
  const HRESULT hr =
      item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
  item->Release();
  return SUCCEEDED(hr) ? data : nullptr;
}

}  // namespace

bool ShellDoDragFiles(const std::vector<std::wstring>& paths) {
  IDataObject* data = CreateShellDataObject(paths);
  if (!data) {
    return false;
  }
  auto* source = new (std::nothrow) SimpleDropSource();
  if (!source) {
    data->Release();
    return false;
  }
  DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
  const HRESULT hr = DoDragDrop(data, source, effect, &effect);
  source->Release();
  data->Release();
  return hr == DRAGDROP_S_DROP;
}

PathDropTarget::PathDropTarget(DropFn on_drop) : on_drop_(std::move(on_drop)) {}

HRESULT STDMETHODCALLTYPE PathDropTarget::QueryInterface(REFIID riid,
                                                         void** ppv) {
  if (!ppv) {
    return E_POINTER;
  }
  if (riid == IID_IUnknown || riid == IID_IDropTarget) {
    *ppv = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE PathDropTarget::AddRef() {
  return InterlockedIncrement(&ref_);
}

ULONG STDMETHODCALLTYPE PathDropTarget::Release() {
  const LONG v = InterlockedDecrement(&ref_);
  if (v == 0) {
    delete this;
  }
  return static_cast<ULONG>(v);
}

bool PathDropTarget::HasFileDrop(IDataObject* data) const {
  if (!data) {
    return false;
  }
  FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  return data->QueryGetData(&fe) == S_OK;
}

std::vector<std::wstring> PathDropTarget::ExtractPaths(IDataObject* data) const {
  std::vector<std::wstring> out;
  if (!data) {
    return out;
  }
  FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM stg{};
  if (FAILED(data->GetData(&fe, &stg)) || !stg.hGlobal) {
    return out;
  }
  const UINT count = DragQueryFileW(static_cast<HDROP>(stg.hGlobal), 0xFFFFFFFF,
                                    nullptr, 0);
  out.reserve(count);
  wchar_t buf[MAX_PATH * 4];
  for (UINT i = 0; i < count; ++i) {
    if (DragQueryFileW(static_cast<HDROP>(stg.hGlobal), i, buf,
                       static_cast<UINT>(ARRAYSIZE(buf)))) {
      out.emplace_back(buf);
    }
  }
  ReleaseStgMedium(&stg);
  return out;
}

HRESULT STDMETHODCALLTYPE PathDropTarget::DragEnter(IDataObject* data,
                                                    DWORD /*key_state*/,
                                                    POINTL /*pt*/,
                                                    DWORD* effect) {
  accept_ = HasFileDrop(data);
  if (effect) {
    *effect = accept_ ? (*effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE))
                      : DROPEFFECT_NONE;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PathDropTarget::DragOver(DWORD /*key_state*/,
                                                   POINTL /*pt*/,
                                                   DWORD* effect) {
  if (effect) {
    *effect = accept_ ? (*effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE))
                      : DROPEFFECT_NONE;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PathDropTarget::DragLeave() {
  accept_ = false;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PathDropTarget::Drop(IDataObject* data,
                                               DWORD /*key_state*/, POINTL /*pt*/,
                                               DWORD* effect) {
  auto paths = ExtractPaths(data);
  if (effect) {
    *effect = paths.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
  }
  if (!paths.empty() && on_drop_) {
    on_drop_(paths);
  }
  accept_ = false;
  return S_OK;
}

bool RegisterPathDropTarget(HWND hwnd, PathDropTarget* target) {
  if (!hwnd || !target) {
    return false;
  }
  target->SetWindow(hwnd);
  return RegisterDragDrop(hwnd, target) == S_OK;
}

void RevokePathDropTarget(HWND hwnd) {
  if (hwnd) {
    RevokeDragDrop(hwnd);
  }
}
