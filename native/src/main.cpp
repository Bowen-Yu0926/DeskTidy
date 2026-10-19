#include "app.h"

#include <Windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // COM for future shell/OLE work; MVP still benefits from STA.
  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    return 1;
  }

  App app;
  if (!app.Init(instance)) {
    MessageBoxW(nullptr, L"Failed to initialize DeskTidy Native MVP.",
                L"DeskTidy Native", MB_ICONERROR);
    CoUninitialize();
    return 1;
  }

  const int code = app.Run();
  app.Shutdown();
  CoUninitialize();
  return code;
}
