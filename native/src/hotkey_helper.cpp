#include "hotkey_helper.h"

#include "hotkey_store.h"
#include "log.h"
#include "paths_util.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <cwctype>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr const char* kShowDesktop = "show_desktop";
constexpr DWORD kPingIntervalMs = 5000;
constexpr int kPingFailLimit = 3;

HHOOK g_hook = nullptr;
int g_port = 0;
struct Chord {
  UINT mods = 0;
  UINT vk = 0;
  std::string action;
};
std::vector<Chord> g_chords;
std::unordered_map<std::string, UINT> g_fkeys;  // action → vk (mods==0)
std::unordered_map<std::string, bool> g_fkey_prev;

bool PidElevated(DWORD pid) {
  HANDLE proc =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) {
    return false;
  }
  HANDLE token = nullptr;
  bool elev = false;
  if (OpenProcessToken(proc, TOKEN_QUERY, &token)) {
    TOKEN_ELEVATION te{};
    DWORD len = 0;
    if (GetTokenInformation(token, TokenElevation, &te, sizeof(te), &len)) {
      elev = te.TokenIsElevated != 0;
    }
    CloseHandle(token);
  }
  CloseHandle(proc);
  return elev;
}

bool ForegroundElevated() {
  HWND fg = GetForegroundWindow();
  if (!fg) {
    return false;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid != 0 && PidElevated(pid);
}

bool Relay(const std::string& action) {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(g_port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  DWORD timeout = 300;
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closesocket(s);
    return false;
  }
  const std::string line = action + "\n";
  send(s, line.c_str(), static_cast<int>(line.size()), 0);
  closesocket(s);
  return true;
}

bool Ping() {
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<u_short>(g_port));
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  DWORD timeout = 300;
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
             sizeof(timeout));
  const bool ok =
      connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  closesocket(s);
  return ok;
}

UINT CurrentMods() {
  UINT mods = 0;
  if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
    mods |= MOD_CONTROL;
  }
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
    mods |= MOD_SHIFT;
  }
  if (GetAsyncKeyState(VK_MENU) & 0x8000) {
    mods |= MOD_ALT;
  }
  if ((GetAsyncKeyState(VK_LWIN) & 0x8000) ||
      (GetAsyncKeyState(VK_RWIN) & 0x8000)) {
    mods |= MOD_WIN;
  }
  return mods;
}

LRESULT CALLBACK HelperKbProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION &&
      (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
    const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (kb) {
      const UINT vk = kb->vkCode;
      const UINT mods = CurrentMods();
      // Win+D: always passthrough; relay show_desktop only when FG elevated.
      if ((mods & MOD_WIN) && vk == 'D') {
        if (ForegroundElevated()) {
          Relay(kShowDesktop);
        }
      } else if (ForegroundElevated()) {
        for (const auto& c : g_chords) {
          if (c.vk == vk && c.mods == mods) {
            Relay(c.action);
            break;
          }
        }
      }
    }
  }
  return CallNextHookEx(g_hook, code, wparam, lparam);
}

