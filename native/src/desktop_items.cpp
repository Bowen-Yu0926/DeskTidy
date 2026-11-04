#include "desktop_items.h"

#include "namespace_icons.h"

#include <CommCtrl.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <Shobjidl.h>
#include <commoncontrols.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <objbase.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

#ifndef SHIL_LARGE
#define SHIL_LARGE 0x0
#endif
#ifndef SHIL_SMALL
#define SHIL_SMALL 0x1
#endif
#ifndef SHIL_EXTRALARGE
#define SHIL_EXTRALARGE 0x2
#endif
#ifndef SHIL_SYSSMALL
#define SHIL_SYSSMALL 0x3
#endif
#ifndef SHIL_JUMBO
#define SHIL_JUMBO 0x4
#endif

bool EndsWithIgnoreCase(const std::wstring& s, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  if (s.size() < n) {
    return false;
  }
  return _wcsicmp(s.c_str() + (s.size() - n), suffix) == 0;
}

// Office lock/temp files (~$foo.docx) — matched by Python is_temp_desktop_file.
bool IsOfficeTempName(const wchar_t* name) {
  return name[0] == L'~' && name[1] == L'$';
}

// 2.5s scan cache (Python _SCAN_CACHE_TTL_S).
constexpr ULONGLONG kScanCacheTtlMs = 2500;
ULONGLONG g_scan_tick = 0;
std::vector<DesktopItem> g_scan_cache;

void AppendFolder(const std::wstring& folder, std::vector<DesktopItem>* out) {
  if (folder.empty()) {
    return;
  }
  const std::wstring pattern = folder + L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == L'\0' ||
         (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
      continue;
    }
    if (IsOfficeTempName(fd.cFileName)) {
      continue;
    }
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) {
      continue;
    }
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
          !EndsWithIgnoreCase(fd.cFileName, L".lnk")) {
        continue;
      }
    }
    DesktopItem item;
    item.path = folder + L"\\" + fd.cFileName;
    item.name = fd.cFileName;
    item.is_link = EndsWithIgnoreCase(item.name, L".lnk") ||
                   EndsWithIgnoreCase(item.name, L".url");
    // SHGetFileInfo(DISPLAYNAME) is ~1–5ms per call. Ordinary files keep the
    // FindFirst name (Fences-class); only resolve shell display names for
    // shortcuts / internet shortcuts where the caption differs from the file.
    if (item.is_link) {
      if (EndsWithIgnoreCase(item.name, L".lnk") && item.name.size() > 4) {
        item.name.resize(item.name.size() - 4);
      } else if (EndsWithIgnoreCase(item.name, L".url") &&
                 item.name.size() > 4) {
        item.name.resize(item.name.size() - 4);
      }
      SHFILEINFOW sfi{};
      if (SHGetFileInfoW(item.path.c_str(), 0, &sfi, sizeof(sfi),
                         SHGFI_DISPLAYNAME) &&
          sfi.szDisplayName[0] != L'\0') {
        item.name = sfi.szDisplayName;
      }
    }
    out->push_back(std::move(item));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

std::wstring ScanDedupKey(const std::wstring& path) {
  std::wstring key = path;
  for (auto& c : key) {
    if (c == L'/') {
      c = L'\\';
    } else if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  return key;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) {
    return {};
  }
  std::wstring out = p;
  CoTaskMemFree(p);
  return out;
}

HICON IconFromImageList(int shil, int index) {
  IImageList* list = nullptr;
  if (FAILED(SHGetImageList(shil, IID_IImageList,
                            reinterpret_cast<void**>(&list))) ||
      !list) {
    return nullptr;
  }
  HICON icon = nullptr;
  list->GetIcon(index, ILD_TRANSPARENT, &icon);
  list->Release();
  return icon;
}

