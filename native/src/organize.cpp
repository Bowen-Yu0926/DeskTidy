#include "organize.h"

#include "app_prefs.h"
#include "organize_suppress.h"
#include "paths_util.h"
#include "whitelist.h"

#include <Windows.h>

#include <cwctype>
#include <string>
#include <vector>

namespace {

std::string NormalizeOrganizeTarget(std::string id) {
  if (id == "system_software") {
    return "system_software_side";
  }
  return id;
}

bool FenceIdAlive(const std::string& id,
                  const std::vector<FenceLayout>* fences) {
  if (id.empty() || !fences) {
    return false;
  }
  for (const auto& f : *fences) {
    if (f.id == id) {
      return true;
    }
  }
  return false;
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
                       const std::vector<std::wstring>& wl, bool reassign,
                       bool honor_suppress) {
  // 一键整理：已钉选保持用户座位。重新整理：按当前规则允许换区。
  if (!reassign) {
    const std::string* pin = FindPin(pins, it.path);
    if (pin && !pin->empty()) {
      return true;
    }
  }
  // Watch/quiet honors unpin suppress; explicit organize / reassign does not
  // (Fences: user-triggered tidy can reclaim floats).
  if (honor_suppress && IsOrganizeSuppressed(it.path)) {
    return true;
  }
  if (IsWhitelisted(it.path, wl) || IsWhitelisted(it.name, wl)) {
    return true;
  }
  return false;
}

}  // namespace

int OrganizeApplyDefaultPins(
    PinMap* pins, const std::vector<DesktopItem>& items, const RuleMap& rules,
    const std::string& software_id, const std::string& common_id,
    const std::vector<FenceLayout>* fences, int current_page, bool reassign,
    bool honor_suppress,
    std::vector<std::pair<std::wstring, std::string>>* ordered_changes) {
  if (!pins) {
    return 0;
  }
  if (ordered_changes) {
    ordered_changes->clear();
  }
  const auto wl = LoadWhitelist();
  CategoryFenceMap cat_map;
  FilenameRulesMap filename_rules;
  LoadCategoriesBoth(&cat_map, &filename_rules);
  int n = 0;
  for (const auto& it : items) {
    if (ShouldSkipVirtual(it, *pins, wl, reassign, honor_suppress)) {
      continue;
    }
    const std::string* existing = FindPin(*pins, it.path);
    const std::string prev = existing ? *existing : std::string();
    // P1-4: kinds-aware fence first (Python resolve_organize_target).
    std::string target;
    if (fences && !fences->empty()) {
      target = ResolveFenceByKind(it, *fences, current_page);
    }
    // Settings 类别分区映射 / 文件名规则 (organize_categories.json).
    if (target.empty()) {
      target = ResolveCategoryFenceId(it.path, cat_map, filename_rules);
    }
    if (target.empty()) {
      // Empty pin map: ResolveFenceId must not echo a stale software-side pin.
      PinMap no_pins;
      target = ResolveFenceId(it.path, it.is_link, no_pins, rules, software_id,
                             common_id);
    }
    target = NormalizeOrganizeTarget(std::move(target));
    // Dead fence_id (deleted partition / stale category map) → skip. Pinning
    // to a ghost id hides the icon from every fence and the public strip.
    if (!FenceIdAlive(target, fences)) {
      continue;
    }
    if (!prev.empty() && prev == target) {
      continue;
    }
    SetPin(pins, it.path, target);
    if (ordered_changes) {
      ordered_changes->emplace_back(it.path, target);
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
    const std::vector<FenceLayout>* fences, int current_page) {
  std::vector<std::wstring> lines;
  const auto wl = LoadWhitelist();
  CategoryFenceMap cat_map;
  FilenameRulesMap filename_rules;
  LoadCategoriesBoth(&cat_map, &filename_rules);
  for (const auto& it : items) {
    if (ShouldSkipVirtual(it, pins, wl, /*reassign=*/false,
                          /*honor_suppress=*/true)) {
      continue;
    }
    std::string target;
    if (fences && !fences->empty()) {
      target = ResolveFenceByKind(it, *fences, current_page);
    }
    if (target.empty()) {
      target = ResolveCategoryFenceId(it.path, cat_map, filename_rules);
    }
    if (target.empty()) {
      target = ResolveFenceId(it.path, it.is_link, pins, rules, software_id,
                             common_id);
    }
    target = NormalizeOrganizeTarget(std::move(target));
    if (!FenceIdAlive(target, fences)) {
      continue;
    }
    const std::wstring name = it.name.empty() ? it.path : it.name;
    lines.push_back(name + L" → " + TitleForFence(target, fence_titles));
    if (lines.size() >= 80) {
      break;
    }
  }
  return lines;
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
  // Also treat launchers/scripts as icon so 常用 pins (.bat/.cmd/.ps1) are not
  // healed into 文档 after pin-skip organize.
  std::wstring name = item.name.empty() ? item.path : item.name;
  const std::wstring ext = LowerW(Extension(BaseName(name)));
  if (item.is_link || ext == L".lnk" || ext == L".url" || ext == L".exe" ||
      ext == L".bat" || ext == L".cmd" || ext == L".ps1" || ext == L".msi" ||
      ext == L".com") {
    return "icon";
  }
  return "file";
}

int HealMisplacedSystemPins(PinMap* pins) {
  // No-op: Fences-class「已经在分区的不参与整理」. Remounting pins after
  // organize/boot fought user seats. Symbol kept for existing call sites.
  (void)pins;
  return 0;
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

// Fence membership on a page (FenceLayout.pages when set, else .page).
static bool FenceOnPage(const FenceLayout& f, int page) {
  if (page < 0) {
    return false;
  }
  if (f.pages.empty()) {
    return f.page == page;
  }
  for (int p : f.pages) {
    if (p == page) {
      return true;
    }
  }
  return false;
}

std::string ResolveFenceByKind(const DesktopItem& item,
                               const std::vector<FenceLayout>& fences,
                               int current_page) {
  // A content-locked fence opts out of rule-driven membership: the lock exists
  // precisely to stop the set from changing on its own.
  auto eligible = [&item](const FenceLayout& f) {
    return !f.content_locked && FenceAcceptsItem(f.organize_kinds, item);
  };
  // Python resolve_organize_target: try the current page first, then others.
  if (current_page >= 0) {
    for (const auto& f : fences) {
      if (FenceOnPage(f, current_page) && eligible(f)) {
        return f.id;
      }
    }
  }
  for (const auto& f : fences) {
    if (FenceOnPage(f, current_page)) {
      continue;
    }
    if (eligible(f)) {
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
    // Soft safety only — Fences/Explorer portals show full folder membership.
    if (out.size() >= 10000) {
      break;
    }
  } while (FindNextFileW(find, &fd));
  FindClose(find);
  return out;
}
