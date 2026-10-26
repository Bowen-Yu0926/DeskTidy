#include "first_run.h"

#include "app_prefs.h"
#include "display_geom.h"
#include "log.h"
#include "paths_util.h"
#include "update_check.h"

#include <windowsx.h>

#include <fstream>
#include <string>

namespace {

constexpr wchar_t kTipClass[] = L"DeskTidyNativeFirstRunTip";
bool g_tip_class = false;

struct TipCtx {
  const wchar_t* body = nullptr;
  int step = 0;
  int total = 3;
  bool done = false;
  bool next = false;
  bool skip = false;
  bool open_settings = false;
  RECT btn_next{};
  RECT btn_skip{};
};

void DrawMistButton(HDC hdc, const RECT& rc, const wchar_t* text, bool primary) {
  const COLORREF fill = primary ? RGB(0x16, 0xA3, 0x4A) : RGB(0xFF, 0xFF, 0xFF);
  const COLORREF border = primary ? RGB(0x16, 0xA3, 0x4A) : RGB(0xC9, 0xD2, 0xDE);
  const COLORREF fg = primary ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1A, 0x23, 0x32);
  HBRUSH br = CreateSolidBrush(fill);
  HPEN pen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ oldb = SelectObject(hdc, br);
  HGDIOBJ oldp = SelectObject(hdc, pen);
  RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
  SelectObject(hdc, oldb);
  SelectObject(hdc, oldp);
  DeleteObject(br);
  DeleteObject(pen);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, fg);
  RECT tr = rc;
  DrawTextW(hdc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK TipProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* ctx =
      reinterpret_cast<TipCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_CREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
      ctx = static_cast<TipCtx*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      HBRUSH bg = CreateSolidBrush(RGB(0xEE, 0xF1, 0xF6));
      FillRect(hdc, &rc, bg);
      DeleteObject(bg);

      RECT card{10, 10, rc.right - 10, rc.bottom - 10};
      HBRUSH card_br = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
      HPEN card_pen = CreatePen(PS_SOLID, 1, RGB(0xC9, 0xD2, 0xDE));
      HGDIOBJ oldb = SelectObject(hdc, card_br);
      HGDIOBJ oldp = SelectObject(hdc, card_pen);
      RoundRect(hdc, card.left, card.top, card.right, card.bottom, 14, 14);
      SelectObject(hdc, oldb);
      SelectObject(hdc, oldp);
      DeleteObject(card_br);
      DeleteObject(card_pen);

      // left inlay
      HBRUSH inl = CreateSolidBrush(RGB(0x1D, 0x4E, 0xD8));
      RECT ir{card.left, card.top + 8, card.left + 3, card.bottom - 8};
      FillRect(hdc, &ir, inl);
      DeleteObject(inl);

      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(0x1A, 0x23, 0x32));
      wchar_t head[64];
      swprintf_s(head, L"DeskTidy 引导  %d / %d", ctx ? ctx->step + 1 : 1,
                 ctx ? ctx->total : 3);
      RECT hr{card.left + 18, card.top + 14, card.right - 18, card.top + 40};
      HFONT font = CreateFontW(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               L"Microsoft YaHei UI");
      HGDIOBJ oldf = SelectObject(hdc, font);
      DrawTextW(hdc, head, -1, &hr, DT_LEFT | DT_SINGLELINE);
      SelectObject(hdc, oldf);
      DeleteObject(font);

      SetTextColor(hdc, RGB(0x64, 0x74, 0x8B));
      RECT br{card.left + 18, card.top + 48, card.right - 18, card.bottom - 60};
      if (ctx && ctx->body) {
        DrawTextW(hdc, ctx->body, -1, &br, DT_LEFT | DT_WORDBREAK);
      }

      if (ctx) {
        ctx->btn_next = {card.right - 210, card.bottom - 48, card.right - 112,
                         card.bottom - 18};
        ctx->btn_skip = {card.right - 104, card.bottom - 48, card.right - 18,
                         card.bottom - 18};
        const wchar_t* next_label =
            ctx->step + 1 >= ctx->total ? L"打开设置" : L"下一步";
        DrawMistButton(hdc, ctx->btn_next, next_label, true);
        DrawMistButton(hdc, ctx->btn_skip, L"跳过", false);
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_LBUTTONUP: {
      if (!ctx) {
        return 0;
      }
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (PtInRect(&ctx->btn_next, pt)) {
        ctx->next = true;
        if (ctx->step + 1 >= ctx->total) {
          ctx->open_settings = true;
        }
        DestroyWindow(hwnd);
      } else if (PtInRect(&ctx->btn_skip, pt)) {
        ctx->skip = true;
        DestroyWindow(hwnd);
      }
      return 0;
    }
    case WM_CLOSE:
      if (ctx) {
        ctx->skip = true;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (ctx) {
        ctx->done = true;
      }
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool ShowTipCard(HWND owner, TipCtx* ctx) {
  if (!g_tip_class) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TipProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(0xEE, 0xF1, 0xF6));
    wc.lpszClassName = kTipClass;
    RegisterClassExW(&wc);
    g_tip_class = true;
  }
  const int ww = 440;
  const int hh = 300;
  const RECT work = PrimaryWorkArea();
  HWND tip = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kTipClass, L"引导",
      WS_POPUP | WS_CLIPCHILDREN, work.left + 48, work.top + 72, ww, hh, owner,
      nullptr, GetModuleHandleW(nullptr), ctx);
  if (!tip) {
    return false;
  }
  ShowWindow(tip, SW_SHOW);
  SetForegroundWindow(tip);
  MSG msg{};
  while (!ctx->done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(tip, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return ctx->next && !ctx->skip;
}

}  // namespace

