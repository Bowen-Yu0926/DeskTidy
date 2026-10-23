#include "hotkey_relay.h"

#include "log.h"
#include "paths_util.h"

#include <Windows.h>
#include <shellapi.h>
#include <ws2tcpip.h>

#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

bool g_wsa = false;

void EnsureWsa() {
  if (g_wsa) {
    return;
  }
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
    g_wsa = true;
  }
}

bool ProcessIsElevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_ELEVATION elev{};
  DWORD len = 0;
  const BOOL ok =
      GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len);
  CloseHandle(token);
  return ok && elev.TokenIsElevated != 0;
}

std::string JsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        o += "\\\"";
        break;
      case '\\':
        o += "\\\\";
        break;
      case '\n':
        o += "\\n";
        break;
      case '\r':
        o += "\\r";
        break;
      case '\t':
        o += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          sprintf_s(buf, "\\u%04x", c);
          o += buf;
        } else {
          o.push_back(static_cast<char>(c));
        }
    }
  }
  return o;
}

std::string MappingJson(const std::vector<HotkeyDef>& keys) {
  std::string out = "{";
  bool first = true;
  for (const auto& k : keys) {
    if (k.vk == 0) {
      continue;
    }
    if (!first) {
      out += ",";
    }
    first = false;
    out += "\"" + JsonEscape(k.id) + "\":\"" +
           JsonEscape(NarrowUtf8(FormatHotkey(k.modifiers, k.vk))) + "\"";
  }
  out += "}";
  return out;
}

bool CoversMonitor(HWND hwnd, const RECT& mon) {
  RECT wr{};
  if (!GetWindowRect(hwnd, &wr)) {
    return false;
  }
  constexpr int kTol = 8;
  return (wr.right - wr.left) > 0 && (wr.bottom - wr.top) > 0 &&
         wr.left <= mon.left + kTol && wr.top <= mon.top + kTol &&
         wr.right >= mon.right - kTol && wr.bottom >= mon.bottom - kTol;
}

bool IsShellFullscreenClass(HWND hwnd) {
  wchar_t cls[128]{};
  GetClassNameW(hwnd, cls, 128);
  return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
         wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
         wcscmp(cls, L"SysListView32") == 0 ||
         wcscmp(cls, L"Shell_TrayWnd") == 0 ||
         wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
         wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0;
}

struct EnumCtx {
  RECT mon{};
  DWORD own_pid = 0;
  HWND found = nullptr;
};

BOOL CALLBACK EnumCoverProc(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<EnumCtx*>(lp);
  if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
    return TRUE;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == ctx->own_pid) {
    return TRUE;
  }
  if (IsShellFullscreenClass(hwnd)) {
    return TRUE;
  }
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if (ex & WS_EX_TOOLWINDOW) {
    return TRUE;
  }
  if (CoversMonitor(hwnd, ctx->mon)) {
    ctx->found = hwnd;
    return FALSE;
  }
  return TRUE;
}

}  // namespace

DWORD WINAPI HotkeyRelay::ThreadProc(LPVOID param) {
  auto* self = static_cast<HotkeyRelay*>(param);
  self->ServeLoop();
  return 0;
}

HotkeyRelay::~HotkeyRelay() { Stop(); }

bool HotkeyRelay::Start(HWND notify_hwnd, UINT notify_msg,
                        const std::vector<HotkeyDef>& keys) {
  if (ProcessIsElevated()) {
    LogLineA("[hotkey_relay] main already elevated — skip helper");
    return true;
  }
  if (Running()) {
    UpdateMapping(keys);
    return true;
  }
  EnsureWsa();
  if (!g_wsa) {
    return false;
  }
  notify_hwnd_ = notify_hwnd;
  notify_msg_ = notify_msg;
  sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock_ == INVALID_SOCKET) {
    return false;
  }
  BOOL reuse = TRUE;
  setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
             sizeof(reuse));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
    return false;
  }
  if (listen(sock_, 1) != 0) {
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
    return false;
  }
  int len = sizeof(addr);
  getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  stop_ = 0;
  if (!WriteMapping(keys)) {
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
    return false;
  }
  thread_ = CreateThread(nullptr, 0, &HotkeyRelay::ThreadProc, this, 0, nullptr);
  if (!thread_) {
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
    return false;
  }
  helper_launched_ = LaunchHelper();
  if (!helper_launched_) {
    LogLineA("[hotkey_relay] elevated helper launch failed (UAC cancel?)");
  } else {
    LogLine(L"[hotkey_relay] helper launched port=%d", port_);
  }
  return true;
}

