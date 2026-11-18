#include "namespace_icons.h"

#include "desktop_shell.h"
#include "log.h"
#include "paths_util.h"
#include "pin_store.h"

#include <ShlObj.h>
#include <Shobjidl.h>
#include <Shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

#ifndef SHCNE_ASSOCCHANGED
#define SHCNE_ASSOCCHANGED 0x08000000
#endif
#ifndef SHCNF_IDLIST
#define SHCNF_IDLIST 0x0000
#endif
#ifndef SHCNF_FLUSHNOWAIT
#define SHCNF_FLUSHNOWAIT 0x3000
#endif

struct NsIcon {
  const wchar_t* clsid;  // with braces
  const wchar_t* name;
};

constexpr NsIcon kSystemIcons[] = {
    {L"{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"此电脑"},
    {L"{645FF040-5081-101B-9F08-00AA002F954E}", L"回收站"},
};

const wchar_t* const kHideRoots[] = {
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\ClassicStartMenu",
};

struct NsIconState {
  std::wstring clsid;
  std::wstring name;
  bool nsp_existed = false;      // NewStartPanel value existed before hide
  DWORD nsp_value = 0;           // original value (0=show, 1=hide)
  bool classic_existed = false;  // ClassicStartMenu value existed before hide
  DWORD classic_value = 0;
};

std::wstring StateFilePath() {
  return JoinPath(NativeDataDir(), L"desktop_icons_state.json");
}

bool StateFileExists() {
  return GetFileAttributesW(StateFilePath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

void WriteIconValue(const wchar_t* clsid, const DWORD* value) {
  // value == nullptr → delete the value (restore "no value" original state).
  for (const wchar_t* sub : kHideRoots) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &key) !=
        ERROR_SUCCESS) {
      continue;  // key absent: a delete is already satisfied; a write needs create
    }
    if (value) {
      RegSetValueExW(key, clsid, 0, REG_DWORD,
                     reinterpret_cast<const BYTE*>(value), sizeof(*value));
    } else {
      RegDeleteValueW(key, clsid);
    }
    RegCloseKey(key);
  }
}

void SetDesktopNamespaceIconVisible(const wchar_t* clsid, bool visible) {
  const DWORD value = visible ? 0 : 1;
  for (const wchar_t* sub : kHideRoots) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS ||
        !key) {
      continue;
    }
    RegSetValueExW(key, clsid, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
  }
}

void ReadRootValue(const wchar_t* sub, const wchar_t* clsid, bool* existed,
                   DWORD* value) {
  *existed = false;
  *value = 0;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &key) !=
      ERROR_SUCCESS) {
    return;
  }
  DWORD v = 0;
  DWORD type = 0;
  DWORD size = sizeof(v);
  const LONG rc =
      RegQueryValueExW(key, clsid, nullptr, &type,
                       reinterpret_cast<LPBYTE>(&v), &size);
  RegCloseKey(key);
  if (rc == ERROR_SUCCESS && type == REG_DWORD) {
    *existed = true;
    *value = v;
  }
}

NsIconState CaptureIconState(const NsIcon& icon) {
  NsIconState s;
  s.clsid = icon.clsid;
  s.name = icon.name;
  ReadRootValue(kHideRoots[0], icon.clsid, &s.nsp_existed, &s.nsp_value);
  ReadRootValue(kHideRoots[1], icon.clsid, &s.classic_existed,
                &s.classic_value);
  return s;
}

void RestoreRootValue(const wchar_t* sub, const wchar_t* clsid, bool existed,
                      DWORD value) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &key) !=
      ERROR_SUCCESS) {
    if (!existed) {
      return;  // nothing to restore
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS ||
        !key) {
      return;
    }
  }
  if (existed) {
    RegSetValueExW(key, clsid, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
  } else {
    RegDeleteValueW(key, clsid);
  }
  RegCloseKey(key);
}

void RestoreIconState(const NsIconState& s) {
  RestoreRootValue(kHideRoots[0], s.clsid.c_str(), s.nsp_existed, s.nsp_value);
  RestoreRootValue(kHideRoots[1], s.clsid.c_str(), s.classic_existed,
                   s.classic_value);
}

void RefreshDesktopNamespaceIcons() {
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr,
                 nullptr);
}

