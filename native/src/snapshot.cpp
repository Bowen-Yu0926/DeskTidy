#include "snapshot.h"

#include "desktop_items.h"
#include "display_geom.h"
#include "float_placements.h"
#include "paths_util.h"
#include "pin_store.h"
#include "shell_icons.h"

#include <Windows.h>
#include <Wincrypt.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

std::string Narrow(const std::wstring& ws) {
  if (ws.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), n, nullptr,
                        nullptr);
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  }
  return out;
}

void EnsureSnapGdiplus() {
  static ULONG_PTR token = 0;
  if (!token) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
  }
}

int SnapPngEncoderClsid(CLSID* clsid) {
  if (!clsid) {
    return -1;
  }
  // Process-level cache: enumerate GDI+ image encoders once (was malloc +
  // full enumeration on every icon→PNG conversion).
  static CLSID cached{};
  static std::once_flag once;
  std::call_once(once, []() {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (!size) {
      return;
    }
    auto* info = static_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
    if (!info) {
      return;
    }
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
      if (wcscmp(info[i].MimeType, L"image/png") == 0) {
        cached = info[i].Clsid;
        break;
      }
    }
    free(info);
  });
  if (cached == CLSID{}) {
    return -1;
  }
  *clsid = cached;
  return 0;
}

std::string B64EncodeBytes(const BYTE* data, DWORD len) {
  if (!data || !len) {
    return {};
  }
  DWORD out_len = 0;
  CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       nullptr, &out_len);
  std::string out(out_len, '\0');
  CryptBinaryToStringA(data, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       out.data(), &out_len);
  while (!out.empty() &&
         (out.back() == '\0' || out.back() == '\n' || out.back() == '\r')) {
    out.pop_back();
  }
  return out;
}

// 20×20 PNG data-URL for snapshot thumbs (real shell glyphs, not grey dots).
// Prefer DrawIconEx into a DIB — Gdiplus::Bitmap::FromHICON fails on some shells.
std::string IconToPngDataUrl(HICON icon, int size = 20) {
  if (!icon || size < 8) {
    return {};
  }
  EnsureSnapGdiplus();
  CLSID png{};
  if (SnapPngEncoderClsid(&png) < 0) {
    return {};
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = size;
  bmi.bmiHeader.biHeight = -size;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return {};
  }
  HGDIOBJ old = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(size) * size * 4);
  DrawIconEx(mem, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
  auto* px = static_cast<BYTE*>(bits);
  const int n = size * size;
  int opaque = 0;
  for (int i = 0; i < n; ++i) {
    BYTE* p = px + i * 4;
    BYTE a = p[3];
    if (a == 0 && (p[0] | p[1] | p[2])) {
      a = 255;
    }
    if (a > 16) {
      ++opaque;
    }
    p[3] = a;
  }

  // Keep DIB alive until after PNG encode (Bitmap wraps px without copy).
  Gdiplus::Bitmap canvas(size, size, size * 4, PixelFormat32bppARGB, px);
  if (opaque < 4) {
    Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromHICON(icon);
    if (src) {
      Gdiplus::Graphics g(&canvas);
      g.Clear(Gdiplus::Color(0, 0, 0, 0));
      g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
      g.DrawImage(src, 0, 0, size, size);
      delete src;
    }
  }

  IStream* stream = nullptr;
  std::string result;
  if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) == S_OK && stream) {
    if (canvas.Save(stream, &png, nullptr) == Gdiplus::Ok) {
      STATSTG stat{};
      if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME))) {
        const ULONG nbytes = static_cast<ULONG>(stat.cbSize.QuadPart);
        if (nbytes > 0 && nbytes <= 64 * 1024) {
          LARGE_INTEGER zero{};
          stream->Seek(zero, STREAM_SEEK_SET, nullptr);
          std::vector<BYTE> bytes(nbytes);
          ULONG read = 0;
          if (SUCCEEDED(stream->Read(bytes.data(), nbytes, &read)) &&
              read == nbytes) {
            const std::string b64 = B64EncodeBytes(bytes.data(), read);
            if (!b64.empty()) {
              result = "data:image/png;base64," + b64;
            }
          }
        }
      }
    }
    stream->Release();
  }

  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return result;
}

