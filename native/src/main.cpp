#include "app.h"

#include <Windows.h>
#include <ole2.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // OLE STA required for RegisterDragDrop / DoDragDrop.
  const HRESULT hr = OleInitialize(nullptr);
  if (FAILED(hr) && hr != S_FALSE && hr != OLE_E_WRONGCOMPOBJ) {
    return 1;
  }

  App app;
  if (!app.Init(instance)) {
    MessageBoxW(nullptr, L"Failed to initialize DeskTidy Native.",
                L"DeskTidy Native", MB_ICONERROR);
    OleUninitialize();
    return 1;
  }

  const int code = app.Run();
  app.Shutdown();
  OleUninitialize();
  return code;
}
