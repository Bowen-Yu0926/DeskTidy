#include "ole_drag.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include <cstring>
#include <new>
#include <vector>

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

HGLOBAL BuildHdrop(const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    return nullptr;
  }
  size_t chars = 1;  // final double-null
  for (const auto& p : paths) {
    chars += p.size() + 1;
  }
  const SIZE_T bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GHND | GMEM_SHARE, bytes);
  if (!mem) {
    return nullptr;
  }
  auto* df = static_cast<DROPFILES*>(GlobalLock(mem));
  if (!df) {
    GlobalFree(mem);
    return nullptr;
  }
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  df->pt.x = 0;
  df->pt.y = 0;
  df->fNC = FALSE;
  wchar_t* dest = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) +
                                             sizeof(DROPFILES));
  for (const auto& p : paths) {
    wcscpy_s(dest, p.size() + 1, p.c_str());
    dest += p.size() + 1;
  }
  *dest = L'\0';
  GlobalUnlock(mem);
  return mem;
}

class HdropDataObject : public IDataObject {
 public:
  explicit HdropDataObject(HGLOBAL hdrop) : hdrop_(hdrop) {}
  ~HdropDataObject() {
    if (hdrop_) {
      GlobalFree(hdrop_);
      hdrop_ = nullptr;
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) {
      return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *ppv = static_cast<IDataObject*>(this);
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

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* stg) override {
    if (!fe || !stg || !hdrop_) {
      return E_INVALIDARG;
    }
    if (fe->cfFormat != CF_HDROP || fe->tymed != TYMED_HGLOBAL) {
      return DV_E_FORMATETC;
    }
    const SIZE_T sz = GlobalSize(hdrop_);
    HGLOBAL copy = GlobalAlloc(GHND | GMEM_SHARE, sz);
    if (!copy) {
      return E_OUTOFMEMORY;
    }
    void* src = GlobalLock(hdrop_);
    void* dst = GlobalLock(copy);
    if (src && dst) {
      memcpy(dst, src, sz);
    }
    GlobalUnlock(hdrop_);
    GlobalUnlock(copy);
    stg->tymed = TYMED_HGLOBAL;
    stg->hGlobal = copy;
    stg->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
    if (!fe) {
      return E_INVALIDARG;
    }
    if (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) {
      return S_OK;
    }
    return DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,
                                                  FORMATETC* out) override {
    if (out) {
      out->ptd = nullptr;
    }
    return DATA_S_SAMEFORMATETC;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir,
                                          IEnumFORMATETC** enum_fe) override {
    if (!enum_fe) {
      return E_POINTER;
    }
    *enum_fe = nullptr;
    if (dir != DATADIR_GET) {
      return E_NOTIMPL;
    }
    FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SHCreateStdEnumFmtEtc(1, &fe, enum_fe);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*,
                                    DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

 private:
  LONG ref_ = 1;
  HGLOBAL hdrop_ = nullptr;
};

IDataObject* CreateShellDataObject(const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    return nullptr;
  }
  if (paths.size() == 1) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(paths[0].c_str(), nullptr,
                                              IID_PPV_ARGS(&item))) &&
        item) {
      IDataObject* data = nullptr;
      const HRESULT hr =
          item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
      item->Release();
      if (SUCCEEDED(hr) && data) {
        return data;
      }
    }
  }
  HGLOBAL hdrop = BuildHdrop(paths);
  if (!hdrop) {
    return nullptr;
  }
  return new (std::nothrow) HdropDataObject(hdrop);
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