std::string PathToPngDataUrl(const std::wstring& path) {
  if (path.empty()) {
    return {};
  }
  HICON icon = LoadItemIcon(path, 32);
  if (!icon) {
    return {};
  }
  std::string url = IconToPngDataUrl(icon, 20);
  DestroyIcon(icon);
  return url;
}

std::string EscapeJson(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

std::wstring StripJsonExt(const std::wstring& path) {
  const std::wstring suf = L".json";
  if (path.size() >= suf.size() &&
      _wcsicmp(path.c_str() + (path.size() - suf.size()), suf.c_str()) == 0) {
    return path.substr(0, path.size() - suf.size());
  }
  return path;
}

std::wstring BaseName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

std::wstring SanitizeName(const std::wstring& name) {
  std::wstring safe = name;
  for (auto& ch : safe) {
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
        ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
      ch = L'_';
    }
  }
  // Trim trailing dots/spaces that Windows rejects as filenames.
  while (!safe.empty() && (safe.back() == L'.' || safe.back() == L' ')) {
    safe.pop_back();
  }
  return safe;
}

// Resolve the final .json path for a snapshot name, appending _2, _3, … when
// a file with the sanitized name already exists (dedup, never overwrites).
std::wstring ResolveSnapshotPath(const std::wstring& name) {
  const std::wstring safe = SanitizeName(name);
  std::wstring path = JoinPath(SnapshotDir(), safe + L".json");
  int i = 2;
  while (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    path = JoinPath(SnapshotDir(),
                    safe + L"_" + std::to_wstring(i++) + L".json");
  }
  return path;
}

std::wstring IconsSidecarPath(const std::wstring& snapshot_json) {
  return StripJsonExt(snapshot_json) + L".icons.json";
}

std::wstring PinsSidecarPath(const std::wstring& snapshot_json) {
  return StripJsonExt(snapshot_json) + L".pins.json";
}

std::wstring FloatsSidecarPath(const std::wstring& snapshot_json) {
  return StripJsonExt(snapshot_json) + L".floats.json";
}

bool IsSidecarName(const std::wstring& file) {
  // *.icons.json / *.pins.json / *.floats.json must not appear as cards.
  const std::wstring i = L".icons.json";
  const std::wstring p = L".pins.json";
  const std::wstring f = L".floats.json";
  if (file.size() >= i.size() &&
      _wcsicmp(file.c_str() + (file.size() - i.size()), i.c_str()) == 0) {
    return true;
  }
  if (file.size() >= p.size() &&
      _wcsicmp(file.c_str() + (file.size() - p.size()), p.c_str()) == 0) {
    return true;
  }
  if (file.size() >= f.size() &&
      _wcsicmp(file.c_str() + (file.size() - f.size()), f.c_str()) == 0) {
    return true;
  }
  return false;
}

// Pins (path → fence id) sidecar, same JSON shape as pins.json so restore can
// reuse the parser. This captures each fence's virtual_items (pinned paths).
bool SavePinsToPath(const std::wstring& path, const PinMap& pins) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "{\n  \"pins\": {\n";
  size_t i = 0;
  for (const auto& kv : pins) {
    out << "    \"" << EscapeJson(Narrow(kv.first)) << "\":\""
        << EscapeJson(kv.second) << "\"";
    if (++i < pins.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  }\n}\n";
  return true;
}

bool LoadPinsFromPath(const std::wstring& path, PinMap* pins) {
  if (!pins) {
    return false;
  }
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  size_t pos = json.find("\"pins\"");
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find('{', pos);
  if (pos == std::string::npos) {
    return false;
  }
  ++pos;
  while (pos < json.size()) {
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' ||
            json[pos] == '\t' || json[pos] == ',')) {
      ++pos;
    }
    if (pos >= json.size() || json[pos] == '}') {
      break;
    }
    if (json[pos] != '"') {
      break;
    }
    ++pos;
    std::string path;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        ++pos;
      }
      path.push_back(json[pos++]);
    }
    if (pos >= json.size()) {
      break;
    }
    ++pos;
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
      break;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
      break;
    }
    ++pos;
    std::string fence;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        ++pos;
      }
      fence.push_back(json[pos++]);
    }
    if (!path.empty() && !fence.empty()) {
      (*pins)[NormalizePinKey(Widen(path))] = fence;
    }
    if (pos < json.size() && json[pos] == '"') {
      ++pos;
    }
  }
  return !pins->empty();
}

