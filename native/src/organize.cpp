#include "organize.h"

#include "app_prefs.h"
#include "organize_suppress.h"
#include "paths_util.h"
#include "whitelist.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::wstring DesktopDir() {
  PWSTR p = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) || !p) {
    return {};
  }
  std::wstring out = p;
  CoTaskMemFree(p);
  return out;
}

bool IsUnderDir(const std::wstring& path, const std::wstring& dir) {
  if (path.size() < dir.size()) {
    return false;
  }
  return _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0;
}

std::string NormalizeOrganizeTarget(std::string id) {
  if (id == "system_software") {
    return "system_software_side";
  }
  return id;
}

std::wstring TitleForFence(
    const std::string& id,
    const std::unordered_map<std::string, std::wstring>& titles) {
  const auto it = titles.find(id);
  if (it != titles.end() && !it->second.empty()) {
    return it->second;
  }
  return WidenUtf8(id);
}

bool ShouldSkipVirtual(const DesktopItem& it, const PinMap& pins,
                       const std::vector<std::wstring>& wl) {
  if (FindPin(pins, it.path)) {
    return true;
  }
  if (IsOrganizeSuppressed(it.path)) {
    return true;
  }
  if (IsWhitelisted(it.path, wl) || IsWhitelisted(it.name, wl)) {
    return true;
  }
  return false;
}

}  // namespace

int OrganizeApplyDefaultPins(PinMap* pins, const std::vector<DesktopItem>& items,
                             const RuleMap& rules,
                             const std::string& software_id,
                             const std::string& common_id,
                             const std::vector<FenceLayout>* fences) {
  if (!pins) {
    return 0;
  }
  const auto wl = LoadWhitelist();
  int n = 0;
  for (const auto& it : items) {
    if (ShouldSkipVirtual(it, *pins, wl)) {
      continue;
    }
    // P1-4: kinds-aware fence first (Python resolve_organize_target).
    std::string target;
    if (fences && !fences->empty()) {
      target = ResolveFenceByKind(it, *fences);
    }
    if (target.empty()) {
      target = ResolveFenceId(it.path, it.is_link, *pins, rules, software_id,
                             common_id);
    }
    target = NormalizeOrganizeTarget(std::move(target));
    SetPin(pins, it.path, target);
    ++n;
  }
  return n;
}

int OrganizeCountPhysicalMoves(const std::vector<DesktopItem>& items) {
  const std::wstring desktop = DesktopDir();
  if (desktop.empty()) {
    return 0;
  }
  const fs::path root = fs::path(desktop) / L"DeskTidy";
  const auto wl = LoadWhitelist();
  int n = 0;
  for (const auto& it : items) {
    if (!IsUnderDir(it.path, desktop)) {
      continue;
    }
    if (IsUnderDir(it.path, root.wstring())) {
      continue;
    }
    if (IsWhitelisted(it.path, wl) || IsWhitelisted(it.name, wl)) {
      continue;
    }
    ++n;
  }
  return n;
}

std::vector<std::wstring> OrganizePreviewVirtualPins(
    const PinMap& pins, const std::vector<DesktopItem>& items,
    const RuleMap& rules,
    const std::unordered_map<std::string, std::wstring>& fence_titles,
    const std::string& software_id, const std::string& common_id,
    const std::vector<FenceLayout>* fences) {
  std::vector<std::wstring> lines;
  const auto wl = LoadWhitelist();
  for (const auto& it : items) {
    if (ShouldSkipVirtual(it, pins, wl)) {
      continue;
    }
    std::string target;
    if (fences && !fences->empty()) {
      target = ResolveFenceByKind(it, *fences);
    }
    if (target.empty()) {
      target = ResolveFenceId(it.path, it.is_link, pins, rules, software_id,
                             common_id);
    }
    target = NormalizeOrganizeTarget(std::move(target));
    const std::wstring name = it.name.empty() ? it.path : it.name;
    lines.push_back(name + L" → " + TitleForFence(target, fence_titles));
    if (lines.size() >= 80) {
      break;
    }
  }
  return lines;
}