// Explorer-class extract: IShellItemImageFactory picks the right .ico frame for
// the requested pixel size (avoids Jumbo→DrawIconEx→GDI+ soft downscale).
HICON IconFromShellItemFactory(const std::wstring& path, int size_px) {
  if (path.empty() || size_px <= 0) {
    return nullptr;
  }
  IShellItem* item = nullptr;
  HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr,
                                           IID_PPV_ARGS(&item));
  if (FAILED(hr) || !item) {
    return nullptr;
  }
  IShellItemImageFactory* factory = nullptr;
  hr = item->QueryInterface(IID_PPV_ARGS(&factory));
  item->Release();
  if (FAILED(hr) || !factory) {
    return nullptr;
  }
  // Exact cell size via the shell's own scaler (Explorer parity). ICONONLY
  // avoids document thumbnails. Fall back to BIGGERSIZEOK if the exact size
  // cannot be produced (rare).
  const int req = (std::max)(16, (std::min)(256, size_px));
  SIZE sz{req, req};
  HBITMAP hbmp = nullptr;
  hr = factory->GetImage(sz, SIIGBF_ICONONLY, &hbmp);
  if (FAILED(hr) || !hbmp) {
    hr = factory->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hbmp);
  }
  factory->Release();
  if (FAILED(hr) || !hbmp) {
    return nullptr;
  }
  BITMAP bm{};
  if (GetObjectW(hbmp, sizeof(bm), &bm) != sizeof(bm) || bm.bmWidth <= 0) {
    DeleteObject(hbmp);
    return nullptr;
  }
  HBITMAP mask = CreateBitmap(bm.bmWidth, bm.bmHeight, 1, 1, nullptr);
  if (!mask) {
    DeleteObject(hbmp);
    return nullptr;
  }
  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = hbmp;
  ii.hbmMask = mask;
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(hbmp);
  DeleteObject(mask);
  return icon;
}

// Snap the continuous icon_zoom (70%..160%) to a fixed set of standard sizes,
// so the process-wide cache cannot grow unboundedly as the zoom changes.
int IconSizeBucket(int size_px) {
  static constexpr int kBuckets[] = {16,  20,  24,  28,  32,  40,  48,
                                     56,  64,  80,  96,  128, 192, 256};
  int best = kBuckets[0];
  int best_d = size_px > best ? size_px - best : best - size_px;
  for (int b : kBuckets) {
    const int d = size_px > b ? size_px - b : b - size_px;
    if (d < best_d) {
      best_d = d;
      best = b;
    }
  }
  return best;
}

// Process-wide master icon cache: maps "lowerpath@bucket" -> already-trimmed
// master HICON. Values live for the process lifetime; callers only ever receive
// a CopyIcon() clone and keep DestroyIcon() ownership semantics unchanged.
// Mutex-guarded so PreloadIconsParallel can fill it from worker threads.
std::unordered_map<std::wstring, HICON> g_master_icon_cache;
std::mutex g_master_icon_cache_mutex;

void DestroyMasterIconCache() {
  std::lock_guard<std::mutex> lk(g_master_icon_cache_mutex);
  for (auto& kv : g_master_icon_cache) {
    if (kv.second) {
      DestroyIcon(kv.second);
    }
  }
  g_master_icon_cache.clear();
}

}  // namespace

std::vector<DesktopItem> ScanDesktopItems() {
  const ULONGLONG now = GetTickCount64();
  if (!g_scan_cache.empty() && now - g_scan_tick < kScanCacheTtlMs) {
    return g_scan_cache;
  }

  std::vector<DesktopItem> items;
  AppendFolder(KnownFolder(FOLDERID_Desktop), &items);
  AppendFolder(KnownFolder(FOLDERID_PublicDesktop), &items);
  // Hosted This PC / Recycle Bin shortcuts (Python .public_system).
  AppendPublicSystemItems(&items);

  std::sort(items.begin(), items.end(),
            [](const DesktopItem& a, const DesktopItem& b) {
              return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
            });
  // O(n) path dedupe (was nested O(n²) _wcsicmp walk).
  std::unordered_set<std::wstring> seen;
  seen.reserve(items.size() * 2 + 8);
  std::vector<DesktopItem> unique;
  unique.reserve(items.size());
  for (auto& it : items) {
    if (!seen.insert(ScanDedupKey(it.path)).second) {
      continue;
    }
    unique.push_back(std::move(it));
  }
  g_scan_cache = unique;
  g_scan_tick = now;
  return unique;
}

void InvalidateDesktopScanCache() {
  g_scan_cache.clear();
  g_scan_tick = 0;
  DestroyMasterIconCache();
}

void ClearItemIconCache() {
  DestroyMasterIconCache();
}