// --- Line-based state persistence -------------------------------------------
// The previous hand-rolled JSON loader split objects with find('{')/find('}'),
// which truncated at the braces inside the CLSID string and therefore always
// read nsp_existed/classic_existed as 0 — wiping user "hidden" preferences on
// restore. The new format is line-based and never splits on braces.
//
//   line 0:   "DTICONS1"
//   line N:   section<TAB>clsid<TAB>existed(0|1)<TAB>value
//   section:  "nsp" (NewStartPanel) or "classic" (ClassicStartMenu)
//   clsid:    registry value name (contains braces), backslash-escaped
//   existed:  1 if the value existed before hiding, else 0
//   value:    the original REG_DWORD (0=show, 1=hide)

constexpr char kStateMagic[] = "DTICONS1";
constexpr char kSectionNsp[] = "nsp";
constexpr char kSectionClassic[] = "classic";

std::string EscapeField(const std::wstring& ws) {
  const std::string utf8 = NarrowUtf8(ws);
  std::string out;
  out.reserve(utf8.size());
  for (char c : utf8) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::wstring UnescapeField(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      const char n = s[i + 1];
      switch (n) {
        case '\\': out.push_back('\\'); break;
        case 't': out.push_back('\t'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        default: out.push_back('\\'); out.push_back(n); break;
      }
      ++i;
    } else {
      out.push_back(s[i]);
    }
  }
  return WidenUtf8(out);
}

bool SaveIconStateJson(const std::vector<NsIconState>& states) {
  std::ofstream out(StateFilePath(), std::ios::binary);
  if (!out) {
    return false;
  }
  out << kStateMagic << "\n";
  for (const auto& s : states) {
    const std::string clsid = EscapeField(s.clsid);
    out << kSectionNsp << "\t" << clsid << "\t"
        << (s.nsp_existed ? 1 : 0) << "\t" << s.nsp_value << "\n";
    out << kSectionClassic << "\t" << clsid << "\t"
        << (s.classic_existed ? 1 : 0) << "\t" << s.classic_value << "\n";
  }
  return true;
}

