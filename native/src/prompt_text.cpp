#include "prompt_text.h"

#include <Windowsx.h>

#include <algorithm>
#include <string>

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativePromptText";
constexpr int kPad = 16;
constexpr int kLabelH = 22;
constexpr int kEditH = 28;
constexpr int kBtnW = 88;
constexpr int kBtnH = 30;
constexpr int kGap = 10;

struct PromptCtx {
  std::wstring label;
  std::wstring initial;
  std::wstring result;
  bool accepted = false;
  bool done = false;
  HWND edit = nullptr;
  HWND owner = nullptr;
};

UINT WindowDpi(HWND hwnd) {
  using Fn = UINT(WINAPI*)(HWND);
  static Fn fn = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    fn = reinterpret_cast<Fn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  }
  if (fn && hwnd) {
    const UINT d = fn(hwnd);
    if (d > 0) {
      return d;
    }
  }
  return 96;
}

int S(HWND hwnd, int px) {
  return MulDiv(px, static_cast<int>(WindowDpi(hwnd)), 96);
}

void Finish(HWND hwnd, PromptCtx* ctx, bool ok) {
  if (!ctx) {
    DestroyWindow(hwnd);
    return;
  }
  if (ok) {
    wchar_t buf[512] = {};
    if (ctx->edit) {
      GetWindowTextW(ctx->edit, buf, 512);
    }
    ctx->result = buf;
    while (!ctx->result.empty() &&
           (ctx->result.back() == L' ' || ctx->result.back() == L'\t')) {
      ctx->result.pop_back();
    }
    size_t i = 0;
    while (i < ctx->result.size() &&
           (ctx->result[i] == L' ' || ctx->result[i] == L'\t')) {
      ++i;
    }
    if (i) {
      ctx->result.erase(0, i);
    }
    if (ctx->result.empty()) {
      MessageBeep(MB_ICONWARNING);
      if (ctx->edit) {
        SetFocus(ctx->edit);
      }
      return;
    }
    ctx->accepted = true;
  } else {
    ctx->accepted = false;
    ctx->result.clear();
  }
  DestroyWindow(hwnd);
}

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  PromptCtx* ctx =
      reinterpret_cast<PromptCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
      ctx = static_cast<PromptCtx*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
      return TRUE;
    }
    case WM_CREATE: {
      if (!ctx) {
        return -1;
      }
      const int pad = S(hwnd, kPad);
      const int label_h = S(hwnd, kLabelH);
      const int edit_h = S(hwnd, kEditH);
      const int btn_w = S(hwnd, kBtnW);
      const int btn_h = S(hwnd, kBtnH);
      const int gap = S(hwnd, kGap);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const int cw = (std::max)(80, static_cast<int>(rc.right - rc.left));
      CreateWindowExW(0, L"STATIC", ctx->label.c_str(),
                      WS_CHILD | WS_VISIBLE | SS_LEFT, pad, pad,
                      (std::max)(40, cw - pad * 2), label_h, hwnd, nullptr,
                      GetModuleHandleW(nullptr), nullptr);
      ctx->edit = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", ctx->initial.c_str(),
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, pad,
          pad + label_h + gap / 2, (std::max)(40, cw - pad * 2), edit_h, hwnd,
          reinterpret_cast<HMENU>(1001), GetModuleHandleW(nullptr), nullptr);
      const int by = pad + label_h + gap / 2 + edit_h + gap;
      CreateWindowExW(0, L"BUTTON", L"取消",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                      cw - pad - btn_w * 2 - gap, by, btn_w, btn_h, hwnd,
                      reinterpret_cast<HMENU>(IDCANCEL),
                      GetModuleHandleW(nullptr), nullptr);
      CreateWindowExW(0, L"BUTTON", L"确定",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      cw - pad - btn_w, by, btn_w, btn_h, hwnd,
                      reinterpret_cast<HMENU>(IDOK), GetModuleHandleW(nullptr),
                      nullptr);
      HFONT font = CreateFontW(
          S(hwnd, 14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
          DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
      if (font) {
        EnumChildWindows(
            hwnd,
            [](HWND child, LPARAM lp) -> BOOL {
              SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(lp), TRUE);
              return TRUE;
            },
            reinterpret_cast<LPARAM>(font));
        SetPropW(hwnd, L"PromptFont", font);
      }
      if (ctx->edit) {
        SetFocus(ctx->edit);
        SendMessageW(ctx->edit, EM_SETSEL, 0, -1);
      }
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      if (id == IDOK) {
        Finish(hwnd, ctx, true);
        return 0;
      }
      if (id == IDCANCEL) {
        Finish(hwnd, ctx, false);
        return 0;
      }
      break;
    }
    case WM_CLOSE:
      Finish(hwnd, ctx, false);
      return 0;
    case WM_DESTROY: {
      HFONT font = static_cast<HFONT>(RemovePropW(hwnd, L"PromptFont"));
      if (font) {
        DeleteObject(font);
      }
      if (ctx) {
        ctx->done = true;
      }
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}  // namespace

std::wstring PromptText(HWND owner, const wchar_t* title, const wchar_t* label,
                        const std::wstring& initial) {
  // Classic Win32 modal — never WebView2. Nested MistWebDialog inside a
  // settings WebView callback left a blank caption-only window and froze.
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PromptProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_3DFACE + 1);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    registered = true;
  }

  PromptCtx ctx;
  ctx.label = label && label[0] ? label : L"请输入：";
  ctx.initial = initial;
  ctx.owner = owner && IsWindow(owner) ? owner : nullptr;

  const UINT dpi = WindowDpi(ctx.owner ? ctx.owner : GetDesktopWindow());
  const int width = MulDiv(420, static_cast<int>(dpi), 96);
  const int height = MulDiv(168, static_cast<int>(dpi), 96);

  RECT anchor{};
  if (ctx.owner) {
    GetWindowRect(ctx.owner, &anchor);
  } else {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
  }
  const int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
  const int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 3;

  HWND hwnd = CreateWindowExW(
      WS_EX_DLGMODALFRAME, kClass, title && title[0] ? title : L"输入",
      WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN, x, y, width, height,
      ctx.owner, nullptr, GetModuleHandleW(nullptr), &ctx);
  if (!hwnd) {
    return {};
  }

  if (ctx.owner) {
    EnableWindow(ctx.owner, FALSE);
  }
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  if (ctx.edit) {
    SetFocus(ctx.edit);
  }

  MSG msg{};
  while (!ctx.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE &&
        msg.hwnd && (msg.hwnd == hwnd || IsChild(hwnd, msg.hwnd))) {
      Finish(hwnd, &ctx, false);
      continue;
    }
    if (!IsDialogMessageW(hwnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  if (ctx.owner) {
    EnableWindow(ctx.owner, TRUE);
    SetForegroundWindow(ctx.owner);
  }
  return ctx.accepted ? ctx.result : std::wstring{};
}
