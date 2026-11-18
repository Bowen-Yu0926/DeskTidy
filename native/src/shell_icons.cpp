#include "shell_icons.h"

#include "log.h"
#include "paths_util.h"

#include <Windows.h>
#include <CommCtrl.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr UINT kWmCommand = 0x0111;
constexpr WPARAM kToggleDesktopIcons = 0x7402;
constexpr wchar_t kExplorerAdvanced[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

constexpr UINT kLvmGetItemCount = 0x1004;
constexpr UINT kLvmGetItemPosition = 0x1010;
constexpr UINT kLvmSetItemPosition = 0x100F;
constexpr UINT kLvmGetItemTextW = 0x1073;

struct POINT_REMOTE {
  LONG x;
  LONG y;
};

struct LVITEMW_REMOTE {
  UINT mask;
  int iItem;
  int iSubItem;
  UINT state;
  UINT stateMask;
  LPWSTR pszText;
  int cchTextMax;
  int iImage;
  LPARAM lParam;
  int iIndent;
  int iGroupId;
  UINT cColumns;
  PUINT puColumns;
  int* piColFmt;
  int iGroup;
};

HWND FindDesktopDefView() {
  HWND progman = FindWindowW(L"Progman", nullptr);
  if (progman) {
    HWND def = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (def) {
      return def;
    }
  }
  HWND worker = nullptr;
  while ((worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) !=
         nullptr) {
    HWND def = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
    if (def) {
      return def;
    }
  }
  return nullptr;
}

HWND FindDesktopListView() {
  HWND def = FindDesktopDefView();
  if (!def) {
    return nullptr;
  }
  return FindWindowExW(def, nullptr, L"SysListView32", nullptr);
}

bool ReadHideIconsRegistry(bool* hidden) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kExplorerAdvanced, 0, KEY_READ, &key) !=
      ERROR_SUCCESS) {
    return false;
  }
  DWORD val = 0;
  DWORD type = 0;
  DWORD size = sizeof(val);
  const LONG rc =
      RegQueryValueExW(key, L"HideIcons", nullptr, &type, reinterpret_cast<LPBYTE>(&val),
                       &size);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS || type != REG_DWORD) {
    return false;
  }
  *hidden = val != 0;
  return true;
}

void WriteHideIconsRegistry(bool hidden) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kExplorerAdvanced, 0, KEY_SET_VALUE,
                    &key) != ERROR_SUCCESS) {
    return;
  }
  DWORD val = hidden ? 1u : 0u;
  RegSetValueExW(key, L"HideIcons", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&val), sizeof(val));
  RegCloseKey(key);
}

bool RemoteReadText(HANDLE proc, HWND lv, int index, std::wstring* out) {
  const SIZE_T text_bytes = 512 * sizeof(wchar_t);
  const SIZE_T item_bytes = sizeof(LVITEMW_REMOTE);
  LPVOID remote_text = VirtualAllocEx(proc, nullptr, text_bytes, MEM_COMMIT,
                                      PAGE_READWRITE);
  LPVOID remote_item =
      VirtualAllocEx(proc, nullptr, item_bytes, MEM_COMMIT, PAGE_READWRITE);
  if (!remote_text || !remote_item) {
    if (remote_text) {
      VirtualFreeEx(proc, remote_text, 0, MEM_RELEASE);
    }
    if (remote_item) {
      VirtualFreeEx(proc, remote_item, 0, MEM_RELEASE);
    }
    return false;
  }
  LVITEMW_REMOTE item{};
  item.mask = LVIF_TEXT;
  item.iItem = index;
  item.pszText = reinterpret_cast<LPWSTR>(remote_text);
  item.cchTextMax = 512;
  WriteProcessMemory(proc, remote_item, &item, sizeof(item), nullptr);
  SendMessageTimeoutW(lv, kLvmGetItemTextW, index,
                      reinterpret_cast<LPARAM>(remote_item), SMTO_ABORTIFHUNG,
                      80, nullptr);
  wchar_t local[512] = {};
  ReadProcessMemory(proc, remote_text, local, sizeof(local) - sizeof(wchar_t),
                    nullptr);
  VirtualFreeEx(proc, remote_text, 0, MEM_RELEASE);
  VirtualFreeEx(proc, remote_item, 0, MEM_RELEASE);
  *out = local;
  return !out->empty();
}

bool RemoteGetPos(HANDLE proc, HWND lv, int index, int* x, int* y) {
  LPVOID remote =
      VirtualAllocEx(proc, nullptr, sizeof(POINT_REMOTE), MEM_COMMIT,
                     PAGE_READWRITE);
  if (!remote) {
    return false;
  }
  SendMessageTimeoutW(lv, kLvmGetItemPosition, index,
                      reinterpret_cast<LPARAM>(remote), SMTO_ABORTIFHUNG, 80,
                      nullptr);
  POINT_REMOTE pt{};
  ReadProcessMemory(proc, remote, &pt, sizeof(pt), nullptr);
  VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
  *x = pt.x;
  *y = pt.y;
  return true;
}

}  // namespace

