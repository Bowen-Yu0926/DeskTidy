#include "cli_ipc.h"

#include "instance_lock.h"

#include <Windows.h>
#include <string>

bool ForwardCliToRunningInstance(const std::wstring& action) {
  HWND hwnd = FindWindowW(kNativeMsgWindowClass, L"DeskTidyNative");
  if (!hwnd) {
    // If the sender is on a different desktop (e.g. background console, service,
    // or detached runner), switch thread desktop to the interactive input desktop.
    HDESK input_desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (input_desk) {
      SetThreadDesktop(input_desk);
      hwnd = FindWindowW(kNativeMsgWindowClass, L"DeskTidyNative");
      CloseDesktop(input_desk);
    }
  }
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