int CountPinnedInFence(const PinMap& pins, const std::string& fence_id) {
  int n = 0;
  for (const auto& kv : pins) {
    if (kv.second == fence_id) {
      ++n;
    }
  }
  return n;
}

}  // namespace

std::wstring SnapshotDir() {
  const std::wstring dir = JoinPath(NativeDataDir(), L"snapshots");
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring DefaultSnapshotName() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t buf[64];
  swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute);
  return buf;
}

bool SaveSnapshot(const AppState& state, const std::wstring& name) {
  if (name.empty()) {
    return false;
  }
  const std::wstring path = ResolveSnapshotPath(name);
  if (!SaveAppState(state)) {
    return false;
  }
  if (!CopyFileW(LayoutFilePath().c_str(), path.c_str(), FALSE)) {
    return false;
  }
  // Capture each fence's pinned paths (virtual_items) so applying the snapshot
  // restores the exact content distribution.
  SavePinsToPath(PinsSidecarPath(path), LoadPins());
  // Loose float seats (page + rx/ry) — without this, restore reshuffles
  // public icons after Explorer-hidden tidy mode.
  CopyFileW(FloatPlacementsPath().c_str(), FloatsSidecarPath(path).c_str(),
            FALSE);
  return true;
}

std::vector<SnapshotInfo> ListSnapshots() {
  std::vector<SnapshotInfo> out;
  const std::wstring pattern = JoinPath(SnapshotDir(), L"*.json");
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return out;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      continue;
    }
    if (IsSidecarName(fd.cFileName)) {
      continue;
    }
    SnapshotInfo info;
    info.name = fd.cFileName;
    if (info.name.size() > 5) {
      info.name.resize(info.name.size() - 5);  // strip .json
    }
    info.path = JoinPath(SnapshotDir(), fd.cFileName);
    out.push_back(std::move(info));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end(),
            [](const SnapshotInfo& a, const SnapshotInfo& b) {
              return a.name > b.name;  // newest snap_YYYY… first
            });
  return out;
}

static int CountIconSidecar(const std::wstring& path) {
  const std::wstring icons_path = IconsSidecarPath(path);
  FILE* fp = nullptr;
  if (_wfopen_s(&fp, icons_path.c_str(), L"rb") != 0 || !fp) {
    return 0;
  }
  std::string ij;
  char buf[4096];
  while (size_t n = fread(buf, 1, sizeof(buf), fp)) {
    ij.append(buf, n);
  }
  fclose(fp);
  // Sidecar entries are {"name":"...","x":..,"y":..} (not "path").
  int icons = 0;
  size_t q = 0;
  while ((q = ij.find("\"name\"", q)) != std::string::npos) {
    ++icons;
    ++q;
  }
  return icons;
}

std::wstring SnapshotBrief(const std::wstring& path) {
  const AppState st = LoadAppStateFromPath(path, false);
  const int fences = static_cast<int>(st.fences.size());
  const int icons = CountIconSidecar(path);
  PinMap pins;
  LoadPinsFromPath(PinsSidecarPath(path), &pins);
  const int pinned = static_cast<int>(pins.size());
  std::wstring out = std::to_wstring(fences) + L" 分区 / " +
                     std::to_wstring(pinned) + L" 钉选";
  if (icons > 0) {
    out += L" / " + std::to_wstring(icons) + L" 壳图标";
  }
  if (!st.page_names.empty()) {
    out += L" · " + std::to_wstring(st.page_names.size()) + L" 页";
  }
  return out;
}

