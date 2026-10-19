#include "shell_icons.h"

#include "paths_util.h"

#include <Windows.h>
#include <CommCtrl.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

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

HWND FindDesktopListView() {
  HWND progman = FindWindowW(L"Progman", nullptr);
  if (progman) {
    HWND def = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (def) {
      HWND lv = FindWindowExW(def, nullptr, L"SysListView32", nullptr);
      if (lv) {
        return lv;
      }
    }
  }
  HWND worker = nullptr;
  while ((worker = FindWindowExW(nullptr, worker, L"WorkerW", nullptr)) !=
         nullptr) {
    HWND def = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!def) {
      continue;
    }
    HWND lv = FindWindowExW(def, nullptr, L"SysListView32", nullptr);
    if (lv) {
      return lv;
    }
  }
  return nullptr;
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
      const LPARAM packed =
          (static_cast<LPARAM>(static_cast<UINT>(icon.y)) << 16) |
          (static_cast<UINT>(icon.x) & 0xFFFF);
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
    out << "    {\"name\":\"" << NarrowUtf8(icons[i].name)
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
  size_t pos = 0;
  while (true) {
    const size_t start = json.find('{', pos);
    if (start == std::string::npos) {
      break;
    }
    const size_t end = json.find('}', start);
    if (end == std::string::npos) {
      break;
    }
    const std::string obj = json.substr(start, end - start + 1);
    pos = end + 1;
    const size_t np = obj.find("\"name\"");
    if (np == std::string::npos) {
      continue;
    }
    size_t q = obj.find('"', obj.find(':', np) + 1);
    if (q == std::string::npos) {
      continue;
    }
    ++q;
    std::string name;
    while (q < obj.size() && obj[q] != '"') {
      if (obj[q] == '\\' && q + 1 < obj.size()) {
        ++q;
      }
      name.push_back(obj[q++]);
    }
    auto find_num = [&](const char* key) {
      const size_t p = obj.find(key);
      if (p == std::string::npos) {
        return 0;
      }
      size_t c = obj.find(':', p);
      if (c == std::string::npos) {
        return 0;
      }
      return atoi(obj.c_str() + c + 1);
    };
    ShellIconPos icon;
    icon.name = WidenUtf8(name);
    icon.x = find_num("\"x\"");
    icon.y = find_num("\"y\"");
    if (!icon.name.empty()) {
      out.push_back(std::move(icon));
    }
  }
  return out;
}
