#include "snapshot.h"

#include "paths_util.h"
#include "pin_store.h"
#include "shell_icons.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

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

bool IsSidecarName(const std::wstring& file) {
  // *.icons.json and *.pins.json must not appear as snapshot cards.
  const std::wstring i = L".icons.json";
  const std::wstring p = L".pins.json";
  if (file.size() >= i.size() &&
      _wcsicmp(file.c_str() + (file.size() - i.size()), i.c_str()) == 0) {
    return true;
  }
  if (file.size() >= p.size() &&
      _wcsicmp(file.c_str() + (file.size() - p.size()), p.c_str()) == 0) {
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

SnapshotCardData BuildSnapshotCard(const SnapshotInfo& info) {
  SnapshotCardData card;
  card.name = info.name;
  card.path = info.path;
  const AppState st = LoadAppStateFromPath(info.path, false);
  PinMap pins;
  LoadPinsFromPath(PinsSidecarPath(info.path), &pins);
  card.meta = SnapshotBrief(info.path);
  card.focus_page = st.current_page;
  card.boxes.reserve(st.fences.size());
  for (const auto& f : st.fences) {
    SnapshotPreviewBox box;
    box.title = f.title;
    box.x = f.x;
    box.y = f.y;
    box.w = f.w;
    box.h = f.h;
    box.page = f.page;
    box.style_id = f.style_id.empty() ? "charcoal" : f.style_id;
    box.collapsed = f.collapsed;
    box.icon_count = CountPinnedInFence(pins, f.id);
    card.boxes.push_back(std::move(box));
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
  // Shell icon dots for the schematic (cap payload; order preserved).
  const auto shell = LoadShellIconsSidecar(info.path);
  constexpr size_t kMaxPreviewIcons = 120;
  card.icons.reserve((std::min)(shell.size(), kMaxPreviewIcons));
  for (size_t i = 0; i < shell.size() && i < kMaxPreviewIcons; ++i) {
    SnapshotPreviewIcon ic;
    ic.x = shell[i].x;
    ic.y = shell[i].y;
    card.icons.push_back(ic);
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

bool LoadSnapshot(const std::wstring& path, AppState* out) {
  if (!out || path.empty()) {
    return false;
  }
  if (!CopyFileW(path.c_str(), LayoutFilePath().c_str(), FALSE)) {
    return false;
  }
  *out = LoadAppState();
  // Restore pinned content distribution from the sidecar when present.
  PinMap pins;
  if (LoadPinsFromPath(PinsSidecarPath(path), &pins)) {
    SavePins(pins);
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
  const auto icons = CaptureShellIconPositions();
  SaveShellIconsSidecar(path, icons);
  return true;
}

bool LoadSnapshotWithIcons(const std::wstring& path, AppState* out) {
  if (!LoadSnapshot(path, out)) {
    return false;
  }
  const auto icons = LoadShellIconsSidecar(path);
  if (!icons.empty()) {
    RestoreShellIconPositions(icons);
  }
  return true;
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
  return true;
}

bool DeleteSnapshot(const std::wstring& path) {
  if (path.empty() || !DeleteFileW(path.c_str())) {
    return false;
  }
  DeleteFileW(IconsSidecarPath(path).c_str());
  DeleteFileW(PinsSidecarPath(path).c_str());
  return true;
}
