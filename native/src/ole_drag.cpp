#include "ole_drag.h"

#include <CommCtrl.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <ShObjIdl.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

namespace {

// Local copy of icon_actions::SameVolume — deskNote links ole_drag without
// icon_actions.obj, and PathDropTarget needs volume-aware MOVE/COPY.
bool SameVolumeLocal(const std::wstring& a, const std::wstring& b) {
  wchar_t va[MAX_PATH] = {};
  wchar_t vb[MAX_PATH] = {};
  if (!GetVolumePathNameW(a.c_str(), va, MAX_PATH)) {
    wcsncpy_s(va, a.c_str(), _TRUNCATE);
  }
  if (!GetVolumePathNameW(b.c_str(), vb, MAX_PATH)) {
    wcsncpy_s(vb, b.c_str(), _TRUNCATE);
  }
  return _wcsicmp(va, vb) == 0;
}

CLIPFORMAT PinTransferFormat() {
  static CLIPFORMAT fmt = 0;
  if (!fmt) {
    fmt = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"DeskTidyPinTransfer"));
  }
  return fmt;
}

PinDragMoveFn g_pin_move_fn;
std::function<bool(POINT)> g_public_sink_hit_fn;
bool g_last_drag_escaped = false;
bool g_over_pin_target = false;
bool g_over_pin_invalid = false;
bool g_pin_transferred = false;
// True while ShellDoDragPins runs — desktop release must NOT CF_HDROP into
// Explorer (files already live on the desktop → 「源相同」 + icon stack).
bool g_pin_drag_active = false;
// True while ShellDoDragFiles runs from the public float host. Same rule:
// releasing over desktop/public must cancel OLE (Fences/Explorer), not drop
// CF_HDROP onto the Desktop folder the files already live in.
bool g_float_drag_active = false;
// Set when QueryContinueDrag cancels because the cursor is over the desktop /
// public unpin zone (not Esc). Caller should unpin in that case only.
bool g_desktop_unpin_requested = false;

// Python start_virtual_item_drag: releasing over the real desktop / public
// float host unpins without OLE. Folder windows + Recycle Bin still get DROP.
bool IsPointOverDesktopUnpinZone(POINT pt);  // defined below
void MoveDragGhost(POINT cursor);  // defined below (ImageList_DragMove)
void BeginOleDragVisual(IDataObject* data,
                        const std::vector<std::wstring>& paths);
void EndOleDragVisual();

// PinDragOverDesktopUnpinZone is defined outside this anonymous namespace
// (see below) so other TUs can link it.

// The Recycle Bin desktop icon sits on the same DefView as the blank desktop,
// but it is a legitimate CF_HDROP target (delete). Detect it so the release
// over it is NOT cancelled as a "desktop unpin zone".
bool IsOverRecycleBinIcon(HWND list, POINT client_pt) {
  LVHITTESTINFO hit{};
  hit.pt = client_pt;
  const int item = ListView_HitTest(list, &hit);
  if (item < 0) {
    return false;
  }
  // Cache by item index: QueryContinueDrag runs on every mouse move, and this
  // did a cross-apartment SHGetDesktopFolder + GetDisplayNameOf per move.
  // The item under the cursor only changes when the hit test says so.
  static HWND s_cached_list = nullptr;
  static int s_cached_item = -1;
  static bool s_cached_recycle = false;
  if (list == s_cached_list && item == s_cached_item) {
    return s_cached_recycle;
  }
  LVITEMW lvi{};
  lvi.mask = LVIF_PARAM;
  lvi.iItem = item;
  if (!ListView_GetItem(list, &lvi) || !lvi.lParam) {
    return false;
  }
  auto* pidl = reinterpret_cast<PCUITEMID_CHILD>(lvi.lParam);
  IShellFolder* desktop = nullptr;
  if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop) {
    return false;
  }
  bool recycle = false;
  STRRET str{};
  if (SUCCEEDED(desktop->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &str))) {
    if (str.uType == STRRET_WSTR && str.pOleStr) {
      recycle = wcsstr(str.pOleStr,
                       L"645FF040-5081-101B-9F08-00AA002F954E") != nullptr;
      CoTaskMemFree(str.pOleStr);
    }
  }
  desktop->Release();
  s_cached_list = list;
  s_cached_item = item;
  s_cached_recycle = recycle;
  return recycle;
}