// P2-18: crop sparse shell jumbo padding (Python _trim_icon_padding).
HICON TrimIconTransparentPadding(HICON src) {
  if (!src) {
    return nullptr;
  }
  ICONINFO ii{};
  if (!GetIconInfo(src, &ii)) {
    return src;
  }
  BITMAP bm{};
  if (!ii.hbmColor || !GetObjectW(ii.hbmColor, sizeof(bm), &bm) ||
      bm.bmWidth < 48 || bm.bmHeight < 48) {
    if (ii.hbmColor) {
      DeleteObject(ii.hbmColor);
    }
    if (ii.hbmMask) {
      DeleteObject(ii.hbmMask);
    }
    return src;
  }
  const int w = bm.bmWidth;
  const int h = bm.bmHeight;
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  std::vector<BYTE> bits(static_cast<size_t>(w) * h * 4);
  HDC hdc = GetDC(nullptr);
  const int got =
      GetDIBits(hdc, ii.hbmColor, 0, h, bits.data(), &bmi, DIB_RGB_COLORS);
  ReleaseDC(nullptr, hdc);
  if (got != h) {
    DeleteObject(ii.hbmColor);
    if (ii.hbmMask) {
      DeleteObject(ii.hbmMask);
    }
    return src;
  }
  constexpr BYTE kAlphaMin = 16;
  int minx = w, miny = h, maxx = -1, maxy = -1;
  for (int y = 0; y < h; ++y) {
    const BYTE* row = bits.data() + static_cast<size_t>(y) * w * 4;
    for (int x = 0; x < w; ++x) {
      if (row[x * 4 + 3] > kAlphaMin) {
        if (x < minx) {
          minx = x;
        }
        if (x > maxx) {
          maxx = x;
        }
        if (y < miny) {
          miny = y;
        }
        if (y > maxy) {
          maxy = y;
        }
      }
    }
  }
  if (maxx < 0) {
    DeleteObject(ii.hbmColor);
    if (ii.hbmMask) {
      DeleteObject(ii.hbmMask);
    }
    return src;
  }
  const int bw = maxx - minx + 1;
  const int bh = maxy - miny + 1;
  const int canvas = w * h;
  if (bw * bh >= canvas * 0.40 && bw >= w * 7 / 10 && bh >= h * 7 / 10) {
    DeleteObject(ii.hbmColor);
    if (ii.hbmMask) {
      DeleteObject(ii.hbmMask);
    }
    return src;
  }
  const int margin = 1;
  const int x0 = (std::max)(0, minx - margin);
  const int y0 = (std::max)(0, miny - margin);
  const int x1 = (std::min)(w, maxx + 1 + margin);
  const int y1 = (std::min)(h, maxy + 1 + margin);
  const int cw = (std::max)(1, x1 - x0);
  const int ch = (std::max)(1, y1 - y0);

  BITMAPINFO out_bmi{};
  out_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  out_bmi.bmiHeader.biWidth = cw;
  out_bmi.bmiHeader.biHeight = -ch;
  out_bmi.bmiHeader.biPlanes = 1;
  out_bmi.bmiHeader.biBitCount = 32;
  out_bmi.bmiHeader.biCompression = BI_RGB;
  void* out_bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(screen, &out_bmi, DIB_RGB_COLORS, &out_bits,
                                   nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!color || !out_bits) {
    if (color) {
      DeleteObject(color);
    }
    DeleteObject(ii.hbmColor);
    if (ii.hbmMask) {
      DeleteObject(ii.hbmMask);
    }
    return src;
  }
  auto* dst = static_cast<BYTE*>(out_bits);
  for (int y = 0; y < ch; ++y) {
    const BYTE* src_row =
        bits.data() + static_cast<size_t>(y0 + y) * w * 4 + x0 * 4;
    BYTE* dst_row = dst + static_cast<size_t>(y) * cw * 4;
    memcpy(dst_row, src_row, static_cast<size_t>(cw) * 4);
  }
  // 1-bit mask from alpha.
  const int mask_stride = ((cw + 31) / 32) * 4;
  std::vector<BYTE> mask_bits(static_cast<size_t>(mask_stride) * ch, 0xFF);
  for (int y = 0; y < ch; ++y) {
    const BYTE* row = dst + static_cast<size_t>(y) * cw * 4;
    for (int x = 0; x < cw; ++x) {
      if (row[x * 4 + 3] > kAlphaMin) {
        const int byte_i = x / 8;
        const int bit = 7 - (x % 8);
        mask_bits[static_cast<size_t>(y) * mask_stride + byte_i] &=
            static_cast<BYTE>(~(1 << bit));
      }
    }
  }
  HBITMAP mask = CreateBitmap(cw, ch, 1, 1, mask_bits.data());
  DeleteObject(ii.hbmColor);
  if (ii.hbmMask) {
    DeleteObject(ii.hbmMask);
  }
  if (!mask) {
    DeleteObject(color);
    return src;
  }
  ICONINFO out{};
  out.fIcon = TRUE;
  out.hbmColor = color;
  out.hbmMask = mask;
  HICON trimmed = CreateIconIndirect(&out);
  DeleteObject(color);
  DeleteObject(mask);
  if (!trimmed) {
    return src;
  }
  DestroyIcon(src);
  return trimmed;
}