// 从 fingerprint（如 0,0,1920x1080@…）解析最大的显示器逻辑分辨率，
// 供快照放大预览按整块桌面比例 letterbox（对标 Fences 缩略）。
static void InferWorkSizeFromFingerprint(const std::string& fp, int* ww,
                                         int* wh) {
  if (!ww || !wh) {
    return;
  }
  *ww = 0;
  *wh = 0;
  for (size_t i = 0; i + 3 < fp.size(); ++i) {
    if (!isdigit(static_cast<unsigned char>(fp[i]))) {
      continue;
    }
    size_t j = i;
    while (j < fp.size() && isdigit(static_cast<unsigned char>(fp[j]))) {
      ++j;
    }
    if (j >= fp.size() || fp[j] != 'x') {
      i = j;
      continue;
    }
    size_t k = j + 1;
    if (k >= fp.size() || !isdigit(static_cast<unsigned char>(fp[k]))) {
      i = j;
      continue;
    }
    while (k < fp.size() && isdigit(static_cast<unsigned char>(fp[k]))) {
      ++k;
    }
    const int w = atoi(fp.c_str() + i);
    const int h = atoi(fp.c_str() + j + 1);
    if (w >= 800 && h >= 600 && w * h > (*ww) * (*wh)) {
      *ww = w;
      *wh = h;
    }
    i = k;
  }
}