// Minimal JSON object parser: "key":"value" pairs (hotkey mapping only).
bool LoadMappingFile(const std::wstring& path,
                     std::unordered_map<std::string, std::string>* out) {
  out->clear();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::string raw((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
  size_t i = 0;
  auto skip_ws = [&] {
    while (i < raw.size() &&
           (raw[i] == ' ' || raw[i] == '\n' || raw[i] == '\r' ||
            raw[i] == '\t' || raw[i] == ',' || raw[i] == '{' ||
            raw[i] == '}')) {
      ++i;
    }
  };
  auto parse_str = [&](std::string* s) -> bool {
    skip_ws();
    if (i >= raw.size() || raw[i] != '"') {
      return false;
    }
    ++i;
    s->clear();
    while (i < raw.size() && raw[i] != '"') {
      if (raw[i] == '\\' && i + 1 < raw.size()) {
        ++i;
        s->push_back(raw[i++]);
      } else {
        s->push_back(raw[i++]);
      }
    }
    if (i >= raw.size() || raw[i] != '"') {
      return false;
    }
    ++i;
    return true;
  };
  skip_ws();
  while (i < raw.size()) {
    std::string key, val;
    if (!parse_str(&key)) {
      break;
    }
    skip_ws();
    if (i < raw.size() && raw[i] == ':') {
      ++i;
    }
    if (!parse_str(&val)) {
      break;
    }
    (*out)[key] = val;
    skip_ws();
  }
  return true;
}

void ApplyMapping(const std::unordered_map<std::string, std::string>& map) {
  g_chords.clear();
  g_fkeys.clear();
  for (const auto& kv : map) {
    UINT mods = 0, vk = 0;
    if (!ParseHotkey(WidenUtf8(kv.second), &mods, &vk) || vk == 0) {
      continue;
    }
    if (mods == 0) {
      g_fkeys[kv.first] = vk;
    } else {
      Chord c;
      c.mods = mods;
      c.vk = vk;
      c.action = kv.first;
      g_chords.push_back(c);
    }
  }
}

void PollFkeys() {
  if (!ForegroundElevated()) {
    for (auto& kv : g_fkey_prev) {
      kv.second = false;
    }
    // Still track state so edge detect works after FG becomes elevated.
    for (const auto& kv : g_fkeys) {
      const bool down = (GetAsyncKeyState(kv.second) & 0x8000) != 0;
      g_fkey_prev[kv.first] = down;
    }
    return;
  }
  for (const auto& kv : g_fkeys) {
    const bool down = (GetAsyncKeyState(kv.second) & 0x8000) != 0;
    const bool was = g_fkey_prev[kv.first];
    if (down && !was) {
      Relay(kv.first);
    }
    g_fkey_prev[kv.first] = down;
  }
}

ULONGLONG MappingMtime(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
    return 0;
  }
  ULARGE_INTEGER u{};
  u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
  u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
  return u.QuadPart;
}

int ParseIntArg(const std::wstring& cmd, const wchar_t* flag) {
  const std::wstring f = flag;
  size_t p = cmd.find(f);
  if (p == std::wstring::npos) {
    return 0;
  }
  p += f.size();
  while (p < cmd.size() && (cmd[p] == L' ' || cmd[p] == L'=')) {
    ++p;
  }
  int v = 0;
  while (p < cmd.size() && iswdigit(cmd[p])) {
    v = v * 10 + (cmd[p] - L'0');
    ++p;
  }
  return v;
}

std::wstring ParseQuotedArg(const std::wstring& cmd, const wchar_t* flag) {
  const std::wstring f = flag;
  size_t p = cmd.find(f);
  if (p == std::wstring::npos) {
    return {};
  }
  p += f.size();
  while (p < cmd.size() && (cmd[p] == L' ' || cmd[p] == L'=')) {
    ++p;
  }
  if (p >= cmd.size()) {
    return {};
  }
  if (cmd[p] == L'"') {
    ++p;
    const size_t end = cmd.find(L'"', p);
    if (end == std::wstring::npos) {
      return cmd.substr(p);
    }
    return cmd.substr(p, end - p);
  }
  size_t end = p;
  while (end < cmd.size() && cmd[end] != L' ') {
    ++end;
  }
  return cmd.substr(p, end - p);
}

}  // namespace

int RunHotkeyHelper(int port, const std::wstring& mapping_path) {
  if (port <= 0 || mapping_path.empty()) {
    return 2;
  }
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return 3;
  }
  g_port = port;
  std::unordered_map<std::string, std::string> map;
  if (!LoadMappingFile(mapping_path, &map)) {
    WSACleanup();
    return 4;
  }
  ApplyMapping(map);
  ULONGLONG last_mtime = MappingMtime(mapping_path);

  g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, HelperKbProc, nullptr, 0);
  if (!g_hook) {
    WSACleanup();
    return 5;
  }
  LogLine(L"[hotkey_helper] running port=%d chords=%zu fkeys=%zu", port,
          g_chords.size(), g_fkeys.size());

  DWORD last_ping = GetTickCount();
  int ping_fail = 0;
  MSG msg{};
  while (true) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        WSACleanup();
        return 0;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    PollFkeys();
    const ULONGLONG mt = MappingMtime(mapping_path);
    if (mt != 0 && mt != last_mtime) {
      last_mtime = mt;
      if (LoadMappingFile(mapping_path, &map)) {
        ApplyMapping(map);
        LogLine(L"[hotkey_helper] mapping reloaded chords=%zu", g_chords.size());
      }
    }
    const DWORD now = GetTickCount();
    if (now - last_ping >= kPingIntervalMs) {
      last_ping = now;
      if (Ping()) {
        ping_fail = 0;
      } else {
        ++ping_fail;
        if (ping_fail >= kPingFailLimit) {
          LogLineA("[hotkey_helper] main gone — exit");
          break;
        }
      }
    }
    Sleep(20);
  }
  UnhookWindowsHookEx(g_hook);
  g_hook = nullptr;
  WSACleanup();
  return 0;
}

int RunHotkeyHelperFromCmd(const std::wstring& cmd_line) {
  const int port = ParseIntArg(cmd_line, L"--port");
  const std::wstring mapping = ParseQuotedArg(cmd_line, L"--mapping");
  return RunHotkeyHelper(port, mapping);
}