std::vector<std::wstring> OrganizePreviewPhysicalMoves(
    const std::vector<DesktopItem>& items) {
  std::vector<std::wstring> names;
  const std::wstring desktop = DesktopDir();
  if (desktop.empty()) {
    return names;
  }
  const fs::path root = fs::path(desktop) / L"DeskTidy";
  const auto wl = LoadWhitelist();
  for (const auto& it : items) {
    if (!IsUnderDir(it.path, desktop)) {
      continue;
    }
    if (IsUnderDir(it.path, root.wstring())) {
      continue;
    }
    if (IsWhitelisted(it.path, wl) || IsWhitelisted(it.name, wl)) {
      continue;
    }
    names.push_back(it.name.empty() ? it.path : it.name);
    if (names.size() >= 40) {
      break;
    }
  }
  return names;
}

bool OrganizeConfirmPhysical(HWND owner, const std::vector<DesktopItem>& items) {
  const auto names = OrganizePreviewPhysicalMoves(items);
  const int will = OrganizeCountPhysicalMoves(items);
  std::wstring msg = L"将把约 " + std::to_wstring(will) +
                     L" 个桌面项目移入 Desktop\\DeskTidy（白名单除外）。\n\n";
  if (names.empty()) {
    msg += L"（没有可移动项）\n";
  } else {
    msg += L"预览：\n";
    for (size_t i = 0; i < names.size(); ++i) {
      msg += L" · " + names[i] + L"\n";
    }
    if (will > static_cast<int>(names.size())) {
      msg += L" · …\n";
    }
  }
  msg += L"\n继续？";
  return MessageBoxW(owner, msg.c_str(), L"物理整理",
                     MB_YESNO | MB_ICONWARNING) == IDYES;
}

int OrganizeMoveIntoFolders(const std::vector<DesktopItem>& items) {
  const std::wstring desktop = DesktopDir();
  if (desktop.empty()) {
    return 0;
  }
  const fs::path root = fs::path(desktop) / L"DeskTidy";
  const fs::path links = root / L"软件";
  const fs::path files = root / L"常用";
  std::error_code ec;
  fs::create_directories(links, ec);
  fs::create_directories(files, ec);
  const auto wl = LoadWhitelist();
  int moved = 0;
  for (const auto& it : items) {
    if (!IsUnderDir(it.path, desktop)) {
      continue;
    }
    if (IsUnderDir(it.path, root.wstring())) {
      continue;
    }
    if (IsWhitelisted(it.path, wl) || IsWhitelisted(it.name, wl)) {
      continue;
    }
    const fs::path src(it.path);
    const fs::path dest_dir = it.is_link ? links : files;
    fs::path dest = dest_dir / src.filename();
    if (fs::exists(dest, ec)) {
      dest = dest_dir /
             (src.stem().wstring() + L"_" + std::to_wstring(GetTickCount()) +
              src.extension().wstring());
    }
    ec.clear();
    fs::rename(src, dest, ec);
    if (!ec) {
      ++moved;
    }
  }
  return moved;
}

// ---------------------------------------------------------------------------
// G4 / G5 — per-fence organize kinds + portal exclude patterns.
// ---------------------------------------------------------------------------
namespace {

std::wstring LowerW(const std::wstring& s) {
  std::wstring o = s;
  for (auto& c : o) {
    c = static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));
  }
  return o;
}

std::wstring BaseName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring Extension(const std::wstring& base) {
  const size_t dot = base.find_last_of(L'.');
  return dot == std::wstring::npos ? std::wstring() : base.substr(dot);
}

// Case-insensitive glob supporting '*' and '?' (Python fnmatch.fnmatchcase).
bool GlobMatch(const std::wstring& n, const std::wstring& p, size_t ni, size_t pi) {
  while (pi < p.size()) {
    if (p[pi] == L'*') {
      while (pi < p.size() && p[pi] == L'*') {
        ++pi;
      }
      if (pi == p.size()) {
        return true;
      }
      for (size_t k = ni; k <= n.size(); ++k) {
        if (GlobMatch(n, p, k, pi)) {
          return true;
        }
      }
      return false;
    }
    if (ni >= n.size()) {
      return false;
    }
    if (p[pi] == L'?' || p[pi] == n[ni]) {
      ++ni;
      ++pi;
    } else {
      return false;
    }
  }
  return ni == n.size();
}

