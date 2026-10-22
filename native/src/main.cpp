#include "app.h"
#include "cli_ipc.h"
#include "instance_lock.h"
#include "log.h"
#include "native_cleanup.h"
#include "update_check.h"

#include <Windows.h>
#include <ole2.h>

#include <cwctype>
#include <string>

namespace {

std::wstring FindCliAction(LPWSTR cmd) {
  if (!cmd) {
    return {};
  }
  const wchar_t* keys[] = {
      L"--settings",        L"--refresh-fences", L"--refresh",
      L"--organize",        L"--notes",          L"--search",
      L"--shot",            L"--todos",          L"--peek",
      L"--new-fence",       L"--vault",          L"--show-window",
      L"--quit-silent",     L"--quit",           L"--status"};
  for (const wchar_t* k : keys) {
    if (wcsstr(cmd, k)) {
      return k;
    }
  }
  return {};
}

// Build "--notes" or "--notes <path>" from the raw command line. The path is
// everything after "--notes" up to the next "--flag" (trimmed of quotes/space).
std::wstring NotesActionFromCmd(const std::wstring& cmd) {
  const size_t pos = cmd.find(L"--notes");
  if (pos == std::wstring::npos) {
    return {};
  }
  size_t start = pos + 7;
  size_t end = cmd.find(L"--", start);
  if (end == std::wstring::npos) {
    end = cmd.size();
  }
  std::wstring rest = cmd.substr(start, end - start);
  while (!rest.empty() &&
         (rest.front() == L' ' || rest.front() == L'"' || rest.front() == L'\'')) {
    rest.erase(rest.begin());
  }
  while (!rest.empty() &&
         (rest.back() == L' ' || rest.back() == L'"' || rest.back() == L'\'')) {
    rest.pop_back();
  }
  if (rest.empty()) {
    return L"--notes";
  }
  return L"--notes " + rest;
}

// Parse "--x N"/"--y N" (with optional '=' and sign) from the command line.
int CoordFromCmd(const std::wstring& cmd, const wchar_t* flag) {
  const std::wstring f = flag;
  size_t p = cmd.find(f);
  if (p == std::wstring::npos) {
    return -1;
  }
  p += f.size();
  while (p < cmd.size() && (cmd[p] == L' ' || cmd[p] == L'=')) {
    ++p;
  }
  int v = 0;
  int sign = 1;
  if (p < cmd.size() && cmd[p] == L'-') {
    sign = -1;
    ++p;
  }
  while (p < cmd.size() && iswdigit(cmd[p])) {
    v = v * 10 + (cmd[p] - L'0');
    ++p;
  }
  return sign * v;
}

// Encode optional x/y coordinates onto "--new-fence" as ":x:y" so the running
// instance can place the fence at the shell-verb click point.
std::wstring WithFenceCoords(const std::wstring& action,
                             const std::wstring& cmd) {
  const int x = CoordFromCmd(cmd, L"--x");
  const int y = CoordFromCmd(cmd, L"--y");
  if (x >= 0 && y >= 0) {
    return action + L":" + std::to_wstring(x) + L":" + std::to_wstring(y);
  }
  return action;
}

// Print running state + version to the parent console or a redirected pipe/file.
// WriteConsoleW for a real console (UTF-16); UTF-8 bytes otherwise so `> file`
// and PowerShell pipelines capture readable text.
void PrintStatus() {
  const bool running = IsNativeInstanceRunning();
  const std::wstring ver = NativeProductVersion();
  const std::wstring line = L"DeskTidy " + ver + L" " +
                            (running ? L"running" : L"stopped") + L"\r\n";
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
    AllocConsole();
  }
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!out || out == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  if (GetFileType(out) == FILE_TYPE_CHAR) {
    WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()), &written,
                  nullptr);
  } else {
    const int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                      static_cast<int>(line.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                        utf8.data(), n, nullptr, nullptr);
    WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
              nullptr);
  }
  FreeConsole();
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int) {
  LogInit();
  LogInstallCrashHandler();

  const std::wstring full_cmd = cmd_line ? cmd_line : L"";
  LogLine(L"[boot] DeskTidy Native %s cmdline=%s",
          NativeProductVersion().c_str(), full_cmd.c_str());

  // Installer-driven headless cleanup: run before any UI / instance lock so it
  // can stop a live instance and clear its state. Never enters the app loop.
  if (full_cmd.find(L"--uninstall-cleanup") != std::wstring::npos) {
    LogLineA("[uninstall] handling --uninstall-cleanup");
    const int rc = RunUninstallCleanup();
    LogLine(L"[uninstall] cleanup finished rc=%d", rc);
    LogLine(L"[boot] exit (uninstall-cleanup)");
    LogShutdown();
    return rc;
  }

  // Per-monitor DPI so fence icons are not bitmap-upscaled by the system.
  using SetDpiCtxFn = BOOL(WINAPI*)(void*);
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32) {
    auto set_ctx = reinterpret_cast<SetDpiCtxFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_ctx) {
      // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (DPI_AWARENESS_CONTEXT)-4
      set_ctx(reinterpret_cast<void*>(static_cast<INT_PTR>(-4)));
    } else {
      using SetDpiAwareFn = BOOL(WINAPI*)();
      auto set_aware = reinterpret_cast<SetDpiAwareFn>(
          GetProcAddress(user32, "SetProcessDPIAware"));
      if (set_aware) {
        set_aware();
      }
    }
  }

  const std::wstring action = FindCliAction(cmd_line);

  // T6: --status reports run state + version without starting or locking.
  if (action == L"--status") {
    PrintStatus();
    LogShutdown();
    return 0;
  }

  // For "--notes <path>", forward/handle the full action so the running
  // instance opens the file (N11) instead of just toggling the notes window.
  std::wstring effective_action = action;
  if (action == L"--notes") {
    const std::wstring notes_action = NotesActionFromCmd(full_cmd);
    if (!notes_action.empty()) {
      effective_action = notes_action;
    }
  }
  // T6: attach optional x/y coordinates onto --new-fence (shell-verb parity).
  if (effective_action == L"--new-fence") {
    effective_action = WithFenceCoords(effective_action, full_cmd);
  }

  if (!AcquireNativeInstanceLock()) {
    if (effective_action.empty()) {
      // T6: second launch focuses the existing instance instead of a dead
      // "already running" dialog.
      effective_action = L"--focus";
    }
    if (ForwardCliToRunningInstance(effective_action)) {
      LogLine(L"[boot] forwarded CLI action to running instance: %s",
              effective_action.c_str());
      LogShutdown();
      return 0;
    }
    MessageBoxW(nullptr, L"DeskTidy Native 已在运行。", L"DeskTidy Native",
                MB_OK | MB_ICONINFORMATION);
    LogLineA("[boot] instance lock held by another process, exiting");
    LogShutdown();
    return 0;
  }

  const HRESULT hr = OleInitialize(nullptr);
  if (FAILED(hr) && hr != S_FALSE && hr != OLE_E_WRONGCOMPOBJ) {
    ReleaseNativeInstanceLock();
    LogLineA("[boot] OleInitialize failed");
    LogShutdown();
    return 1;
  }

  App app;
  if (!app.Init(instance)) {
    MessageBoxW(nullptr, L"Failed to initialize DeskTidy Native.",
                L"DeskTidy Native", MB_ICONERROR);
    OleUninitialize();
    ReleaseNativeInstanceLock();
    LogLineA("[boot] App::Init failed");
    LogShutdown();
    return 1;
  }

  if (!effective_action.empty()) {
    app.HandleCliAction(effective_action);
  }

  const int code = app.Run();
  app.Shutdown();
  LogLine(L"[boot] exit code=%d", code);
  OleUninitialize();
  ReleaseNativeInstanceLock();
  LogShutdown();
  return code;
}