void MaybeShowFirstRun(HWND owner) {
  AppPrefs prefs = LoadAppPrefs();
  const std::wstring marker = JoinPath(NativeDataDir(), L"firstrun.done");
  const bool legacy_marker =
      GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;

  // U6 upgrade detection: log a new-features tip when the product version
  // changed since the previous run (Python upgrade path skips the guide for
  // existing profiles and surfaces changes via update notification).
  const std::wstring cur_ver = NativeProductVersion();
  if (!prefs.last_seen_version.empty() && prefs.last_seen_version != cur_ver) {
    LogLine(L"[first_run] upgrade detected %s -> %s",
            prefs.last_seen_version.c_str(), cur_ver.c_str());
  }

  // Legacy firstrun.done (old installs) migrates to the settings field so the
  // guide is not re-shown on upgrade (Python first_run_guide_done).
  if (legacy_marker && !prefs.first_run_guide_done) {
    prefs.first_run_guide_done = true;
    SaveAppPrefs(prefs);
  }

  if (prefs.first_run_guide_done) {
    if (prefs.last_seen_version != cur_ver) {
      prefs.last_seen_version = cur_ver;
      SaveAppPrefs(prefs);
    }
    return;
  }

  const wchar_t* bodies[] = {
      L"分区可贴在桌面层：拖动边角缩放，把桌面图标拖进分区即可钉选。\n\n"
      L"设置里「框选添加」可拖出一块区域新建分区。\n"
      L"桌面空白处右键 → DeskTidy 也可新建 / 刷新分区。",

      L"按规则一键整理：点托盘「整理」或按 O，把桌面图标按扩展名\n"
      L"（快捷方式 → 软件、文档 → 文档、图片 → 图片…）自动钉进分区。\n\n"
      L"设置 → 整理 可开启「启动自动整理」与「监视新文件」。\n"
      L"默认跳过 desktop.ini 与 DeskTidy（可改排除模式）。",

      L"· Ctrl+Shift+← / →：切换分区页\n"
      L"· Ctrl+Space：Peek 置顶查看分区（盖在其它窗口上）\n"
      L"· Ctrl+Shift+F：显示/隐藏分区　O：一键整理　H：桌面图标\n"
      L"· Ctrl+Alt+1：区域截图　2：贴图　3：录屏　4：搜索\n\n"
      L"托盘或设置 → 快捷键 可改绑。",

      L"右键托盘：搜索、保险库、壁纸、快照、检查更新…\n"
      L"设置 → 整理：启动自动钉选 / 监视新文件 / 白名单与规则。\n"
      L"宠物右键可换角色与姿态；拖入文件进回收站。\n\n"
      L"此引导只显示一次。",
  };

  bool open_settings = false;
  for (int i = 0; i < 4; ++i) {
    TipCtx ctx;
    ctx.body = bodies[i];
    ctx.step = i;
    ctx.total = 4;
    const bool cont = ShowTipCard(owner, &ctx);
    if (ctx.open_settings) {
      open_settings = true;
    }
    if (!cont || ctx.skip) {
      break;
    }
  }

  prefs.first_run_guide_done = true;
  prefs.last_seen_version = cur_ver;
  SaveAppPrefs(prefs);

  std::ofstream out(marker);
  out << "1\n";
  if (open_settings && owner) {
    ShowWindow(owner, SW_SHOW);
    SetForegroundWindow(owner);
  }
}