bool AllDigits(const std::string& s) {
  if (s.empty()) {
    return false;
  }
  for (char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return true;
}

void ApplyStateLine(const std::string& raw,
                    std::map<std::wstring, NsIconState>* by_clsid) {
  // section<TAB>clsid<TAB>existed<TAB>value
  std::vector<std::string> f;
  size_t start = 0;
  while (true) {
    const size_t tab = raw.find('\t', start);
    if (tab == std::string::npos) {
      f.push_back(raw.substr(start));
      break;
    }
    f.push_back(raw.substr(start, tab - start));
    start = tab + 1;
  }
  if (f.size() != 4) {
    LogLineA("[namespace] malformed state line (%zu fields): %s", f.size(),
             raw.c_str());
    return;
  }
  if (f[2] != "0" && f[2] != "1") {
    LogLineA("[namespace] invalid existed flag in state line: %s", raw.c_str());
    return;
  }
  if (!AllDigits(f[3])) {
    LogLineA("[namespace] non-numeric value in state line: %s", raw.c_str());
    return;
  }
  const std::wstring clsid = UnescapeField(f[1]);
  if (clsid.empty()) {
    LogLineA("[namespace] empty clsid in state line: %s", raw.c_str());
    return;
  }
  const bool existed = f[2] == "1";
  const DWORD value = static_cast<DWORD>(strtoul(f[3].c_str(), nullptr, 10));
  NsIconState& s = (*by_clsid)[clsid];
  s.clsid = clsid;
  if (f[0] == kSectionNsp) {
    s.nsp_existed = existed;
    s.nsp_value = value;
  } else if (f[0] == kSectionClassic) {
    s.classic_existed = existed;
    s.classic_value = value;
  } else {
    LogLineA("[namespace] unknown section in state line: %s", raw.c_str());
  }
}

// Safe fallback when the original state is unknown: restore to "visible"
// (existed=true, value=0) instead of deleting the value, so a user's hidden
// preference is never silently dropped.
void PushVisibleDefaults(std::vector<NsIconState>* out) {
  for (const auto& icon : kSystemIcons) {
    NsIconState s;
    s.clsid = icon.clsid;
    s.nsp_existed = true;
    s.nsp_value = 0;
    s.classic_existed = true;
    s.classic_value = 0;
    out->push_back(std::move(s));
  }
}

bool LoadIconStateV1(const std::string& content, std::vector<NsIconState>* out) {
  std::map<std::wstring, NsIconState> by_clsid;
  size_t pos = 0;
  while (pos <= content.size()) {
    size_t nl = content.find('\n', pos);
    if (nl == std::string::npos) {
      nl = content.size();
    }
    std::string raw = content.substr(pos, nl - pos);
    pos = nl + 1;
    if (!raw.empty() && raw.back() == '\r') {
      raw.pop_back();
    }
    if (raw.empty() || raw == kStateMagic) {
      continue;
    }
    ApplyStateLine(raw, &by_clsid);
  }
  for (auto& kv : by_clsid) {
    out->push_back(std::move(kv.second));
  }
  if (out->empty()) {
    LogLineA(
        "[namespace] state file present but no usable entries; restoring icons "
        "to visible");
    PushVisibleDefaults(out);
  }
  return !out->empty();
}

// Legacy hand-rolled JSON (written by older builds). Match each icon object by
// its own fields instead of splitting on braces, so CLSIDs with braces parse
// correctly. Uses a fallback-to-visible on any parse failure.
const std::regex kLegacyIconRe(
    R"RE("clsid"\s*:\s*"(\{[0-9A-Fa-f-]+\})"\s*,\s*"name"\s*:\s*"[^"]*"\s*,\s*"nsp_existed"\s*:\s*([01])\s*,\s*"nsp_value"\s*:\s*(\d+)\s*,\s*"classic_existed"\s*:\s*([01])\s*,\s*"classic_value"\s*:\s*(\d+))RE");

bool LoadIconStateLegacy(const std::string& content,
                         std::vector<NsIconState>* out) {
  std::sregex_iterator it(content.begin(), content.end(), kLegacyIconRe);
  const std::sregex_iterator end;
  for (; it != end; ++it) {
    const std::smatch& m = *it;
    NsIconState s;
    s.clsid = WidenUtf8(m[1].str());
    s.nsp_existed = m[2].str() == "1";
    s.nsp_value = static_cast<DWORD>(strtoul(m[3].str().c_str(), nullptr, 10));
    s.classic_existed = m[4].str() == "1";
    s.classic_value = static_cast<DWORD>(strtoul(m[5].str().c_str(), nullptr, 10));
    if (!s.clsid.empty()) {
      out->push_back(std::move(s));
    }
  }
  if (out->empty()) {
    LogLineA(
        "[namespace] legacy JSON state could not be parsed; restoring icons to "
        "visible (never deleting values)");
    PushVisibleDefaults(out);
  }
  return !out->empty();
}

bool LoadIconStateJson(std::vector<NsIconState>* out) {
  if (!out) {
    return false;
  }
  std::ifstream in(StateFilePath(), std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string content = ss.str();
  if (content.rfind(kStateMagic, 0) == 0) {
    return LoadIconStateV1(content, out);
  }
  return LoadIconStateLegacy(content, out);
}

void DeleteIconStateFile() {
  DeleteFileW(StateFilePath().c_str());
}

// Persist original visibility exactly once (before the first hide), so the
// state file always holds pre-hide values — never the hidden ones.
void PersistOriginalIconStateIfNeeded() {
  if (StateFileExists()) {
    return;
  }
  std::vector<NsIconState> states;
  for (const auto& icon : kSystemIcons) {
    states.push_back(CaptureIconState(icon));
  }
  if (SaveIconStateJson(states)) {
    LogLineA("[namespace] persisted original desktop icon visibility");
  }
}

// --- Explorer-rebuild guard (lazy, detached, <=2s FindWindow poll) ----------

std::atomic<bool> g_guard_stop{false};
std::atomic<bool> g_guard_started{false};
std::mutex g_guard_mutex;

void GuardLoop() {
  while (!g_guard_stop.load()) {
    Sleep(2000);
    if (g_guard_stop.load()) {
      break;
    }
    if (PollExplorerDesktopRebuild()) {
      LogLineA("[namespace] Explorer desktop rebuilt; re-applying hidden state");
      ReapplyNamespaceIconHiddenState();
    }
  }
  g_guard_started.store(false);
}

std::wstring ShortcutPathFor(const std::wstring& dir, const wchar_t* name) {
  return JoinPath(dir, std::wstring(name) + L".lnk");
}

// Returns true when the shortcut exists after the call. *created is set true
// only when a shortcut file was newly written this call — callers use it to skip
// the shell-wide SHCNE_ASSOCCHANGED rebuild when nothing changed.
bool CreateNamespaceShortcut(const std::wstring& dest, const wchar_t* clsid,
                             const wchar_t* display_name, bool* created) {
  if (created) {
    *created = false;
  }
  if (dest.empty() || !clsid) {
    return false;
  }
  // Already present — nothing changed, skip the icon-cache rebuild.
  if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return true;
  }

  IShellLinkW* psl = nullptr;
  HRESULT hr =
      CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                       IID_IShellLinkW, reinterpret_cast<void**>(&psl));
  if (FAILED(hr) || !psl) {
    return false;
  }

  std::wstring parsing = L"::";
  parsing += clsid;
  PIDLIST_ABSOLUTE pidl = nullptr;
  SFGAOF attrs = 0;
  hr = SHParseDisplayName(parsing.c_str(), nullptr, &pidl, 0, &attrs);
  if (SUCCEEDED(hr) && pidl) {
    psl->SetIDList(pidl);
    CoTaskMemFree(pidl);
  } else {
    // Fallback: TargetPath = ::{CLSID} (WScript.Shell style).
    psl->SetPath(parsing.c_str());
  }
  std::wstring desc = L"DeskTidyNS:";
  desc += clsid;
  psl->SetDescription(desc.c_str());
  if (display_name && display_name[0]) {
    // Icon location left to shell defaults for the namespace item.
  }

  IPersistFile* ppf = nullptr;
  hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
  bool saved = false;
  if (SUCCEEDED(hr) && ppf) {
    saved = SUCCEEDED(ppf->Save(dest.c_str(), TRUE));
    ppf->Release();
  }
  (void)saved;
  psl->Release();
  const bool ok = GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES;
  if (ok && created) {
    *created = true;
  }
  return ok;
}

}  // namespace

