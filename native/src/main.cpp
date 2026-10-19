#include "app.h"
#include "instance_lock.h"

#include <Windows.h>
#include <ole2.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  if (!AcquireNativeInstanceLock()) {
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

  const int code = app.Run();
  app.Shutdown();
  OleUninitialize();
  ReleaseNativeInstanceLock();
  return code;
}