SnapshotCardData BuildSnapshotCard(const SnapshotInfo& info,
                                  const SnapshotCardOptions& opts) {
  SnapshotCardData card;
  card.name = info.name;
  card.path = info.path;
  const AppState st = LoadAppStateFromPath(info.path, false);
  PinMap pins;
  LoadPinsFromPath(PinsSidecarPath(info.path), &pins);
  card.meta = SnapshotBrief(info.path);
  card.focus_page = st.current_page;
  InferWorkSizeFromFingerprint(st.last_display_fingerprint, &card.work_w,
                               &card.work_h);
  if (card.work_w < 800 || card.work_h < 600) {
    const RECT wa = PrimaryWorkArea();
    card.work_w = (std::max)(1, static_cast<int>(wa.right - wa.left));
    card.work_h = (std::max)(1, static_cast<int>(wa.bottom - wa.top));
  }
  // Resolve desktop paths once so free-icon thumbs can match by display name.
  const auto desktop_items = ScanDesktopItems();
  // Pre-build a case-insensitive name/stem → path map (O(icons) once), then
  // O(1) lookup per shell icon instead of an O(icons×shell) linear scan with
  // per-item substring/rfind/_wcsicmp churn.
  auto lower = [](std::wstring s) {
    for (auto& c : s) {
      if (c >= L'A' && c <= L'Z') {
        c = static_cast<wchar_t>(c - L'A' + L'a');
      }
    }
    return s;
  };
  std::unordered_map<std::wstring, std::wstring> name_to_path;
  name_to_path.reserve(desktop_items.size() * 2);
  for (const auto& it : desktop_items) {
    // Same priority as the original linear scan: per item, full display name
    // first, then extension-stripped file stem. First insertion wins.
    std::wstring nm = lower(it.name);
    if (!nm.empty()) {
      name_to_path.emplace(std::move(nm), it.path);
    }
    const size_t slash = it.path.find_last_of(L"\\/");
    std::wstring file = (slash == std::wstring::npos)
                            ? it.path
                            : it.path.substr(slash + 1);
    const size_t dot = file.rfind(L'.');
    if (dot != std::wstring::npos) {
      file.resize(dot);
    }
    std::wstring stem = lower(file);
    if (!stem.empty()) {
      name_to_path.emplace(std::move(stem), it.path);
    }
  }
  auto path_for_shell_name = [&](const std::wstring& name) -> std::wstring {
    if (name.empty()) {
      return {};
    }
    const auto it = name_to_path.find(lower(name));
    return it == name_to_path.end() ? std::wstring() : it->second;
  };

  card.page_names = st.page_names;
  NormalizePageNames(&card.page_names);

  card.boxes.reserve(st.fences.size() * 2);
  const size_t kMaxPinThumbs = opts.render_thumbs ? opts.max_pin_thumbs : 0;
  for (const auto& f : st.fences) {
    // Prefer saved manual order, then remaining pins for this fence.
    std::vector<std::wstring> ordered = f.pin_order;
    for (auto& p : ordered) {
      p = NormalizeStoredPath(p);
    }
    auto already = [&](const std::wstring& p) {
      for (const auto& q : ordered) {
        if (_wcsicmp(q.c_str(), p.c_str()) == 0) {
          return true;
        }
      }
      return false;
    };
    for (const auto& kv : pins) {
      if (kv.second != f.id) {
        continue;
      }
      const std::wstring path = NormalizeStoredPath(kv.first);
      if (!already(path)) {
        ordered.push_back(path);
      }
    }
    std::vector<std::string> thumbs;
    thumbs.reserve((std::min)(ordered.size(), kMaxPinThumbs));
    for (size_t i = 0; i < ordered.size() && thumbs.size() < kMaxPinThumbs;
         ++i) {
      std::string url = PathToPngDataUrl(ordered[i]);
      if (!url.empty()) {
        thumbs.push_back(std::move(url));
      }
    }
    const int icon_count = CountPinnedInFence(pins, f.id);
    // One preview box per page membership (geometry may differ per page).
    std::vector<int> pgs =
        f.pages.empty() ? std::vector<int>{f.page} : f.pages;
    for (int pg : pgs) {
      SnapshotPreviewBox box;
      box.title = f.title;
      box.x = f.x;
      box.y = f.y;
      box.w = f.w;
      box.h = f.h;
      box.page = pg;
      box.style_id = f.style_id.empty() ? "navy" : f.style_id;
      box.collapsed = f.collapsed;
      box.icon_count = icon_count;
      box.pin_thumbs = thumbs;
      const auto git = f.page_geometries.find(pg);
      if (git != f.page_geometries.end()) {
        if (!git->second.visible) {
          continue;
        }
        box.x = git->second.x;
        box.y = git->second.y;
        box.w = git->second.w;
        box.h = git->second.h;
        box.collapsed = git->second.collapsed;
      }
      card.boxes.push_back(std::move(box));
    }
  }
  // Prefer a page that actually has fences for the thumb (saved current_page
  // may point at an empty page).
  bool focus_has = false;
  for (const auto& b : card.boxes) {
    if (b.page == card.focus_page) {
      focus_has = true;
      break;
    }
  }
  if (!focus_has && !card.boxes.empty()) {
    card.focus_page = card.boxes.front().page;
  }
  // Free floats only for the schematic. Explorer DefView still lists every
  // desktop file (virtual pins stay on disk), so painting the full shell
  // sidecar duplicated fence contents as a ghost column. Market (Fences):
  // preview = fences + unpinned floats, not the raw shell grid.
  std::unordered_set<std::wstring> pinned_keys;
  pinned_keys.reserve(pins.size() * 2);
  auto stem_of = [](const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring file =
        (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    const size_t dot = file.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) {
      file.resize(dot);
    }
    return file;
  };
  for (const auto& kv : pins) {
    pinned_keys.insert(NormalizePinKey(kv.first));
    const std::wstring stem = stem_of(kv.first);
    if (!stem.empty()) {
      pinned_keys.insert(stem);
    }
  }
  auto is_pinned_shell = [&](const std::wstring& name,
                             const std::wstring& path) {
    if (!path.empty() &&
        pinned_keys.count(NormalizePinKey(path)) != 0) {
      return true;
    }
    if (!name.empty() && pinned_keys.count(name) != 0) {
      return true;
    }
    return false;
  };

  const auto shell = LoadShellIconsSidecar(info.path);
  constexpr size_t kMaxPreviewIcons = 80;
  const size_t kMaxFreeThumbs =
      opts.render_thumbs ? opts.max_free_thumbs : 0;
  card.icons.reserve((std::min)(shell.size(), kMaxPreviewIcons));
  for (size_t i = 0; i < shell.size() && card.icons.size() < kMaxPreviewIcons;
       ++i) {
    const std::wstring path =
        kMaxFreeThumbs > 0 ? path_for_shell_name(shell[i].name)
                           : std::wstring();
    if (kMaxFreeThumbs > 0 && is_pinned_shell(shell[i].name, path)) {
      continue;
    }
    SnapshotPreviewIcon ic;
    ic.x = shell[i].x;
    ic.y = shell[i].y;
    if (card.icons.size() < kMaxFreeThumbs && !path.empty()) {
      ic.thumb = PathToPngDataUrl(path);
    }
    // Skip blank placeholders — only keep free icons we can thumb.
    if (ic.thumb.empty()) {
      continue;
    }
    card.icons.push_back(std::move(ic));
  }
  // Refresh meta: 「壳图标」→ only unpinned floats actually drawn.
  {
    std::wstring meta = std::to_wstring(static_cast<int>(card.boxes.size())) +
                        L" 分区 / " + std::to_wstring(static_cast<int>(pins.size())) +
                        L" 钉选";
    if (!card.icons.empty()) {
      meta += L" / " + std::to_wstring(static_cast<int>(card.icons.size())) +
              L" 浮标";
    }
    if (!st.page_names.empty()) {
      meta += L" · " + std::to_wstring(st.page_names.size()) + L" 页";
    }
    card.meta = std::move(meta);
  }
  // When pins sidecar is missing, count shell icons that sit inside each fence
  // so the thumb still shows in-fence dots (older snapshots).
  if (pins.empty() && !card.icons.empty()) {
    for (auto& box : card.boxes) {
      if (box.collapsed || box.icon_count > 0) {
        continue;
      }
      int n = 0;
      for (const auto& ic : card.icons) {
        if (ic.x >= box.x && ic.x < box.x + box.w && ic.y >= box.y &&
            ic.y < box.y + box.h) {
          ++n;
        }
      }
      box.icon_count = n;
    }
  }
  return card;
}