bool NameMatchesExclude(const std::wstring& name, const std::wstring& pattern) {
  std::wstring raw = pattern;
  size_t a = 0;
  while (a < raw.size() && (raw[a] == L' ' || raw[a] == L'\t')) {
    ++a;
  }
  size_t b = raw.size();
  while (b > a && (raw[b - 1] == L' ' || raw[b - 1] == L'\t')) {
    --b;
  }
  raw = raw.substr(a, b - a);
  if (raw.empty()) {
    return false;
  }
  const std::wstring n = LowerW(name);
  const std::wstring p = LowerW(raw);
  if (p.find(L'*') != std::wstring::npos ||
      p.find(L'?') != std::wstring::npos) {
    return GlobMatch(n, p, 0, 0);
  }
  if (n == p) {
    return true;
  }
  const size_t dot = n.rfind(L'.');
  if (dot != std::wstring::npos && n.substr(0, dot) == p) {
    return true;
  }
  return false;
}

}  // namespace

std::string OrganizeKindOfItem(const DesktopItem& item) {
  // Python item_organize_kind: .lnk/.url/.exe → "icon"; folders/others → "file".
  std::wstring name = item.name.empty() ? item.path : item.name;
  const std::wstring ext = LowerW(Extension(BaseName(name)));
  if (item.is_link || ext == L".lnk" || ext == L".url" || ext == L".exe") {
    return "icon";
  }
  return "file";
}

bool FenceAcceptsItem(const std::vector<std::string>& organize_kinds,
                      const DesktopItem& item) {
  if (organize_kinds.empty()) {
    return false;
  }
  const std::string kind = OrganizeKindOfItem(item);
  for (const auto& k : organize_kinds) {
    if (k == kind) {
      return true;
    }
  }
  return false;
}

std::string ResolveFenceByKind(const DesktopItem& item,
                               const std::vector<FenceLayout>& fences) {
  for (const auto& f : fences) {
    if (FenceAcceptsItem(f.organize_kinds, item)) {
      return f.id;
    }
  }
  return {};
}

std::vector<std::wstring> DefaultExcludePatterns() {
  return {L"desktop.ini", L"DeskTidy"};
}

std::vector<std::wstring> LoadExcludePatterns() {
  std::vector<std::wstring> patterns = LoadAppPrefs().exclude_patterns;
  if (patterns.empty()) {
    patterns = DefaultExcludePatterns();
  }
  return patterns;
}

bool IsPortalEntryExcluded(const std::wstring& name,
                           const std::vector<std::wstring>& exclude_patterns) {
  if (name.empty()) {
    return false;
  }
  // Hidden / dot files.
  if (name[0] == L'.') {
    return true;
  }
  // Office temp lock / scratch files (~$..., ~...).
  if (name.rfind(L"~$", 0) == 0 || name[0] == L'~') {
    return true;
  }
  const std::wstring lower = LowerW(name);
  if (lower.find(L".tmp.") != std::wstring::npos) {
    return true;
  }
  static const wchar_t* kTempSuffixes[] = {
      L".tmp",     L".download", L".crdownload", L".wbk", L".asd",
      L".xlk",     L".lck",      L".temp",       L".part", L".partial",
  };
  for (const wchar_t* s : kTempSuffixes) {
    const size_t slen = wcslen(s);
    if (lower.size() >= slen &&
        lower.compare(lower.size() - slen, slen, s) == 0) {
      return true;
    }
  }
  for (const auto& pat : exclude_patterns) {
    if (NameMatchesExclude(name, pat)) {
      return true;
    }
  }
  return false;
}

std::vector<DesktopItem> ScanPortalItems(
    const std::wstring& portal_dir,
    const std::vector<std::wstring>& exclude_patterns) {
  std::vector<DesktopItem> out;
  if (portal_dir.empty()) {
    return out;
  }
  WIN32_FIND_DATAW fd{};
  const std::wstring pattern = portal_dir + L"\\*";
  HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) {
    return out;
  }
  do {
    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
      continue;
    }
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) {
      continue;
    }
    if (IsPortalEntryExcluded(fd.cFileName, exclude_patterns)) {
      continue;
    }
    DesktopItem item;
    item.path = portal_dir + L"\\" + fd.cFileName;
    item.name = fd.cFileName;
    item.is_link =
        item.name.size() >= 4 &&
        _wcsicmp(item.name.c_str() + item.name.size() - 4, L".lnk") == 0;
    out.push_back(std::move(item));
    if (out.size() >= 200) {
      break;
    }
  } while (FindNextFileW(find, &fd));
  FindClose(find);
  return out;
}
