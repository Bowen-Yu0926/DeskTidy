#include "log.h"

#include "paths_util.h"

#include <Windows.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <string>

namespace {

constexpr wchar_t kLogName[] = L"desktidy.log";
constexpr wchar_t kLogHistName[] = L"desktidy.1.log";
constexpr LONGLONG kMaxLogBytes = 2LL * 1024 * 1024;

CRITICAL_SECTION g_lock;
bool g_inited = false;
std::wstring g_log_path;
std::wstring g_hist_path;

// Guard against recursive crashes inside a handler (the handler must never
// fault again and re-enter itself).
volatile LONG g_in_crash = 0;

std::wstring LogDirPath() {
  std::wstring dir = NativeDataDir();
  dir += L"\\logs";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring TimestampNow() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t buf[40];
  swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

// Raw append used by the crash path: no CRITICAL_SECTION, no allocation beyond
// the UTF-8 conversion of a fixed-size prefix. Callers must guard via g_in_crash.
void CrashWrite(const wchar_t* message) {
  std::wstring path = g_log_path;
  if (path.empty()) {
    path = LogDirPath() + L"\\" + kLogName;
  }
  HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  wchar_t prefix[96];
  swprintf_s(prefix, L"%s [%lu:%lu] ", TimestampNow().c_str(),
             GetCurrentProcessId(), GetCurrentThreadId());
  std::string head = NarrowUtf8(prefix);
  std::string body = NarrowUtf8(message);
  DWORD written = 0;
  WriteFile(h, head.data(), static_cast<DWORD>(head.size()), &written, nullptr);
  WriteFile(h, body.data(), static_cast<DWORD>(body.size()), &written, nullptr);
  const char nl = '\n';
  WriteFile(h, &nl, 1, &written, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
}

// Core append: thread-safe, rotates when oversized, flushes at ~2 Hz.
void WriteCore(const char* utf8, size_t len) {
  if (!g_inited) {
    return;
  }
  EnterCriticalSection(&g_lock);

  bool fresh = false;
  HANDLE h =
      CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    LeaveCriticalSection(&g_lock);
    return;
  }
  LARGE_INTEGER size{};
  if (GetFileSizeEx(h, &size)) {
    fresh = size.QuadPart == 0;
    if (size.QuadPart > kMaxLogBytes) {
      CloseHandle(h);
      MoveFileExW(g_log_path.c_str(), g_hist_path.c_str(),
                  MOVEFILE_REPLACE_EXISTING);
      h = CreateFileW(g_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                      nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        LeaveCriticalSection(&g_lock);
        return;
      }
      fresh = true;
    }
  }

  DWORD written = 0;
  if (fresh) {
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    WriteFile(h, bom, 3, &written, nullptr);
  }
  wchar_t prefix[96];
  swprintf_s(prefix, L"%s [%lu:%lu] ", TimestampNow().c_str(),
             GetCurrentProcessId(), GetCurrentThreadId());
  std::string head = NarrowUtf8(prefix);
  WriteFile(h, head.data(), static_cast<DWORD>(head.size()), &written, nullptr);
  WriteFile(h, utf8, static_cast<DWORD>(len), &written, nullptr);
  const char nl = '\n';
  WriteFile(h, &nl, 1, &written, nullptr);
  // Flush at most ~2 Hz. Every-line FlushFileBuffers stalled the UI during
  // RefreshDesktopIcons / shell-notify log storms (N fences × sync disk).
  static ULONGLONG s_last_flush_ms = 0;
  const ULONGLONG now = GetTickCount64();
  if (now - s_last_flush_ms >= 500) {
    FlushFileBuffers(h);
    s_last_flush_ms = now;
  }
  CloseHandle(h);
  LeaveCriticalSection(&g_lock);
}

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
  if (InterlockedExchange(&g_in_crash, 1) != 0) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode
                                           : 0;
  ULONG_PTR addr =
      (ep && ep->ExceptionRecord)
          ? reinterpret_cast<ULONG_PTR>(ep->ExceptionRecord->ExceptionAddress)
          : 0;
  wchar_t module_path[MAX_PATH] = L"<unknown>";
  HMODULE mod = nullptr;
  if (addr) {
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(addr), &mod) &&
        mod) {
      GetModuleFileNameW(mod, module_path, MAX_PATH);
    }
  }
  wchar_t msg[512];
  swprintf_s(msg,
             L"[crash] unhandled exception code=0x%08X addr=0x%llX module=%s",
             code, static_cast<unsigned long long>(addr), module_path);
  CrashWrite(msg);
  return EXCEPTION_CONTINUE_SEARCH;
}

void SignalHandler(int sig) {
  if (InterlockedExchange(&g_in_crash, 1) != 0) {
    return;
  }
  wchar_t msg[128];
  swprintf_s(msg, L"[crash] signal %d received", sig);
  CrashWrite(msg);
  signal(sig, SIG_DFL);
  raise(sig);
}

void TerminateHandler() {
  if (InterlockedExchange(&g_in_crash, 1) != 0) {
    std::abort();
  }
  CrashWrite(L"[crash] std::terminate called (uncaught exception)");
  std::abort();
}

}  // namespace

void LogInit() {
  if (g_inited) {
    return;
  }
  InitializeCriticalSection(&g_lock);
  const std::wstring dir = LogDirPath();
  g_log_path = dir + L"\\" + kLogName;
  g_hist_path = dir + L"\\" + kLogHistName;
  g_inited = true;
}

void LogShutdown() {
  if (!g_inited) {
    return;
  }
  static const char kShutdown[] = "[log] shutdown";
  WriteCore(kShutdown, strlen(kShutdown));
}

void LogLine(const wchar_t* fmt, ...) {
  if (!g_inited || !fmt) {
    return;
  }
  va_list args;
  va_start(args, fmt);
  va_list copy;
  va_copy(copy, args);
  const int needed = _vscwprintf(fmt, copy) + 1;
  va_end(copy);
  if (needed <= 1) {
    va_end(args);
    return;
  }
  std::wstring line(static_cast<size_t>(needed), L'\0');
  _vsnwprintf_s(&line[0], line.size(), _TRUNCATE, fmt, args);
  va_end(args);
  std::string utf8 = NarrowUtf8(line.c_str());
  WriteCore(utf8.data(), utf8.size());
}

void LogLineA(const char* fmt, ...) {
  if (!g_inited || !fmt) {
    return;
  }
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
  va_end(args);
  WriteCore(buf, strlen(buf));
}

void LogInstallCrashHandler() {
  SetUnhandledExceptionFilter(UnhandledFilter);
  std::set_terminate(TerminateHandler);
  signal(SIGABRT, SignalHandler);
  signal(SIGSEGV, SignalHandler);
  signal(SIGILL, SignalHandler);
  signal(SIGFPE, SignalHandler);
  signal(SIGTERM, SignalHandler);
}