void HotkeyRelay::UpdateMapping(const std::vector<HotkeyDef>& keys) {
  if (mapping_path_.empty()) {
    return;
  }
  WriteMapping(keys);
  if (!helper_launched_) {
    helper_launched_ = LaunchHelper();
    if (helper_launched_) {
      LogLine(L"[hotkey_relay] helper launched (retry) port=%d", port_);
    }
  }
}

void HotkeyRelay::Stop() {
  InterlockedExchange(&stop_, 1);
  if (sock_ != INVALID_SOCKET) {
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
  }
  if (thread_) {
    WaitForSingleObject(thread_, 2000);
    CloseHandle(thread_);
    thread_ = nullptr;
  }
  if (!mapping_path_.empty()) {
    DeleteFileW(mapping_path_.c_str());
    mapping_path_.clear();
  }
  port_ = 0;
  helper_launched_ = false;
}

bool HotkeyRelay::WriteMapping(const std::vector<HotkeyDef>& keys) {
  if (mapping_path_.empty()) {
    wchar_t tmp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tmp) == 0) {
      return false;
    }
    wchar_t file[MAX_PATH]{};
    if (GetTempFileNameW(tmp, L"dthk", 0, file) == 0) {
      return false;
    }
    mapping_path_ = file;
    // Prefer .json suffix for readability.
    const std::wstring json_path = mapping_path_ + L".json";
    MoveFileExW(mapping_path_.c_str(), json_path.c_str(),
                MOVEFILE_REPLACE_EXISTING);
    mapping_path_ = json_path;
  }
  const std::string body = MappingJson(keys);
  std::ofstream out(mapping_path_, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out.write(body.data(), static_cast<std::streamsize>(body.size()));
  return static_cast<bool>(out);
}

bool HotkeyRelay::LaunchHelper() {
  wchar_t exe[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) {
    return false;
  }
  std::wstring params = L"--hotkey-helper --port ";
  params += std::to_wstring(port_);
  params += L" --mapping \"";
  params += mapping_path_;
  params += L"\"";
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = exe;
  sei.lpParameters = params.c_str();
  sei.nShow = SW_HIDE;
  if (!ShellExecuteExW(&sei)) {
    return false;
  }
  if (sei.hProcess) {
    CloseHandle(sei.hProcess);
  }
  return true;
}

void HotkeyRelay::ServeLoop() {
  while (InterlockedCompareExchange(&stop_, 0, 0) == 0) {
    fd_set rd{};
    FD_ZERO(&rd);
    FD_SET(sock_, &rd);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 500000;
    const int sel = select(0, &rd, nullptr, nullptr, &tv);
    if (sel <= 0) {
      continue;
    }
    SOCKET client = accept(sock_, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      break;
    }
    char buf[256]{};
    const int n = recv(client, buf, sizeof(buf) - 1, 0);
    closesocket(client);
    if (n <= 0 || !notify_hwnd_ || !notify_msg_) {
      continue;
    }
    buf[n] = 0;
    std::string action(buf);
    while (!action.empty() &&
           (action.back() == '\n' || action.back() == '\r' ||
            action.back() == ' ')) {
      action.pop_back();
    }
    if (action.empty()) {
      continue;
    }
    auto* heap = new std::string(std::move(action));
    if (!PostMessageW(notify_hwnd_, notify_msg_, 0,
                      reinterpret_cast<LPARAM>(heap))) {
      delete heap;
    }
  }
}

bool MinimizeFullscreenCover() {
  HWND fg = GetForegroundWindow();
  HMONITOR mon = MonitorFromWindow(fg ? fg : GetDesktopWindow(),
                                   MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  if (!GetMonitorInfoW(mon, &mi)) {
    return false;
  }
  HWND target = nullptr;
  if (fg && IsWindowVisible(fg) && !IsIconic(fg) &&
      !IsShellFullscreenClass(fg) && CoversMonitor(fg, mi.rcMonitor)) {
    target = fg;
  } else {
    EnumCtx ctx;
    ctx.mon = mi.rcMonitor;
    ctx.own_pid = GetCurrentProcessId();
    EnumWindows(EnumCoverProc, reinterpret_cast<LPARAM>(&ctx));
    target = ctx.found;
  }
  if (!target || IsIconic(target)) {
    return false;
  }
  if (!PostMessageW(target, WM_SYSCOMMAND, SC_MINIMIZE, 0)) {
    ShowWindow(target, SW_MINIMIZE);
  }
  return true;
}
