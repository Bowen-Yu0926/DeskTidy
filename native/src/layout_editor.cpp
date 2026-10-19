#include "layout_editor.h"

#include "display_geom.h"
#include "prompt_text.h"

#include <ShlObj.h>
#include <windowsx.h>

#include <algorithm>
#include <string>

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeLayoutEditor";
AppState* g_state = nullptr;
int g_drag = -1;
POINT g_grab{};
bool g_ok = false;
HWND g_hwnd = nullptr;

RECT CanvasRect(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  rc.left = 8;
  rc.top = 36;
  rc.right -= 8;
  rc.bottom -= 48;
  return rc;
}

int HitFence(HWND hwnd, int x, int y) {
  if (!g_state) {
    return -1;
  }
  const RECT canvas = CanvasRect(hwnd);
  const RECT work = VirtualScreenRect();
  const int vw = (std::max)(1, static_cast<int>(work.right - work.left));
  const int vh = (std::max)(1, static_cast<int>(work.bottom - work.top));
  const int cw = (std::max)(1, static_cast<int>(canvas.right - canvas.left));
  const int ch = (std::max)(1, static_cast<int>(canvas.bottom - canvas.top));
  for (int i = static_cast<int>(g_state->fences.size()) - 1; i >= 0; --i) {
    const auto& f = g_state->fences[static_cast<size_t>(i)];
    RECT r{canvas.left + (f.x - work.left) * cw / vw,
           canvas.top + (f.y - work.top) * ch / vh,
           canvas.left + (f.x + f.w - work.left) * cw / vw,
           canvas.top + (f.y + f.h - work.top) * ch / vh};
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
      return i;
    }
  }
  return -1;
}

void PaintEditor(HWND hwnd, HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  HBRUSH bg = CreateSolidBrush(RGB(32, 34, 40));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(220, 224, 230));
  RECT title{8, 8, rc.right - 8, 32};
  DrawTextW(hdc, L"全部页面。拖动色块调整分区，双击重命名。", -1, &title,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  const RECT canvas = CanvasRect(hwnd);
  HBRUSH desk = CreateSolidBrush(RGB(18, 20, 26));
  FillRect(hdc, &canvas, desk);
  DeleteObject(desk);
  if (!g_state) {
    return;
  }
  const RECT work = VirtualScreenRect();
  const int vw = (std::max)(1, static_cast<int>(work.right - work.left));
  const int vh = (std::max)(1, static_cast<int>(work.bottom - work.top));
  const int cw = canvas.right - canvas.left;
  const int ch = canvas.bottom - canvas.top;
  for (size_t i = 0; i < g_state->fences.size(); ++i) {
    const auto& f = g_state->fences[i];
    RECT r{canvas.left + (f.x - work.left) * cw / vw,
           canvas.top + (f.y - work.top) * ch / vh,
           canvas.left + (f.x + f.w - work.left) * cw / vw,
           canvas.top + (f.y + f.h - work.top) * ch / vh};
    const int page = (std::max)(0, f.page);
    const COLORREF fill =
        f.portal_path.empty()
            ? RGB(50 + (page * 37) % 120, 80 + (page * 19) % 80, 150)
            : RGB(30, 120 + (page * 17) % 80, 70);
    HBRUSH br = CreateSolidBrush(fill);
    FillRect(hdc, &r, br);
    DeleteObject(br);
    std::wstring label = f.title;
    if (static_cast<size_t>(f.page) < g_state->page_names.size()) {
      label += L" · ";
      label += g_state->page_names[static_cast<size_t>(f.page)];
    }
    SetTextColor(hdc, RGB(255, 255, 255));
    DrawTextW(hdc, label.c_str(), -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
}

LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      PaintEditor(hwnd, hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      const int hit = HitFence(hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0 && g_state) {
        g_drag = hit;
        g_grab = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      if (g_drag >= 0 && (wparam & MK_LBUTTON) && g_state) {
        const RECT canvas = CanvasRect(hwnd);
        const RECT work = VirtualScreenRect();
        const int cw = (std::max)(1, static_cast<int>(canvas.right - canvas.left));
        const int ch = (std::max)(1, static_cast<int>(canvas.bottom - canvas.top));
        const int vw = (std::max)(1, static_cast<int>(work.right - work.left));
        const int vh = (std::max)(1, static_cast<int>(work.bottom - work.top));
        const int dx = GET_X_LPARAM(lparam) - g_grab.x;
        const int dy = GET_Y_LPARAM(lparam) - g_grab.y;
        g_grab = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        auto& f = g_state->fences[static_cast<size_t>(g_drag)];
        f.x += dx * vw / cw;
        f.y += dy * vh / ch;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
      g_drag = -1;
      ReleaseCapture();
      return 0;
    case WM_LBUTTONDBLCLK: {
      const int hit = HitFence(hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0 && g_state) {
        auto& f = g_state->fences[static_cast<size_t>(hit)];
        const auto name = PromptText(hwnd, L"重命名分区", L"标题：", f.title);
        if (!name.empty()) {
          f.title = name;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == 1) {
        g_ok = true;
        DestroyWindow(hwnd);
      } else if (LOWORD(wparam) == 2) {
        g_ok = false;
        DestroyWindow(hwnd);
      } else if (LOWORD(wparam) == 3 && g_state && g_drag >= 0) {
        BROWSEINFOW bi{};
        bi.hwndOwner = hwnd;
        bi.lpszTitle = L"门户文件夹";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
          wchar_t path[MAX_PATH] = {};
          if (SHGetPathFromIDListW(pidl, path)) {
            g_state->fences[static_cast<size_t>(g_drag)].portal_path = path;
            InvalidateRect(hwnd, nullptr, FALSE);
          }
          CoTaskMemFree(pidl);
        }
      }
      return 0;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      g_hwnd = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}  // namespace

bool ShowLayoutEditor(HWND owner, AppState* state) {
  g_state = state;
  g_ok = false;
  g_drag = -1;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = EditorProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  g_hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"布局编辑",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 80, 80, 760,
                           520, owner, nullptr, wc.hInstance, nullptr);
  if (!g_hwnd) {
    return false;
  }
  CreateWindowW(L"BUTTON", L"应用", WS_CHILD | WS_VISIBLE, 8, 448, 80, 28,
                g_hwnd, reinterpret_cast<HMENU>(1), wc.hInstance, nullptr);
  CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 96, 448, 80, 28,
                g_hwnd, reinterpret_cast<HMENU>(2), wc.hInstance, nullptr);
  CreateWindowW(L"BUTTON", L"选中项设为门户", WS_CHILD | WS_VISIBLE, 190, 448, 140,
                28, g_hwnd, reinterpret_cast<HMENU>(3), wc.hInstance, nullptr);
  ShowWindow(g_hwnd, SW_SHOW);
  EnableWindow(owner, FALSE);
  MSG msg{};
  while (IsWindow(g_hwnd)) {
    const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
    if (got == 0) {
      PostQuitMessage(static_cast<int>(msg.wParam));
      break;
    }
    if (got < 0) {
      break;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  EnableWindow(owner, TRUE);
  SetForegroundWindow(owner);
  g_state = nullptr;
  return g_ok;
}