std::wstring PublicSystemDir() {
  static const std::wstring s_dir = []() {
    std::wstring d = JoinPath(NativeDataDir(), L".public_system");
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
  }();
  return s_dir;
}

void AppendPublicSystemItems(std::vector<DesktopItem>* out) {
  if (!out) {
    return;
  }
  const std::wstring dir = PublicSystemDir();
  const std::wstring pattern = dir + L"\\*.lnk";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    DesktopItem item;
    item.path = dir + L"\\" + fd.cFileName;
    item.name = fd.cFileName;
    item.is_link = true;
    if (item.name.size() > 4) {
      item.name.resize(item.name.size() - 4);
    }
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(item.path.c_str(), 0, &sfi, sizeof(sfi),
                       SHGFI_DISPLAYNAME) &&
        sfi.szDisplayName[0] != L'\0') {
      item.name = sfi.szDisplayName;
    }
    out->push_back(std::move(item));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}

int EnsureHostedSystemNamespaceIcons(PinMap* pins) {
  if (!pins) {
    return 0;
  }
  // Crash-safe: capture + persist the original visibility before first hide.
  PersistOriginalIconStateIfNeeded();
  const std::wstring dir = PublicSystemDir();
  // Returns the number of pin CHANGES (stale drops + new pins) so the caller
  // knows whether the map needs persisting.
  int changed = 0;
  // Set only when a shortcut file was actually newly written — gates the
  // shell-wide SHCNE_ASSOCCHANGED icon-cache rebuild (broadcasting it on every
  // organize/watch tick, even when nothing changed, is a desktop-flicker source).
  bool created_any = false;
  // Drop pins pointing at a PREVIOUS .public_system directory. The data root
  // moved once (Python .desktidy\storage → DeskTidyNative), leaving 回收站 /
  // 此电脑 pinned under both paths; RefreshDesktopIcons re-adds any pinned file
  // that still exists on disk, so the fence showed two of each icon.
  {
    const std::wstring suffix = L"\\.public_system\\";
    const std::wstring live_prefix = NormalizePinKey(dir) + L"\\";
    for (auto it = pins->begin(); it != pins->end();) {
      const std::wstring& p = it->first;  // already NormalizePinKey'd
      const size_t at = p.find(suffix);
      if (at == std::wstring::npos ||
          p.compare(0, live_prefix.size(), live_prefix) == 0) {
        ++it;
        continue;
      }
      LogLine(L"[namespace] dropping stale pin %s", p.c_str());
      it = pins->erase(it);
      ++changed;
    }
  }
  for (const auto& ns : kSystemIcons) {
    const std::wstring dest = ShortcutPathFor(dir, ns.name);
    bool created = false;
    if (!CreateNamespaceShortcut(dest, ns.clsid, ns.name, &created)) {
      continue;
    }
    created_any = created_any || created;
    SetDesktopNamespaceIconVisible(ns.clsid, false);
    // Prefer「常用」like Python ensure_hosted_system_namespace_public_icons.
    if (!FindPin(*pins, dest)) {
      SetPin(pins, dest, "system_common");
      ++changed;
    }
  }
  // Only rebuild the shell icon cache when a shortcut was actually created.
  // SHCNE_ASSOCCHANGED is a shell-wide icon-cache flush; broadcasting it on
  // every organize / watch tick — even when nothing changed — is a visible
  // desktop-flicker source.
  if (created_any) {
    RefreshDesktopNamespaceIcons();
  }
  StartNamespaceIconRebuildGuard();
  return changed;
}