ULONGLONG SnapshotMtimeQuad(const std::wstring& path) {
  if (path.empty()) {
    return 0;
  }
  WIN32_FILE_ATTRIBUTE_DATA fd{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fd)) {
    return 0;
  }
  return (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
         static_cast<ULONGLONG>(fd.ftLastWriteTime.dwLowDateTime);
}

namespace {

// Memoizes BuildSnapshotCard by snapshot path + mtime + thumb budget, so
// reopening the settings window does not re-extract and re-encode every
// thumbnail on the UI thread. Only thumb-bearing cards are stored (see
// BuildSnapshotCardCached).
constexpr size_t kSnapshotCardCacheMax = 24;

struct SnapshotCardCacheEntry {
  SnapshotCardData card;
  ULONGLONG mtime = 0;
  size_t max_pin_thumbs = 0;
  size_t max_free_thumbs = 0;
};

std::mutex g_snapshot_card_mutex;
std::unordered_map<std::wstring, SnapshotCardCacheEntry>
    g_snapshot_card_cache;

}  // namespace

SnapshotCardData BuildSnapshotCardCached(const SnapshotInfo& info,
                                        const SnapshotCardOptions& opts) {
  const ULONGLONG mtime = SnapshotMtimeQuad(info.path);
  const bool cacheable = opts.render_thumbs && mtime != 0;
  if (cacheable) {
    std::lock_guard<std::mutex> lock(g_snapshot_card_mutex);
    const auto it = g_snapshot_card_cache.find(info.path);
    if (it != g_snapshot_card_cache.end() && it->second.mtime == mtime &&
        it->second.max_pin_thumbs >= opts.max_pin_thumbs &&
        it->second.max_free_thumbs >= opts.max_free_thumbs) {
      // Return a copy — the caller moves out of the card's vectors.
      return it->second.card;
    }
  }

  SnapshotCardData card = BuildSnapshotCard(info, opts);

  if (cacheable) {
    std::lock_guard<std::mutex> lock(g_snapshot_card_mutex);
    SnapshotCardCacheEntry entry;
    entry.card = card;
    entry.mtime = mtime;
    entry.max_pin_thumbs = opts.max_pin_thumbs;
    entry.max_free_thumbs = opts.max_free_thumbs;
    // LRU-ish trim: the settings list is bounded (ListSnapshots), but a
    // long-lived session that keeps creating snapshots could otherwise grow
    // the map without limit.
    if (g_snapshot_card_cache.size() >= kSnapshotCardCacheMax &&
        g_snapshot_card_cache.find(info.path) == g_snapshot_card_cache.end()) {
      g_snapshot_card_cache.erase(g_snapshot_card_cache.begin());
    }
    g_snapshot_card_cache[info.path] = std::move(entry);
  }
  return card;
}

