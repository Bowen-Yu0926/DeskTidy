#include "prompt_text.h"

namespace {
constexpr int kEdit = 1;
constexpr int kOk = 2;
constexpr int kCancel = 3;
thread_local std::wstring g_result;
}  // namespace

std::wstring PromptText(HWND owner, const wchar_t* title, const wchar_t* label,
                        const std::wstring& initial) {
  g_result.clear();
  HINSTANCE inst = GetModuleHandleW(nullptr);
  const wchar_t* cls = L"DeskTidyNativePrompt";
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wparam,
                      LPARAM lparam) -> LRESULT {
    switch (msg) {
      case WM_CREATE:
        CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 12, 300, 20,
                      hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(100)),
                      GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 12, 36, 300, 24,
                        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEdit)),
                        GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      140, 72, 80, 28, hwnd,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOk)),
                      GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      232, 72, 80, 28, hwnd,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancel)),
                      GetModuleHandleW(nullptr), nullptr);
        return 0;
      case WM_COMMAND:
        if (LOWORD(wparam) == kOk) {
          wchar_t buf[512] = {};
          GetDlgItemTextW(hwnd, kEdit, buf, 512);
          g_result = buf;
          DestroyWindow(hwnd);
          return 0;
        }
        if (LOWORD(wparam) == kCancel) {
          g_result.clear();
          DestroyWindow(hwnd);
          return 0;
        }
        break;
      case WM_CLOSE:
        g_result.clear();
        DestroyWindow(hwnd);
        return 0;
      default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  };
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = cls;
  RegisterClassExW(&wc);

  HWND hwnd =
      CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, cls, title,
                      WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 340, 140, owner,
                      nullptr, inst, nullptr);
  if (!hwnd) {
    return {};
  }
  SetDlgItemTextW(hwnd, 100, label ? label : L"");
  SetDlgItemTextW(hwnd, kEdit, initial.c_str());
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  SetWindowPos(hwnd, nullptr, (sw - 340) / 2, (sh - 140) / 2, 340, 140,
               SWP_NOZORDER);
  ShowWindow(hwnd, SW_SHOW);
  if (owner) {
    EnableWindow(owner, FALSE);
  }
  MSG msg{};
  while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  if (owner) {
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }
  return g_result;
}