void RestoreDesktopIconsIfHidden() {
  std::vector<NsIconState> states;
  if (!LoadIconStateJson(&states) || states.empty()) {
    return;  // no state file → no-op
  }
  for (const auto& s : states) {
    RestoreIconState(s);
  }
  DeleteIconStateFile();
  RefreshDesktopNamespaceIcons();
  LogLineA("[namespace] restored desktop namespace icons from state file");
}

std::wstring GetLnkNamespaceClsid(const std::wstring& lnk_path) {
  if (lnk_path.size() < 5) {
    return {};
  }
  const size_t dot = lnk_path.rfind(L'.');
  if (dot == std::wstring::npos) {
    return {};
  }
  std::wstring ext = lnk_path.substr(dot);
  for (auto& c : ext) {
    c = static_cast<wchar_t>(towlower(c));
  }
  if (ext != L".lnk") {
    return {};
  }
  IShellLinkW* psl = nullptr;
  HRESULT hr =
      CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                       IID_IShellLinkW, reinterpret_cast<void**>(&psl));
  if (FAILED(hr) || !psl) {
    return {};
  }
  IPersistFile* ppf = nullptr;
  hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
  std::wstring clsid;
  if (SUCCEEDED(hr) && ppf) {
    if (SUCCEEDED(ppf->Load(lnk_path.c_str(), STGM_READ))) {
      wchar_t desc[INFOTIPSIZE]{};
      if (SUCCEEDED(psl->GetDescription(desc, INFOTIPSIZE)) && desc[0]) {
        constexpr wchar_t kPrefix[] = L"DeskTidyNS:";
        const size_t plen = wcslen(kPrefix);
        if (wcsncmp(desc, kPrefix, plen) == 0 && desc[plen] == L'{') {
          clsid = desc + plen;
        }
      }
    }
    ppf->Release();
  }
  psl->Release();
  return clsid;
}

void ReturnNamespaceIconToDesktop(const std::wstring& clsid) {
  if (clsid.empty() || clsid.front() != L'{') {
    return;
  }
  SetDesktopNamespaceIconVisible(clsid.c_str(), true);
  RefreshDesktopNamespaceIcons();
  LogLine(L"[namespace] returned Explorer icon %s", clsid.c_str());
}

void ReapplyNamespaceIconHiddenState() {
  if (!StateFileExists()) {
    return;  // not currently hosting → nothing to re-hide
  }
  for (const auto& ns : kSystemIcons) {
    SetDesktopNamespaceIconVisible(ns.clsid, false);
  }
  RefreshDesktopNamespaceIcons();
}

void ReconcileNamespaceIconsOnStartup(bool should_hide) {
  if (!should_hide) {
    RestoreDesktopIconsIfHidden();
  }
  // should_hide → EnsureHostedSystemNamespaceIcons re-hides and keeps the
  // state file intact.
}

void StartNamespaceIconRebuildGuard() {
  if (g_guard_started.load()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_guard_mutex);
  if (g_guard_started.load()) {
    return;
  }
  g_guard_stop.store(false);
  g_guard_started.store(true);
  std::thread t(GuardLoop);
  t.detach();
}

void StopNamespaceIconRebuildGuard() {
  g_guard_stop.store(true);
}