bool LoadSnapshot(const std::wstring& path, AppState* out) {
  if (!out || path.empty()) {
    return false;
  }
  if (!CopyFileW(path.c_str(), LayoutFilePath().c_str(), FALSE)) {
    return false;
  }
  // Faithful restore: do NOT inject defaults / HealFenceLayouts / migrate
  // kinds — that rewrote「其他」and resized fences away from the snapshot.
  // Profiles for the current display fingerprint still apply inside the load.
  *out = LoadAppStateFromPath(LayoutFilePath(), /*inject_defaults=*/false);
  // Restore pinned content distribution from the sidecar when present.
  PinMap pins;
  if (LoadPinsFromPath(PinsSidecarPath(path), &pins)) {
    SavePins(pins);
  }
  // Restore float seats when the sidecar exists (older snapshots may lack it).
  const std::wstring floats_src = FloatsSidecarPath(path);
  if (GetFileAttributesW(floats_src.c_str()) != INVALID_FILE_ATTRIBUTES) {
    CopyFileW(floats_src.c_str(), FloatPlacementsPath().c_str(), FALSE);
  }
  return true;
}

bool SaveSnapshotWithIcons(const AppState& state, const std::wstring& name) {
  if (name.empty()) {
    return false;
  }
  const std::wstring path = ResolveSnapshotPath(name);
  if (!SaveAppState(state)) {
    return false;
  }
  if (!CopyFileW(LayoutFilePath().c_str(), path.c_str(), FALSE)) {
    return false;
  }
  SavePinsToPath(PinsSidecarPath(path), LoadPins());
  CopyFileW(FloatPlacementsPath().c_str(), FloatsSidecarPath(path).c_str(),
            FALSE);
  const auto icons = CaptureShellIconPositions();
  SaveShellIconsSidecar(path, icons);
  return true;
}

bool LoadSnapshotWithIcons(const std::wstring& path, AppState* out) {
  // Layout + pins only. Shell icon positions must be restored by the host
  // AFTER fences reload and RefreshDesktopIcons — otherwise icon reflow
  // immediately stomps the sidecar coordinates (apply looked like a no-op).
  return LoadSnapshot(path, out);
}

bool RestoreSnapshotShellIcons(const std::wstring& path) {
  const auto icons = LoadShellIconsSidecar(path);
  if (icons.empty()) {
    return false;
  }
  return RestoreShellIconPositions(icons) > 0;
}

bool RenameSnapshot(const std::wstring& path, const std::wstring& new_name) {
  if (path.empty() || new_name.empty()) {
    return false;
  }
  const std::wstring safe = SanitizeName(new_name);
  if (safe.empty()) {
    return false;
  }
  std::wstring target = JoinPath(SnapshotDir(), safe + L".json");
  int i = 2;
  while (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES &&
         _wcsicmp(target.c_str(), path.c_str()) != 0) {
    target = JoinPath(SnapshotDir(),
                      safe + L"_" + std::to_wstring(i++) + L".json");
  }
  if (_wcsicmp(target.c_str(), path.c_str()) == 0) {
    return true;  // same name — nothing to do
  }
  if (!MoveFileW(path.c_str(), target.c_str())) {
    return false;
  }
  MoveFileW(IconsSidecarPath(path).c_str(), IconsSidecarPath(target).c_str());
  MoveFileW(PinsSidecarPath(path).c_str(), PinsSidecarPath(target).c_str());
  MoveFileW(FloatsSidecarPath(path).c_str(), FloatsSidecarPath(target).c_str());
  return true;
}

bool DeleteSnapshot(const std::wstring& path) {
  if (path.empty() || !DeleteFileW(path.c_str())) {
    return false;
  }
  DeleteFileW(IconsSidecarPath(path).c_str());
  DeleteFileW(PinsSidecarPath(path).c_str());
  DeleteFileW(FloatsSidecarPath(path).c_str());
  return true;
}
