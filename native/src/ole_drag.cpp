#include "ole_drag.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include <cstring>
#include <new>
#include <vector>

namespace {

CLIPFORMAT PinTransferFormat() {
  static CLIPFORMAT fmt = 0;
  if (!fmt) {
    fmt = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DeskTidyPinTransfer"));
  }
  return fmt;
}

PinDragMoveFn g_pin_move_fn;
bool g_over_pin_target = false;
bool g_over_pin_invalid = false;
bool g_pin_transferred = false;
// True while ShellDoDragPins runs — desktop release must NOT CF_HDROP into
// Explorer (files already live on the desktop → 「源相同」 + icon stack).
bool g_pin_drag_active = false;
// Set when QueryContinueDrag cancels because the cursor is over the desktop /
// public unpin zone (not Esc). Caller should unpin in that case only.
bool g_desktop_unpin_requested = false;

// Python start_virtual_item_drag: releasing over the real desktop / public
// float host unpins without OLE. Folder windows + Recycle Bin still get DROP.
bool IsPointOverDesktopUnpinZone(POINT pt);  // defined below

bool PinDragOverDesktopUnpinZone(POINT pt) {
  return IsPointOverDesktopUnpinZone(pt);
}

bool IsPointOverDesktopUnpinZone(POINT pt) {
  HWND hwnd = WindowFromPoint(pt);
  if (!hwnd) {
    return false;
  }
  for (HWND cur = hwnd; cur; cur = GetParent(cur)) {
    wchar_t cls[256] = {};
    GetClassNameW(cur, cls, 256);
    if (wcscmp(cls, L"DeskTidyNativeFence") == 0 ||
        wcscmp(cls, L"DeskTidyNativePet") == 0 ||
        wcscmp(cls, L"DeskTidyNativePetBuoy") == 0 ||
        wcscmp(cls, L"DeskTidyNativePetPages") == 0) {
      return false;
    }
    if (wcscmp(cls, L"DeskTidyNativePublic") == 0 ||
        wcscmp(cls, L"DeskTidyNativeDock") == 0) {
      return true;
    }
    if (wcscmp(cls, L"CabinetWClass") == 0 ||
        wcscmp(cls, L"ExploreWClass") == 0) {
      return false;
    }
    if (wcscmp(cls, L"SHELLDLL_DefView") == 0) {
      HWND parent = GetParent(cur);
      wchar_t pcls[256] = {};
      if (parent) {
        GetClassNameW(parent, pcls, 256);
      }
      return wcscmp(pcls, L"Progman") == 0 || wcscmp(pcls, L"WorkerW") == 0;
    }
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) {
      return true;
    }
  }
  return false;
}

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
      g_desktop_unpin_requested = false;
      return DRAGDROP_S_CANCEL;
    }
    if ((key_state & MK_LBUTTON) == 0) {
      // Pin drag released over desktop/public → cancel OLE so Explorer never
      // sees same-folder CF_HDROP (「源和目标文件名相同」). Caller unpins.
      if (g_pin_drag_active) {
        POINT pt{};
        GetCursorPos(&pt);
        if (IsPointOverDesktopUnpinZone(pt)) {
          g_desktop_unpin_requested = true;
          return DRAGDROP_S_CANCEL;
        }
      }
      g_desktop_unpin_requested = false;
      return DRAGDROP_S_DROP;
    }
    if (g_pin_move_fn) {
      POINT pt{};
      GetCursorPos(&pt);
      PinDragHint hint;
      hint.over_target = g_over_pin_target;
      hint.invalid = g_over_pin_invalid;
      hint.over_desktop =
          !g_over_pin_target && IsPointOverDesktopUnpinZone(pt);
      g_pin_move_fn(pt, hint);
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

HGLOBAL BuildPinPayload(const std::wstring& src_fence_id,
                        const std::vector<std::wstring>& paths) {
  std::wstring payload;
  payload.reserve(src_fence_id.size() + paths.size() * 64 + 16);
  payload += src_fence_id;
  for (const auto& p : paths) {
    payload.push_back(L'\n');
    payload += p;
  }
  const SIZE_T bytes = (payload.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GHND | GMEM_SHARE, bytes);
  if (!mem) {
    return nullptr;
  }
  auto* dst = static_cast<wchar_t*>(GlobalLock(mem));
  if (!dst) {
    GlobalFree(mem);
    return nullptr;
  }
  memcpy(dst, payload.c_str(), payload.size() * sizeof(wchar_t));
  dst[payload.size()] = L'\0';
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

class PinTransferDataObject : public IDataObject {
 public:
  PinTransferDataObject(HGLOBAL hdrop, HGLOBAL pin)
      : hdrop_(hdrop), pin_(pin) {}
  ~PinTransferDataObject() {
    if (hdrop_) {
      GlobalFree(hdrop_);
      hdrop_ = nullptr;
    }
    if (pin_) {
      GlobalFree(pin_);
      pin_ = nullptr;
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

  HGLOBAL HandleFor(const FORMATETC* fe) const {
    if (!fe || fe->tymed != TYMED_HGLOBAL) {
      return nullptr;
    }
    if (fe->cfFormat == CF_HDROP) {
      return hdrop_;
    }
    if (fe->cfFormat == PinTransferFormat()) {
      return pin_;
    }
    return nullptr;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* stg) override {
    if (!fe || !stg) {
      return E_INVALIDARG;
    }
    HGLOBAL src = HandleFor(fe);
    if (!src) {
      return DV_E_FORMATETC;
    }
    const SIZE_T sz = GlobalSize(src);
    HGLOBAL copy = GlobalAlloc(GHND | GMEM_SHARE, sz);
    if (!copy) {
      return E_OUTOFMEMORY;
    }
    void* s = GlobalLock(src);
    void* d = GlobalLock(copy);
    if (s && d) {
      memcpy(d, s, sz);
    }
    GlobalUnlock(src);
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
    if ((fe->tymed & TYMED_HGLOBAL) && HandleFor(fe)) {
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
    FORMATETC fe[2] = {
        {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        {PinTransferFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
    };
    return SHCreateStdEnumFmtEtc(2, fe, enum_fe);
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
  HGLOBAL pin_ = nullptr;
};

IDataObject* CreateShellDataObject(const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    return nullptr;
  }
  // Shell-native data object for every count, matching Python's
  // SHCreateDataObject path. A CF_HDROP-only object (HdropDataObject) answers
  // E_NOTIMPL to SetData/EnumFormatEtc non-HDROP formats, so multi-select
  // drags broke on targets that need Shell IDList / FileGroupDescriptor /
  // Preferred DropEffect (Explorer "move to folder", Outlook mail, IDEs).
  std::vector<PIDLIST_ABSOLUTE> pidls;
  pidls.reserve(paths.size());
  for (const auto& p : paths) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(
            SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr)) &&
        pidl) {
      pidls.push_back(pidl);
    }
  }
  IDataObject* shell_data = nullptr;
  if (pidls.size() == 1) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(SHCreateItemFromIDList(pidls[0], IID_PPV_ARGS(&item))) &&
        item) {
      if (FAILED(
              item->BindToHandler(nullptr, BHID_DataObject,
                                  IID_PPV_ARGS(&shell_data)))) {
        shell_data = nullptr;
      }
      item->Release();
    }
  } else if (pidls.size() > 1) {
    std::vector<const ITEMIDLIST*> list;
    list.reserve(pidls.size());
    for (PIDLIST_ABSOLUTE pidl : pidls) {
      list.push_back(pidl);
    }
    IShellItemArray* array = nullptr;
    if (SUCCEEDED(SHCreateShellItemArrayFromIDLists(
            static_cast<UINT>(list.size()), list.data(), &array)) &&
        array) {
      if (FAILED(array->BindToHandler(nullptr, BHID_DataObject,
                                      IID_PPV_ARGS(&shell_data)))) {
        shell_data = nullptr;
      }
      array->Release();
    }
  }
  for (PIDLIST_ABSOLUTE pidl : pidls) {
    CoTaskMemFree(pidl);
  }
  if (shell_data) {
    return shell_data;
  }
  // Fallback keeps plain CF_HDROP drops (Explorer / desktop) working even when
  // PIDL parsing fails (e.g. a namespace-only path).
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

void SetPinDragMoveFn(PinDragMoveFn fn) { g_pin_move_fn = std::move(fn); }

PinDragResult ShellDoDragPins(const std::vector<std::wstring>& paths,
                              const std::wstring& src_fence_id) {
  if (paths.empty()) {
    return PinDragResult::KeptInFence;
  }
  HGLOBAL hdrop = BuildHdrop(paths);
  if (!hdrop) {
    return PinDragResult::KeptInFence;
  }
  HGLOBAL pin = BuildPinPayload(src_fence_id, paths);
  if (!pin) {
    GlobalFree(hdrop);
    return PinDragResult::KeptInFence;
  }
  auto* data = new (std::nothrow) PinTransferDataObject(hdrop, pin);
  if (!data) {
    GlobalFree(hdrop);
    GlobalFree(pin);
    return PinDragResult::KeptInFence;
  }
  auto* source = new (std::nothrow) SimpleDropSource();
  if (!source) {
    data->Release();
    return PinDragResult::KeptInFence;
  }
  g_pin_transferred = false;
  g_desktop_unpin_requested = false;
  g_pin_drag_active = true;
  DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
  const HRESULT hr = DoDragDrop(data, source, effect, &effect);
  g_pin_drag_active = false;
  const bool desktop_unpin = g_desktop_unpin_requested;
  g_desktop_unpin_requested = false;
  const bool transferred = g_pin_transferred;
  source->Release();
  data->Release();
  if (transferred) {
    return PinDragResult::TransferredFence;
  }
  // Desktop/public release cancels OLE → unpin (float back on desktop).
  if (hr == DRAGDROP_S_CANCEL && desktop_unpin) {
    return PinDragResult::Unpinned;
  }
  // Successful DROP with MOVE: only unpin when the desktop path is actually
  // gone (Recycle / move-to-folder). Some apps advertise MOVE but leave the
  // file in place (WeChat-class) — keep the fence pin then.
  if (hr == DRAGDROP_S_DROP && (effect & DROPEFFECT_MOVE) != 0) {
    bool any_gone = false;
    for (const auto& p : paths) {
      if (p.empty()) {
        continue;
      }
      if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
        any_gone = true;
        break;
      }
    }
    if (any_gone) {
      return PinDragResult::Unpinned;
    }
  }
  return PinDragResult::KeptInFence;
}

PathDropTarget::PathDropTarget(DropFn on_drop, PinDropFn on_pin_drop)
    : on_drop_(std::move(on_drop)), on_pin_drop_(std::move(on_pin_drop)) {}

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

bool PathDropTarget::HasPinTransfer(IDataObject* data) const {
  if (!data) {
    return false;
  }
  FORMATETC fe{PinTransferFormat(), nullptr, DVASPECT_CONTENT, -1,
               TYMED_HGLOBAL};
  return data->QueryGetData(&fe) == S_OK;
}

bool PathDropTarget::ExtractPinTransfer(IDataObject* data, std::wstring* src,
                                        std::vector<std::wstring>* paths) const {
  if (!data || !src || !paths) {
    return false;
  }
  FORMATETC fe{PinTransferFormat(), nullptr, DVASPECT_CONTENT, -1,
               TYMED_HGLOBAL};
  STGMEDIUM stg{};
  if (FAILED(data->GetData(&fe, &stg)) || !stg.hGlobal) {
    return false;
  }
  const auto* buf = static_cast<const wchar_t*>(GlobalLock(stg.hGlobal));
  if (buf) {
    std::wstring s;
    const wchar_t* p = buf;
    while (*p && *p != L'\n') {
      s.push_back(*p++);
    }
    *src = s;
    if (*p == L'\n') {
      ++p;
    }
    while (*p) {
      std::wstring path;
      while (*p && *p != L'\n') {
        path.push_back(*p++);
      }
      if (!path.empty()) {
        paths->push_back(path);
      }
      if (*p == L'\n') {
        ++p;
      }
    }
    GlobalUnlock(stg.hGlobal);
  }
  ReleaseStgMedium(&stg);
  return !src->empty() && !paths->empty();
}

HRESULT STDMETHODCALLTYPE PathDropTarget::DragEnter(IDataObject* data,
                                                    DWORD /*key_state*/,
                                                    POINTL /*pt*/,
                                                    DWORD* effect) {
  g_over_pin_target = false;
  g_over_pin_invalid = false;
  pin_mode_ = false;
  accept_ = false;

  std::wstring src;
  std::vector<std::wstring> paths;
  if (on_pin_drop_ && ExtractPinTransfer(data, &src, &paths)) {
    pin_mode_ = true;
    const DWORD eff = on_pin_drop_(src, paths, /*commit=*/false);
    accept_ = (eff != DROPEFFECT_NONE);
    g_over_pin_target = true;
    g_over_pin_invalid = !accept_;
    if (effect) {
      *effect = accept_ ? eff : DROPEFFECT_NONE;
    }
    if (highlight_fn_) {
      highlight_fn_(accept_);
    }
    return S_OK;
  }

  accept_ = HasFileDrop(data);
  if (effect) {
    *effect = accept_ ? (*effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE))
                      : DROPEFFECT_NONE;
  }
  if (highlight_fn_) {
    highlight_fn_(accept_);
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PathDropTarget::DragOver(DWORD /*key_state*/,
                                                   POINTL /*pt*/,
                                                   DWORD* effect) {
  if (pin_mode_) {
    if (effect) {
      *effect = accept_ ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
    }
    return S_OK;
  }
  if (effect) {
    *effect = accept_ ? (*effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE))
                      : DROPEFFECT_NONE;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PathDropTarget::DragLeave() {
  g_over_pin_target = false;
  g_over_pin_invalid = false;
  accept_ = false;
  pin_mode_ = false;
  if (highlight_fn_) {
    highlight_fn_(false);
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PathDropTarget::Drop(IDataObject* data,
                                               DWORD /*key_state*/, POINTL /*pt*/,
                                               DWORD* effect) {
  if (pin_mode_) {
    std::wstring src;
    std::vector<std::wstring> paths;
    if (ExtractPinTransfer(data, &src, &paths)) {
      const DWORD eff = on_pin_drop_(src, paths, /*commit=*/true);
      if (eff != DROPEFFECT_NONE) {
        g_pin_transferred = true;
      }
      if (effect) {
        *effect = eff;
      }
    } else if (effect) {
      *effect = DROPEFFECT_NONE;
    }
    g_over_pin_target = false;
    g_over_pin_invalid = false;
    accept_ = false;
    pin_mode_ = false;
    if (highlight_fn_) {
      highlight_fn_(false);
    }
    return S_OK;
  }

  auto paths = ExtractPaths(data);
  if (effect) {
    *effect = paths.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
  }
  if (!paths.empty() && on_drop_) {
    on_drop_(paths);
  }
  accept_ = false;
  if (highlight_fn_) {
    highlight_fn_(false);
  }
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
