#include "cli_ipc.h"

#include "instance_lock.h"

#include <string>

bool ForwardCliToRunningInstance(const std::wstring& action) {
  HWND hwnd = FindWindowW(kNativeMsgWindowClass, L"DeskTidyNative");
  if (!hwnd || action.empty()) {
    return false;
  }
  COPYDATASTRUCT cds{};
  cds.dwData = kCliActionMsg;
  cds.cbData = static_cast<DWORD>((action.size() + 1) * sizeof(wchar_t));
  cds.lpData = const_cast<wchar_t*>(action.c_str());
  SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                      SMTO_ABORTIFHUNG, 3000, nullptr);
  return true;
}

bool IsNativeInstanceRunning() {
  HANDLE m = CreateMutexW(nullptr, FALSE, kNativeInstanceMutexName);
  if (!m) {
    return false;
  }
  const bool exists = (GetLastError() == ERROR_ALREADY_EXISTS);
  CloseHandle(m);
  return exists;
}