std::vector<ShellIconPos> CaptureShellIconPositions() {
  std::vector<ShellIconPos> out;
  HWND lv = FindDesktopListView();
  if (!lv) {
    return out;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(lv, &pid);
  if (!pid) {
    return out;
  }
  HANDLE proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ |
                                PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
                            FALSE, pid);
  if (!proc) {
    return out;
  }
  const int count =
      static_cast<int>(SendMessageW(lv, kLvmGetItemCount, 0, 0));
  int empty = 0;
  for (int i = 0; i < count && i < 256; ++i) {
    ShellIconPos icon;
    if (!RemoteReadText(proc, lv, i, &icon.name) || icon.name.empty()) {
      ++empty;
      if (empty >= 3 && out.empty()) {
        break;
      }
      continue;
    }
    empty = 0;
    RemoteGetPos(proc, lv, i, &icon.x, &icon.y);
    out.push_back(std::move(icon));
  }
  CloseHandle(proc);
  return out;
}

int RestoreShellIconPositions(const std::vector<ShellIconPos>& icons) {
  if (icons.empty()) {
    return 0;
  }
  HWND lv = FindDesktopListView();
  if (!lv) {
    return 0;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(lv, &pid);
  HANDLE proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ |
                                PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
                            FALSE, pid);
  if (!proc) {
    return 0;
  }
  const int count =
      static_cast<int>(SendMessageW(lv, kLvmGetItemCount, 0, 0));
  int restored = 0;
  for (int i = 0; i < count && i < 256; ++i) {
    std::wstring name;
    if (!RemoteReadText(proc, lv, i, &name)) {
      continue;
    }
    for (const auto& icon : icons) {
      if (_wcsicmp(icon.name.c_str(), name.c_str()) != 0) {
        continue;
      }
      const LPARAM packed = MAKELPARAM(icon.x, icon.y);
      SendMessageTimeoutW(lv, kLvmSetItemPosition, i, packed, SMTO_ABORTIFHUNG,
                          80, nullptr);
      ++restored;
      break;
    }
  }
  CloseHandle(proc);
  return restored;
}

bool SaveShellIconsSidecar(const std::wstring& snapshot_json_path,
                           const std::vector<ShellIconPos>& icons) {
  std::wstring path = snapshot_json_path;
  const size_t dot = path.find_last_of(L'.');
  if (dot != std::wstring::npos) {
    path = path.substr(0, dot) + L".icons.json";
  } else {
    path += L".icons.json";
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << "{\n  \"icons\": [\n";
  for (size_t i = 0; i < icons.size(); ++i) {
    const std::string raw = NarrowUtf8(icons[i].name);
    std::string esc;
    esc.reserve(raw.size() + 4);
    for (char c : raw) {
      if (c == '\\' || c == '"') {
        esc.push_back('\\');
      }
      esc.push_back(c);
    }
    out << "    {\"name\":\"" << esc
        << "\",\"x\":" << icons[i].x << ",\"y\":" << icons[i].y << "}";
    if (i + 1 < icons.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
  return true;
}

std::vector<ShellIconPos> LoadShellIconsSidecar(
    const std::wstring& snapshot_json_path) {
  std::vector<ShellIconPos> out;
  std::wstring path = snapshot_json_path;
  const size_t dot = path.find_last_of(L'.');
  if (dot != std::wstring::npos) {
    path = path.substr(0, dot) + L".icons.json";
  } else {
    path += L".icons.json";
  }
  std::ifstream in(path);
  if (!in) {
    return out;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  // Brace-safe parse: match each icon object by its fields rather than
  // splitting on '{'/'}', so desktop icon names containing braces are handled
  // correctly.
  static const std::regex kIconRe(
      R"RX("name"\s*:\s*"((?:[^"\\]|\\.)*)"\s*,\s*"x"\s*:\s*(-?\d+)\s*,\s*"y"\s*:\s*(-?\d+))RX");
  std::sregex_iterator it(json.begin(), json.end(), kIconRe);
  const std::sregex_iterator end;
  for (; it != end; ++it) {
    const std::smatch& m = *it;
    const std::string raw_name = m[1].str();
    std::string unescaped;
    unescaped.reserve(raw_name.size());
    for (size_t j = 0; j < raw_name.size(); ++j) {
      if (raw_name[j] == '\\' && j + 1 < raw_name.size()) {
        ++j;
      }
      unescaped.push_back(raw_name[j]);
    }
    ShellIconPos icon;
    icon.name = WidenUtf8(unescaped);
    icon.x = atoi(m[2].str().c_str());
    icon.y = atoi(m[3].str().c_str());
    if (icon.name.empty()) {
      LogLineA("[shell_icons] sidecar entry with empty name skipped");
      continue;
    }
    out.push_back(std::move(icon));
  }
  if (out.empty() && !json.empty()) {
    LogLineA("[shell_icons] sidecar parse produced no entries: %.120s",
             json.c_str());
  }
  return out;
}

bool AreDesktopIconsVisible() {
  HWND lv = FindDesktopListView();
  if (lv) {
    return IsWindowVisible(lv) != FALSE;
  }
  bool hidden = false;
  if (ReadHideIconsRegistry(&hidden)) {
    return !hidden;
  }
  return true;
}

void SetDesktopIconsVisible(bool visible) {
  const bool want_hidden = !visible;
  bool currently_visible = AreDesktopIconsVisible();
  if (currently_visible == visible) {
    bool hidden = false;
    if (ReadHideIconsRegistry(&hidden) && hidden != want_hidden) {
      WriteHideIconsRegistry(want_hidden);
    }
    return;
  }
  HWND def = FindDesktopDefView();
  if (def) {
    SendMessageW(def, kWmCommand, kToggleDesktopIcons, 0);
  }
  bool hidden = false;
  if (ReadHideIconsRegistry(&hidden) && hidden != want_hidden) {
    WriteHideIconsRegistry(want_hidden);
  }
}