HICON LoadItemIcon(const std::wstring& path, int size_px) {
  if (path.empty()) {
    return nullptr;
  }

  // Encode the source tier (JUMBO vs ExtraLarge) into the key: IconSizeBucket
  // maps 36..44 into the same "40" bucket, so 39px (ExtraLarge) and 44px
  // (JUMBO) would otherwise collide and a 44px request could hit a 48px master
  // then upscale it back — the exact blur we removed.
  const bool jumbo_source = size_px >= 40;
  const std::wstring key =
      ScanDedupKey(path) + L"@" + std::to_wstring(IconSizeBucket(size_px)) +
      (jumbo_source ? L"j" : L"x");
  {
    std::lock_guard<std::mutex> lk(g_master_icon_cache_mutex);
    const auto hit = g_master_icon_cache.find(key);
    if (hit != g_master_icon_cache.end()) {
      return CopyIcon(hit->second);
    }
  }

  HICON master = nullptr;
  // 1) Same pipeline Explorer uses for per-pixel icon views.
  master = IconFromShellItemFactory(path, size_px);

  if (!master) {
    SHFILEINFOW sfi{};
    const DWORD_PTR ok = SHGetFileInfoW(
        path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX);
    if (!ok) {
      // Fallback: stock large/small icon (often soft on HiDPI).
      UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
      if (size_px < 28) {
        flags = SHGFI_ICON | SHGFI_SMALLICON;
      }
      if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags)) {
        return nullptr;
      }
      master = TrimIconTransparentPadding(sfi.hIcon);
    } else {
      // Jumbo + trim: last resort when ShellItemImageFactory is unavailable.
      const int order[] = {
          size_px >= 40 ? SHIL_JUMBO : SHIL_EXTRALARGE,
          SHIL_JUMBO,
          SHIL_EXTRALARGE,
          SHIL_LARGE,
          SHIL_SMALL,
      };
      for (int shil : order) {
        HICON icon = IconFromImageList(shil, sfi.iIcon);
        if (icon) {
          master = TrimIconTransparentPadding(icon);
          break;
        }
      }
      if (!master) {
        UINT flags = SHGFI_ICON | SHGFI_LARGEICON;
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags)) {
          master = TrimIconTransparentPadding(sfi.hIcon);
        }
      }
    }
  }

  if (!master) {
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lk(g_master_icon_cache_mutex);
    auto result = g_master_icon_cache.emplace(key, master);
    if (!result.second) {
      // Another worker already cached this icon — drop our duplicate.
      DestroyIcon(master);
      return CopyIcon(result.first->second);
    }
  }
  return CopyIcon(master);
}

void PreloadIconsParallel(const std::vector<std::pair<std::wstring, int>>& jobs) {
  if (jobs.empty()) {
    return;
  }
  // Cap workers so boot stays bounded. SHGetImageList / IImageList::GetIcon are
  // COM calls — each worker must init COM, otherwise the icon load can fail
  // (CO_E_NOTINITIALIZED) and the preload silently degrades on some machines.
  const unsigned hw = std::thread::hardware_concurrency();
  const unsigned nthreads = (std::max)(1u, (std::min)(8u, hw ? hw : 4u));
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  pool.reserve(nthreads);
  auto worker = [&jobs, &next]() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
      const size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= jobs.size()) {
        break;
      }
      HICON icon = LoadItemIcon(jobs[i].first, jobs[i].second);
      if (icon) {
        DestroyIcon(icon);  // cache keeps the master; our clone is disposable
      }
    }
    CoUninitialize();
  };
  try {
    for (unsigned t = 0; t < nthreads; ++t) {
      pool.emplace_back(worker);
    }
  } catch (...) {
    // Thread creation failed mid-way — join what we spawned before unwinding,
    // otherwise the destructor of a joinable std::thread calls std::terminate.
    for (auto& th : pool) {
      if (th.joinable()) {
        th.join();
      }
    }
    throw;
  }
  for (auto& th : pool) {
    th.join();
  }
}
