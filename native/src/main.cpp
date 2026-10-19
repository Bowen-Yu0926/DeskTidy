#include "app.h"
#include "cli_ipc.h"
#include "instance_lock.h"

#include <Windows.h>
#include <ole2.h>

#include <string>

namespace {

std::wstring FindCliAction(LPWSTR cmd) {
  if (!cmd) {
    return {};
  }
  const wchar_t* keys[] = {L"--settings", L"--refresh", L"--organize",
                           L"--notes",    L"--search",  L"--shot",
                           L"--todos",    L"--peek"};
  for (const wchar_t* k : keys) {
    if (wcsstr(cmd, k)) {
      return k;
    }
  }
  return {};
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmd_line, int) {
  const std::wstring action = FindCliAction(cmd_line);

  if (!AcquireNativeInstanceLock()) {
    if (!action.empty() && ForwardCliToRunningInstance(action)) {
      return 0;
    }
    MessageBoxW(nullptr, L"DeskTidy Native 已在运行。", L"DeskTidy Native",
                MB_OK | MB_ICONINFORMATION);
    return 0;
  }

  const HRESULT hr = OleInitialize(nullptr);
  if (FAILED(hr) && hr != S_FALSE && hr != OLE_E_WRONGCOMPOBJ) {
    ReleaseNativeInstanceLock();
    return 1;
  }

  App app;
  if (!app.Init(instance)) {
    MessageBoxW(nullptr, L"Failed to initialize DeskTidy Native.",
                L"DeskTidy Native", MB_ICONERROR);
    OleUninitialize();
    ReleaseNativeInstanceLock();
    return 1;
  }

  if (!action.empty()) {
    app.HandleCliAction(action);
  }

  const int code = app.Run();
  app.Shutdown();
  OleUninitialize();
  ReleaseNativeInstanceLock();
  return code;
}