bool IsPointOverDesktopUnpinZone(POINT pt) {
  HWND hwnd = WindowFromPoint(pt);
  if (!hwnd) {
    return false;
  }
  for (HWND cur = hwnd; cur; cur = GetParent(cur)) {
    wchar_t cls[256] = {};
    GetClassNameW(cur, cls, 256);
    if (wcscmp(cls, L"SysListView32") == 0) {
      // Over a desktop icon — if it's the Recycle Bin, let the drop proceed.
      POINT cp = pt;
      ScreenToClient(cur, &cp);
      if (IsOverRecycleBinIcon(cur, cp)) {
        return false;
      }
    }
    if (wcscmp(cls, L"DeskTidyNativeFence") == 0 ||
        wcscmp(cls, L"DeskTidyNativePet") == 0 ||
        wcscmp(cls, L"DeskTidyNativePetBuoy") == 0 ||
        wcscmp(cls, L"DeskTidyNativePetPages") == 0) {
      return false;
    }
    if (wcscmp(cls, L"DeskTidyNativePublic") == 0) {
      if (g_public_sink_hit_fn && g_public_sink_hit_fn(pt)) {
        return false;
      }
      return true;
    }
    if (wcscmp(cls, L"DeskTidyNativeMarquee") == 0 ||
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
      g_last_drag_escaped = true;
      g_desktop_unpin_requested = false;
      return DRAGDROP_S_CANCEL;
    }
    // Explorer: left- or right-button drag; drop when neither button is down
    // (right-drag sinks show Copy/Move/Create-shortcut menu).
    if ((key_state & (MK_LBUTTON | MK_RBUTTON)) == 0) {
      // Pin / float drag released over desktop/public → cancel OLE so Explorer
      // never sees same-folder CF_HDROP (「源和目标文件名相同」). Pin caller
      // unpins; float caller keeps prior seats (no rewrite).
      if (g_pin_drag_active || g_float_drag_active) {
        POINT pt{};
        GetCursorPos(&pt);
        if (IsPointOverDesktopUnpinZone(pt)) {
          g_desktop_unpin_requested = g_pin_drag_active;
          return DRAGDROP_S_CANCEL;
        }
      }
      g_desktop_unpin_requested = false;
      return DRAGDROP_S_DROP;
    }
    POINT pt{};
    GetCursorPos(&pt);
    MoveDragGhost(pt);
    if (g_pin_move_fn) {
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

HRESULT DupHglobal(HGLOBAL src, STGMEDIUM* stg) {
  if (!src || !stg) {
    return E_INVALIDARG;
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

// IDragSourceHelper::InitializeFromBitmap needs SetData on the IDataObject.
// Without it, OLE falls back to per-file icon+label ghosts that stack into an
// unreadable blob on multi-select fence drags.
class ExtraMediumStore {
 public:
  ~ExtraMediumStore() { Clear(); }

  void Clear() {
    for (auto& e : items_) {
      ReleaseStgMedium(&e.stg);
    }
    items_.clear();
  }

  HRESULT Set(FORMATETC* fe, STGMEDIUM* stg, BOOL release) {
    if (!fe || !stg) {
      return E_INVALIDARG;
    }
    for (auto it = items_.begin(); it != items_.end();) {
      if (it->fe.cfFormat == fe->cfFormat && it->fe.dwAspect == fe->dwAspect &&
          it->fe.lindex == fe->lindex) {
        ReleaseStgMedium(&it->stg);
        it = items_.erase(it);
      } else {
        ++it;
      }
    }
    Entry e;
    e.fe = *fe;
    e.fe.ptd = nullptr;
    if (release) {
      // Take ownership of any tymed (DragImageBits may be HGLOBAL/ISTREAM/GDI).
      e.stg = *stg;
    } else if (stg->tymed == TYMED_HGLOBAL && stg->hGlobal) {
      e.stg = {};
      const HRESULT hr = DupHglobal(stg->hGlobal, &e.stg);
      if (FAILED(hr)) {
        return hr;
      }
    } else if (stg->tymed == TYMED_GDI && stg->hBitmap) {
      HBITMAP copy = static_cast<HBITMAP>(
          CopyImage(stg->hBitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
      if (!copy) {
        return E_OUTOFMEMORY;
      }
      e.stg.tymed = TYMED_GDI;
      e.stg.hBitmap = copy;
      e.stg.pUnkForRelease = nullptr;
    } else if (stg->tymed == TYMED_ISTREAM && stg->pstm) {
      e.stg.tymed = TYMED_ISTREAM;
      e.stg.pstm = stg->pstm;
      e.stg.pstm->AddRef();
      e.stg.pUnkForRelease = nullptr;
    } else {
      return DV_E_TYMED;
    }
    items_.push_back(e);
    return S_OK;
  }

  HRESULT Get(FORMATETC* fe, STGMEDIUM* stg) const {
    if (!fe || !stg) {
      return E_INVALIDARG;
    }
    for (const auto& e : items_) {
      if (e.fe.cfFormat != fe->cfFormat || e.fe.dwAspect != fe->dwAspect ||
          e.fe.lindex != fe->lindex) {
        continue;
      }
      if ((fe->tymed & e.stg.tymed) == 0) {
        return DV_E_TYMED;
      }
      if (e.stg.tymed == TYMED_HGLOBAL && e.stg.hGlobal) {
        return DupHglobal(e.stg.hGlobal, stg);
      }
      if (e.stg.tymed == TYMED_GDI && e.stg.hBitmap) {
        HBITMAP copy = static_cast<HBITMAP>(
            CopyImage(e.stg.hBitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
        if (!copy) {
          return E_OUTOFMEMORY;
        }
        stg->tymed = TYMED_GDI;
        stg->hBitmap = copy;
        stg->pUnkForRelease = nullptr;
        return S_OK;
      }
      if (e.stg.tymed == TYMED_ISTREAM && e.stg.pstm) {
        stg->tymed = TYMED_ISTREAM;
        stg->pstm = e.stg.pstm;
        stg->pstm->AddRef();
        stg->pUnkForRelease = nullptr;
        return S_OK;
      }
      if (e.stg.pUnkForRelease) {
        *stg = e.stg;
        e.stg.pUnkForRelease->AddRef();
        return S_OK;
      }
      return DV_E_FORMATETC;
    }
    return DV_E_FORMATETC;
  }

  HRESULT Query(FORMATETC* fe) const {
    if (!fe) {
      return E_INVALIDARG;
    }
    for (const auto& e : items_) {
      if (e.fe.cfFormat == fe->cfFormat && e.fe.dwAspect == fe->dwAspect &&
          e.fe.lindex == fe->lindex && (fe->tymed & e.stg.tymed)) {
        return S_OK;
      }
    }
    return DV_E_FORMATETC;
  }

 private:
  struct Entry {
    FORMATETC fe{};
    STGMEDIUM stg{};
  };
  std::vector<Entry> items_;
};

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
    if (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) {
      return DupHglobal(hdrop_, stg);
    }
    return extras_.Get(fe, stg);
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
    return extras_.Query(fe);
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,
                                                  FORMATETC* out) override {
    if (out) {
      out->ptd = nullptr;
    }
    return DATA_S_SAMEFORMATETC;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC* fe, STGMEDIUM* stg,
                                    BOOL release) override {
    return extras_.Set(fe, stg, release);
  }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir,
                                          IEnumFORMATETC** enum_fe) override {
    if (!enum_fe) {
      return E_POINTER;
    }
    *enum_fe = nullptr;
    // IDragSourceHelper probes DATADIR_SET; empty enum is enough for SetData.
    if (dir == DATADIR_SET) {
      return SHCreateStdEnumFmtEtc(0, nullptr, enum_fe);
    }
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
  ExtraMediumStore extras_;
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
    if (!fe || !(fe->tymed & TYMED_HGLOBAL)) {
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
    if (src) {
      return DupHglobal(src, stg);
    }
    return extras_.Get(fe, stg);
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
    if (!fe) {
      return E_INVALIDARG;
    }
    if (HandleFor(fe)) {
      return S_OK;
    }
    return extras_.Query(fe);
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*,
                                                  FORMATETC* out) override {
    if (out) {
      out->ptd = nullptr;
    }
    return DATA_S_SAMEFORMATETC;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC* fe, STGMEDIUM* stg,
                                    BOOL release) override {
    return extras_.Set(fe, stg, release);
  }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir,
                                          IEnumFORMATETC** enum_fe) override {
    if (!enum_fe) {
      return E_POINTER;
    }
    *enum_fe = nullptr;
    if (dir == DATADIR_SET) {
      return SHCreateStdEnumFmtEtc(0, nullptr, enum_fe);
    }
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
  ExtraMediumStore extras_;
};

// Fences / Explorer multi-drag: icon stack only (never filenames).
// Win11 synthesizes icon+label ghosts from CF_HDROP on custom IDataObjects;
// that path ignores a failed IDragSourceHelper. Use a real shell IDataObject
// and replace its drag image via InitializeFromWindow (DI_GETDRAGIMAGE).
constexpr COLORREF kDragKey = RGB(255, 0, 255);
constexpr wchar_t kDragImgClass[] = L"DeskTidyDragImageSrc";
HWND g_drag_img_hwnd = nullptr;
UINT g_msg_get_drag_image = 0;
std::vector<std::wstring> g_drag_img_paths;
HBITMAP g_drag_img_bmp = nullptr;  // owned until next BeginOleDragVisual
int g_drag_img_w = 0;
int g_drag_img_h = 0;
int g_drag_hot_x = 0;
int g_drag_hot_y = 0;

HBITMAP BuildIconStackBitmap(const std::vector<std::wstring>& paths, int* out_w,
                             int* out_h, int* out_hot_x, int* out_hot_y) {
  if (paths.empty() || !out_w || !out_h || !out_hot_x || !out_hot_y) {
    return nullptr;
  }
  const int stack_n = (std::min)(3, static_cast<int>(paths.size()));
  const int total_n = static_cast<int>(paths.size());
  const int icon = GetSystemMetrics(SM_CXICON);
  const int gap = (std::max)(6, icon / 8);
  // Explorer/Fences: multi-drag shows a count badge (+N / N).
  const int badge = (total_n > 1) ? (std::max)(18, icon / 3) : 0;
  const int w = icon + (stack_n - 1) * gap + (badge ? badge / 3 : 0);
  const int h = icon + (stack_n - 1) * gap + (badge ? badge / 3 : 0);
  *out_w = w;
  *out_h = h;
  *out_hot_x = icon / 2;
  *out_hot_y = icon / 2;

  HDC screen = GetDC(nullptr);
  // Compatible bitmap (not DIBSection): required by IDragSourceHelper samples.
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  if (!bmp) {
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return nullptr;
  }
  HGDIOBJ old = SelectObject(mem, bmp);
  RECT fill{0, 0, w, h};
  HBRUSH brush = CreateSolidBrush(kDragKey);
  FillRect(mem, &fill, brush);
  DeleteObject(brush);
  for (int i = 0; i < stack_n; ++i) {
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(paths[static_cast<size_t>(i)].c_str(), 0, &sfi,
                        sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) ||
        !sfi.hIcon) {
      continue;
    }
    DrawIconEx(mem, i * gap, i * gap, sfi.hIcon, icon, icon, 0, nullptr,
               DI_NORMAL);
    DestroyIcon(sfi.hIcon);
  }
  if (badge > 0) {
    const int bx = w - badge;
    const int by = h - badge;
    HBRUSH blue = CreateSolidBrush(RGB(0, 120, 215));
    HBRUSH old_br = static_cast<HBRUSH>(SelectObject(mem, blue));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ old_pen = SelectObject(mem, pen);
    Ellipse(mem, bx, by, bx + badge, by + badge);
    SelectObject(mem, old_pen);
    SelectObject(mem, old_br);
    DeleteObject(pen);
    DeleteObject(blue);
    wchar_t label[16] = {};
    if (total_n > 9) {
      swprintf_s(label, L"%d", total_n);
    } else {
      swprintf_s(label, L"%d", total_n);
    }
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    HFONT font =
        CreateFontW(-(std::max)(10, badge * 2 / 3), 0, 0, 0, FW_BOLD, FALSE,
                    FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ old_font = font ? SelectObject(mem, font) : nullptr;
    RECT tr{bx, by, bx + badge, by + badge};
    DrawTextW(mem, label, -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    if (old_font) {
      SelectObject(mem, old_font);
    }
    if (font) {
      DeleteObject(font);
    }
  }
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return bmp;
}

LRESULT CALLBACK DragImageSrcWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  if (g_msg_get_drag_image && msg == g_msg_get_drag_image) {
    auto* di = reinterpret_cast<SHDRAGIMAGE*>(l);
    if (!di) {
      return FALSE;
    }
    if (g_drag_img_bmp) {
      DeleteObject(g_drag_img_bmp);
      g_drag_img_bmp = nullptr;
    }
    g_drag_img_bmp = BuildIconStackBitmap(g_drag_img_paths, &g_drag_img_w,
                                          &g_drag_img_h, &g_drag_hot_x,
                                          &g_drag_hot_y);
    if (!g_drag_img_bmp) {
      return FALSE;
    }
    di->sizeDragImage = {g_drag_img_w, g_drag_img_h};
    di->ptOffset = {g_drag_hot_x, g_drag_hot_y};
    di->hbmpDragImage = g_drag_img_bmp;
    di->crColorKey = kDragKey;
    // Ownership transferred to the drag-image manager.
    g_drag_img_bmp = nullptr;
    return TRUE;
  }
  return DefWindowProcW(hwnd, msg, w, l);
}

HWND EnsureDragImageSrcWindow() {
  if (g_drag_img_hwnd) {
    return g_drag_img_hwnd;
  }
  if (!g_msg_get_drag_image) {
    g_msg_get_drag_image = RegisterWindowMessage(DI_GETDRAGIMAGE);
  }
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DragImageSrcWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kDragImgClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
  g_drag_img_hwnd =
      CreateWindowExW(WS_EX_TOOLWINDOW, kDragImgClass, L"", WS_POPUP, 0, 0, 8,
                      8, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  return g_drag_img_hwnd;
}

bool AttachIconOnlyShellDragImage(IDataObject* data,
                                  const std::vector<std::wstring>& paths) {
  if (!data || paths.empty()) {
    return false;
  }
  g_drag_img_paths = paths;
  HWND hwnd = EnsureDragImageSrcWindow();
  IDragSourceHelper* helper = nullptr;
  if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&helper))) ||
      !helper) {
    return false;
  }
  bool ok = false;
  if (hwnd) {
    POINT pt{g_drag_hot_x, g_drag_hot_y};
    // Prefer InitializeFromWindow — DI_GETDRAGIMAGE supplies icons-only.
    // Pre-build hot-spot using a throwaway bitmap size probe.
    int w = 0;
    int h = 0;
    HBITMAP probe =
        BuildIconStackBitmap(paths, &w, &h, &g_drag_hot_x, &g_drag_hot_y);
    if (probe) {
      DeleteObject(probe);
    }
    pt = {g_drag_hot_x, g_drag_hot_y};
    ok = SUCCEEDED(helper->InitializeFromWindow(hwnd, &pt, data));
  }
  if (!ok) {
    int w = 0;
    int h = 0;
    int hx = 0;
    int hy = 0;
    HBITMAP bmp = BuildIconStackBitmap(paths, &w, &h, &hx, &hy);
    if (bmp) {
      SHDRAGIMAGE di{};
      di.sizeDragImage = {w, h};
      di.ptOffset = {hx, hy};
      di.hbmpDragImage = bmp;
      di.crColorKey = kDragKey;
      ok = SUCCEEDED(helper->InitializeFromBitmap(&di, data));
      if (!ok) {
        DeleteObject(bmp);
      }
    }
  }
  helper->Release();
  return ok;
}

void MoveDragGhost(POINT /*cursor*/) {
  // Shell drag-image manager tracks the cursor; nothing to move.
}

void BeginOleDragVisual(IDataObject* data,
                        const std::vector<std::wstring>& paths) {
  AttachIconOnlyShellDragImage(data, paths);
}

void EndOleDragVisual() {
  g_drag_img_paths.clear();
  if (g_drag_img_bmp) {
    DeleteObject(g_drag_img_bmp);
    g_drag_img_bmp = nullptr;
  }
}

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

CLIPFORMAT PreferredDropEffectFormat() {
  static CLIPFORMAT fmt = 0;
  if (!fmt) {
    fmt = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
  }
  return fmt;
}

bool AttachPreferredDropEffect(IDataObject* data, bool cut) {
  if (!data) {
    return false;
  }
  const CLIPFORMAT fmt = PreferredDropEffectFormat();
  if (!fmt) {
    return false;
  }
  HGLOBAL mem = GlobalAlloc(GHND, sizeof(DWORD));
  if (!mem) {
    return false;
  }
  auto* pe = static_cast<DWORD*>(GlobalLock(mem));
  if (!pe) {
    GlobalFree(mem);
    return false;
  }
  *pe = cut ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
  GlobalUnlock(mem);
  FORMATETC fe{fmt, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM stg{};
  stg.tymed = TYMED_HGLOBAL;
  stg.hGlobal = mem;
  if (FAILED(data->SetData(&fe, &stg, TRUE))) {
    GlobalFree(mem);
    return false;
  }
  return true;
}

bool OleSetClipboardFilesImpl(const std::vector<std::wstring>& paths,
                              bool cut) {
  if (paths.empty()) {
    return false;
  }
  IDataObject* data = CreateShellDataObject(paths);
  if (!data) {
    return false;
  }
  // Cut ghost / paste MOVE: Explorer reads Preferred DropEffect from OLE.
  // Shell BindToHandler objects accept SetData; HdropDataObject stores via
  // ExtraMediumStore. Failure still leaves a usable CF_HDROP copy clipboard.
  AttachPreferredDropEffect(data, cut);
  const HRESULT hr = OleSetClipboard(data);
  data->Release();
  return SUCCEEDED(hr);
}

}  // namespace

bool ShellOleSetClipboardFiles(const std::vector<std::wstring>& paths,
                               bool cut) {
  return OleSetClipboardFilesImpl(paths, cut);
}

bool PinDragOverDesktopUnpinZone(POINT pt) {
  return IsPointOverDesktopUnpinZone(pt);
}

bool ShellDoDragFiles(const std::vector<std::wstring>& paths,
                      bool cancel_desktop_self_drop) {
  IDataObject* data = CreateShellDataObject(paths);
  if (!data) {
    return false;
  }
  auto* source = new (std::nothrow) SimpleDropSource();
  if (!source) {
    data->Release();
    return false;
  }
  // AIGC START
  g_last_drag_escaped = false;
  g_float_drag_active = cancel_desktop_self_drop;
  BeginOleDragVisual(data, paths);
  DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
  const HRESULT hr = DoDragDrop(data, source, effect, &effect);
  EndOleDragVisual();
  g_float_drag_active = false;
  // AIGC END
  source->Release();
  data->Release();
  return hr == DRAGDROP_S_DROP;
}

void SetPinDragMoveFn(PinDragMoveFn fn) { g_pin_move_fn = std::move(fn); }
bool LastShellDragWasEscaped() { return g_last_drag_escaped; }
void SetPublicDropSinkHitFn(std::function<bool(POINT)> fn) {
  g_public_sink_hit_fn = std::move(fn);
}

PinDragResult ShellDoDragPins(const std::vector<std::wstring>& paths,
                              const std::wstring& src_fence_id) {
  if (paths.empty()) {
    return PinDragResult::KeptInFence;
  }
  HGLOBAL pin = BuildPinPayload(src_fence_id, paths);
  if (!pin) {
    return PinDragResult::KeptInFence;
  }

  // Prefer shell IDataObject so Win11 drag-image manager honors
  // IDragSourceHelper (custom CF_HDROP objects get auto icon+label ghosts).
  IDataObject* data = CreateShellDataObject(paths);
  if (data) {
    FORMATETC fe{PinTransferFormat(), nullptr, DVASPECT_CONTENT, -1,
                 TYMED_HGLOBAL};
    STGMEDIUM stg{};
    stg.tymed = TYMED_HGLOBAL;
    stg.hGlobal = pin;
    if (FAILED(data->SetData(&fe, &stg, TRUE))) {
      data->Release();
      data = nullptr;
    } else {
      pin = nullptr;
    }
  }
  if (!data) {
    HGLOBAL hdrop = BuildHdrop(paths);
    if (!hdrop) {
      if (pin) {
        GlobalFree(pin);
      }
      return PinDragResult::KeptInFence;
    }
    if (!pin) {
      pin = BuildPinPayload(src_fence_id, paths);
    }
    if (!pin) {
      GlobalFree(hdrop);
      return PinDragResult::KeptInFence;
    }
    data = new (std::nothrow) PinTransferDataObject(hdrop, pin);
    if (!data) {
      GlobalFree(hdrop);
      GlobalFree(pin);
      return PinDragResult::KeptInFence;
    }
    pin = nullptr;
  }

  auto* source = new (std::nothrow) SimpleDropSource();
  if (!source) {
    data->Release();
    return PinDragResult::KeptInFence;
  }
  g_pin_transferred = false;
  g_desktop_unpin_requested = false;
  g_pin_drag_active = true;
  BeginOleDragVisual(data, paths);
  DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
  const HRESULT hr = DoDragDrop(data, source, effect, &effect);
  EndOleDragVisual();
  g_pin_drag_active = false;
  const bool desktop_unpin = g_desktop_unpin_requested;
  g_desktop_unpin_requested = false;
  const bool transferred = g_pin_transferred;
  source->Release();
  data->Release();
  // Desktop/public release cancels OLE → unpin (float back on desktop).
  if (hr == DRAGDROP_S_CANCEL && desktop_unpin) {
    return PinDragResult::Unpinned;
  }
  // Successful DROP with MOVE: prefer Unpinned when the path is gone
  // (Recycle Bin / move-to-folder), even if the sink also flagged a pin
  // transfer — otherwise drop-on-hosted-bin looks like「拖进回收站」and
  // leaves a stale pin.
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
  if (transferred) {
    return PinDragResult::TransferredFence;
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

FileDropSemantics PathDropTarget::ActiveSemantics() const {
  return semantics_fn_ ? semantics_fn_() : semantics_;
}

std::wstring PathDropTarget::ActiveDestDir() const {
  return dest_fn_ ? dest_fn_() : dest_dir_;
}

DWORD PathDropTarget::ResolveFileEffect(DWORD key_state, DWORD allowed,
                                        const std::wstring& sample_src) const {
  // AIGC START
  // Fences/Explorer effect matrix for CF_HDROP sinks.
  allowed &= (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
  if (!allowed) {
    return DROPEFFECT_NONE;
  }
  switch (ActiveSemantics()) {
    case FileDropSemantics::VirtualPin: {
      // Pin-only never MOVE (would delete source). Still honor Alt /
      // Ctrl+Shift → LINK so the cursor matches Fences and DropAt can
      // land a Desktop .lnk then pin it.
      const bool ctrl = (key_state & MK_CONTROL) != 0;
      const bool shift = (key_state & MK_SHIFT) != 0;
      const bool alt = (key_state & MK_ALT) != 0;
      if (((ctrl && shift) || alt) && (allowed & DROPEFFECT_LINK)) {
        return DROPEFFECT_LINK;
      }
      return (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY
                                         : DROPEFFECT_NONE;
    }
    case FileDropSemantics::PreferMove:
      if (allowed & DROPEFFECT_MOVE) {
        return DROPEFFECT_MOVE;
      }
      return (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    case FileDropSemantics::Physical:
    default:
      break;
  }
  const bool ctrl = (key_state & MK_CONTROL) != 0;
  const bool shift = (key_state & MK_SHIFT) != 0;
  const bool alt = (key_state & MK_ALT) != 0;
  if ((ctrl && shift) || alt) {
    if (allowed & DROPEFFECT_LINK) {
      return DROPEFFECT_LINK;
    }
  }
  if (ctrl && (allowed & DROPEFFECT_COPY)) {
    return DROPEFFECT_COPY;
  }
  if (shift && (allowed & DROPEFFECT_MOVE)) {
    return DROPEFFECT_MOVE;
  }
  const std::wstring dest = ActiveDestDir();
  if (!dest.empty() && !sample_src.empty() &&
      SameVolumeLocal(sample_src, dest) && (allowed & DROPEFFECT_MOVE)) {
    return DROPEFFECT_MOVE;
  }
  if (allowed & DROPEFFECT_COPY) {
    return DROPEFFECT_COPY;
  }
  if (allowed & DROPEFFECT_MOVE) {
    return DROPEFFECT_MOVE;
  }
  return DROPEFFECT_NONE;
  // AIGC END
}

void PathDropTarget::NotifyDragOver(POINTL pt, bool active) {
  if (drag_over_fn_) {
    drag_over_fn_(pt, active);
  }
}

HRESULT STDMETHODCALLTYPE PathDropTarget::DragEnter(IDataObject* data,
                                                    DWORD key_state,
                                                    POINTL pt,
                                                    DWORD* effect) {
  g_over_pin_target = false;
  g_over_pin_invalid = false;
  pin_mode_ = false;
  accept_ = false;
  sample_src_.clear();
  pin_src_.clear();
  pin_paths_.clear();

  std::wstring src;
  std::vector<std::wstring> paths;
  if (on_pin_drop_ && ExtractPinTransfer(data, &src, &paths)) {
    pin_mode_ = true;
    pin_src_ = src;
    pin_paths_ = paths;
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
    NotifyDragOver(pt, accept_);
    return S_OK;
  }

  accept_ = HasFileDrop(data);
  if (accept_) {
    const auto dropped = ExtractPaths(data);
    if (!dropped.empty()) {
      sample_src_ = dropped.front();
    }
  }
  if (effect) {
    const DWORD allowed =
        effect ? (*effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK))
               : (DROPEFFECT_COPY | DROPEFFECT_MOVE);
    *effect = accept_ ? ResolveFileEffect(key_state, allowed, sample_src_)
                      : DROPEFFECT_NONE;
  }
  if (highlight_fn_) {
    highlight_fn_(accept_);
  }
  NotifyDragOver(pt, accept_);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PathDropTarget::DragOver(DWORD key_state, POINTL pt,
                                                   DWORD* effect) {
  if (pin_mode_) {
    // Re-query so hovering the hosted Recycle Bin can accept even when the
    // source fence == dest (src==dst transfer is otherwise rejected).
    if (on_pin_drop_) {
      const DWORD eff =
          on_pin_drop_(pin_src_, pin_paths_, /*commit=*/false);
      accept_ = (eff != DROPEFFECT_NONE);
      g_over_pin_target = true;
      g_over_pin_invalid = !accept_;
      if (effect) {
        *effect = accept_ ? eff : DROPEFFECT_NONE;
      }
    } else if (effect) {
      *effect = accept_ ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
    }
    NotifyDragOver(pt, accept_);
    return S_OK;
  }
  if (effect) {
    const DWORD allowed =
        *effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    *effect = accept_ ? ResolveFileEffect(key_state, allowed, sample_src_)
                      : DROPEFFECT_NONE;
  }
  NotifyDragOver(pt, accept_);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PathDropTarget::DragLeave() {
  g_over_pin_target = false;
  g_over_pin_invalid = false;
  accept_ = false;
  pin_mode_ = false;
  sample_src_.clear();
  pin_src_.clear();
  pin_paths_.clear();
  if (highlight_fn_) {
    highlight_fn_(false);
  }
  NotifyDragOver(POINTL{}, false);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE PathDropTarget::Drop(IDataObject* data,
                                               DWORD key_state, POINTL pt,
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
    sample_src_.clear();
    pin_src_.clear();
    pin_paths_.clear();
    if (highlight_fn_) {
      highlight_fn_(false);
    }
    NotifyDragOver(pt, false);
    return S_OK;
  }

  auto paths = ExtractPaths(data);
  const std::wstring sample = paths.empty() ? sample_src_ : paths.front();
  const DWORD allowed =
      effect ? (*effect & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK))
             : (DROPEFFECT_COPY | DROPEFFECT_MOVE);
  const DWORD proposed =
      paths.empty() ? DROPEFFECT_NONE
                    : ResolveFileEffect(key_state, allowed, sample);
  // AIGC START
  // Ask the sink what actually happened — never advertise MOVE/COPY if the
  // handler no-oped (desktop self-drop skip, recycle fail, portal import fail).
  DWORD actual = proposed;
  if (!paths.empty() && on_drop_) {
    actual = on_drop_(paths, proposed);
  } else if (paths.empty()) {
    actual = DROPEFFECT_NONE;
  }
  if (effect) {
    *effect = actual;
  }
  // AIGC END
  accept_ = false;
  sample_src_.clear();
  if (highlight_fn_) {
    highlight_fn_(false);
  }
  NotifyDragOver(pt, false);
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
