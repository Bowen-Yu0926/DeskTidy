#include "app.h"

#include "autostart.h"
#include "desktop_dblclick.h"
#include "desktop_items.h"
#include "desktop_shell.h"
#include "display_geom.h"
#include "first_run.h"
#include "hotkey_help.h"
#include "hotkey_store.h"
#include "i18n.h"
#include "icon_actions.h"
#include "log.h"
#include "organize.h"
#include "organize_suppress.h"
#include "screen_record.h"
#include "toast.h"
#include "minutes.h"
#include "native_cleanup.h"
#include "page_folders.h"
#include "snapshot_ui.h"
#include "whitelist.h"
#include "paths_util.h"
#include "mist_msg.h"
#include "mist_webview.h"
#include "prompt_text.h"
#include "cli_ipc.h"
#include "fence_keys.h"
#include "fence_style.h"
#include "layout_editor.h"
#include "python_layout_sync.h"
#include "shell_clipboard.h"
#include "shell_context_menu.h"
#include "pin_store.h"
#include "brand_mark.h"
#include "namespace_icons.h"
#include "shell_icons.h"
#include "shell_verbs.h"
#include "desknote_launch.h"
#include "file_assoc.h"
#include "float_placements.h"
#include "snapshot.h"
#include "tool_paths.h"
#include "tools_misc.h"
#include "ui_theme.h"
#include "update_check.h"
#include "rules_editor.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <cstring>
#include <Shellapi.h>
#include <windowsx.h>
#include <ShlObj.h>
#include <gdiplus.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#pragma comment(lib, "gdiplus.lib")

// Defined in layout.cpp — scales the stored layout old work area → new (U4).
// Declared at global scope so it links against the external definition.
bool PrepareDestinationProfileForDisplayChange(const RECT& old_work,
                                               const RECT& new_work);

namespace {
constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kMsgBandRestore = WM_APP + 40;
constexpr UINT kMsgDesktopDblClick = WM_APP + 41;
constexpr UINT kMsgLlHotkey = WM_APP + 42;
constexpr UINT kMsgRelayHotkey = WM_APP + 43;  // LPARAM = new std::string*
constexpr UINT kHotkeyBase = 100;
constexpr UINT kTimerRestore = 40;
constexpr UINT kTimerPet = 41;
constexpr UINT kTimerWatch = 42;
constexpr UINT kTimerBandSettle = 43;
// Upper bound on how long a Win+D transition may keep the settle timer alive.
// The shell's re-composite settles well inside this; the cap only exists so a
// permanently flapping band probe cannot spin the timer forever.
constexpr ULONGLONG kBandSettleMaxMs = 1500;
constexpr UINT kTimerUpdate = 44;
constexpr UINT kTimerOpenSettingsOrg = 47;
constexpr UINT kTimerDisplayChange = 49;
constexpr UINT kTimerIconsRefresh = 50;
constexpr UINT kTimerMemTrim = 51;
constexpr UINT kTimerPersistLayouts = 52;
constexpr UINT kTimerReflowPublic = 53;
// Debounced desktop content refresh after SHChangeNotify / portal watch
// (Fences/Explorer: wait for the FS event to settle, then one scan).
constexpr UINT kTimerDesktopShell = 54;
// Explorer paste is async: SHCNE_CREATE/UPDATEDIR for a multi-file paste land
// seconds after OnDesktopPaste's immediate rescan. Instead of a fixed "ignore
// everything for N ms" window (which swallowed late events until the next
// PollChanged), suppress only while a burst is actually arriving: each
// suppressed notify extends the deadline by this much, and once the burst
// stops the very next notify — even minutes later — goes through.
constexpr UINT kPasteRescanSuppressMs = 400;
// Single-App process (g_app_for_hook is assigned/released per instance), so a
// file-scope deadline is equivalent to an App member here — see app.h note on
// last_manual_refresh_.
ULONGLONG g_paste_rescan_deadline = 0;
// Idle warm-paint for soft-parked (off-page) fences — one per tick.
constexpr UINT kTimerWarmFences = 55;
// Work-spreader cadence for kTimerWarmFences. Each tick does one full DWM
// layered blit on the UI thread, so 32ms (31 Hz) hogged the pump and could
// hitch an in-progress fence drag when several fences were parked. 80ms (12.5
// warm-blits/sec) still warms a dozen fences in under a second but leaves the
// UI thread far more headroom.
constexpr UINT kWarmFencesTickMs = 80;
constexpr UINT kTimerWebViewWarm = 56;
// Hybrid-GPU (Optimus) recovery: re-upload visible overlays after boot settles.
constexpr UINT kTimerOverlayRefresh = 57;
// After fence→desktop unpin: Present must run off the OLE stack (see Reveal).
constexpr UINT kTimerRevealPublic = 58;
constexpr UINT kTimerClipboard = 59;
constexpr UINT kMsgOrganizeDone = WM_APP + 49;
constexpr UINT kMsgDesktopBlankDown = WM_APP + 50;
constexpr UINT kMsgDesktopPaste = WM_APP + 51;
constexpr UINT kMsgShellNotify = WM_APP + 52;
constexpr UINT kMsgHideOffPage = WM_APP + 53;
constexpr UINT kMsgRevealPublic = WM_APP + 54;
// Desktop FG with apps still restored: sink band (no Win+D HWND_TOP restore).
// +55 is kDeskTidyMsgIconsPreloaded (desktop_items.h).
constexpr UINT kMsgSeatUnderApps = WM_APP + 56;
// FG hook posts only this — never call Shell/Z-order APIs inside WinEvent.
constexpr UINT kMsgFgChanged = WM_APP + 57;
// Organized blank wallpaper RMB → Desktop background menu (Paste/New/Undo).
constexpr UINT kMsgDesktopBlankRClick = WM_APP + 58;
// Hook-driven desktop marquee (the overlay must not SetCapture).
constexpr UINT kMsgDesktopRubberMove = WM_APP + 59;
constexpr UINT kMsgDesktopRubberUp = WM_APP + 61;
// kDeskTidyMsgIconsPreloaded (desktop_items.h) — STA icon preload done.
constexpr DWORD kTrayOpenDebounceMs = 450;
// Min interval between full DisplayFingerprint() recomputes on the 2s watch
// tick. WM_DISPLAYCHANGE handles real topology changes immediately; this is
// only a fallback, so a 10s cadence keeps EnumDisplayMonitors+GetDeviceCaps
// off the hot path without missing changes.
constexpr ULONGLONG kDisplayFingerprintIntervalMs = 10000;
constexpr wchar_t kMsgClass[] = L"DeskTidyNativeMsg";
bool g_organize_busy = false;
// Coalesce organize requests while a worker runs (watch + manual + reassign).
bool g_organize_pending = false;
bool g_organize_pending_quiet = true;
bool g_organize_pending_reassign = false;
// Count monitors encoded in a DisplayFingerprint() string ("|"-joined panels).
int CountMonitorsInFingerprint(const std::string& fp) {
  if (fp.empty()) {
    return 0;
  }
  int n = 1;
  for (char c : fp) {
    if (c == '|') {
      ++n;
    }
  }
  return n;
}

std::wstring ParentDirOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool PathIsUnderRoot(const std::wstring& path, const std::wstring& root) {
  if (root.empty() || path.size() <= root.size()) {
    return false;
  }
  if (_wcsnicmp(path.c_str(), root.c_str(), root.size()) != 0) {
    return false;
  }
  const wchar_t sep = path[root.size()];
  return sep == L'\\' || sep == L'/';
}

bool PathIsDesktopChild(const std::wstring& path, const std::wstring& desk) {
  return !desk.empty() &&
         _wcsicmp(ParentDirOf(path).c_str(), desk.c_str()) == 0;
}

// Folder Portal → virtual fence: FO onto Desktop first so the same path is not
// dual-listed (portal scan + pin). Explorer off-Desktop pin-only stays intact.
DWORD LandPortalSourcesOnDesktop(
    HWND owner, const std::vector<std::unique_ptr<FenceWindow>>& fences,
    const std::vector<std::wstring>& paths, std::vector<std::wstring>* out) {
  if (!out) {
    return DROPEFFECT_NONE;
  }
  out->clear();
  out->reserve(paths.size());
  PWSTR desk_raw = nullptr;
  std::wstring desk;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) &&
      desk_raw) {
    desk = desk_raw;
    CoTaskMemFree(desk_raw);
  }
  std::vector<std::wstring> portal_roots;
  for (const auto& f : fences) {
    if (!f || f->PortalPath().empty()) {
      continue;
    }
    portal_roots.push_back(f->PortalPath());
  }
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  int moved = 0;
  int ok = 0;
  for (const auto& src : paths) {
    if (src.empty()) {
      continue;
    }
    bool from_portal = false;
    for (const auto& root : portal_roots) {
      if (PathIsUnderRoot(src, root)) {
        from_portal = true;
        break;
      }
    }
    if (!from_portal || PathIsDesktopChild(src, desk) || desk.empty()) {
      out->push_back(src);
      ++ok;
      continue;
    }
    bool do_move = SameVolume(src, desk);
    if (ctrl) {
      do_move = false;
    }
    if (shift) {
      do_move = true;
    }
    std::wstring dest;
    if (!MoveOrCopyFileIntoDir(owner, src, desk, do_move, &dest) ||
        dest.empty()) {
      continue;
    }
    SuppressOrganizePath(src);
    SuppressOrganizePath(dest);
    out->push_back(dest);
    ++ok;
    if (do_move) {
      ++moved;
    }
  }
  if (ok <= 0) {
    return DROPEFFECT_NONE;
  }
  if (moved == ok) {
    return DROPEFFECT_MOVE;
  }
  return DROPEFFECT_COPY;
}

App* g_app_for_hook = nullptr;
HWND g_band_hwnd = nullptr;
HHOOK g_kb_hook = nullptr;

// P1-12: floating drop-hint window (layered, rounded, no-activate).
constexpr wchar_t kPinHintClass[] = L"DeskTidyPinHint";
HWND g_pin_hint_hwnd = nullptr;

// Shutdown steps must never let one failing step (SEH access violation or a
// C++ exception) abort the rest. C++ exceptions are caught in InvokeStepBody
// (no __try there) and SEH faults in RunShutdownStep (no C++ objects there) —
// kept in separate functions because MSVC forbids mixing __try with try/catch
// inside one function (/EHsc).
void InvokeStepBody(const std::function<void()>& body) {
  try {
    body();
  } catch (...) {
    LogLineA("[shutdown] step body threw C++ exception");
  }
}

void RunShutdownStep(const char* name, const std::function<void()>& body) {
  LogLineA("[shutdown] %s begin", name);
  __try {
    InvokeStepBody(body);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogLineA("[shutdown] %s SEH exception code=0x%08X", name,
             static_cast<unsigned int>(GetExceptionCode()));
  }
  LogLineA("[shutdown] %s end", name);
}

LRESULT CALLBACK PinHintWndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  return DefWindowProcW(hwnd, msg, w, l);
}

void EnsurePinHintWindow(HINSTANCE inst) {
  if (g_pin_hint_hwnd) {
    return;
  }
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = PinHintWndProc;
  wc.hInstance = inst;
  wc.lpszClassName = kPinHintClass;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return;
  }
  g_pin_hint_hwnd =
      CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                          WS_EX_LAYERED,
                      kPinHintClass, L"", WS_POPUP, 0, 0, 16, 16, nullptr,
                      nullptr, inst, nullptr);
}

void ShowPinHintWindow(HINSTANCE inst, const std::wstring& text, POINT cursor) {
  EnsurePinHintWindow(inst);
  if (!g_pin_hint_hwnd) {
    return;
  }
  const int x = cursor.x + 12;
  const int y = cursor.y + 12;
  static std::wstring s_last_text;
  static int s_last_w = 0;
  static int s_last_h = 0;
  if (text == s_last_text && s_last_w > 0 && s_last_h > 0) {
    SetWindowPos(g_pin_hint_hwnd, HWND_TOPMOST, x, y, s_last_w, s_last_h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return;
  }
  static ULONG_PTR gdi_token = 0;
  if (!gdi_token) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&gdi_token, &input, nullptr);
  }
  Gdiplus::FontFamily family(L"Microsoft YaHei UI");
  const Gdiplus::FontFamily* face =
      family.IsAvailable() ? &family : Gdiplus::FontFamily::GenericSansSerif();
  Gdiplus::Font font(face, 13.0f, Gdiplus::FontStyleRegular,
                     Gdiplus::UnitPixel);
  Gdiplus::Bitmap tmp(1, 1, PixelFormat32bppARGB);
  Gdiplus::Graphics mg(&tmp);
  Gdiplus::RectF mbox;
  mg.MeasureString(text.c_str(), -1, &font, Gdiplus::PointF(0, 0), &mbox);
  const int pad_x = 14;
  const int pad_y = 7;
  const int w = (std::max)(16, static_cast<int>(mbox.Width + 0.5f) + pad_x * 2);
  const int h = (std::max)(16, static_cast<int>(mbox.Height + 0.5f) + pad_y * 2);

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ old = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  {
    Gdiplus::Graphics g(mem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    const int radius = 8;
    Gdiplus::GraphicsPath path;
    const int rr = radius * 2;
    if (rr > 2 && rr < w && rr < h) {
      path.AddArc(0, 0, rr, rr, 180, 90);
      path.AddArc(w - rr, 0, rr, rr, 270, 90);
      path.AddArc(w - rr, h - rr, rr, rr, 0, 90);
      path.AddArc(0, h - rr, rr, rr, 90, 90);
      path.CloseFigure();
    } else {
      path.AddRectangle(Gdiplus::Rect(0, 0, w, h));
    }
    Gdiplus::SolidBrush bg(Gdiplus::Color(214, 22, 27, 34));
    g.FillPath(&bg, &path);
    Gdiplus::Pen edge(Gdiplus::Color(120, 120, 170, 255), 1.0f);
    g.DrawPath(&edge, &path);
    Gdiplus::SolidBrush text_br(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF box(0.f, 0.f, static_cast<Gdiplus::REAL>(w),
                       static_cast<Gdiplus::REAL>(h));
    g.DrawString(text.c_str(), -1, &font, box, &fmt, &text_br);
  }
  {
    auto* px = static_cast<uint32_t*>(bits);
    const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < total; ++i) {
      const uint32_t c = px[i];
      const uint32_t a = (c >> 24) & 0xFFu;
      if (a == 0) {
        px[i] = 0;
      } else if (a < 255) {
        const uint32_t r = (((c >> 16) & 0xFFu) * a + 127u) / 255u;
        const uint32_t g_ch = (((c >> 8) & 0xFFu) * a + 127u) / 255u;
        const uint32_t b = ((c & 0xFFu) * a + 127u) / 255u;
        px[i] = (a << 24) | (r << 16) | (g_ch << 8) | b;
      }
    }
  }
  POINT origin{x, y};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateLayeredWindow(g_pin_hint_hwnd, screen, &origin, &size, mem, &src, 0,
                      &blend, ULW_ALPHA);
  if (!IsWindowVisible(g_pin_hint_hwnd)) {
    ShowWindow(g_pin_hint_hwnd, SW_SHOWNOACTIVATE);
  }
  s_last_text = text;
  s_last_w = w;
  s_last_h = h;
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

void HidePinHintWindow() {
  if (g_pin_hint_hwnd) {
    ShowWindow(g_pin_hint_hwnd, SW_HIDE);
  }
}

}  // namespace

struct OrganizeJobResult {
  PinMap pins;
  PinMap pins_at_start;
  // Scan-order (path, fence_id) for pin_order append — not unordered_map iter.
  std::vector<std::pair<std::wstring, std::string>> neu;
  int n = 0;
  int hosted = 0;
  bool quiet = false;
  bool reassign = false;
  // Reassign replaces the whole map — abort if the user edited pins mid-job.
  unsigned long long pins_epoch = 0;
};

namespace {

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code != HC_ACTION) {
    return CallNextHookEx(nullptr, code, wparam, lparam);
  }
  // Single WH_KEYBOARD_LL for the process: fence shortcuts + desktop paste +
  // Win+D band restore + configured hotkeys (was three hooks on every key).
  if (TryHandleFenceLlKey(wparam, lparam)) {
    return 1;
  }
  if (TryHandleDesktopPasteLlKey(wparam, lparam)) {
    return 1;
  }
  if (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) {
    const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (kb) {
      // Fast bypass: Win+L is system lock workstation chord, never delay.
      if (kb->vkCode == 'L') {
        const bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                         (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
        if (win) {
          return CallNextHookEx(nullptr, code, wparam, lparam);
        }
      }
      if (g_band_hwnd && kb->vkCode == 'D') {
        const bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                         (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
        if (win) {
          // Passthrough: still CallNextHookEx so Explorer gets Win+D.
          // wParam=1: Win+D path — also drop fullscreen cover (Fences-class)
          // before seating chrome; plain band restore must not minimize apps.
          PostMessageW(g_band_hwnd, kMsgBandRestore, 1, 0);
        }
      }
      // P1-28 Tier A: when exclusive fullscreen swallows RegisterHotKey /
      // WM_HOTKEY, still match configured chords via LL hook.
      if (g_app_for_hook && g_app_for_hook->MessageHwnd()) {
        g_app_for_hook->TryDispatchLlHotkey(kb->vkCode);
      }
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

std::unordered_map<std::string, std::wstring> FenceTitlesOf(
    const std::vector<std::unique_ptr<FenceWindow>>& fences) {
  std::unordered_map<std::string, std::wstring> titles;
  for (const auto& f : fences) {
    if (f) {
      titles[f->Id()] = f->Title();
    }
  }
  return titles;
}

std::vector<std::wstring> PreviewVirtualOrganize(
    const PinMap& pins, const RuleMap& rules,
    const std::vector<std::unique_ptr<FenceWindow>>& fences) {
  return OrganizePreviewVirtualPins(pins, ScanDesktopItems(), rules,
                                    FenceTitlesOf(fences));
}

void FillOrgFileTypeAndSize(const DesktopItem& it, std::wstring* out_type,
                            std::wstring* out_size) {
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  const bool has_attr =
      GetFileAttributesExW(it.path.c_str(), GetFileExInfoStandard, &fad) != 0;
  const bool is_dir =
      has_attr && ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
  if (it.is_link) {
    *out_type = L"快捷方式";
  } else if (is_dir) {
    *out_type = L"文件夹";
  } else {
    const size_t dot = it.name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < it.name.size()) {
      std::wstring ext = it.name.substr(dot + 1);
      for (auto& c : ext) {
        if (c >= L'a' && c <= L'z') {
          c = static_cast<wchar_t>(c - L'a' + L'A');
        }
      }
      *out_type = ext + L" 文件";
    } else {
      *out_type = L"文件";
    }
  }
  if (!has_attr || is_dir) {
    *out_size = L"—";
    return;
  }
  ULARGE_INTEGER sz{};
  sz.HighPart = fad.nFileSizeHigh;
  sz.LowPart = fad.nFileSizeLow;
  const double bytes = static_cast<double>(sz.QuadPart);
  wchar_t buf[32];
  if (bytes < 1024) {
    swprintf_s(buf, L"%.0f B", bytes);
  } else if (bytes < 1024 * 1024) {
    swprintf_s(buf, L"%.1f KB", bytes / 1024.0);
  } else {
    swprintf_s(buf, L"%.1f MB", bytes / (1024.0 * 1024.0));
  }
  *out_size = buf;
}

void PushOrganizePage(SettingsWindow* settings, const PinMap& pins,
                      const RuleMap& rules,
                      const std::vector<std::unique_ptr<FenceWindow>>& fences,
                      int current_page) {
  if (!settings) {
    return;
  }
  const std::vector<DesktopItem> items = ScanDesktopItems();
  std::vector<FenceLayout> layouts;
  layouts.reserve(fences.size());
  for (const auto& f : fences) {
    if (f) {
      layouts.push_back(f->CurrentLayout());
    }
  }
  // Preview must use the same resolver as the real organize: fence
  // organize_kinds (current page first), then legacy extension rules.
  const auto preview = OrganizePreviewVirtualPins(
      pins, items, rules, FenceTitlesOf(fences), "system_software",
      "system_docs", &layouts, current_page);
  auto lower_name = [](std::wstring s) {
    if (!s.empty()) {
      CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    }
    return s;
  };
  struct NameBucket {
    std::vector<const DesktopItem*> list;
    size_t cursor = 0;
  };
  std::unordered_map<std::wstring, NameBucket> by_name;
  by_name.reserve(items.size());
  int organized = 0;
  for (const auto& it : items) {
    if (FindPin(pins, it.path)) {
      ++organized;
    } else {
      const std::wstring n = it.name.empty() ? it.path : it.name;
      by_name[lower_name(n)].list.push_back(&it);
    }
  }
  for (const auto& it : items) {
    if (FindPin(pins, it.path)) {
      const std::wstring n = it.name.empty() ? it.path : it.name;
      by_name[lower_name(n)].list.push_back(&it);
    }
  }
  std::vector<SettingsWindow::OrgFileRow> files;
  files.reserve(preview.size());
  for (const auto& line : preview) {
    SettingsWindow::OrgFileRow row;
    const size_t arrow = line.find(L" → ");
    row.name = arrow == std::wstring::npos ? line : line.substr(0, arrow);
    row.type = L"文件";
    row.size = L"—";
    auto bit = by_name.find(lower_name(row.name));
    if (bit != by_name.end() && !bit->second.list.empty()) {
      const size_t idx =
          (std::min)(bit->second.cursor, bit->second.list.size() - 1);
      bit->second.cursor++;
      FillOrgFileTypeAndSize(*bit->second.list[idx], &row.type, &row.size);
    }
    files.push_back(std::move(row));
  }
  settings->SetOrganizePreview(preview, static_cast<int>(preview.size()),
                               organized, static_cast<int>(items.size()),
                               files);
}
}  // namespace

// P1-17: push the pet below any visible fence on the current page so fence fill
// cannot cover the pet's chrome (page chips + crown). Mirrors the fence-rect
// collection in App::SetPage.
static void NudgePetClearOfFences(
    PetWindow& pet, const std::vector<std::unique_ptr<FenceWindow>>& fences,
    int current_page) {
  if (!pet.Hwnd() || !IsWindowVisible(pet.Hwnd())) {
    return;
  }
  std::vector<RECT> rects;
  for (const auto& f : fences) {
    if (!f || !f->Hwnd() || !IsWindowVisible(f->Hwnd())) {
      continue;
    }
    if (!f->VisibleOnPage(current_page)) {
      continue;
    }
    RECT wr{};
    if (GetWindowRect(f->Hwnd(), &wr)) {
      rects.push_back(wr);
    }
  }
  pet.NudgeClearOfFenceBottoms(rects);
}

void App::WireFence(FenceWindow* fence) {
  if (!fence) {
    return;
  }
  // Style / sort / lock / zoom / collapse toggles all land here. They change
  // layout fields, not geometry, so a coalesced write is enough — persisting
  // synchronously made each toggle block the UI thread on a full state build
  // plus four file operations.
  fence->SetOnMoved([this](FenceWindow*) { SchedulePersistLayouts(); });
  fence->SetOnGeometryMoved([this](FenceWindow* moved) {
    if (!moved) {
      return;
    }
    // Snap against fences on the page the user is viewing (pages[] membership).
    const int page = current_page_;
    std::vector<FenceWindow*> list;
    std::vector<LayoutRect> rects;
    for (auto& f : fences_) {
      if (!f || !f->OnPage(page) || f->IsCollapsed() ||
          (!f->TabGroup().empty() && !f->TabActive())) {
        continue;
      }
      RECT r{};
      GetWindowRect(f->Hwnd(), &r);
      list.push_back(f.get());
      rects.push_back({r.left, r.top, r.right - r.left, r.bottom - r.top});
    }
    RECT mr{};
    GetWindowRect(moved->Hwnd(), &mr);
    // Prefer primary when the fence straddles monitors (Python snap_geometry).
    const RECT work = WorkAreaForRect(mr);
    const LayoutRect work_r{work.left, work.top, work.right - work.left,
                            work.bottom - work.top};
    if (moved->IsCollapsed()) {
      // Collapsed chrome height is not a layout rect; snap edges + persist.
      SnapRectToWorkArea(&mr, 0);
      SetWindowPos(moved->Hwnd(), nullptr, mr.left, mr.top, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      PersistLayouts();
      return;
    }
    int moved_i = -1;
    for (size_t i = 0; i < list.size(); ++i) {
      if (list[i] == moved) {
        moved_i = static_cast<int>(i);
        break;
      }
    }
    if (moved_i < 0) {
      PersistLayouts();
      return;
    }
    const std::vector<LayoutRect> before_rects = rects;
    std::vector<LayoutRect> others;
    others.reserve(rects.size() > 1 ? rects.size() - 1 : 0);
    for (size_t i = 0; i < rects.size(); ++i) {
      if (static_cast<int>(i) != moved_i) {
        others.push_back(rects[i]);
      }
    }
    const LayoutPoint snapped =
        SnapFencePosition(rects[moved_i], others, work_r, 24);
    rects[moved_i].x = snapped.x;
    rects[moved_i].y = snapped.y;
    ResolveFenceCollisions(&rects, kFenceMinW, kFenceMinH);
    // Ensure horizontal siblings on the same row seamlessly abut (gap=0) without gap or overlap
    for (size_t i = 0; i < rects.size(); ++i) {
      for (size_t j = i + 1; j < rects.size(); ++j) {
        LayoutRect& a = rects[i];
        LayoutRect& b = rects[j];
        const bool v_overlap =
            (std::max)(a.y, b.y) < (std::min)(a.bottom(), b.bottom()) ||
            std::abs(a.y - b.y) <= 60;
        if (v_overlap) {
          LayoutRect* left_r = a.x <= b.x ? &a : &b;
          LayoutRect* right_r = a.x <= b.x ? &b : &a;
          const int gap_dist = right_r->x - left_r->right();
          // Magnetic seamless snapping when within 24px or overlapping (never force a white gap)
          if (gap_dist <= 24) {
            right_r->x = left_r->right();
            if (right_r->right() > work_r.right()) {
              if (work_r.right() - right_r->x >= kFenceMinW) {
                right_r->w = work_r.right() - right_r->x;
              } else {
                right_r->w = kFenceMinW;
                right_r->x = work_r.right() - kFenceMinW;
                left_r->w = (std::max)(kFenceMinW, right_r->x - left_r->x);
              }
            }
          }
        }
      }
    }
    for (size_t i = 0; i < rects.size(); ++i) {
      RECT rr{rects[i].x, rects[i].y, rects[i].x + rects[i].w,
              rects[i].y + rects[i].h};
      ClampRectToWorkArea(&rr);
      FenceWindow* fw = list[i];
      // A position-locked fence is a snap reference, never a rearrangement
      // target: collision push and the seam pass above both write into `rects`
      // without knowing about locks, so restore its live rect before applying.
      // Otherwise dragging fence A shoves locked fence B off its seat.
      if (fw != moved && fw->IsPositionLocked()) {
        RECT live{};
        GetWindowRect(fw->Hwnd(), &live);
        rr = live;
      }
      const int new_w = rr.right - rr.left;
      const int new_h = rr.bottom - rr.top;
      if (fw != moved &&
          rr.left == before_rects[i].x && rr.top == before_rects[i].y &&
          new_w == before_rects[i].w && new_h == before_rects[i].h) {
        continue;
      }
      FencePageGeometry geo;
      geo.x = rr.left;
      geo.y = rr.top;
      geo.w = new_w;
      geo.h = new_h;
      geo.visible = fw->VisibleOnPage(page);
      geo.collapsed = fw->IsCollapsed();
      // ApplyPageGeometry refreshes expanded_h_ so Present keeps owned size.
      fw->ApplyPageGeometry(geo, /*present=*/true);
      fw->AttachToDesktopLayer();
      if (!fw->TabGroup().empty()) {
        for (auto& peer : fences_) {
          if (peer && peer.get() != fw && peer->TabGroup() == fw->TabGroup()) {
            peer->ApplyPageGeometry(geo, /*present=*/false);
          }
        }
      }
    }
    PersistLayouts();
    // Reflow only — RebuildPublicItems rescans the desktop, re-sorts every
    // float and rebuilds all slots, which ran on every drag-end / resize-end.
    // ReflowPublicFloats updates blocked rects + layout from current state.
    ReflowPublicFloats();
    NudgePetClearOfFences(pet_, fences_, current_page_);
  });
  fence->SetOnZoomChanged([this](FenceWindow* f, int zoom) {
    (void)f;
    (void)zoom;
    SchedulePersistLayouts();
    NudgePetClearOfFences(pet_, fences_, current_page_);
  });
  fence->SetOnReorder([this](FenceWindow* f,
                             const std::vector<std::wstring>& order) {
    if (f) {
      f->SetPinOrder(order);
    }
    SchedulePersistLayouts();
  });
  fence->SetOnPathsDroppedAt(
      [this](FenceWindow* f, const std::vector<std::wstring>& paths,
             int at) -> DWORD {
        // Virtual fence: SetPin + insert order. Portal: real import only —
        // pin_order does not apply (Fences Folder Portal).
        // Alt / Ctrl+Shift: land .lnk on Desktop then pin the shortcut at the
        // drop seat (Fences) — never pin the untouched source path.
        if (f && f->PortalPath().empty() && WantDropLinkEffect()) {
          PWSTR desk_raw = nullptr;
          std::wstring desk;
          if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr,
                                             &desk_raw)) &&
              desk_raw) {
            desk = desk_raw;
            CoTaskMemFree(desk_raw);
          }
          if (desk.empty()) {
            return DROPEFFECT_NONE;
          }
          HWND owner = settings_.Hwnd() ? settings_.Hwnd() : message_hwnd_;
          std::vector<std::wstring> landed;
          landed.reserve(paths.size());
          for (const auto& src : paths) {
            std::wstring dest;
            if (!CreateShortcutIntoDir(owner, src, desk, &dest) ||
                dest.empty()) {
              continue;
            }
            SuppressOrganizePath(dest);
            landed.push_back(dest);
          }
          if (landed.empty()) {
            return DROPEFFECT_NONE;
          }
          f->SetSortBy("manual");
          f->InsertPinOrder(landed, at);
          // paste=true skips WantDropLinkEffect inside HandleIncomingPaths
          // (shortcuts already materialized).
          HandleIncomingPaths(f, landed, /*paste=*/true, /*move=*/false);
          SchedulePersistLayouts();
          ShowToast(L"已创建快捷方式 " + std::to_wstring(landed.size()) +
                    L" 项");
          return DROPEFFECT_LINK;
        }
        if (f && f->PortalPath().empty()) {
          // Portal-folder sources: land on Desktop first (avoid dual-list),
          // then pin the Desktop path at the drop seat.
          HWND owner = settings_.Hwnd() ? settings_.Hwnd() : message_hwnd_;
          std::vector<std::wstring> resolved;
          const DWORD land =
              LandPortalSourcesOnDesktop(owner, fences_, paths, &resolved);
          if (resolved.empty()) {
            return DROPEFFECT_NONE;
          }
          f->SetSortBy("manual");
          f->InsertPinOrder(resolved, at);
          // paste=true: pin only (landing / link already done above).
          HandleIncomingPaths(f, resolved, /*paste=*/true, /*move=*/false);
          SchedulePersistLayouts();
          // Virtual pin is COPY to the OLE source; portal→Desktop FO already
          // ran inside LandPortalSourcesOnDesktop (never ask source to delete).
          (void)land;
          return DROPEFFECT_COPY;
        }
        const DWORD eff = OnPathsDropped(f, paths);
        SchedulePersistLayouts();
        return eff;
      });
  fence->SetOnPathsDropped(
      [this](FenceWindow* f, const std::vector<std::wstring>& paths) -> DWORD {
        return OnPathsDropped(f, paths);
      });
  fence->SetOnDragDone([this](FenceWindow*) { RefreshDesktopIcons(); });
  fence->SetOnPinDrop([this](FenceWindow* f, const std::wstring& src,
                             const std::vector<std::wstring>& paths,
                             bool commit, int at) {
    return OnPinDropped(f, src, paths, commit, at);
  });
  fence->SetOnPinDragEnd([this](FenceWindow*) { HidePinDragHint(); });
  fence->SetOnUnpin(
      [this](FenceWindow* f, const std::wstring& path, const POINT* drop) {
        if (f) {
          f->RemovePinOrder({path});
        }
        UnpinPath(path, drop);
      });
  fence->SetOnUnpinMany(
      [this](FenceWindow* f, const std::vector<std::wstring>& paths,
             const POINT* drop) {
        if (f) {
          f->RemovePinOrder(paths);
        }
        UnpinPaths(paths, drop);
      });
  fence->SetOnPathReplaced(
      [this](const std::wstring& from, const std::wstring& to) {
        RetargetPin(from, to);
      });
  fence->SetOnSetPage([this](FenceWindow* f, int page) {
    if (!f) {
      return;
    }
    if (PageLockedForFences(page) || PageLockedForFences(f->Page())) {
      ShowToast(L"该页面已锁定，无法移动分区");
      return;
    }
    f->SetPage(page);
    PersistLayouts();
    ApplyPageVisibility();
    RefreshSettingsList();
  });
  // Inline title rename commits inside FenceWindow; this only persists.
  fence->SetOnRename([this](FenceWindow* f) {
    if (!f) {
      return;
    }
    PersistLayouts();
    RefreshSettingsList();
  });
  fence->SetOnStyleChanged([this](FenceWindow*) { PersistLayouts(); });
  fence->SetOnPortal([this](FenceWindow* f, const std::wstring&) {
    if (f) {
      f->SetPageNames(page_names_);
    }
    PersistLayouts();
    RefreshDesktopIcons();
  });
  fence->SetOnPasted([this](FenceWindow* f,
                            const std::vector<std::wstring>& paths, bool move,
                            bool as_link) {
    OnPathsPasted(f, paths, move, as_link);
  });
  fence->SetOnCreated([this](FenceWindow* f,
                             const std::vector<std::wstring>& paths) {
    OnPathsCreated(f, paths);
  });
  fence->SetOnUndo([this]() { RepinFromUndoStack(); });
  fence->SetOnRefresh([this](FenceWindow*) {
    InvalidateDesktopScanCache();
    RefreshDesktopIcons();
  });
  // Fences / Explorer: only one selection context — activating a fence clears
  // other fences and public floats.
  fence->SetOnActivated([this](FenceWindow* self) {
    for (auto& f : fences_) {
      if (f && f.get() != self) {
        f->ClearSelection();
      }
    }
    public_host_.ClearSelection();
  });
  fence->SetOnDissolve([this](FenceWindow* f) { DissolveFence(f); });
  fence->SetOnMoveToFence(
      [this](FenceWindow*, const std::vector<std::wstring>& paths,
             const std::string& fence_id) {
        MoveDesktopItemsToFence(paths, fence_id);
      });
  fence->SetOnMoveToPage(
      [this](FenceWindow*, const std::vector<std::wstring>& paths, int page) {
        MoveDesktopItemsToPage(paths, page);
      });
  fence->SetOnWhitelist(
      [this](FenceWindow*, const std::vector<std::wstring>& paths, bool add) {
        SetWhitelistForPaths(paths, add);
      });
  fence->SetOnOpenSettings([this]() { OpenSettings(); });
  fence->SetOnOpenNotes(
      [this](FenceWindow*, const std::vector<std::wstring>& paths) {
        OpenPathsInNotes(paths);
      });
  fence->SetQueryFences([this]() {
    std::vector<FenceMoveTarget> out;
    for (const auto& f : fences_) {
      if (!f) {
        continue;
      }
      FenceMoveTarget t;
      t.id = f->Id();
      t.title = f->Title();
      t.pages = f->Pages().empty() ? std::vector<int>{f->Page()} : f->Pages();
      out.push_back(std::move(t));
    }
    return out;
  });
  fence->SetPageNames(page_names_);
  fence->SetOnTabPicked([this](const std::string& id) {
    ActivateFenceTab(id);
  });
  fence->SetOnJoinTabGroup(
      [this](FenceWindow* f) { JoinFenceTabGroup(f); });
  fence->SetOnLeaveTabGroup(
      [this](FenceWindow* f) { LeaveFenceTabGroup(f); });
  fence->SetPageLockedQuery([this](int page) -> bool {
    // Same invariant as layout.cpp IsPageLocked: system pages 0/1 are locked.
    if (page == 0 || page == 1) {
      return true;
    }
    if (page < 0 || page >= static_cast<int>(page_locked_.size())) {
      return false;
    }
    return page_locked_[static_cast<size_t>(page)];
  });
}

bool App::SpawnFence(const FenceLayout& layout) {
  auto fence = std::make_unique<FenceWindow>();
  if (!fence->Create(instance_, layout)) {
    return false;
  }
  fence->SetIconZoom(layout.icon_zoom);
  WireFence(fence.get());
  fences_.push_back(std::move(fence));
  return true;
}

bool App::Init(HINSTANCE instance) {
  instance_ = instance;
  LoadLanguage();
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = App::TrayWndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kMsgClass;
  RegisterClassExW(&wc);

  message_hwnd_ =
      CreateWindowExW(0, kMsgClass, L"DeskTidyNative", WS_POPUP, 0, 0, 0, 0,
                      nullptr, nullptr, instance, this);
  if (!message_hwnd_) {
    return false;
  }
  SetWindowLongPtrW(message_hwnd_, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(this));
  // Cut-ghost dimming tracks Explorer Preferred DropEffect changes.
  ChangeWindowMessageFilter(WM_COPYDATA, 1);
  ChangeWindowMessageFilter(kCliActionMsg, 1);
  AddClipboardFormatListener(message_hwnd_);
  WTSRegisterSessionNotification(message_hwnd_, NOTIFY_FOR_THIS_SESSION);
  // P1-12: drive the floating drop hint from the OLE drag loop.
  SetPinDragMoveFn([this](POINT cursor, const PinDragHint& hint) {
    UpdatePinDragHint(cursor, hint);
  });

  // P1-29/P1-30: migrate Python settings/hotkeys/prefs/first_run BEFORE loading
  // native values, otherwise the first launch loads stale defaults and the
  // migrated values are overwritten by the in-memory copies.
  MigratePythonLayoutOnce();
  // Portable debug builds write to <exe>\data; installed builds use
  // %LocalAppData%\DeskTidyNative. Adopt sparse leftovers (page_folders, …)
  // before the first prefs/hotkey load so switching trees does not look like
  // a factory reset.
  AdoptOrphanPortableSidecarOnce();

  hotkeys_ = LoadHotkeys();
  // Do not SaveHotkeys on every boot — rewriting the file from in-memory
  // defaults/fallbacks was a silent wipe vector for user chords.
  // Retire show/hide-fences chord (feature removed; fences always visible).
  for (auto& h : hotkeys_) {
    if (h.id == "toggle_fences" && (h.vk != 0 || h.modifiers != 0)) {
      h.modifiers = 0;
      h.vk = 0;
      SaveHotkeys(hotkeys_);
      break;
    }
  }
  RebuildLlHotkeyVkIndex();

  if (!settings_.Create(instance)) {
    return false;
  }
  // Do NOT WarmWebView here — layout/page_names_/fences_ are still empty, and
  // SetOnReady is not wired yet. Prefetch after LoadAppState + SetOnReady.
  settings_.SetHandlers(
      [this]() { AddFence(); },
      [this](int index) { RemoveFenceAt(index); },
      [this]() {
        settings_.Show();
      },
      [this](int index, const std::wstring& title) {
        RenameFenceAt(index, title);
      },
      [this](int index, int page) { SetFencePageAt(index, page); },
      [this]() { OrganizeVirtual(); },
      [this](bool on) { SetDockVisible(on); },
      [this](bool on) { SetPetVisible(on); }, [this]() { EditRules(); },
      [this](bool on) { SetAutostart(on); },
      [this]() { EditWhitelistDialog(settings_.Hwnd()); },
      [this]() {
        EditPageFoldersDialog(settings_.Hwnd());
        SyncPageChrome();
        PushExtensionSettings();
      },
      [this]() { ShowAppHelp(settings_.Hwnd()); },
      [this](bool on) { SetOrganizeStartup(on); },
      [this](bool on) { SetOrganizeWatch(on); },
      [this]() { ConfigureScreenRecord(settings_.Hwnd()); },
      [this]() { PushOrganizePage(&settings_, pins_, rules_, fences_, current_page_); });
  settings_.SetOnOrganizeReassign([this]() { OrganizeReassign(); });
  settings_.SetExtraHandlers(
      [this]() { SaveLayoutSnapshot(); },
      [this]() { RestoreLayoutSnapshot(); },
      [this]() { OpenMinutes(); },
      [this]() { PickAndSetWallpaper(settings_.Hwnd()); },
      [this]() { OpenLayoutEditor(); }, [this]() { OpenNotes(); },
      [this]() { CycleTheme(); }, [this]() { EditHotkeys(); },
      [this]() { ShowHotkeys(); });
  settings_.SetCommandHandler([this](const std::wstring& id) {
    DispatchSettingsCommand(id);
  });
  settings_.SetOnHotkeyCapture(
      [this](const std::wstring& key) { CaptureHotkey(key); });
  settings_.SetOnHotkeySet(
      [this](const std::wstring& key, const std::wstring& chord) {
        ApplyHotkeyChord(key, chord);
      });
  settings_.SetOnFenceStyle(
      [this](int index, const std::wstring& style) {
        SetFenceStyleAt(index, style);
      });
  settings_.SetOnFenceStyleAll(
      [this](const std::wstring& style) { SetAllFenceStyle(style); });
  settings_.SetOnFenceOpacity(
      [this](int index, int pct) { SetFenceOpacityPctAt(index, pct); });
  settings_.SetOnFenceColors(
      [this](int index, bool enabled, const std::wstring& bg,
             const std::wstring& accent) {
        SetFenceColorsAt(index, enabled, bg, accent);
      });
  settings_.SetOnFencePage(
      [this](int index, int page) { SetFencePageAt(index, page); });
  settings_.SetOnFenceLabels([this](int index, int lines, bool show) {
    if (index < 0 || index >= static_cast<int>(fences_.size()) ||
        !fences_[static_cast<size_t>(index)]) {
      return;
    }
    fences_[static_cast<size_t>(index)]->SetLabelStyle(lines, show);
    PushExtensionSettings();
  });
  settings_.SetOnFenceScale([this](int index, int value) {
    if (index < 0 || index >= static_cast<int>(fences_.size()) ||
        !fences_[static_cast<size_t>(index)]) {
      return;
    }
    fences_[static_cast<size_t>(index)]->SetCellScale(value);
    PushExtensionSettings();
  });
  settings_.SetOnFenceSort(
      [this](int index, const std::wstring& by, bool desc) {
        if (index < 0 || index >= static_cast<int>(fences_.size()) ||
            !fences_[static_cast<size_t>(index)]) {
          return;
        }
        FenceWindow* f = fences_[static_cast<size_t>(index)].get();
        // Order matters: SetSortBy resets the direction to the new key's
        // default, so apply the explicit direction afterwards. Both persist
        // via on_moved_; the final push just refreshes the settings UI.
        f->SetSortBy(NarrowUtf8(by));
        f->SetSortDescending(desc);
        PushExtensionSettings();
      });
  settings_.SetOnFenceRadius([this](int index, int value) {
    if (index < 0 || index >= static_cast<int>(fences_.size()) ||
        !fences_[static_cast<size_t>(index)]) {
      return;
    }
    fences_[static_cast<size_t>(index)]->SetRadius(value);
    PersistLayouts();
    PushExtensionSettings();
  });
  settings_.SetOnFenceTitle([this](int index) {
    if (index < 0 || index >= static_cast<int>(fences_.size()) ||
        !fences_[static_cast<size_t>(index)]) {
      return;
    }
    FenceWindow* f = fences_[static_cast<size_t>(index)].get();
    f->SetShowTitle(!f->ShowTitle());
    PushExtensionSettings();
  });
  settings_.SetOnFenceFade([this](int index) {
    if (index < 0 || index >= static_cast<int>(fences_.size()) ||
        !fences_[static_cast<size_t>(index)]) {
      return;
    }
    FenceWindow* f = fences_[static_cast<size_t>(index)].get();
    f->SetFadeOnIdle(!f->FadeOnIdle());
    PushExtensionSettings();
  });
  settings_.SetOnExtToggle([this](const std::wstring& id, bool value) {
    if (id == L"minutes_enabled" || id == L"minutes_on_bar") {
      // One switch: enable feature ≡ show「纪要」on the page bar.
      prefs_.minutes_enabled = value;
      prefs_.minutes_on_bar = value;
    } else if (id == L"notepad_enabled") {
      prefs_.notepad_enabled = value;
      SyncDeskNoteShortcuts(value);
    } else if (id == L"elevate_hotkey_helper") {
      prefs_.elevate_hotkey_helper = value;
      SyncHotkeyRelay();
    } else if (id == L"record_enabled") {
      prefs_.record_enabled = value;
      // Feature off ⇒ never keep a page-bar chip (stale on_bar was the bug).
      if (!value) {
        prefs_.record_on_bar = false;
      }
    } else if (id == L"record_on_bar") {
      prefs_.record_on_bar = value;
      // Showing on the bar implies the feature is on.
      if (value) {
        prefs_.record_enabled = true;
      }
    } else if (id == L"calculator_enabled") {
      prefs_.calculator_enabled = value;
      if (!value) {
        prefs_.calculator_on_bar = false;
      }
    } else if (id == L"calculator_on_bar") {
      prefs_.calculator_on_bar = value;
      if (value) {
        prefs_.calculator_enabled = true;
      }
    } else if (id == L"todos_enabled") {
      prefs_.todos_enabled = value;
      if (!value) {
        prefs_.todos_on_bar = false;
      }
    } else if (id == L"todos_on_bar") {
      prefs_.todos_on_bar = value;
      if (value) {
        prefs_.todos_enabled = true;
      }
    } else if (id == L"screenshot_auto_pin") {
      prefs_.screenshot_auto_pin = value;
    } else if (id == L"search_desktop") {
      prefs_.search_desktop = value;
    } else if (id == L"search_documents") {
      prefs_.search_documents = value;
    } else if (id == L"search_downloads") {
      prefs_.search_downloads = value;
    } else if (id == L"search_all_drives") {
      prefs_.search_all_drives = value;
    } else if (id == L"search_include_open_folders") {
      prefs_.search_include_open_folders = value;
    } else if (id == L"search_include_hidden") {
      prefs_.search_include_hidden = value;
    } else if (id == L"pet_auto_wander") {
      pet_.SetAutoWander(value);
      PushExtensionSettings();
      return;
    } else if (id == L"pet_show_bubbles") {
      pet_.SetShowPageBubbles(value);
      SyncPageChrome();
      PushExtensionSettings();
      return;
    } else if (id == L"check_update_on_startup") {
      prefs_.check_update_on_startup = value;
    } else if (id == L"auto_snapshot_on_change") {
      // Feature removed — ignore leftover prefs / stale UI.
    } else if (id == L"desktop_right_click_menu") {
      prefs_.desktop_right_click_menu = value;
      SyncNativeShellVerbs(prefs_.desktop_right_click_menu);
    } else if (id == L"close_to_tray") {
      prefs_.close_to_tray = value;
    } else if (id == L"show_fences" || id == L"double_click_hide") {
      // Feature removed: fences always visible; desktop dbl-click hide gone.
      prefs_.show_fences = true;
      prefs_.double_click_hide = false;
      fences_hidden_ = false;
      ApplyPageVisibility();
    } else if (id == L"enable_public_desktop") {
      SetPublicVisible(value);
    } else if (id == L"show_page_indicator") {
      prefs_.show_page_indicator = value;
    } else {
      return;
    }
    SaveAppPrefs(prefs_);
    // Push prefs into settings ext_ first, then refresh the edge bar.
    // (Old order pushed stale ext_ via SetPageNames and could re-check toggles.)
    PushExtensionSettings();
    SyncPageChrome();
  });
  settings_.SetOnExtField([this](const std::wstring& id, const std::wstring& value) {
    if (id == L"minutes_folder") {
      SetMinutesFolder(value);
    } else if (id == L"notepad_folder") {
      prefs_.notepad_folder = value;
      SaveAppPrefs(prefs_);
    } else if (id == L"wallpaper_folder") {
      SetWallpaperLibraryFolder(value);
    } else if (id == L"record_dir") {
      prefs_.record_output_dir = value;
      SaveAppPrefs(prefs_);
    } else if (id == L"record_mic") {
      prefs_.record_mic = value;
      SaveAppPrefs(prefs_);
    } else if (id == L"screenshot_max_pins") {
      prefs_.screenshot_max_pins = _wtoi(value.c_str());
      if (prefs_.screenshot_max_pins < 1) {
        prefs_.screenshot_max_pins = 1;
      }
      if (prefs_.screenshot_max_pins > 5) {
        prefs_.screenshot_max_pins = 5;
      }
      SaveAppPrefs(prefs_);
    } else if (id == L"record_quality") {
      prefs_.record_quality = _wtoi(value.c_str());
      if (prefs_.record_quality < 0 || prefs_.record_quality > 2) {
        prefs_.record_quality = 1;
      }
      SaveAppPrefs(prefs_);
    } else if (id == L"search_max_results") {
      prefs_.search_max_results = _wtoi(value.c_str());
      if (prefs_.search_max_results < 10) {
        prefs_.search_max_results = 10;
      }
      if (prefs_.search_max_results > 500) {
        prefs_.search_max_results = 500;
      }
      SaveAppPrefs(prefs_);
    } else if (id == L"search_max_depth") {
      prefs_.search_max_depth = _wtoi(value.c_str());
      if (prefs_.search_max_depth < 1) {
        prefs_.search_max_depth = 1;
      }
      if (prefs_.search_max_depth > 32) {
        prefs_.search_max_depth = 32;
      }
      SaveAppPrefs(prefs_);
    } else if (id == L"search_threads") {
      prefs_.search_threads = _wtoi(value.c_str());
      if (prefs_.search_threads < 1) {
        prefs_.search_threads = 1;
      }
      if (prefs_.search_threads > 64) {
        prefs_.search_threads = 64;
      }
      SaveAppPrefs(prefs_);
    } else if (id == L"pet_size_scale") {
      pet_.SetSizeScale(static_cast<float>(_wtof(value.c_str())));
    } else if (id == L"pet_character") {
      pet_.ApplyCharacter(value);
    } else if (id == L"pet_actions") {
      // value: "hoodie:wait,sleep" or "voyage:wait"
      const size_t colon = value.find(L':');
      if (colon != std::wstring::npos) {
        const std::wstring cid = value.substr(0, colon);
        const std::wstring rest = value.substr(colon + 1);
        std::vector<std::string> acts;
        size_t start = 0;
        while (start <= rest.size()) {
          size_t comma = rest.find(L',', start);
          if (comma == std::wstring::npos) {
            comma = rest.size();
          }
          std::wstring part = rest.substr(start, comma - start);
          if (!part.empty()) {
            acts.push_back(NarrowUtf8(part));
          }
          if (comma >= rest.size()) {
            break;
          }
          start = comma + 1;
        }
        pet_.SetEnabledActions(cid, acts);
        PushExtensionSettings();
      }
    } else if (id == L"dock_edge") {
      prefs_.dock_edge = _wtoi(value.c_str());
      if (prefs_.dock_edge < 0 || prefs_.dock_edge > 2) {
        prefs_.dock_edge = 0;
      }
      SaveAppPrefs(prefs_);
      ApplyDockPrefs();
    } else if (id == L"dock_opacity") {
      prefs_.dock_opacity = _wtoi(value.c_str());
      if (prefs_.dock_opacity < 50) {
        prefs_.dock_opacity = 50;
      }
      if (prefs_.dock_opacity > 100) {
        prefs_.dock_opacity = 100;
      }
      SaveAppPrefs(prefs_);
      ApplyDockPrefs();
    } else if (id == L"theme_id") {
      ApplyThemeId(NarrowUtf8(value));
      return;
    } else if (id == L"ui_theme") {
      ApplyUiThemeId(NarrowUtf8(value));
      return;
    } else if (id == L"lang") {
      // Explicit language choice from the settings dropdown. SetLanguage is
      // idempotent, so no toggle-vs-set ambiguity (ToggleLanguage would flip
      // back if the UI ever re-sent the current value).
      const std::string lang = (value == L"en") ? "en" : "zh";
      SetLanguage(lang.c_str());
      ShowToast(lang == "en" ? L"Language: English" : L"语言：中文");
      PushHotkeyState();
      PushExtensionSettings();
      return;
    } else if (id == L"page_chrome_style") {
      const std::string style = NarrowUtf8(value);
      prefs_.page_chrome_style =
          (style == "tabs" || style == "tab" || style == "chips") ? "tabs"
                                                                 : "float";
      SaveAppPrefs(prefs_);
      SyncPageChrome();
    } else if (id == L"page_bar_style") {
      SetPageBarStyle(value);
      return;
    } else if (id == L"exclude_patterns") {
      prefs_.exclude_patterns = SplitDockCsv(value);
      if (prefs_.exclude_patterns.empty()) {
        prefs_.exclude_patterns = {L"desktop.ini", L"DeskTidy"};
      }
      SaveAppPrefs(prefs_);
      InvalidateDesktopScanCache();
      RefreshDesktopIcons();
    } else if (id == L"dock_items") {
      prefs_.dock_items = SplitDockCsv(value);
      if (prefs_.dock_items.size() > 8) {
        prefs_.dock_items.resize(8);
      }
      SaveAppPrefs(prefs_);
      RebuildDockItems();
    } else {
      return;
    }
    PushExtensionSettings();
  });

  todos_.Create(instance);
  search_.Create(instance);
  dock_.Create(instance);
  pet_.Create(instance);
  pet_.SetOnTrash([this](const std::vector<std::wstring>& paths) -> bool {
    if (SendPathsToRecycleBin(paths)) {
      RefreshDesktopIcons();
      return true;
    }
    ShowToast(L"移入回收站失败");
    return false;
  });
  pet_.SetOnPage([this](int page) { SetPage(page); });
  pet_.SetOnCreateFence([this](int page) { CreateFenceOnPage(page); });
  pet_.SetOnHide([this]() { SetPetVisible(false); });
  auto activate_float_tool = [this](const std::wstring& id) {
    if (id == L"record") {
      if (!prefs_.record_enabled && !IsScreenRecording()) {
        ShowToast(L"录屏未启用 — 请在设置 → 扩展功能中打开");
        return;
      }
      StartScreenRecord(settings_.Hwnd());
    } else if (id == L"minutes") {
      if (!prefs_.minutes_enabled) {
        ShowToast(L"会议纪要未启用 — 请在设置 → 扩展功能中打开");
        return;
      }
      OpenMinutes();
    } else if (id == L"calculator") {
      if (!prefs_.calculator_enabled) {
        ShowToast(L"计算器未启用 — 请在设置 → 扩展功能中打开");
        return;
      }
      OpenCalculator();
    } else if (id == L"todo") {
      if (!prefs_.todos_enabled) {
        ShowToast(L"桌面待办未启用 — 请在设置 → 扩展功能中打开");
        return;
      }
      OpenTodos();
    }
  };
  pet_.SetOnTool(activate_float_tool);
  auto open_folder = [this](const std::wstring& path) {
    if (path.empty()) {
      return;
    }
    CreateDirectoryW(path.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
  };
  pet_.SetOnFolder(open_folder);
  page_bar_.Create(instance);
  page_bar_.SetOnPage([this](int page) { SetPage(page); });
  page_bar_.SetOnRenamePage([this]() { RenameCurrentPage(); });
  page_bar_.SetOnCreateFence([this](int page) { CreateFenceOnPage(page); });
  page_bar_.SetOnDeletePage([this](int page) { DeletePageAt(page); });
  page_bar_.SetOnRenamePageAt([this](int page) { RenamePageAt(page); });
  page_bar_.SetOnTogglePageLock([this](int page) { TogglePageLock(page); });
  page_bar_.SetOnMoveFenceToPage(
      [this](int page, int fence) { SetFencePageAt(fence, page); });
  page_bar_.SetOnTool(activate_float_tool);
  page_bar_.SetOnFolder(open_folder);
  page_bar_.SetOnMoved([this]() {
    prefs_.page_bar_anchor_y = page_bar_.AnchorY();
    SaveAppPrefs(prefs_);
  });
  page_bar_.SetOnSetStyle([this](const std::string& style_id) {
    SetPageBarStyle(WidenUtf8(style_id));
  });
  public_host_.Create(instance);
  dock_.SetOnPathReplaced(
      [this](const std::wstring& from, const std::wstring& to) {
        RetargetPin(from, to);
      });
  dock_.SetOnRemoved([this](const std::wstring& path) {
    UnpinPath(path, nullptr);  // menu remove → auto-pack float
  });
  public_host_.SetOnPathReplaced(
      [this](const std::wstring& from, const std::wstring& to) {
        RetargetPin(from, to);
      });
  public_host_.SetOnRemoved([this](const std::wstring& path) {
    EraseFloatPlacement(&float_placements_, path);
    SaveFloatPlacements(float_placements_);
    if (const std::string* cur = FindPin(pins_, path)) {
      for (auto& f : fences_) {
        if (f && f->Id() == *cur) {
          f->RemovePinOrder({path});
          PersistLayouts();
          break;
        }
      }
      ClearPin(&pins_, path);
      BumpPinsEpoch();
      SavePins(pins_);
    }
    RebuildPublicItems();
  });
  // Same path as fence refresh: drop scan TTL cache then rebuild fences + floats.
  // RebuildPublicItems alone kept a warm ScanDesktopItems cache and skipped
  // portal fences — paste/new via shell menu looked like a no-op until 刷新.
  public_host_.SetOnActivated([this]() {
    for (auto& f : fences_) {
      if (f && f->VisibleOnPage(current_page_)) {
        f->ClearSelection();
      }
    }
  });
  public_host_.SetOnOpenSettings([this]() { OpenSettings(); });
  public_host_.SetOnNewFence([this](POINT pt) {
    RECT rc{pt.x, pt.y, pt.x + 280, pt.y + 360};
    AddFenceAtRect(rc);
  });
  public_host_.SetOnOrganize([this]() { OrganizeVirtual(true); });
  public_host_.SetOnNotes([this]() { OpenNotes(); });
  public_host_.SetOnRefresh([this]() {
    InvalidateDesktopScanCache();
    RefreshDesktopIcons();
  });
  public_host_.SetOnPaste([this](bool as_link) { OnDesktopPaste(as_link); });
  public_host_.SetOnUndo([this]() { RepinFromUndoStack(); });
  public_host_.SetOnPathsDropped(
      [this](const std::vector<std::wstring>& paths) -> DWORD {
        return OnDesktopDropPaths(paths);
      });
  public_host_.SetOnPlacementsChanged([this](const FloatPlacementMap& m) {
    float_placements_ = m;
    SaveFloatPlacements(float_placements_);
  });
  public_host_.SetOnIconZoomChanged([this](int zoom) {
    prefs_.public_icon_zoom = zoom;
    SaveAppPrefs(prefs_);
  });
  public_host_.SetOnMoveToFence(
      [this](const std::vector<std::wstring>& paths, const std::string& fence_id) {
        MoveDesktopItemsToFence(paths, fence_id);
      });
  public_host_.SetQueryFences([this]() {
    std::vector<MoveFenceMenuTarget> out;
    for (const auto& f : fences_) {
      if (!f) {
        continue;
      }
      MoveFenceMenuTarget t;
      t.id = f->Id();
      t.title = f->Title();
      t.pages = f->Pages().empty() ? std::vector<int>{f->Page()} : f->Pages();
      out.push_back(std::move(t));
    }
    return out;
  });
  search_.SetOnPathReplaced(
      [this](const std::wstring& from, const std::wstring& to) {
        RetargetPin(from, to);
      });
  search_.SetOnRemoved([this](const std::wstring& path) {
    UnpinPath(path, nullptr);
  });
  search_.SetOnAllDrivesChanged([this](bool on) {
    prefs_.search_all_drives = on;
    SaveAppPrefs(prefs_);
    PushExtensionSettings();
  });

  prefs_ = LoadAppPrefs();
  // Former dual toggles → one: enable ≡ show「纪要」on the page bar.
  if (prefs_.minutes_on_bar != prefs_.minutes_enabled) {
    prefs_.minutes_on_bar = prefs_.minutes_enabled;
  }
  // Heal stale pairs: chip without enable (e.g. record_on_bar=true while
  // record_enabled=false) left ghost buttons on the page bar.
  if (!prefs_.record_enabled) {
    prefs_.record_on_bar = false;
  }
  if (!prefs_.calculator_enabled) {
    prefs_.calculator_on_bar = false;
  }
  if (!prefs_.todos_enabled) {
    prefs_.todos_on_bar = false;
  }
  prefs_.ui_theme = NormalizeUiTheme(prefs_.ui_theme);
  prefs_.session_active = true;
  // Product: fences always on; desktop dbl-click hide removed.
  prefs_.show_fences = true;
  prefs_.double_click_hide = false;
  fences_hidden_ = false;
  SaveAppPrefs(prefs_);
  todos_.ApplyUiTheme(prefs_.ui_theme);
  page_bar_.SetAnchorY(prefs_.page_bar_anchor_y);
  public_host_.SetIconZoom(prefs_.public_icon_zoom);
  ApplyDockPrefs();
  RegisterNoteFileAssociations();
  display_fp_ = DisplayFingerprint();
  display_work_ = PrimaryWorkArea();
  app_dpi_ = SystemDpi();
  // public_visible_ is set inside RefreshDesktopIcons (public toggle | loose).

  // (Migration already ran above, before LoadHotkeys/LoadAppPrefs.)

  const AppState state = LoadAppState();
  current_page_ = state.current_page;
  page_names_ = state.page_names;
  page_locked_ = state.page_locked;
  NormalizePageNames(&page_names_);
  dock_visible_ = prefs_.dock_visible;
  pet_visible_ = state.pet_visible;
  pins_ = LoadPins();
  float_placements_ = LoadFloatPlacements();
  if (HealPinsFromBakOnce(&pins_) > 0) {
    SavePins(pins_);
  }
  // Remap pins that still point at pre-migration fence ids.
  // 文档类文件曾被钉到 system_common / system_software_side，迁回文档分区。
  bool pins_migrated = false;
  for (auto& kv : pins_) {
    if (kv.second == "system_software") {
      kv.second = "system_software_side";
      pins_migrated = true;
    } else if (kv.second == "system_page2") {
      kv.second = "system_docs";
      pins_migrated = true;
    }
  }
  // Do not HealMisplacedSystemPins here — already-pinned fence icons must stay
  // where the user put them (Fences-class organize / boot).
  if (pins_migrated) {
    SavePins(pins_);
  }
  rules_ = LoadRules();
  // Load only — rewriting rules.json every boot is unnecessary churn.

  for (const auto& layout : state.fences) {
    if (!SpawnFence(layout)) {
      return false;
    }
  }
  // Drop pins aimed at fence ids that no longer exist (deleted partitions /
  // stale category maps) so icons reappear as floats instead of vanishing.
  {
    std::unordered_set<std::string> live;
    for (const auto& f : fences_) {
      if (f && !f->Id().empty()) {
        live.insert(f->Id());
      }
    }
    std::vector<std::wstring> dead;
    for (const auto& kv : pins_) {
      if (kv.second.empty() || live.find(kv.second) != live.end()) {
        continue;
      }
      dead.push_back(kv.first);
    }
    if (!dead.empty()) {
      for (const auto& p : dead) {
        ClearPin(&pins_, p);
      }
      SavePins(pins_);
      LogLine(L"[pin] boot dropped %d stale fence_id pin(s)",
              static_cast<int>(dead.size()));
    }
  }
  // Reconcile pin_order with pins.json: collapse corrupt "\\\\" paths, drop
  // empty/stale seats (Fences packs top-left), and append missing membership.
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    const std::string& fid = fence->Id();
    std::vector<std::wstring> order = fence->PinOrder();
    for (auto& p : order) {
      p = NormalizeStoredPath(p);
    }
    std::unordered_set<std::wstring> seen;
    std::vector<std::wstring> cleaned;
    cleaned.reserve(order.size() + 8);
    auto norm_key = [](std::wstring s) {
      for (auto& c : s) {
        if (c >= L'A' && c <= L'Z') {
          c = static_cast<wchar_t>(c - L'A' + L'a');
        }
      }
      return s;
    };
    for (size_t i = 0; i < order.size(); ++i) {
      if (order[i].empty()) {
        continue;  // strip legacy "" sentinels
      }
      const auto* pin = FindPin(pins_, order[i]);
      if (!pin || *pin != fid) {
        continue;
      }
      const std::wstring key = norm_key(order[i]);
      if (!seen.insert(key).second) {
        continue;
      }
      cleaned.push_back(order[i]);
    }
    for (const auto& kv : pins_) {
      if (kv.second != fid) {
        continue;
      }
      const std::wstring path = NormalizeStoredPath(kv.first);
      if (!seen.insert(norm_key(path)).second) {
        continue;
      }
      cleaned.push_back(path);
    }
    if (cleaned != fence->PinOrder()) {
      fence->SetPinOrder(std::move(cleaned));
    }
  }
  PersistLayouts();
  // Match Python: host This PC / Recycle Bin as fence pins before first paint.
  if (EnsureHostedSystemNamespaceIcons(&pins_) > 0) {
    SavePins(pins_);
  }
  RefreshDesktopIcons();
  RefreshSettingsList();
  settings_.SetToggles(dock_visible_, pet_visible_, IsNativeAutostartEnabled());
  settings_.SetOrganizePrefs(prefs_.auto_organize_on_startup,
                             prefs_.auto_organize_watch);
  SyncPageChrome();
  // Organized mode is the product default: Explorer icons hidden, fences shown.
  // This must be set BEFORE ApplyPageVisibility(), which skips every fence when
  // icons_visible_intent_ is true (its member initialiser). Booting with that
  // default left the fences unmapped until the first band restore, which is
  // exactly why the very first Win+D looked like "fences don't show".
  icons_visible_intent_ = false;
  SyncFenceTabGroups();
  ApplyPageVisibility();
  InstallFenceKeyHook();
  dock_.SetVisible(dock_visible_);
  pet_.SetVisible(pet_visible_);
  public_host_.SetVisible(public_visible_);
  RebuildDockItems();
  RebuildPublicItems();
  desktop_watch_.Reset();
  StartDesktopShellNotify();
  SyncNativeShellVerbs(prefs_.desktop_right_click_menu);

  // Win+D settle is driven by the keyboard hook. A tight restore timer
  // re-layered the fences and flashed the desktop.
  restore_tick_ms_ = 4000;
  restore_timer_ = SetTimer(message_hwnd_, kTimerRestore, restore_tick_ms_, nullptr);
  pet_tick_ms_ = 200;
  pet_timer_ = SetTimer(message_hwnd_, kTimerPet, pet_tick_ms_, nullptr);
  watch_tick_ms_ = 2000;
  watch_timer_ = SetTimer(message_hwnd_, kTimerWatch, watch_tick_ms_, nullptr);
  // P2-1: low-frequency background trim (Python ~60s while tray/background).
  SetTimer(message_hwnd_, kTimerMemTrim, 60000, nullptr);
  // Defer WebView2 warm so boot paints fences before browser processes spin up
  // (todo/search stay lazy; settings warms a few seconds later).
  SetTimer(message_hwnd_, kTimerWebViewWarm, 3000, nullptr);
  // Hybrid-GPU (Optimus / AMD switchable) recovery: re-upload visible overlays
  // a few times after boot to cover slow DWM composition across common machines.
  overlay_refresh_left_ = 4;
  SetTimer(message_hwnd_, kTimerOverlayRefresh, 1200, nullptr);

  CreateTrayIcon();
  ReregisterHotkeys();
  // Refresh DeskNote.lnk → sibling deskNote.exe.
  SyncDeskNoteShortcuts(prefs_.notepad_enabled);
  // Organized mode: Explorer icons off so fence pins are not duplicated.
  ReconcileNamespaceIconsOnStartup(true);
  SetDesktopIconsVisible(false);
  icons_visible_intent_ = false;
  SetDesktopOrganizedBlankMenu(true);
  // Loose floats mirror the intent, not Explorer's (lagging) live state.
  if (message_hwnd_) {
    SetTimer(message_hwnd_, kTimerIconsRefresh, 600, nullptr);
  }
  StartNamespaceIconRebuildGuard();
  // Recover a recording interrupted by a crash: merge leftover segments.
  if (RecoverInterruptedRecording()) {
    ShowTrayBalloon(L"录屏", L"已恢复上次中断的录制，文件已保存到录屏目录。");
  }
  StartForegroundHook();
  // One-binary Win10/Win11 profile (Fences-class runtime branch, not split builds).
  LogDesktopShellCompatProfile();
  StartDesktopDblClickMonitor(message_hwnd_, kMsgDesktopDblClick,
                              kMsgDesktopBlankDown, kMsgDesktopPaste,
                              kMsgDesktopBlankRClick, kMsgDesktopRubberMove,
                              kMsgDesktopRubberUp);
  settings_.SetOnExit([this]() { HandleSettingsClose(); });
  settings_.SetOnHideToTray([this]() {
    // P2-1: tray balloon (Shell_NotifyIcon NIF_INFO), not the custom toast.
    ShowTrayBalloon(L"DeskTidy", L"程序已最小化到系统托盘，继续在后台运行。");
    TrimMemory();
  });
  settings_.SetOnReady([this]() {
    // Push settings data only — do NOT SyncPageChrome here (that re-seats
    // fences/page bar and flashes the desktop every time settings.html loads).
    RefreshSettingsList();
    settings_.SetToggles(dock_visible_, pet_visible_, IsNativeAutostartEnabled());
    settings_.SetOrganizePrefs(prefs_.auto_organize_on_startup,
                               prefs_.auto_organize_watch);
    settings_.SetPageNames(page_names_, false);
    settings_.SetCurrentPage(current_page_, false);
    PushExtensionSettings();
    PushHotkeyState();
  });
  // Settings WebView warm is deferred (kTimerWebViewWarm) so boot paints fences
  // first; the first ready push still carries live pages / fence_cards.
  PushHotkeyState();
  ScheduleDesktopBandRestore();
  MigratePythonAutostartOnce();
  // Product default: start with Windows (Fences-class / installer task checked).
  // One-shot so existing profiles pick up the new default without re-forcing
  // after the user later turns it off.
  {
    const std::wstring marker =
        JoinPath(NativeDataDir(), L"autostart_default_on.done");
    if (FileMissingOrTiny(marker, 1)) {
      if (!IsNativeAutostartEnabled()) {
        SetNativeAutostart(true);
      }
      HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        const char ok[] = "1\n";
        DWORD w = 0;
        WriteFile(h, ok, 2, &w, nullptr);
        CloseHandle(h);
      }
      settings_.SetToggles(dock_visible_, pet_visible_,
                          IsNativeAutostartEnabled());
    }
  }
  MaybeShowFirstRun(settings_.Hwnd());
  // Fences-class once: old default watch=off + hidden Explorer icons made new
  // Desktop files land only in floats (easy to miss). Rules already pin by
  // extension — flip watch on once; reassign=false so manual pins stay put.
  bool fences_watch_migrated = false;
  {
    const std::wstring marker =
        JoinPath(NativeDataDir(), L"organize_watch_fences_default.done");
    if (FileMissingOrTiny(marker, 1)) {
      if (!prefs_.auto_organize_watch) {
        prefs_.auto_organize_watch = true;
        SaveAppPrefs(prefs_);
        fences_watch_migrated = true;
      }
      HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        const char ok[] = "1\n";
        DWORD w = 0;
        WriteFile(h, ok, 2, &w, nullptr);
        CloseHandle(h);
      }
      settings_.SetOrganizePrefs(prefs_.auto_organize_on_startup,
                                 prefs_.auto_organize_watch);
    }
  }
  // U6: one-time first-run organize (independent of auto_organize_on_startup).
  // Startup auto-organize also requires auto_organize_watch (P1-29 / Python
  // _startup_deferred_organize). Manual 「一键整理」is unaffected.
  const bool first_organize = !prefs_.first_run_organize_done;
  if (first_organize) {
    OrganizeVirtual(true);
    prefs_.first_run_organize_done = true;
    SaveAppPrefs(prefs_);
  } else if (fences_watch_migrated ||
             (prefs_.auto_organize_on_startup && prefs_.auto_organize_watch)) {
    OrganizeVirtual(true);
  }
  // P1-25: reclaim orphan ffmpeg left by a previous crash.
  AdoptOrphanScreenRecord();
  // Deferred quiet update check (Python _startup_deferred_update_check).
  if (prefs_.check_update_on_startup && message_hwnd_) {
    update_timer_ = SetTimer(message_hwnd_, kTimerUpdate, 4000, nullptr);
  }
  return true;
}

void App::ReregisterHotkeys() {
  if (!message_hwnd_) {
    return;
  }
  RebuildLlHotkeyVkIndex();
  for (size_t i = 0; i < 32; ++i) {
    UnregisterHotKey(message_hwnd_, static_cast<int>(kHotkeyBase + i));
  }
  std::vector<std::wstring> conflicts;
  for (size_t i = 0; i < hotkeys_.size(); ++i) {
    const HotkeyDef& k = hotkeys_[i];
    if (k.vk == 0) {
      continue;  // unbound — shown in settings for user to assign
    }
    const int hid = static_cast<int>(kHotkeyBase + i);
    bool ok = RegisterHotKey(message_hwnd_, hid, k.modifiers | MOD_NOREPEAT,
                             k.vk);
    if (!ok) {
      ok = RegisterHotKey(message_hwnd_, hid, k.modifiers, k.vk);
    }
    if (ok) {
      LogLine(L"[hotkey] registered %s (%s) id=%d", WidenUtf8(k.id).c_str(),
              FormatHotkey(k.modifiers, k.vk).c_str(), hid);
      continue;
    }
    // Primary occupied: try fallback chords at runtime only. Never rewrite
    // hotkeys.json — persisting fallbacks was wiping user chords (e.g. F1→Ctrl+Alt+1).
    bool fell_back = false;
    std::wstring live_chord;
    for (const auto& alt : HotkeyFallbacks(k.id)) {
      UINT m = 0, v = 0;
      if (!ParseHotkey(alt, &m, &v) || v == 0) {
        continue;
      }
      if (m == k.modifiers && v == k.vk) {
        continue;  // same chord as the failed primary
      }
      if (RegisterHotKey(message_hwnd_, hid, m | MOD_NOREPEAT, v) ||
          RegisterHotKey(message_hwnd_, hid, m, v)) {
        fell_back = true;
        live_chord = alt;
        LogLine(L"[hotkey] %s primary occupied -> runtime fallback %s "
                L"(prefs kept %s)",
                WidenUtf8(k.id).c_str(), alt.c_str(),
                FormatHotkey(k.modifiers, k.vk).c_str());
        break;
      }
    }
    if (!fell_back) {
      conflicts.push_back(k.label + L"（" + FormatHotkey(k.modifiers, k.vk) +
                          L"）");
      LogLine(L"[hotkey] register FAILED %s (%s)", WidenUtf8(k.id).c_str(),
              FormatHotkey(k.modifiers, k.vk).c_str());
    } else if (!live_chord.empty()) {
      conflicts.push_back(k.label + L"（偏好 " +
                          FormatHotkey(k.modifiers, k.vk) + L" 被占用，本机暂用 " +
                          live_chord + L"）");
    }
  }
  // Do not SaveHotkeys here — registration must not mutate user prefs.
  PushHotkeyState();
  SyncHotkeyRelay();
  if (!conflicts.empty()) {
    std::wstring msg = L"以下快捷键注册失败（可能被其他程序占用）：\n";
    for (const auto& c : conflicts) {
      msg += L"• " + c + L"\n";
    }
    ShowToast(msg);
  }
}

void App::SyncHotkeyRelay() {
  if (!message_hwnd_) {
    return;
  }
  const bool elevate = prefs_.elevate_hotkey_helper;
  if (hotkey_relay_.Running()) {
    hotkey_relay_.UpdateMapping(hotkeys_);
    if (elevate && !hotkey_relay_.HelperLaunched()) {
      hotkey_relay_.EnsureElevatedHelper();
    }
    return;
  }
  // Default: no UAC. Pref elevate_hotkey_helper opts into runas helper (P1-28).
  if (!hotkey_relay_.Start(message_hwnd_, kMsgRelayHotkey, hotkeys_, elevate)) {
    LogLineA("[hotkey_relay] Start failed");
  }
}

void App::OnRelayHotkeyAction(const std::string& action) {
  if (action == "show_desktop") {
    MinimizeFullscreenCover();
    ScheduleDesktopBandRestore();
    return;
  }
  const ULONGLONG now = GetTickCount64();
  const auto sit = hotkey_suppress_until_.find(action);
  if (sit != hotkey_suppress_until_.end() && now < sit->second) {
    return;
  }
  const auto lit = hotkey_last_fire_.find(action);
  if (lit != hotkey_last_fire_.end() && now - lit->second < 300) {
    return;
  }
  hotkey_last_fire_[action] = now;
  DispatchHotkey(action);
}

void App::RebuildLlHotkeyVkIndex() {
  std::fill(std::begin(ll_hotkey_vk_), std::end(ll_hotkey_vk_), false);
  for (const auto& k : hotkeys_) {
    if (k.vk != 0 && k.vk < 256) {
      ll_hotkey_vk_[k.vk] = true;
    }
  }
}

void App::TryDispatchLlHotkey(UINT vk) {
  if (!message_hwnd_ || vk == 0 || session_locked_) {
    return;
  }
  // Ignore pure modifier keys.
  if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN ||
      vk == VK_RWIN || vk == VK_LCONTROL || vk == VK_RCONTROL ||
      vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LMENU || vk == VK_RMENU) {
    return;
  }
  // Typing path: skip GetAsyncKeyState + scan unless this vk is bound.
  if (vk >= 256 || !ll_hotkey_vk_[vk]) {
    return;
  }
  UINT mods = 0;
  if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
    mods |= MOD_CONTROL;
  }
  if (GetAsyncKeyState(VK_SHIFT) & 0x8000) {
    mods |= MOD_SHIFT;
  }
  if (GetAsyncKeyState(VK_MENU) & 0x8000) {
    mods |= MOD_ALT;
  }
  if (GetAsyncKeyState(VK_LWIN) & 0x8000 ||
      GetAsyncKeyState(VK_RWIN) & 0x8000) {
    mods |= MOD_WIN;
  }
  // Win+D is handled separately for band restore — skip here.
  if ((mods & MOD_WIN) && (vk == 'D' || vk == 'L')) {
    return;
  }
  for (size_t i = 0; i < hotkeys_.size(); ++i) {
    const HotkeyDef& k = hotkeys_[i];
    if (k.vk == 0) {
      continue;
    }
    if (k.vk == vk && k.modifiers == mods) {
      PostMessageW(message_hwnd_, kMsgLlHotkey, static_cast<WPARAM>(i), 0);
      return;
    }
  }
}

void App::SetIconsVisible(bool visible, bool toast) {
  icons_visible_intent_ = visible;
  SetDesktopIconsVisible(visible);
  // Organized: blank wallpaper RMB is hosted (DefView Paste can't seat floats).
  SetDesktopOrganizedBlankMenu(!visible);
  // Stock vs organized (Fences-class): showing Explorer icons hides fence
  // chrome so pins are not double-painted on DefView. Hiding icons restores
  // the tidy layer. Public floats already follow !icons_visible_intent_.
  if (visible) {
    for (auto& f : fences_) {
      if (f) {
        f->HideTransient();
      }
    }
    public_host_.Hide();
  } else if (!fences_hidden_ && !peek_active_) {
    ApplyPageVisibility();
  }
  // Explorer applies the toggle asynchronously; defer the float refresh.
  if (message_hwnd_) {
    SetTimer(message_hwnd_, kTimerIconsRefresh, 400, nullptr);
  }
  if (toast) {
    ShowToast(visible ? L"已显示桌面图标（库存桌面）"
                      : L"已隐藏桌面图标（整理桌面）");
  }
}

void App::DispatchHotkey(const std::string& id) {
  if (id == "organize") {
    OrganizeVirtual(true);
  } else if (id == "toggle_fences" || id == "fences") {
    ToggleFencesVisible();
  } else if (id == "toggle_icons") {
    SetIconsVisible(!icons_visible_intent_, true);
  } else if (id == "peek_fences" || id == "peek") {
    TogglePeek();
  } else if (id == "page_next") {
    if (!page_names_.empty()) {
      SetPage((current_page_ + 1) % static_cast<int>(page_names_.size()));
    }
  } else if (id == "page_prev") {
    if (!page_names_.empty()) {
      const int n = static_cast<int>(page_names_.size());
      SetPage((current_page_ + n - 1) % n);
    }
  } else if (id == "screenshot" || id == "shot") {
    StartRegionCapture();
  } else if (id == "show_screenshot" || id == "fullscreen") {
    // F2 /「显示贴图」: fill the desktop pin stack up to screenshot_max_pins
    // from newest recent shots (settings「同时贴图最多 N 张」).
    int max_pins = prefs_.screenshot_max_pins;
    if (max_pins < 1) {
      max_pins = 1;
    }
    if (max_pins > 8) {
      max_pins = 8;
    }
    // New clipboard image (external copy) can occupy one slot first.
    PinClipboardImage();
    int added = 0;
    for (const auto& path : recent_shots_) {
      if (static_cast<int>(pins_img_.size()) >= max_pins) {
        break;
      }
      if (path.empty() || IsPathPinned(path)) {
        continue;
      }
      const size_t before = pins_img_.size();
      PinImage(path);
      if (pins_img_.size() > before) {
        ++added;
      }
    }
    if (pins_img_.empty()) {
      ShowToast(L"剪贴板中无图片，请先截图或复制图片 (F2贴图)");
      return;
    }
    for (auto& p : pins_img_) {
      if (p) {
        p->Show();
      }
    }
    if (added > 0) {
      ShowToast(L"已贴图 " + std::to_wstring(static_cast<int>(pins_img_.size())) +
                L"/" + std::to_wstring(max_pins) + L" 张");
    }
  } else if (id == "screen_record" || id == "record") {
    if (!prefs_.record_enabled && !IsScreenRecording()) {
      ShowToast(L"录屏未启用 — 请在设置 → 扩展功能中打开");
      return;
    }
    StartScreenRecord(settings_.Hwnd());
  } else if (id == "file_search" || id == "search") {
    OpenSearch();
  } else if (id == "calculator") {
    if (!prefs_.calculator_enabled) {
      ShowToast(L"计算器未启用 — 请在设置 → 扩展功能中打开");
      return;
    }
    OpenCalculator();
  } else if (id == "settings") {
    OpenSettings();
  } else if (id == "notes") {
    OpenNotes();
  } else if (id == "todos") {
    if (!prefs_.todos_enabled) {
      ShowToast(L"桌面待办未启用 — 请在设置 → 扩展功能中打开");
      return;
    }
    OpenTodos();
  } else if (id == "page1") {
    SetPage(0);
  } else if (id == "page2") {
    SetPage(1);
  }
}

void App::EditHotkeys() {
  OpenSettings();
  settings_.FocusSubpage(L"hotkey");
}

void App::CaptureHotkey(const std::wstring& key_id) {
  const std::string id = NarrowUtf8(key_id);
  int idx = -1;
  for (size_t i = 0; i < hotkeys_.size(); ++i) {
    if (hotkeys_[i].id == id) {
      idx = static_cast<int>(i);
      break;
    }
  }
  if (idx < 0) {
    return;
  }
  HotkeyDef* target = &hotkeys_[static_cast<size_t>(idx)];
  // Unregister while capturing so the global hotkey cannot fire mid-capture
  // (the capture loop pumps the thread queue and would otherwise dispatch it).
  UnregisterHotKey(message_hwnd_, static_cast<int>(kHotkeyBase + idx));
  UINT mods = 0, vk = 0;
  if (!CaptureHotkeyChord(settings_.Hwnd(), &mods, &vk)) {
    if (target->vk) {
      RegisterHotKey(message_hwnd_, static_cast<int>(kHotkeyBase + idx),
                     target->modifiers | MOD_NOREPEAT, target->vk) ||
          RegisterHotKey(message_hwnd_, static_cast<int>(kHotkeyBase + idx),
                         target->modifiers, target->vk);
    }
    return;
  }
  target->modifiers = mods;
  target->vk = vk;
  SaveHotkeys(hotkeys_);
  ReregisterHotkeys();
  PushHotkeyState();
  // Suppress the just-bound chord briefly (Python _arm_after_register).
  hotkey_suppress_until_[id] = GetTickCount64() + 550;
  ShowToast(target->label + L" → " + FormatHotkey(mods, vk));
}

void App::ApplyHotkeyChord(const std::wstring& key_id,
                           const std::wstring& chord) {
  const std::string id = NarrowUtf8(key_id);
  HotkeyDef* target = nullptr;
  for (auto& k : hotkeys_) {
    if (k.id == id) {
      target = &k;
      break;
    }
  }
  if (!target) {
    return;
  }
  UINT mods = 0, vk = 0;
  if (chord.empty()) {
    // Unbind (settings / extensions Delete·Backspace).
    target->modifiers = 0;
    target->vk = 0;
    SaveHotkeys(hotkeys_);
    ReregisterHotkeys();
    PushHotkeyState();
    ShowToast(target->label + L" → （未绑定）");
    return;
  }
  if (!ParseHotkey(chord, &mods, &vk) || vk == 0) {
    ShowToast(L"无效快捷键");
    return;
  }
  target->modifiers = mods;
  target->vk = vk;
  SaveHotkeys(hotkeys_);
  ReregisterHotkeys();
  PushHotkeyState();
  hotkey_suppress_until_[id] = GetTickCount64() + 550;
  ShowToast(target->label + L" → " + FormatHotkey(mods, vk));
}

int App::Run() {
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

void App::ReapFinishedWorkers() {
  for (auto it = workers_.begin(); it != workers_.end();) {
    if (!it->joinable()) {
      it = workers_.erase(it);
      continue;
    }
    const HANDLE h = static_cast<HANDLE>(it->native_handle());
    if (h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
      it->join();
      it = workers_.erase(it);
      continue;
    }
    ++it;
  }
}

void App::JoinWorkers() {
  ReapFinishedWorkers();
  for (auto& t : workers_) {
    if (!t.joinable()) {
      continue;
    }
    const HANDLE h = static_cast<HANDLE>(t.native_handle());
    const DWORD r = h ? WaitForSingleObject(h, 2000) : WAIT_TIMEOUT;
    if (r == WAIT_OBJECT_0) {
      t.join();
    } else {
      LogLineA("[shutdown] worker join timeout");
      t.detach();
    }
  }
  workers_.clear();
}

void App::Shutdown() {
  // 0) shell notifications before tearing down the message HWND.
  RunShutdownStep("step 0 shell notify", [this]() { StopDesktopShellNotify(); });

  // 1) stop all timers.
  RunShutdownStep("step 1 timers", [this]() {
    if (message_hwnd_) {
      KillTimer(message_hwnd_, kTimerRestore);
      KillTimer(message_hwnd_, kTimerPet);
      KillTimer(message_hwnd_, kTimerWatch);
      KillTimer(message_hwnd_, kTimerBandSettle);
      KillTimer(message_hwnd_, kTimerUpdate);
      KillTimer(message_hwnd_, kTimerOpenSettingsOrg);
      KillTimer(message_hwnd_, kTimerMemTrim);
      KillTimer(message_hwnd_, kTimerDisplayChange);
      KillTimer(message_hwnd_, kTimerIconsRefresh);
      KillTimer(message_hwnd_, kTimerPersistLayouts);
      KillTimer(message_hwnd_, kTimerReflowPublic);
      KillTimer(message_hwnd_, kTimerDesktopShell);
      KillTimer(message_hwnd_, kTimerWarmFences);
      KillTimer(message_hwnd_, kTimerWebViewWarm);
      KillTimer(message_hwnd_, kTimerOverlayRefresh);
      KillTimer(message_hwnd_, kTimerRevealPublic);
      KillTimer(message_hwnd_, kTimerClipboard);
      restore_timer_ = pet_timer_ = watch_timer_ = 0;
    }
  });

  // 2) revoke drop targets.
  RunShutdownStep("step 2 drop targets", [this]() {
    for (auto& fence : fences_) {
      if (fence && fence->Hwnd()) {
        RevokePathDropTarget(fence->Hwnd());
      }
    }
    if (pet_.Hwnd()) {
      RevokePathDropTarget(pet_.Hwnd());
    }
  });

  // 3) unregister hotkeys.
  RunShutdownStep("step 3 hotkeys", [this]() {
    if (message_hwnd_) {
      for (size_t i = 0; i < 32; ++i) {
        UnregisterHotKey(message_hwnd_, static_cast<int>(kHotkeyBase + i));
      }
    }
  });

  // 4) stop hooks.
  RunShutdownStep("step 4 hooks", [this]() {
    RemoveFenceKeyHook();
    StopForegroundHook();
    StopDesktopDblClickMonitor();
    StopNamespaceIconRebuildGuard();
  });

  // 5) stop hotkey relay + join background workers.
  RunShutdownStep("step 5 relay", [this]() {
    hotkey_relay_.Stop();
    JoinWorkers();
  });

  // 6) save state.
  RunShutdownStep("step 6 persist", [this]() {
    PersistLayouts();
    SavePins(pins_);
    SaveRules(rules_);
    SaveAppPrefs(prefs_);
  });

  // 7) restore desktop icons.
  RunShutdownStep("step 7 restore icons", [this]() {
    RestoreDesktopIconsIfHidden();
    icons_visible_intent_ = true;
    if (!AreDesktopIconsVisible()) {
      SetDesktopIconsVisible(true);
    }
    if (!AreDesktopIconsVisible()) {
      SetDesktopIconsVisible(true);
    }
    LogLine(L"[shell] shutdown icons restored visible=%d",
            AreDesktopIconsVisible() ? 1 : 0);
  });

  // 8) reap in-flight recording.
  RunShutdownStep("step 8 record reap", [this]() { ReapScreenRecordOnExit(); });

  // 9) destroy WebView2 windows (settings/todo/search).
  RunShutdownStep("step 9 webview", [this]() {
    settings_.Destroy();
    todos_.Destroy();
    search_.Destroy();
  });

  // 10) destroy fences.
  RunShutdownStep("step 10 fences", [this]() {
    for (auto& fence : fences_) {
      if (fence) {
        fence->Destroy();
      }
    }
    fences_.clear();
  });

  // 11) destroy panels.
  RunShutdownStep("step 11 panels", [this]() {
    page_bar_.Destroy();
    public_host_.Destroy();
    dock_.Destroy();
    pet_.Destroy();
  });

  // 12) destroy pinned images.
  RunShutdownStep("step 12 pins", [this]() {
    if (g_pin_hint_hwnd && IsWindow(g_pin_hint_hwnd)) {
      DestroyWindow(g_pin_hint_hwnd);
      g_pin_hint_hwnd = nullptr;
    }
    for (auto& p : pins_img_) {
      if (p) {
        p->Destroy();
      }
    }
    pins_img_.clear();
  });

  // 13) remove tray icon.
  RunShutdownStep("step 13 tray", [this]() { RemoveTrayIcon(); });

  // 14) remove clipboard listener + destroy message window (WM_DESTROY posts
  // the quit message; the loop has already exited by now).
  RunShutdownStep("step 14 msg hwnd", [this]() {
    if (message_hwnd_) {
      RemoveClipboardFormatListener(message_hwnd_);
      WTSUnRegisterSessionNotification(message_hwnd_);
      DestroyWindow(message_hwnd_);
      message_hwnd_ = nullptr;
    }
  });

  // 15) finalize.
  RunShutdownStep("step 15 done", []() {
    ShutdownIconPreloadWorker();
    InvalidateDefViewCache();
  });
}

void App::StartRegionCapture() {
  capture_.Cancel();
  const bool auto_pin = prefs_.screenshot_auto_pin;
  capture_.Begin(
      instance_,
      [this](const std::wstring& path, bool pin) {
        if (!path.empty()) {
          recent_shots_.insert(recent_shots_.begin(), path);
          if (recent_shots_.size() > 12) {
            recent_shots_.resize(12);
          }
          recent_shot_i_ = 0;
          // Overlay already copied pixels to the clipboard before this cb.
          last_capture_clipboard_seq_ = GetClipboardSequenceNumber();
        }
        if (pin) {
          PinImage(path);
          // Mark so the next F2 does not pin the same shot again.
          RememberPinClipboardSeq();
        }
      },
      auto_pin);
}

void App::RememberPinClipboardSeq() {
  last_pin_clipboard_seq_ = GetClipboardSequenceNumber();
}

bool App::IsPathPinned(const std::wstring& path) const {
  if (path.empty()) {
    return false;
  }
  for (const auto& p : pins_img_) {
    if (p && p->Hwnd() && !p->Path().empty() &&
        _wcsicmp(p->Path().c_str(), path.c_str()) == 0) {
      return true;
    }
  }
  return false;
}

// Drop entries whose window has already closed. A closed PinnedImage keeps its
// object alive in pins_img_ (Destroy only nulls hwnd_), so without pruning the
// list fills with dead entries: IsPathPinned() reports a pin that can no longer
// be shown, and the max_pins cap evicts live pins while dead ones linger.
void App::PruneClosedPins() {
  pins_img_.erase(
      std::remove_if(pins_img_.begin(), pins_img_.end(),
                     [](const std::unique_ptr<PinnedImage>& p) {
                       return !p || !p->Hwnd();
                     }),
      pins_img_.end());
}

static int GetPngEncoderClsid(CLSID* pClsid) {
  UINT num = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&num, &size);
  if (!size) return -1;
  auto* info = static_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
  if (!info) return -1;
  Gdiplus::GetImageEncoders(num, size, info);
  for (UINT i = 0; i < num; ++i) {
    if (wcscmp(info[i].MimeType, L"image/png") == 0) {
      *pClsid = info[i].Clsid;
      free(info);
      return static_cast<int>(i);
    }
  }
  free(info);
  return -1;
}

void App::PinImage(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  PruneClosedPins();
  // Same path already open → just raise it (Snipaste: no duplicate sticker).
  if (IsPathPinned(path)) {
    for (auto& p : pins_img_) {
      if (p && _wcsicmp(p->Path().c_str(), path.c_str()) == 0) {
        p->Show();
        break;
      }
    }
    return;
  }
  int max_pins = prefs_.screenshot_max_pins;
  if (max_pins < 1) {
    max_pins = 1;
  }
  if (max_pins > 8) {
    max_pins = 8;
  }
  while (static_cast<int>(pins_img_.size()) >= max_pins) {
    if (pins_img_.front()) {
      pins_img_.front()->Destroy();
    }
    pins_img_.erase(pins_img_.begin());
  }
  auto img = std::make_unique<PinnedImage>();
  if (!img->Create(instance_, path)) {
    return;
  }
  img->Show();
  pins_img_.push_back(std::move(img));
}

bool App::PinClipboardImage() {
  // Unchanged clipboard → caller cycles recent_shots_ (multi-pin of different
  // captures). Previously every F2 re-saved CF_BITMAP to a new clip_pin_*.png
  // and stacked identical stickers.
  const DWORD seq = GetClipboardSequenceNumber();
  if (seq != 0 && seq == last_pin_clipboard_seq_) {
    return false;
  }
  if (!OpenClipboard(nullptr)) {
    return false;
  }

  std::wstring file_path;
  HBITMAP clip_bmp = nullptr;

  // 1. 优先检查剪贴板是否复制了图片文件 (CF_HDROP)
  if (IsClipboardFormatAvailable(CF_HDROP)) {
    HDROP hdrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (hdrop) {
      wchar_t buf[MAX_PATH]{};
      const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < count; ++i) {
        if (DragQueryFileW(hdrop, i, buf, MAX_PATH)) {
          const std::wstring p = buf;
          const size_t dot = p.find_last_of(L'.');
          if (dot != std::wstring::npos) {
            std::wstring ext = p.substr(dot);
            for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
            if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
                ext == L".bmp" || ext == L".gif" || ext == L".webp") {
              file_path = p;
              break;
            }
          }
        }
      }
    }
  }

  // 2. 检查剪贴板中直接复制的位图 (CF_BITMAP / CF_DIB)
  if (file_path.empty() && (IsClipboardFormatAvailable(CF_BITMAP) ||
                            IsClipboardFormatAvailable(CF_DIB))) {
    HBITMAP raw = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    if (raw) {
      clip_bmp = static_cast<HBITMAP>(
          CopyImage(raw, IMAGE_BITMAP, 0, 0, LR_COPYRETURNORG));
    }
  }

  CloseClipboard();

  if (!file_path.empty()) {
    if (IsPathPinned(file_path)) {
      RememberPinClipboardSeq();
      return false;
    }
    PinImage(file_path);
    recent_shots_.insert(recent_shots_.begin(), file_path);
    if (recent_shots_.size() > 12) {
      recent_shots_.resize(12);
    }
    RememberPinClipboardSeq();
    return true;
  }

  // Capture already saved a PNG and filled the clipboard — pin that file
  // instead of writing a twin clip_pin_*.png of the same pixels.
  if (seq != 0 && seq == last_capture_clipboard_seq_ &&
      !recent_shots_.empty() && !IsPathPinned(recent_shots_[0])) {
    if (clip_bmp) {
      DeleteObject(clip_bmp);
      clip_bmp = nullptr;
    }
    PinImage(recent_shots_[0]);
    RememberPinClipboardSeq();
    return true;
  }

  if (clip_bmp) {
    const std::wstring dir = JoinPath(NativeDataDir(), L"pins");
    CreateDirectoryW(dir.c_str(), nullptr);
    const ULONGLONG tick = GetTickCount64();
    const std::wstring out_path =
        JoinPath(dir, L"clip_pin_" + std::to_wstring(tick) + L".png");

    static ULONG_PTR s_gdip_token = 0;
    static const bool s_gdip_ok = []() {
      Gdiplus::GdiplusStartupInput input;
      return Gdiplus::GdiplusStartup(&s_gdip_token, &input, nullptr) ==
             Gdiplus::Ok;
    }();
    (void)s_gdip_ok;
    {
      Gdiplus::Bitmap gbmp(clip_bmp, nullptr);
      CLSID clsid{};
      if (GetPngEncoderClsid(&clsid) >= 0) {
        gbmp.Save(out_path.c_str(), &clsid, nullptr);
      }
    }
    DeleteObject(clip_bmp);

    if (GetFileAttributesW(out_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      PinImage(out_path);
      recent_shots_.insert(recent_shots_.begin(), out_path);
      if (recent_shots_.size() > 12) {
        recent_shots_.resize(12);
      }
      RememberPinClipboardSeq();
      return true;
    }
  }

  return false;
}

void App::ShowHotkeys() { ShowHotkeyHelp(settings_.Hwnd()); }

void App::DispatchSettingsCommand(const std::wstring& id) {
  const HWND owner = settings_.Hwnd();
  if (id == L"whitelist_pick") {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"All\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) {
      auto names = LoadWhitelist();
      const std::wstring path = file;
      const size_t slash = path.find_last_of(L"\\/");
      const std::wstring name =
          slash == std::wstring::npos ? path : path.substr(slash + 1);
      if (!name.empty()) {
        names.push_back(name);
        SaveWhitelist(names);
        PushExtensionSettings();
      }
    }
    return;
  }
  if (id.rfind(L"whitelist_add:", 0) == 0) {
    const std::wstring value = id.substr(14);
    if (!value.empty()) {
      auto names = LoadWhitelist();
      names.push_back(value);
      SaveWhitelist(names);
      PushExtensionSettings();
    }
    return;
  }
  if (id.rfind(L"whitelist_remove:", 0) == 0) {
    const int index = _wtoi(id.c_str() + 17);
    auto names = LoadWhitelist();
    if (index >= 0 && index < static_cast<int>(names.size())) {
      names.erase(names.begin() + index);
      SaveWhitelist(names);
      PushExtensionSettings();
    }
    return;
  }
  if (id == L"snap_refresh") {
    PushExtensionSettings();
    return;
  }
  if (id == L"fences_refresh") {
    // Do NOT SyncPageChrome here — reseating chrome on settings open freezes
    // the desktop. One full PushExtensionSettings (names + fence_cards + pages).
    RefreshSettingsList();
    settings_.SetPageNames(page_names_, false);
    settings_.SetCurrentPage(current_page_, false);
    PushExtensionSettings();
    return;
  }
  if (id == L"hotkeys_refresh") {
    PushHotkeyState();
    return;
  }
  if (id.rfind(L"snap_apply:", 0) == 0) {
    const int index = _wtoi(id.c_str() + 11);
    const auto list = ListSnapshots();
    if (index >= 0 && index < static_cast<int>(list.size())) {
      const auto& item = list[static_cast<size_t>(index)];
      const std::wstring ask =
          std::wstring(L"确定恢复快照「") + item.name + L"」？";
      if (MistConfirm(owner, L"恢复快照", ask.c_str(), MistMsgTone::Warn)) {
        AppState state;
        if (LoadSnapshotWithIcons(item.path, &state)) {
          ReloadFencesFromState(state, /*heal_geometry=*/false);
          RestoreSnapshotShellIcons(item.path);
          ShowToast(L"已恢复所选快照");
          PushExtensionSettings();
        } else {
          MistAlert(owner, L"布局快照", L"无法读取该快照。", MistMsgTone::Warn);
        }
      }
    }
    return;
  }
  if (id.rfind(L"snap_delete:", 0) == 0) {
    const int index = _wtoi(id.c_str() + 12);
    const auto list = ListSnapshots();
    if (index >= 0 && index < static_cast<int>(list.size())) {
      const auto& item = list[static_cast<size_t>(index)];
      const std::wstring ask =
          std::wstring(L"确定删除快照「") + item.name + L"」？";
      if (MistConfirm(owner, L"删除快照", ask.c_str(), MistMsgTone::Danger)) {
        if (DeleteSnapshot(item.path) ||
            GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
          ShowToast(L"已删除快照");
          PushExtensionSettings();
        } else {
          MistAlert(owner, L"布局快照", L"删除失败。", MistMsgTone::Warn);
        }
      }
    }
    return;
  }
  if (id.rfind(L"goto_page:", 0) == 0) {
    const int page = _wtoi(id.c_str() + 10);
    SetPage(page);
    settings_.SetCurrentPage(current_page_);
    return;
  }
  if (id.rfind(L"toggle_page_lock:", 0) == 0) {
    TogglePageLock(_wtoi(id.c_str() + 17));
    PushExtensionSettings();
    return;
  }
  if (id.rfind(L"fence_toggle_lock:", 0) == 0) {
    const int index = _wtoi(id.c_str() + 18);
    if (index >= 0 && index < static_cast<int>(fences_.size()) &&
        fences_[static_cast<size_t>(index)]) {
      FenceWindow* f = fences_[static_cast<size_t>(index)].get();
      f->SetPositionLocked(!f->IsPositionLocked());
      PersistLayouts();
      PushExtensionSettings();
      ShowToast(f->IsPositionLocked() ? L"已锁定分区位置" : L"已解锁分区位置");
    }
    return;
  }
  if (id.rfind(L"fence_toggle_content_lock:", 0) == 0) {
    const int index = _wtoi(id.c_str() + 26);
    if (index >= 0 && index < static_cast<int>(fences_.size()) &&
        fences_[static_cast<size_t>(index)]) {
      FenceWindow* f = fences_[static_cast<size_t>(index)].get();
      f->SetContentLocked(!f->IsContentLocked());
      PersistLayouts();
      PushExtensionSettings();
      ShowToast(f->IsContentLocked() ? L"已锁定分区内容" : L"已解锁分区内容");
    }
    return;
  }
  if (id.rfind(L"fence_toggle_visible:", 0) == 0) {
    const int index = _wtoi(id.c_str() + 21);
    if (index >= 0 && index < static_cast<int>(fences_.size()) &&
        fences_[static_cast<size_t>(index)]) {
      FenceWindow* f = fences_[static_cast<size_t>(index)].get();
      const bool now_vis = f->VisibleOnPage(f->Page());
      if (now_vis) {
        f->Hide();
        ShowToast(L"已隐藏分区");
      } else {
        // Restore persist flag; ApplyPageVisibility seats HWND only on
        // the current page (other pages stay soft-parked until switch).
        // A tab-group member must not be un-hidden on its own: it shares one
        // seat with its siblings, so showing it would stack two panels.
        if (!f->TabGroup().empty()) {
          ActivateFenceTab(f->Id());
          ShowToast(L"已切换到该标签");
          PushExtensionSettings();
          return;
        }
        f->Show();
        PersistLayouts();
        ApplyPageVisibility();
        ShowToast(L"已显示分区");
      }
      PushExtensionSettings();
    }
    return;
  }
  if (id.rfind(L"rename_fence:", 0) == 0) {
    const int index = _wtoi(id.c_str() + 13);
    if (index >= 0 && index < static_cast<int>(fences_.size()) &&
        fences_[static_cast<size_t>(index)]) {
      // Prefer on-fence inline title EDIT (Fences); Mist dialog if hidden.
      FenceWindow* f = fences_[static_cast<size_t>(index)].get();
      if (f->Hwnd() && IsWindowVisible(f->Hwnd())) {
        f->BeginTitleInlineRename();
      } else {
        const auto name =
            PromptText(settings_.Hwnd(), Tr("rename_fence").c_str(),
                       Tr("rename_fence_label").c_str(), f->Title());
        if (!name.empty()) {
          f->SetTitle(name);
          PersistLayouts();
          RefreshSettingsList();
          PushExtensionSettings();
        }
      }
    }
    return;
  }
  // edit_fence:INDEX|kinds|title  — kinds = "icon,file" / "icon" / "file" / ""
  if (id.rfind(L"edit_fence:", 0) == 0) {
    const std::wstring rest = id.substr(11);
    const size_t p1 = rest.find(L'|');
    if (p1 == std::wstring::npos) {
      return;
    }
    const int index = _wtoi(rest.substr(0, p1).c_str());
    const size_t p2 = rest.find(L'|', p1 + 1);
    const std::wstring kinds_w =
        p2 == std::wstring::npos ? rest.substr(p1 + 1)
                                 : rest.substr(p1 + 1, p2 - p1 - 1);
    const std::wstring title =
        p2 == std::wstring::npos ? L"" : rest.substr(p2 + 1);
    if (index < 0 || index >= static_cast<int>(fences_.size()) ||
        !fences_[static_cast<size_t>(index)]) {
      return;
    }
    FenceWindow* f = fences_[static_cast<size_t>(index)].get();
    if (!title.empty()) {
      f->SetTitle(title);
    }
    std::vector<std::string> kinds;
    size_t start = 0;
    const std::string kinds_utf8 = NarrowUtf8(kinds_w);
    while (start < kinds_utf8.size()) {
      size_t comma = kinds_utf8.find(',', start);
      if (comma == std::string::npos) {
        comma = kinds_utf8.size();
      }
      std::string part = kinds_utf8.substr(start, comma - start);
      while (!part.empty() && (part.front() == ' ' || part.front() == '\t')) {
        part.erase(part.begin());
      }
      while (!part.empty() && (part.back() == ' ' || part.back() == '\t')) {
        part.pop_back();
      }
      if (part == "icon" || part == "file") {
        if (std::find(kinds.begin(), kinds.end(), part) == kinds.end()) {
          kinds.push_back(part);
        }
      }
      start = comma + 1;
    }
    // Stable order: icon then file.
    std::vector<std::string> ordered;
    for (const char* k : {"icon", "file"}) {
      if (std::find(kinds.begin(), kinds.end(), k) != kinds.end()) {
        ordered.push_back(k);
      }
    }
    f->SetOrganizeKinds(std::move(ordered));
    PersistLayouts();
    RefreshSettingsList();
    PushExtensionSettings();
    return;
  }
  if (id == L"pet_show") {
    SetPetVisible(true);
  } else if (id == L"todos") {
    OpenTodos();
  } else if (id == L"search") {
    OpenSearch();
  } else if (id == L"shot") {
    StartRegionCapture();
  } else if (id == L"shot_full") {
    CaptureRegionToPng(owner);
  } else if (id == L"minutes_browse") {
    ConfigureMinutesFolder(owner);
    PushExtensionSettings();
  } else if (id == L"minutes_reset") {
    ResetMinutesFolder();
    PushExtensionSettings();
  } else if (id == L"minutes_open") {
    OpenMinutesFolder(owner);
  } else if (id == L"notepad_browse") {
    const std::wstring path =
        BrowseFolderDialog(owner, L"选择 DeskNote 默认文件夹");
    if (!path.empty()) {
      prefs_.notepad_folder = path;
      SaveAppPrefs(prefs_);
      PushExtensionSettings();
    }
  } else if (id == L"wallpaper_browse") {
    BrowseWallpaperLibrary(owner);
    PushExtensionSettings();
  } else if (id == L"wallpaper_random") {
    RandomWallpaperFromLibrary(owner);
  } else if (id == L"record_browse") {
    BrowseRecordOutputDir(owner);
    prefs_ = LoadAppPrefs();
    PushExtensionSettings();
  } else if (id == L"record_reset") {
    ResetRecordOutputDir();
    prefs_ = LoadAppPrefs();
    PushExtensionSettings();
  } else if (id == L"record_open") {
    const std::wstring dir = ResolveRecordOutputDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    ShellExecuteW(owner, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  } else if (id == L"record_start") {
    if (IsScreenRecording() && IsScreenRecordPaused()) {
      PauseScreenRecord(false);
    } else {
      StartScreenRecord(owner);
    }
  } else if (id == L"updates") {
    OpenUpdatePage(owner);
  } else if (id == L"peek") {
    TogglePeek();
  } else if (id == L"toggle_fences") {
    ToggleFencesVisible();
  } else if (id == L"refresh") {
    rules_ = LoadRules();
    RefreshDesktopIcons();
    PushOrganizePage(&settings_, pins_, rules_, fences_, current_page_);
  } else if (id == L"restore_chrome") {
    RestoreDesktopChrome();
  } else if (id == L"heal") {
    HealLayoutsNow(true);
  } else if (id == L"language") {
    ApplyUiLanguage();
  } else if (id == L"calculator") {
    OpenCalculator();
  } else if (id == L"cleanup") {
    RunNativeCleanup(owner);
  } else if (id == L"copy_data") {
    if (ClipboardSetText(owner, NativeDataDir())) {
      ShowToast(L"数据目录路径已复制");
    }
  } else if (id == L"add_page") {
    AddPage();
  } else if (id == L"remove_page") {
    RemoveCurrentPage();
  } else if (id == L"rename_page") {
    RenameCurrentPage();
  } else if (id == L"folders") {
    EditPageFoldersDialog(owner);
    SyncPageChrome();
    PushExtensionSettings();
  } else if (id.rfind(L"folder_remove:", 0) == 0) {
    const int index = _wtoi(id.c_str() + 14);
    auto items = LoadPageFolders();
    if (index >= 0 && index < static_cast<int>(items.size())) {
      items.erase(items.begin() + index);
      SavePageFolders(items);
      SyncPageChrome();
      PushExtensionSettings();
    }
  } else if (id == L"export_prefs") {
    ExportPrefs();
  } else if (id == L"import_prefs") {
    ImportPrefs();


  }
}

void App::HandleCliAction(const std::wstring& action) {
  if (action == L"--settings" || action == L"--options") {
    OpenSettings();
  } else if (action == L"--refresh-fences") {
    RefreshDesktopIcons();
    HealLayoutsNow(false);
    NudgePetClearOfFences(pet_, fences_, current_page_);
    RestoreDesktopChrome();
    ShowToast(L"已刷新分区");
  } else if (action == L"--refresh") {
    RefreshDesktopIcons();
  } else if (action == L"--organize") {
    OrganizeVirtual(true);
  } else if (action == L"--notes") {
    OpenNotes();
  } else if (action.rfind(L"--notes ", 0) == 0) {
    // N11: open a specific file in the (existing or new) notes window and
    // bring it to the foreground maximized.
    const std::wstring path = action.substr(8);
    if (!path.empty()) {
      OpenPathsInNotes({path});
    } else {
      OpenNotes();
    }
  } else if (action == L"--search") {
    OpenSearch();
  } else if (action == L"--shot") {
    StartRegionCapture();
  } else if (action == L"--todos") {
    OpenTodos();
  } else if (action == L"--peek") {
    TogglePeek();
  } else if (action == L"--new-fence" ||
             action.rfind(L"--new-fence:", 0) == 0) {
    POINT pt{};
    // Optional ":x:y" coordinate payload (shell verb parity); else last
    // desktop-background right-click.
    const size_t sep = action.find(L':');
    if (sep != std::wstring::npos) {
      pt.x = _wtoi(action.c_str() + sep + 1);
      const size_t sep2 = action.find(L':', sep + 1);
      if (sep2 != std::wstring::npos) {
        pt.y = _wtoi(action.c_str() + sep2 + 1);
      }
      RECT rc{pt.x, pt.y, pt.x + 280, pt.y + 360};
      AddFenceAtRect(rc);
    } else if (GetLastDesktopRightClick(&pt)) {
      RECT rc{pt.x, pt.y, pt.x + 280, pt.y + 360};
      AddFenceAtRect(rc);
    } else {
      AddFence();
    }
  } else if (action == L"--show-window") {
    // Show/activate fences + main chrome without opening settings.
    RestoreDesktopChrome();
    OpenSettings();
  } else if (action == L"--quit-silent") {
    PostQuitMessage(0);
  } else if (action == L"--quit") {
    RequestQuit();
  } else if (action == L"--focus") {
    // Second-launch focus: restore chrome + foreground main window without
    // aggressively stealing focus (no flash).
    RestoreDesktopChrome();
    if (settings_.Hwnd() && IsWindow(settings_.Hwnd())) {
      settings_.Show();
      SetForegroundWindow(settings_.Hwnd());
    }
  }
}

void App::OpenTodos() { todos_.Show(); }
void App::OpenSearch() {
  SearchWindow::Options opt;
  opt.desktop = prefs_.search_desktop;
  opt.documents = prefs_.search_documents;
  opt.downloads = prefs_.search_downloads;
  opt.all_drives = prefs_.search_all_drives;
  opt.max_results = prefs_.search_max_results;
  opt.max_depth = prefs_.search_max_depth;
  opt.include_open_folders = prefs_.search_include_open_folders;
  opt.include_hidden = prefs_.search_include_hidden;
  opt.threads = prefs_.search_threads;
  search_.SetOptions(opt);
  search_.Show();
}
void App::TogglePeek() {
  peek_active_ = !peek_active_;
  if (peek_active_) {
    // Peek: raise current-page fences, public floats, and the dock to topmost
    // (Fences: whole organized layer above apps; loose icons stay clickable).
    auto raise_topmost = [](HWND hwnd) {
      if (!hwnd || !IsWindow(hwnd)) {
        return;
      }
      DetachOverlayFromDesktop(hwnd);
      SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    };
    for (auto& fence : fences_) {
      if (!fence || !fence->Hwnd() ||
          !fence->VisibleOnPage(current_page_)) {
        continue;
      }
      // Tabbed fences: only the active member occupies the group's seat, so
      // raising every member would stack hidden tabs on top of each other.
      if (!fence->TabGroup().empty() && !fence->TabActive()) {
        continue;
      }
      raise_topmost(fence->Hwnd());
      fence->RefreshLayer();
    }
    if (public_visible_ && public_host_.Hwnd() && public_host_.HasItems()) {
      raise_topmost(public_host_.Hwnd());
      public_host_.RefreshLayer();
    }
    if (dock_visible_) {
      raise_topmost(dock_.Hwnd());
    }
  } else {
    // Restore the desktop band (re-attach + page visibility).
    RestoreDesktopChrome();
  }
}

void App::EnsureFencesVisible() {
  prefs_.show_fences = true;
  prefs_.double_click_hide = false;
  const bool was_hidden = fences_hidden_;
  fences_hidden_ = false;
  if (was_hidden) {
    SaveAppPrefs(prefs_);
    RefreshDesktopIcons();
    ApplyPageVisibility();
    PushExtensionSettings();
  }
}

void App::ToggleFencesVisible() {
  // Kept for call sites / tray legacy ids; product no longer hides fences.
  EnsureFencesVisible();
}

void App::SaveLayoutSnapshot() {
  AppState state;
  state.current_page = current_page_;
  state.page_names = page_names_;
  // Page locks are part of "the layout" — without this the snapshot restored
  // every user-locked page as unlocked.
  state.page_locked = page_locked_;
  state.dock_visible = dock_visible_;
  state.pet_visible = pet_visible_;
  for (const auto& f : fences_) {
    if (f) {
      state.fences.push_back(f->CurrentLayout());
    }
  }
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf_s(name, L"snap_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  if (SaveSnapshotWithIcons(state, name)) {
    ShowToast(std::wstring(L"已保存快照：") + name);
    PushExtensionSettings();
  }
}

void App::RestoreLayoutSnapshot() {
  AppState state;
  std::wstring applied_path;
  if (!ManageSnapshotsDialog(settings_.Hwnd(), &state, &applied_path)) {
    return;
  }
  // A snapshot carries its own Dock visibility — apply it, don't let the live
  // preference silently win (that made "restore layout" not restore).
  ReloadFencesFromState(state, /*heal_geometry=*/false,
                        /*apply_dock_visibility=*/true);
  if (!applied_path.empty()) {
    RestoreSnapshotShellIcons(applied_path);
  }
  ShowToast(L"已恢复所选快照");
}

void App::ReloadFencesFromState(const AppState& state, bool heal_geometry,
                                bool apply_dock_visibility) {
  for (auto& f : fences_) {
    if (f) {
      f->Destroy();
    }
  }
  fences_.clear();
  current_page_ = state.current_page;
  page_names_ = state.page_names;
  page_locked_ = state.page_locked;
  NormalizePageNames(&page_names_);
  // Layout load / display-change keep the live Dock preference; a snapshot
  // restore applies the Dock state captured in the snapshot. The single
  // dock_.SetVisible() at the end of this function consumes the value.
  if (apply_dock_visibility) {
    dock_visible_ = state.dock_visible;
  } else {
    dock_visible_ = prefs_.dock_visible;
  }
  pet_visible_ = state.pet_visible;
  // Snapshot / layout file may have rewritten pins.json — reload or the UI
  // keeps the pre-apply pin map and looks unchanged. Same for float seats.
  pins_ = LoadPins();
  float_placements_ = LoadFloatPlacements();
  AppState healed = state;
  if (heal_geometry) {
    HealFenceLayouts(&healed.fences);
  }
  for (const auto& layout : healed.fences) {
    SpawnFence(layout);
  }
  PersistLayouts();
  SyncPageChrome();
  RefreshDesktopIcons();
  RefreshSettingsList();
  SyncFenceTabGroups();
  ApplyPageVisibility();
  dock_.SetVisible(dock_visible_);
  pet_.SetVisible(pet_visible_);
  NudgePetClearOfFences(pet_, fences_, current_page_);
}

void App::HealLayoutsNow(bool toast) {
  std::vector<FenceLayout> layouts;
  layouts.reserve(fences_.size());
  for (auto& f : fences_) {
    if (f) {
      layouts.push_back(f->CurrentLayout());
    }
  }
  // Explicit repair only: proportion restore + off-screen clamp.
  int n = RestoreSystemFenceProportions(&layouts);
  n += HealFenceLayouts(&layouts);
  if (n <= 0) {
    if (toast) {
      ShowToast(L"布局正常，无需修复");
    }
    return;
  }
  for (size_t i = 0; i < fences_.size() && i < layouts.size(); ++i) {
    if (!fences_[i]) {
      continue;
    }
    const FencePageGeometry geo =
        FenceGeometryForPage(layouts[i], layouts[i].page);
    fences_[i]->SetPageGeometry(fences_[i]->Page(), geo);
    fences_[i]->ApplyPageGeometry(geo, /*present=*/true);
    fences_[i]->AttachToDesktopLayer();
  }
  PersistLayouts();
  if (toast) {
    ShowToast(L"已修复 " + std::to_wstring(n) + L" 个分区位置");
  }
}

void App::OnDisplayTopologyMaybeChanged() {
  if (session_locked_) {
    return;
  }
  // Called from the 2s watch tick. WM_DISPLAYCHANGE converges real changes
  // right away, so only recompute the full fingerprint (monitor enum + per-
  // monitor DC caps) when the throttle interval has elapsed.
  const ULONGLONG now_ms = GetTickCount64();
  if (now_ms - display_check_last_ms_ < kDisplayFingerprintIntervalMs) {
    return;
  }
  display_check_last_ms_ = now_ms;
  const std::string fp = DisplayFingerprint();
  if (FingerprintIsTransient(fp) || fp == display_fp_) {
    return;
  }
  // Python: persist current geometry under the old fingerprint before switch.
  PersistLayouts();
  display_fp_ = fp;
  display_work_ = PrimaryWorkArea();
  AppState state = LoadAppState();
  HealFenceLayouts(&state.fences);
  ReloadFencesFromState(state);
  ShowToast(L"显示器变化：已按配置恢复分区");
}

void App::ConvergeDisplayChange() {
  if (session_locked_) {
    return;
  }
  // Persist already ran under the outgoing fingerprint (display_fp_). Load /
  // heal restore via per-monitor donor ratios — not primary sx/sy.
  PrepareDestinationProfileForDisplayChange(display_work_, PrimaryWorkArea());
  ReloadFencesFromState(LoadAppState());
  display_work_ = PrimaryWorkArea();
  display_fp_ = DisplayFingerprint();
  ShowToast(L"显示器变化：已按配置恢复分区");
}

void App::OnDisplayChangeTimerFired() {
  if (session_locked_) {
    return;
  }
  const std::string fp = DisplayFingerprint();
  if (FingerprintIsTransient(fp) || fp == display_fp_) {
    display_change_stage_ = 0;
    return;
  }
  const int old_count = CountMonitorsInFingerprint(display_fp_);
  const int new_count = static_cast<int>(EnumMonitors().size());
  PersistLayouts();
  display_fp_ = fp;
  ConvergeDisplayChange();
  LogLine(L"[display] converge display change finished (mon %d->%d, stage %d)",
          old_count, new_count, display_change_stage_);
  if (new_count < old_count && display_change_stage_ == 1) {
    // Removal: two-stage converge (second converge 1000ms later) so taskbar settles
    display_change_stage_ = 2;
    display_change_timer_ =
        SetTimer(message_hwnd_, kTimerDisplayChange, 1000, nullptr);
  } else {
    display_change_stage_ = 0;
  }
}

bool App::PageLockedForFences(int page) const {
  // User-toggled lock only. Pages 0/1 are locked for PAGE delete (IsPageLocked)
  // but still allow fence create (page_bar / Python P0-11).
  if (page < 2 || page >= static_cast<int>(page_locked_.size())) {
    return false;
  }
  return page_locked_[static_cast<size_t>(page)];
}

void App::TogglePageLock(int page) {
  // System pages 0/1 cannot be unlocked (persisted as always locked).
  if (page < 2 || page >= static_cast<int>(page_names_.size())) {
    return;
  }
  if (static_cast<int>(page_locked_.size()) <= page) {
    page_locked_.resize(static_cast<size_t>(page) + 1, false);
  }
  page_locked_[static_cast<size_t>(page)] = !page_locked_[static_cast<size_t>(page)];
  PersistLayouts();
  SyncPageChrome();
  ShowToast(page_locked_[static_cast<size_t>(page)] ? L"已锁定页面"
                                                    : L"已解锁页面");
}

void App::OpenMinutes() {
  const MistMsgResult choice = MistMessage(
      settings_.Hwnd(), L"会议纪要",
      L"会议纪要：\n\n"
      L"是 = 新建 Markdown\n"
      L"否 = 新建 Word (.docx)\n"
      L"取消 = 更多…",
      MistMsgKind::YesNoCancel, MistMsgTone::Info);
  if (choice == MistMsgResult::Yes) {
    CreateAndOpenMinutes(settings_.Hwnd());
    return;
  }
  if (choice == MistMsgResult::No) {
    CreateAndOpenMinutesDocx(settings_.Hwnd());
    return;
  }
  const MistMsgResult more = MistMessage(
      settings_.Hwnd(), L"会议纪要",
      L"更多：\n\n是 = 打开最近纪要\n否 = 打开文件夹\n取消 = 设置文件夹",
      MistMsgKind::YesNoCancel, MistMsgTone::Info);
  if (more == MistMsgResult::Yes) {
    OpenRecentMinutes(settings_.Hwnd());
  } else if (more == MistMsgResult::No) {
    OpenMinutesFolder(settings_.Hwnd());
  } else if (more == MistMsgResult::Cancel) {
    ConfigureMinutesFolder(settings_.Hwnd());
  }
}

void App::RebuildPublicItems(const std::vector<DesktopItem>* pre_scanned) {
  // Floats = desktop items NOT pinned to any fence (Fences-class loose icons).
  // enable_public_desktop ON  → shared across pages unless placement.page_local.
  // enable_public_desktop OFF → always page-local (placement.page == current).
  // 「移动到分页」sets page_local so the icon leaves other pages.
  // Own when we scan here — SaveFloat / SetItems must not leave a Ref dangling
  // if anything refreshes the 2.5s scan cache underneath.
  std::vector<DesktopItem> owned;
  const std::vector<DesktopItem>* items_ptr = pre_scanned;
  if (!items_ptr) {
    owned = ScanDesktopItems();
    items_ptr = &owned;
  }
  const std::vector<DesktopItem>& items = *items_ptr;
  bool placements_dirty = false;
  const bool shared = prefs_.enable_public_desktop;
  // Heal ghosts: OLE post-drag used to write float rows back after SetPin.
  // float map keys are NormalizePinKey'd; path-empty rows use the key itself.
  for (auto it = float_placements_.begin(); it != float_placements_.end();) {
    bool pinned = false;
    if (it->second.path.empty()) {
      const auto pit = pins_.find(it->first);
      pinned = pit != pins_.end() && !pit->second.empty();
    } else {
      const auto* pin = FindPin(pins_, it->second.path);
      pinned = pin && !pin->empty();
    }
    if (pinned) {
      it = float_placements_.erase(it);
      placements_dirty = true;
    } else {
      ++it;
    }
  }
  // Decorate once so sort does not re-NormalizePinKey on every comparison.
  struct PubEntry {
    DesktopItem item;
    bool has_pos = false;
    double rx = 0;
    double ry = 0;
  };
  std::vector<PubEntry> entries;
  entries.reserve(items.size());
  for (const auto& it : items) {
    const std::wstring key = NormalizePinKey(it.path);
    auto pit = pins_.find(key);
    // Match RefreshDesktopIcons: empty fence id is not a live pin.
    if (pit != pins_.end() && !pit->second.empty()) {
      continue;
    }
    auto fit = float_placements_.find(key);
    if (fit == float_placements_.end()) {
      FloatPlacement fp;
      fp.path = it.path;
      fp.page = current_page_;
      fp.has_pos = false;
      fp.page_local = false;  // new loose icons stay shared when public on
      float_placements_[key] = fp;
      placements_dirty = true;
      entries.push_back({it, false, 0, 0});
      continue;
    }
    const bool page_bound = !shared || fit->second.page_local;
    if (page_bound && fit->second.page != current_page_) {
      continue;
    }
    entries.push_back(
        {it, fit->second.has_pos, fit->second.rx, fit->second.ry});
  }
  if (placements_dirty) {
    SaveFloatPlacements(float_placements_);
  }
  // Prefer saved positions (top-left first) then name for unplaced items.
  std::sort(entries.begin(), entries.end(),
            [](const PubEntry& a, const PubEntry& b) {
              if (a.has_pos != b.has_pos) {
                return a.has_pos && !b.has_pos;
              }
              if (a.has_pos && b.has_pos) {
                if (a.ry != b.ry) {
                  return a.ry < b.ry;
                }
                if (a.rx != b.rx) {
                  return a.rx < b.rx;
                }
              }
              return _wcsicmp(a.item.name.c_str(), b.item.name.c_str()) < 0;
            });
  std::vector<DesktopItem> pub;
  pub.reserve(entries.size());
  for (auto& e : entries) {
    pub.push_back(std::move(e.item));
  }
  std::vector<RECT> blocked;
  blocked.reserve(fences_.size() + 4);
  constexpr int kFencePad = 12;
  auto push_hwnd_block = [&](HWND hwnd, int pad) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
      return;
    }
    RECT r{};
    if (!GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) {
      return;
    }
    if (pad) {
      InflateRect(&r, pad, pad);
    }
    blocked.push_back(r);
  };
  if (!fences_hidden_) {
    for (auto& f : fences_) {
      if (!f || !f->VisibleOnPage(current_page_)) {
        continue;
      }
      RECT r{};
      bool got = false;
      if (f->Hwnd() && IsWindow(f->Hwnd()) && GetWindowRect(f->Hwnd(), &r) &&
          r.right > r.left && r.bottom > r.top) {
        got = true;
      }
      if (!got) {
        const FencePageGeometry geo = f->GeometryForPage(current_page_);
        if (geo.w > 0 && geo.h > 0) {
          r = {geo.x, geo.y, geo.x + geo.w, geo.y + geo.h};
          got = true;
        }
      }
      if (!got) {
        continue;
      }
      InflateRect(&r, kFencePad, kFencePad);
      blocked.push_back(r);
    }
  }
  // AIGC START
  // Work-area float host must not swallow page bar / dock / pet.
  if (page_bar_.Hwnd() && IsWindowVisible(page_bar_.Hwnd())) {
    push_hwnd_block(page_bar_.Hwnd(), 4);
  }
  if (pet_visible_ && pet_.Hwnd()) {
    push_hwnd_block(pet_.Hwnd(), 4);
    if (pet_.PagesHwnd()) {
      push_hwnd_block(pet_.PagesHwnd(), 4);
    }
  }
  if (dock_visible_) {
    push_hwnd_block(dock_.Hwnd(), 4);
  }
  // AIGC END
  public_host_.SetCurrentPage(current_page_);
  public_host_.SetPlacements(float_placements_);
  public_host_.SetBlockedRects(std::move(blocked));
  const bool no_floats = pub.empty();
  public_host_.SetItems(std::move(pub));
  if (no_floats) {
    public_host_.Hide();
  } else if (public_visible_) {
    public_host_.Show();
  }
}

void App::PushHotkeyState() {
  std::vector<SettingsWindow::HotkeyRow> rows;
  rows.reserve(hotkeys_.size());
  for (const auto& k : hotkeys_) {
    SettingsWindow::HotkeyRow row;
    row.id = WidenUtf8(k.id);
    const std::string tr_key = std::string("hk_") + k.id;
    const std::wstring tr = Tr(tr_key.c_str());
    row.label = (!tr.empty() && tr != WidenUtf8(tr_key)) ? tr : k.label;
    row.chord = FormatHotkey(k.modifiers, k.vk);
    rows.push_back(std::move(row));
  }
  settings_.SetHotkeyRows(rows);
}

void App::ApplyUiLanguage() {
  ToggleLanguage();
  const char* lang = CurrentLang();
  ShowToast(std::strcmp(lang, "en") == 0 ? L"Language: English"
                                         : L"语言：中文");
  PushHotkeyState();
  PushExtensionSettings();
}

void App::PushExtensionSettings() {
  SettingsWindow::ExtState ext;
  // Keep the pair aligned — settings UI exposes a single「启用会议纪要」toggle.
  ext.minutes_enabled = prefs_.minutes_enabled;
  ext.minutes_on_bar = prefs_.minutes_enabled;
  ext.notepad_enabled = prefs_.notepad_enabled;
  ext.elevate_hotkey_helper = prefs_.elevate_hotkey_helper;
  ext.record_enabled = prefs_.record_enabled;
  ext.record_on_bar = prefs_.record_enabled && prefs_.record_on_bar;
  ext.calculator_enabled = prefs_.calculator_enabled;
  ext.calculator_on_bar =
      prefs_.calculator_enabled && prefs_.calculator_on_bar;
  ext.todos_enabled = prefs_.todos_enabled;
  ext.todos_on_bar = prefs_.todos_enabled && prefs_.todos_on_bar;
  ext.screenshot_auto_pin = prefs_.screenshot_auto_pin;
  ext.screenshot_max_pins = prefs_.screenshot_max_pins;
  ext.record_quality = prefs_.record_quality;
  ext.minutes_folder = MinutesFolderOverride();
  ext.notepad_folder = prefs_.notepad_folder;
  ext.wallpaper_folder = WallpaperLibraryFolder();
  ext.record_dir = prefs_.record_output_dir;
  const auto audio_mics = ListAudioCaptureDevices();
  std::wstring norm_mic = NormalizeRecordMic(prefs_.record_mic);
  if (norm_mic.empty() && !audio_mics.empty()) {
    norm_mic = audio_mics.front();
    prefs_.record_mic = norm_mic;
    SaveAppPrefs(prefs_);
  }
  ext.record_mic = norm_mic;
  ext.record_mics = audio_mics;
  // Always show install-dir defaults in placeholders (not a corrupt override).
  ext.minutes_folder_default = JoinPath(ExeDir(), L"纪要");
  ext.notepad_folder_default = JoinPath(ExeDir(), L"笔记");
  ext.record_dir_default = JoinPath(ExeDir(), L"录屏");
  ext.page_folders.clear();
  for (const auto& f : LoadPageFolders()) {
    SettingsWindow::ExtState::PageFolder row;
    row.name = f.name;
    row.path = f.path;
    ext.page_folders.push_back(std::move(row));
  }
  ext.whitelist = LoadWhitelist();
  ext.search_desktop = prefs_.search_desktop;
  ext.search_documents = prefs_.search_documents;
  ext.search_downloads = prefs_.search_downloads;
  ext.search_all_drives = prefs_.search_all_drives;
  ext.search_max_results = prefs_.search_max_results;
  ext.search_max_depth = prefs_.search_max_depth;
  ext.pet_auto_wander = pet_.AutoWander();
  ext.pet_show_bubbles = pet_.ShowPageBubbles();
  ext.pet_size_scale = pet_.SizeScale();
  ext.pet_character = pet_.Character();
  ext.pet_characters = pet_.Characters();
  ext.pet_actions.clear();
  for (const auto& cid : pet_.Characters()) {
    std::vector<std::wstring> acts;
    for (const auto& a : pet_.EnabledActions(cid)) {
      acts.push_back(WidenUtf8(a));
    }
    ext.pet_actions[cid] = std::move(acts);
  }
  ext.dock_edge = prefs_.dock_edge;
  ext.dock_opacity = prefs_.dock_opacity;
  ext.dock_items = prefs_.dock_items;
  ext.exclude_patterns = prefs_.exclude_patterns.empty()
                             ? std::vector<std::wstring>{L"desktop.ini",
                                                         L"DeskTidy"}
                             : prefs_.exclude_patterns;
  ext.check_update_on_startup = prefs_.check_update_on_startup;
  ext.desktop_right_click_menu = prefs_.desktop_right_click_menu;
  ext.close_to_tray = prefs_.close_to_tray && prefs_.close_behavior_prompted;
  ext.show_fences = prefs_.show_fences;
  ext.double_click_hide = prefs_.double_click_hide;
  ext.enable_public_desktop = prefs_.enable_public_desktop;
  ext.show_page_indicator = prefs_.show_page_indicator;
  ext.page_chrome_style = prefs_.page_chrome_style == "tabs" ? "tabs" : "float";
  ext.lang = CurrentLang() ? CurrentLang() : "zh";
  ext.app_version = NativeDisplayVersion();
  ext.theme_id = prefs_.theme_id;
  ext.page_bar_style =
      prefs_.page_bar_style.empty() ? "navy" : prefs_.page_bar_style;
  ext.ui_theme = NormalizeUiTheme(prefs_.ui_theme);
  ext.themes.clear();
  for (const auto* s : AllFenceStyles()) {
    if (!s) {
      continue;
    }
    SettingsWindow::ExtState::ThemeOption opt;
    opt.id = s->id;
    opt.name = s->name;
    ext.themes.push_back(std::move(opt));
  }
  ext.ui_themes.clear();
  for (const auto& t : AllUiThemes()) {
    SettingsWindow::ExtState::ThemeOption opt;
    opt.id = t.id;
    opt.name = t.name_zh;
    ext.ui_themes.push_back(std::move(opt));
  }
  ext.fence_cards.clear();
  for (size_t i = 0; i < fences_.size(); ++i) {
    FenceWindow* f = fences_[i].get();
    if (!f) {
      continue;
    }
    SettingsWindow::ExtState::FenceCard card;
    card.index = static_cast<int>(i);
    card.title = f->Title();
    card.page = f->Page();
    card.pages = f->Pages();
    if (card.pages.empty()) {
      card.pages.push_back(card.page);
    }
    card.style_id = f->StyleId();
    card.organize_kinds = f->OrganizeKinds();
    card.system_fence = f->Id().rfind("system_", 0) == 0;
    card.position_locked = f->IsPositionLocked();
    card.content_locked = f->IsContentLocked();
    card.label_lines = f->LabelLines();
    card.show_labels = f->ShowLabels();
    card.cell_scale = f->CellScale();
    card.sort_by = f->SortBy();
    card.sort_desc = f->SortDescending();
    card.radius = f->RadiusOverride();
    card.show_title = f->ShowTitle();
    card.fade_on_idle = f->FadeOnIdle();
    card.visible = f->VisibleOnPage(f->Page());
    card.has_custom_colors = f->HasCustomColors();
    if (card.has_custom_colors) {
      auto hex = [](COLORREF c) {
        wchar_t buf[8] = {};
        swprintf_s(buf, L"#%02X%02X%02X", GetRValue(c), GetGValue(c),
                   GetBValue(c));
        return std::wstring(buf);
      };
      card.custom_bg = hex(f->CustomBg());
      card.custom_accent = hex(f->CustomAccent());
    }
    int alpha = f->Opacity();
    if (alpha < 0) {
      const FenceStylePreset* st = FenceStyleById(f->StyleId());
      alpha = st ? static_cast<int>(st->alpha) : 180;
    }
    card.opacity_pct = (alpha * 100 + 127) / 255;
    if (card.opacity_pct < 35) {
      card.opacity_pct = 35;
    }
    if (card.opacity_pct > 100) {
      card.opacity_pct = 100;
    }
    ext.fence_cards.push_back(std::move(card));
  }
  ext.page_locked = page_locked_;
  if (ext.page_locked.size() < page_names_.size()) {
    ext.page_locked.resize(page_names_.size(), false);
  }
  ext.snapshots.clear();
  const auto snapshots = ListSnapshots();
  // Each thumbnail is an icon extract + GDI+ PNG encode + base64 on this (UI)
  // thread. Rendering full cards for every snapshot while the settings window
  // opens used to stall for seconds. Eagerly render only the most recent
  // snapshots at full fidelity; older ones get geometry + meta with
  // placeholders. Rebuilds reuse the mtime-keyed card cache in snapshot.cpp,
  // so re-opening the window does not re-encode anything unchanged.
  constexpr size_t kFullCards = 6;   // newest N: full thumb budget
  constexpr size_t kEagerCards = 8;  // next M: thinned thumb budget
  std::vector<std::pair<ULONGLONG, size_t>> order;
  order.reserve(snapshots.size());
  for (size_t i = 0; i < snapshots.size(); ++i) {
    order.emplace_back(SnapshotMtimeQuad(snapshots[i].path), i);
  }
  std::stable_sort(order.begin(), order.end(),
                   [](const auto& a, const auto& b) {
                     return a.first > b.first;  // newest first
                   });
  std::vector<size_t> render_order;
  render_order.reserve(order.size());
  for (const auto& kv : order) {
    render_order.push_back(kv.second);
  }
  const size_t eager = (std::min)(kEagerCards, render_order.size());
  for (size_t n = 0; n < render_order.size(); ++n) {
    const SnapshotInfo& s = snapshots[render_order[n]];
    SnapshotCardOptions opts;
    if (n >= eager) {
      // Older than the eager window: geometry/meta only (placeholders).
      opts.render_thumbs = false;
    } else if (n >= kFullCards) {
      // Thinned budget for the tail so a long list stays responsive.
      opts.max_pin_thumbs = 6;
      opts.max_free_thumbs = 8;
    }
    const SnapshotCardData data = BuildSnapshotCardCached(s, opts);
    SettingsWindow::ExtState::SnapCard card;
    card.name = data.name;
    card.meta = data.meta;
    card.focus_page = data.focus_page;
    card.work_w = data.work_w;
    card.work_h = data.work_h;
    card.page_names = data.page_names;
    card.preview.reserve(data.boxes.size());
    for (const auto& b : data.boxes) {
      SettingsWindow::ExtState::SnapPreviewBox box;
      box.title = b.title;
      box.x = b.x;
      box.y = b.y;
      box.w = b.w;
      box.h = b.h;
      box.page = b.page;
      box.style_id = b.style_id;
      box.collapsed = b.collapsed;
      box.icon_count = b.icon_count;
      box.pin_thumbs = b.pin_thumbs;
      card.preview.push_back(std::move(box));
    }
    card.icons.reserve(data.icons.size());
    for (const auto& ic : data.icons) {
      SettingsWindow::ExtState::SnapPreviewIcon out;
      out.x = ic.x;
      out.y = ic.y;
      out.thumb = ic.thumb;
      card.icons.push_back(std::move(out));
    }
    ext.snapshots.push_back(std::move(card));
  }
  // Keep settings WebView page list in sync with live layout (避免仍显示「第 N 页」).
  NormalizePageNames(&page_names_);
  settings_.SetPageNames(page_names_, false);
  settings_.SetCurrentPage(current_page_, false);
  settings_.SetExtensionState(ext);
  settings_.ApplyUiThemeVars(UiThemeCssVars(ext.ui_theme));
  PushHotkeyState();
}

void App::SetFenceStyleAt(int index, const std::wstring& style_id) {
  if (index < 0 || index >= static_cast<int>(fences_.size()) || !fences_[index]) {
    return;
  }
  const std::string id = NormalizeFenceStyleId(NarrowUtf8(style_id));
  fences_[index]->SetStyleId(id);
  PersistLayouts();
  PushExtensionSettings();
  ShowToast(L"已更新分区样式");
}

void App::SetAllFenceStyle(const std::wstring& style_id) {
  const std::string id = NormalizeFenceStyleId(NarrowUtf8(style_id));
  prefs_.theme_id = id;
  SaveAppPrefs(prefs_);
  for (auto& fence : fences_) {
    if (fence) {
      fence->SetStyleId(id);
    }
  }
  PersistLayouts();
  PushExtensionSettings();
  const auto* opt = FenceStyleById(id);
  if (opt) {
    ShowToast(std::wstring(L"已应用分区样式：") + opt->name);
  } else {
    ShowToast(L"已更新所有分区样式");
  }
}

void App::SetPageBarStyle(const std::wstring& style_id) {
  const std::string id = NormalizeFenceStyleId(NarrowUtf8(style_id));
  prefs_.page_bar_style = id;
  SaveAppPrefs(prefs_);
  const auto* opt = FenceStyleById(id);
  if (opt) {
    page_bar_.ApplyStyle(opt->background, opt->accent, opt->title);
    ShowToast(std::wstring(L"已更新页签颜色：") + opt->name);
  } else {
    ShowToast(L"已更新页签颜色");
  }
  PushExtensionSettings();
}

void App::SetFenceOpacityPctAt(int index, int opacity_pct) {
  if (index < 0 || index >= static_cast<int>(fences_.size()) || !fences_[index]) {
    return;
  }
  if (opacity_pct < 35) {
    opacity_pct = 35;
  }
  if (opacity_pct > 100) {
    opacity_pct = 100;
  }
  const int alpha = (opacity_pct * 255 + 50) / 100;
  fences_[index]->SetOpacity(alpha);
  PersistLayouts();
  PushExtensionSettings();
}

void App::SetFenceColorsAt(int index, bool enabled, const std::wstring& bg,
                           const std::wstring& accent) {
  if (index < 0 || index >= static_cast<int>(fences_.size()) ||
      !fences_[index]) {
    return;
  }
  // Colours arrive as "#rrggbb" from the settings page.
  auto parse = [](const std::wstring& s, COLORREF fallback) -> COLORREF {
    if (s.size() != 7 || s[0] != L'#') {
      return fallback;
    }
    auto hex = [&](size_t at) -> int {
      wchar_t buf[3] = {s[at], s[at + 1], 0};
      wchar_t* end = nullptr;
      const long v = wcstol(buf, &end, 16);
      return (end && *end == 0) ? static_cast<int>(v) : -1;
    };
    const int r = hex(1);
    const int g = hex(3);
    const int b = hex(5);
    if (r < 0 || g < 0 || b < 0) {
      return fallback;
    }
    return RGB(r, g, b);
  };
  const FenceStylePreset* preset = FenceStyleById(fences_[index]->StyleId());
  const COLORREF fb = preset ? preset->background : RGB(24, 58, 112);
  const COLORREF fa = preset ? preset->accent : RGB(56, 189, 248);
  fences_[index]->SetCustomColors(enabled, parse(bg, fb), parse(accent, fa));
  PersistLayouts();
  PushExtensionSettings();
  if (enabled) {
    ShowToast(L"已应用自定义配色");
  }
}

bool App::TryConsumeTrayOpen() {
  const DWORD now = GetTickCount();
  if (last_tray_open_tick_ != 0 &&
      (now - last_tray_open_tick_) < kTrayOpenDebounceMs) {
    return false;
  }
  last_tray_open_tick_ = now;
  return true;
}

void App::OpenSettings() {
  // 保持后台纯净打开：暂停并抑制任何桌面层级重排，确保分区绝对原封不动
  if (message_hwnd_ && band_restore_left_ > 0) {
    KillTimer(message_hwnd_, kTimerBandSettle);
    band_restore_left_ = 0;
  }
  suppress_band_restore_until_ = GetTickCount64() + 2000;
  // Tray click: show the HWND immediately; fill data on the next timer tick.
  settings_.Show();
  if (settings_.Hwnd()) {
    SetWindowPos(settings_.Hwnd(), HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(settings_.Hwnd());
  }
  if (message_hwnd_) {
    KillTimer(message_hwnd_, kTimerOpenSettingsOrg);
  }
}

void App::OnDesktopBlankDoubleClick() {
  // Double-click hide/show desktop icons removed — keep organized chrome.
}

void App::OnDesktopBlankLeftDown(LPARAM screen_pt) {
  // Same as Explorer / Fences: click outside a fence drops icon selection
  // and clears keyboard activation (Ctrl+C… stop targeting that fence).
  ClearFenceKeyTarget();
  // Only current-page visible fences — hidden pages must not PresentLayered.
  for (auto& fence : fences_) {
    if (fence && fence->VisibleOnPage(current_page_)) {
      fence->ClearSelection();
    }
  }
  // AIGC START
  // Organized mode hides SysListView32 — DefView blank clicks never reach the
  // float host. Arm public marquee from the hook (cover expands on drag only).
  if (!icons_visible_intent_ && public_visible_ && public_host_.HasItems()) {
    POINT pt{GET_X_LPARAM(screen_pt), GET_Y_LPARAM(screen_pt)};
    const bool additive = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    public_host_.BeginRubberFromScreen(pt, additive);
  } else if (public_host_.HasSelection()) {
    // Stock desktop / no floats to rubber: still drop float selection like
    // Explorer clears when clicking empty wallpaper.
    public_host_.ClearSelection();
  }
  // Organized: blank wallpaper is the DefView stand-in — keep keyboard on the
  // float host (Ctrl+A / typeahead / Ctrl+Shift+N) like Explorer desktop focus.
  if (!icons_visible_intent_ && public_host_.Hwnd()) {
    SetFenceKeyTarget(public_host_.Hwnd());
  }
  // AIGC END
}

void App::OnDesktopBlankRightClick(LPARAM screen_pt) {
  // Organized wallpaper blank: host Explorer Desktop background menu so Paste
  // / New / Undo seat floats (DefView listview is hidden — Fences parity).
  if (icons_visible_intent_) {
    return;
  }
  // Blank RMB activates the float layer for subsequent chords (Explorer focus).
  if (public_host_.Hwnd()) {
    SetFenceKeyTarget(public_host_.Hwnd());
  }
  POINT screen{GET_X_LPARAM(screen_pt), GET_Y_LPARAM(screen_pt)};
  PWSTR desk_raw = nullptr;
  std::wstring desk;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) &&
      desk_raw) {
    desk = desk_raw;
    CoTaskMemFree(desk_raw);
  }
  if (desk.empty()) {
    return;
  }
  std::vector<std::wstring> desk_before;
  auto list_desk = [&desk]() {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((desk + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
      return out;
    }
    do {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
        continue;
      }
      out.push_back(desk + L"\\" + fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return out;
  };
  ShellBeforeInvokeFn on_before = [&]() { desk_before = list_desk(); };
  ShellInterceptFn on_intercept = [this](const std::wstring& verb) {
    if (verb == L"paste" || verb == L"pasteto") {
      OnDesktopPaste(/*as_link=*/false);
      return true;
    }
    if (verb == L"pastelink") {
      OnDesktopPaste(/*as_link=*/true);
      return true;
    }
    return false;
  };
  ShellVerbFn on_verb = [this, &desk_before, &list_desk](const std::wstring& verb) {
    if (verb == L"paste" || verb == L"pastelink" || verb == L"pasteto") {
      return;
    }
    if (verb == L"undo") {
      RepinFromUndoStack();
    }
    if (!ShellVerbNeedsDesktopRescan(verb)) {
      return;
    }
    if (verb == L"newfolder" || verb.rfind(L"new", 0) == 0) {
      const auto after = list_desk();
      for (const auto& p : after) {
        bool was = false;
        for (const auto& b : desk_before) {
          if (_wcsicmp(b.c_str(), p.c_str()) == 0) {
            was = true;
            break;
          }
        }
        if (!was) {
          SuppressOrganizePath(p, 24.0 * 3600.0);
        }
      }
    }
    InvalidateDesktopScanCache();
    RefreshDesktopIcons();
  };
  const int rc = TrackShellFolderBackgroundMenu(
      message_hwnd_, screen, desk, {}, on_verb, on_before, on_intercept);
  if (rc < 0) {
    // Explorer's background IContextMenu could not be acquired (shell restart,
    // Explorer crash, non-standard shell). Previously this returned silently
    // and the user got no menu at all — no Refresh / New / Paste — which reads
    // as "the system options disappeared". Show a minimal working menu instead.
    LogLine(L"[menu] desktop background menu unavailable; using fallback");
    TrackFallbackDesktopMenu(message_hwnd_, screen, desk);
  }
}

void App::RememberPinUndo(const std::wstring& path,
                          const std::string& fence_id) {
  if (path.empty() || fence_id.empty()) {
    return;
  }
  const std::wstring key = NormalizePinKey(path);
  const ULONGLONG now = GetTickCount64();
  for (auto& e : pin_undo_) {
    if (NormalizePinKey(e.path) == key) {
      e.fence_id = fence_id;
      e.tick = now;
      return;
    }
  }
  pin_undo_.push_back({path, fence_id, now});
  if (pin_undo_.size() > 32) {
    pin_undo_.erase(pin_undo_.begin());
  }
}

void App::RepinFromUndoStack() {
  const ULONGLONG now = GetTickCount64();
  // Drop stale entries (>90s) — Explorer Undo is only meaningful near the op.
  pin_undo_.erase(
      std::remove_if(pin_undo_.begin(), pin_undo_.end(),
                     [now](const PinUndoEntry& e) {
                       return now - e.tick > 90000ULL;
                     }),
      pin_undo_.end());
  if (pin_undo_.empty()) {
    InvalidateDesktopScanCache();
    RefreshDesktopIcons();
    return;
  }
  std::vector<PinUndoEntry> keep;
  bool any = false;
  for (const auto& e : pin_undo_) {
    if (GetFileAttributesW(e.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      keep.push_back(e);
      continue;
    }
    FenceWindow* fence = nullptr;
    for (auto& f : fences_) {
      if (f && f->Id() == e.fence_id) {
        fence = f.get();
        break;
      }
    }
    if (!fence) {
      keep.push_back(e);
      continue;
    }
    // Undo re-adds a member, which a content-locked fence must refuse. Keep
    // the entry so it still applies once the user unlocks.
    if (fence->IsContentLocked()) {
      keep.push_back(e);
      continue;
    }
    SuppressOrganizePath(e.path, 24.0 * 3600.0);
    SetPin(&pins_, e.path, e.fence_id);
    fence->SetSortBy("manual");
    fence->InsertPinOrder({e.path}, -1);
    any = true;
  }
  pin_undo_ = std::move(keep);
  if (any) {
    BumpPinsEpoch();
    SavePins(pins_);
    PersistLayouts();
  }
  InvalidateDesktopScanCache();
  RefreshDesktopIcons();
}

void App::TryShellUndoAndRepin() {
  if (icons_visible_intent_) {
    return;
  }
  PWSTR desk_raw = nullptr;
  std::wstring desk;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) &&
      desk_raw) {
    desk = desk_raw;
    CoTaskMemFree(desk_raw);
  }
  if (!desk.empty()) {
    InvokeShellFolderVerb(message_hwnd_, desk, L"undo");
  }
  RepinFromUndoStack();
}

void App::OnDesktopPaste(bool as_link) {
  // Stock view: Explorer DefView owns paste. Organized view hides the listview,
  // so we land CF_HDROP into the Desktop folder and show floats (Fences).
  if (icons_visible_intent_) {
    return;
  }
  // Read cut/copy before PasteClipboard* (it reopens the clipboard).
  std::vector<std::wstring> clip;
  bool cut = false;
  if (!ClipboardGetFilesWithEffect(&clip, &cut) || clip.empty()) {
    ShowToast(L"剪贴板里没有文件");
    return;
  }
  // Explorer: single selected float folder → paste into it (not Desktop root).
  const std::wstring into = public_host_.SelectedSingleDirectory();
  std::vector<std::wstring> landed;
  HWND owner = message_hwnd_ ? message_hwnd_ : nullptr;
  int n = 0;
  if (!into.empty()) {
    if (as_link) {
      n = PasteClipboardLinksIntoDir(owner, into, &landed);
    } else {
      for (const auto& src : clip) {
        std::wstring dest;
        if (MoveOrCopyFileIntoDir(owner, src, into, cut, &dest) &&
            !dest.empty()) {
          SuppressOrganizePath(dest);
          landed.push_back(dest);
          ++n;
        }
      }
      if (cut && n > 0) {
        ClipboardConsumeCut();
      }
    }
    if (n <= 0) {
      ShowToast(L"无法粘贴到所选文件夹");
      return;
    }
    ShowToast(L"已粘贴 " + std::to_wstring(n) + L" 项");
    InvalidateDesktopScanCache();
    RefreshDesktopIcons();
    return;
  }
  if (as_link) {
    PWSTR desk_raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) &&
        desk_raw) {
      const std::wstring desk = desk_raw;
      CoTaskMemFree(desk_raw);
      n = PasteClipboardLinksIntoDir(owner, desk, &landed);
    }
  } else {
    n = PasteClipboardToDesktopFolder(owner, &landed);
  }
  if (n <= 0) {
    ShowToast(L"剪贴板里没有文件");
    return;
  }
  // Explorer / Fences: seat new floats near the cursor (or recent blank RMB).
  POINT seat{};
  GetCursorPos(&seat);
  {
    POINT rclick{};
    ULONGLONG tick = 0;
    if (GetLastDesktopRightClick(&rclick, &tick) &&
        GetTickCount64() - tick < 8000) {
      seat = rclick;
    }
  }
  auto seat_landed_floats = [&]() {
    const IconGridMetrics grid = QueryIconGridMetrics();
    const int step = (std::max)(48, grid.cell_w);
    int cascade = 0;
    for (const auto& p : landed) {
      if (p.empty()) {
        continue;
      }
      POINT pt{seat.x + cascade * step / 4, seat.y + cascade * step / 4};
      UpsertFloatPlacement(&float_placements_, p, current_page_, pt);
      ++cascade;
    }
    if (!landed.empty()) {
      SaveFloatPlacements(float_placements_);
    }
  };
  for (const auto& p : landed) {
    SuppressOrganizePath(p);
    LogLine(L"[paste] landed%s: %s", as_link ? L" link" : L"", p.c_str());
  }
  if (as_link) {
    seat_landed_floats();
    InvalidateDesktopScanCache();
    RefreshDesktopIcons();
    public_host_.SelectPaths(landed);
    ShowToast(L"已创建快捷方式 " + std::to_wstring(n) + L" 项");
    return;
  }
  if (cut) {
    // Fences: Cut → blank desktop releases the virtual pin (Desktop self-MOVE
    // is a no-op on disk; without Unpin the icon would stay inside the fence).
    std::vector<std::wstring> to_unpin;
    auto add = [&](const std::wstring& p) {
      if (p.empty() || !FindPin(pins_, p)) {
        return;
      }
      const std::wstring key = NormalizePinKey(p);
      for (const auto& u : to_unpin) {
        if (NormalizePinKey(u) == key) {
          return;
        }
      }
      to_unpin.push_back(p);
    };
    for (const auto& p : clip) {
      add(p);
    }
    for (const auto& p : landed) {
      add(p);
    }
    if (!to_unpin.empty()) {
      UnpinPaths(to_unpin, &seat);
      seat_landed_floats();
      ClipboardConsumeCut();
      last_manual_refresh_ = GetTickCount64();
      g_paste_rescan_deadline = last_manual_refresh_ + kPasteRescanSuppressMs;
      ShowToast(L"已粘贴 " + std::to_wstring(n) + L" 项到桌面");
      return;
    }
  }
  if (cut) {
    ClipboardConsumeCut();
  }
  seat_landed_floats();
  InvalidateDesktopScanCache();
  RefreshDesktopIcons();
  {
    const auto scan = ScanDesktopItems();
    bool found = false;
    for (const auto& it : scan) {
      if (!landed.empty() && it.path == landed[0]) {
        found = true;
        break;
      }
    }
    LogLine(L"[paste] scan=%u found_new=%d public_has=%d public_visible=%d",
            static_cast<unsigned>(scan.size()), found ? 1 : 0,
            public_host_.HasItems() ? 1 : 0, public_visible_ ? 1 : 0);
  }
  last_manual_refresh_ = GetTickCount64();  // mark the immediate manual rescan
  g_paste_rescan_deadline = last_manual_refresh_ + kPasteRescanSuppressMs;
  public_host_.SelectPaths(landed);
  ShowToast(L"已粘贴 " + std::to_wstring(n) + L" 项到桌面");
}

DWORD App::OnDesktopDropPaths(const std::vector<std::wstring>& paths) {
  if (icons_visible_intent_ || paths.empty()) {
    return DROPEFFECT_NONE;
  }
  PWSTR desk_raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) ||
      !desk_raw) {
    return DROPEFFECT_NONE;
  }
  const std::wstring desk = desk_raw;
  CoTaskMemFree(desk_raw);
  HWND owner = message_hwnd_ ? message_hwnd_ : nullptr;
  int ok = 0;
  int moved = 0;
  // Fences/Explorer: ignore Desktop self-drop; Alt/Ctrl+Shift → .lnk;
  // same-volume default MOVE; Ctrl=copy, Shift=move.
  auto parent_of = [](const std::wstring& p) -> std::wstring {
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash);
  };
  const bool want_link = WantDropLinkEffect();
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  for (const auto& src : paths) {
    if (_wcsicmp(parent_of(src).c_str(), desk.c_str()) == 0) {
      continue;
    }
    std::wstring dest;
    if (want_link) {
      if (!CreateShortcutIntoDir(owner, src, desk, &dest)) {
        continue;
      }
      ++ok;
      if (!dest.empty()) {
        SuppressOrganizePath(dest);
      }
      continue;
    }
    bool do_move = SameVolume(src, desk);
    if (ctrl) {
      do_move = false;
    }
    if (shift) {
      do_move = true;
    }
    if (MoveOrCopyFileIntoDir(owner, src, desk, do_move, &dest)) {
      ++ok;
      if (do_move) {
        ++moved;
      }
      if (!dest.empty()) {
        SuppressOrganizePath(dest);
      }
    }
  }
  if (ok <= 0) {
    // Self-drop / no-op: must NOT advertise MOVE or Explorer deletes sources.
    return DROPEFFECT_NONE;
  }
  InvalidateDesktopScanCache();
  RefreshDesktopIcons();
  if (want_link) {
    ShowToast(L"已创建快捷方式 " + std::to_wstring(ok) + L" 项");
    return DROPEFFECT_LINK;
  }
  if (moved == ok) {
    ShowToast(L"已移动到桌面 " + std::to_wstring(ok) + L" 项");
    return DROPEFFECT_MOVE;
  }
  if (moved == 0) {
    ShowToast(L"已复制到桌面 " + std::to_wstring(ok) + L" 项");
  } else {
    ShowToast(L"已放入桌面 " + std::to_wstring(ok) + L" 项");
  }
  return DROPEFFECT_COPY;
}

void App::ShowStockDesktopView() {
  // Hide DeskTidy chrome without permanently clearing prefs (Python stock view).
  for (auto& f : fences_) {
    if (f) {
      f->HideTransient();
    }
  }
  dock_.SetVisible(false);
  page_bar_.Hide();
  pet_.SetVisible(false);
  public_host_.SetItems({});
  public_host_.Hide();
  RestoreDesktopIconsIfHidden();
  SetIconsVisible(true, true);
  LogLineA("[desktop_view] stock");
}

void App::ShowOrganizedDesktopView() {
  SetIconsVisible(false, true);
  ReapplyNamespaceIconHiddenState();
  EnsureHostedSystemNamespaceIcons(&pins_);
  SavePins(pins_);
  if (!fences_hidden_) {
    ApplyPageVisibility();
  }
  dock_.SetVisible(dock_visible_);
  pet_.SetVisible(pet_visible_);
  SyncPageChrome();
  RefreshDesktopIcons();
  LogLineA("[desktop_view] organized");
}

void App::ApplyLoadedGeneralPrefs() {
  prefs_.show_fences = true;
  prefs_.double_click_hide = false;
  fences_hidden_ = false;
  SyncNativeShellVerbs(prefs_.desktop_right_click_menu);
  // Public/loose visibility is derived in RefreshDesktopIcons (pin-only + floats).
  RefreshDesktopIcons();
  ApplyPageVisibility();
  public_host_.SetIconZoom(prefs_.public_icon_zoom);
  ApplyDockPrefs();
  SyncPageChrome();
  PushExtensionSettings();
}

void App::ExportPrefs() {
  wchar_t path[MAX_PATH] = L"desktidy_settings.json";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = settings_.Hwnd();
  ofn.lpstrFilter = L"JSON (*.json)\0*.json\0All\0*.*\0";
  ofn.lpstrFile = path;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = L"导出配置";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  ofn.lpstrDefExt = L"json";
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  SaveAppPrefsToPath(prefs_, path);
  ShowToast(std::wstring(L"配置已导出到 ") + path);
}

void App::ImportPrefs() {
  wchar_t path[MAX_PATH]{};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = settings_.Hwnd();
  ofn.lpstrFilter = L"JSON (*.json)\0*.json\0All\0*.*\0";
  ofn.lpstrFile = path;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrTitle = L"导入配置";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&ofn)) {
    return;
  }
  if (!MistConfirm(settings_.Hwnd(), L"确认导入",
                   L"导入配置将覆盖当前设置，是否继续？", MistMsgTone::Warn)) {
    return;
  }
  prefs_ = LoadAppPrefsFromPath(path);
  prefs_.ui_theme = NormalizeUiTheme(prefs_.ui_theme);
  SaveAppPrefs(prefs_);
  ApplyLoadedGeneralPrefs();
  ApplyUiThemeId(prefs_.ui_theme);
  settings_.SetOrganizePrefs(prefs_.auto_organize_on_startup,
                             prefs_.auto_organize_watch);
  MistAlert(settings_.Hwnd(), L"导入完成",
            L"配置已导入，部分选项可能需要重启生效。", MistMsgTone::Info);
}

void App::OpenNotes() { LaunchOrFocusDeskNote(); }

void App::OpenPathsInNotes(const std::vector<std::wstring>& paths) {
  LaunchOrFocusDeskNote(paths);
}

void App::RequestQuit() {
  // T4: confirm when a recording is in progress (exit would stop it).
  if (IsScreenRecording()) {
    if (!MistConfirm(
            settings_.Hwnd(), L"退出确认",
            L"正在录屏，退出将停止录制并保存当前片段。\n是否继续退出？",
            MistMsgTone::Warn)) {
      return;
    }
  }
  prefs_.session_active = false;
  SaveAppPrefs(prefs_);
  PostQuitMessage(0);
}

void App::HandleSettingsClose() {
  // Only reached when close_to_tray is false. First close asks once and
  // remembers (Python closeEvent → close_behavior_prompted).
  if (!prefs_.close_behavior_prompted) {
    const bool keep_background = MistConfirm(
        settings_.Hwnd(), L"后台运行",
        L"关闭 DeskTidy 窗口后是否在系统托盘后台继续运行？\n\n"
        L"「是」：隐藏到托盘，桌面分区继续工作，此选择将被记住。\n"
        L"「否」：按退出流程处理（分区整理会保留）。",
        MistMsgTone::Info);
    prefs_.close_behavior_prompted = true;
    prefs_.close_to_tray = keep_background;
    SaveAppPrefs(prefs_);
    PushExtensionSettings();
    if (keep_background) {
      settings_.Hide();
      TrimMemory();
      return;
    }
  }
  RequestQuit();
}

void App::HideOffPageFences() {
  if (peek_active_) {
    return;
  }
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd()) {
      continue;
    }
    const bool want = !fences_hidden_ && !icons_visible_intent_ &&
                      fence->VisibleOnPage(current_page_) &&
                      (fence->TabGroup().empty() || fence->TabActive());
    if (want) {
      continue;
    }
    const HWND hwnd = fence->Hwnd();
    // Soft-park must be a real hide. Visible-but-under-DefView off-page fences
    // look fine on a bare desktop, then both pages ghost when an app covers
    // DefView (Progman-owned layered popups still paint on the wallpaper).
    if (IsWindowVisible(hwnd) || IsIconic(hwnd)) {
      ShowWindow(hwnd, SW_HIDE);
    }
  }
}

void App::SeatAllFencesUnderApps() {
  // Market/Python `_keep_overlays_under_apps`: HWND_BOTTOM sink under APPWINDOWs.
  // Do NOT RaiseOverlay here — Raise after Place was lifting Progman-owned
  // fences over WeChat/VS Code when opening settings / 账号浮窗.
  //
  // Critical: AttachOverlay may ShowWindow when hidden. Only seat chrome that
  // should be visible — otherwise opening Settings resurrects a pet/dock the
  // user just turned off.
  HideOffPageFences();
  // SoftSeat FRAMECHANGED (esp. raised-desktop DefView→Progman retarget) can
  // wipe ULW — re-Present only when we actually healed or the layer is dirty.
  const bool raised = IsRaisedDesktopShell();
  const DefViewHost host = raised ? FindDefViewHost() : DefViewHost{};
  // Resolve the band facts ONCE for this pass: every overlay below is seated
  // against the same snapshot instead of re-walking the window stack per HWND.
  const DesktopBandState band = QueryDesktopBandState();
  auto seat = [&host, raised, &band](HWND hwnd) -> bool {
    if (!hwnd || !IsWindow(hwnd)) {
      return false;
    }
    bool healed = false;
    if (!IsAttachedToDesktop(hwnd)) {
      AttachOverlayToDesktop(hwnd, true);
      healed = true;
    } else if (raised && host.defview && host.host &&
               host.defview != host.host &&
               GetWindow(hwnd, GW_OWNER) == host.defview) {
      // IsAttached allows DefView owner; raised desktop must SoftSeat to Progman.
      AttachOverlayToDesktop(hwnd, true);
      healed = true;
    }
    // HWND_BOTTOM alone can slip under DefView (blank fences until page
    // switch). Raise keeps the Fences-class seat: above DefView, under apps.
    RaiseOverlayInDesktopBandWith(hwnd, band);
    return healed;
  };
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd()) {
      continue;
    }
    const bool want = !fences_hidden_ && !icons_visible_intent_ &&
                      !peek_active_ && fence->VisibleOnPage(current_page_) &&
                      (fence->TabGroup().empty() || fence->TabActive());
    if (!want) {
      continue;
    }
    const bool healed = seat(fence->Hwnd());
    if ((healed || fence->LayerDirty()) && IsWindowVisible(fence->Hwnd()) &&
        !IsIconic(fence->Hwnd())) {
      fence->RefreshLayer();
    }
  }
  if (dock_visible_ && dock_.Hwnd() && dock_.HasItems()) {
    if (seat(dock_.Hwnd()) && IsWindowVisible(dock_.Hwnd()) &&
        !IsIconic(dock_.Hwnd())) {
      dock_.RefreshLayer();
    }
  }
  if (pet_visible_ && pet_.Hwnd()) {
    const bool healed = seat(pet_.Hwnd());
    if (pet_.PagesHwnd()) {
      seat(pet_.PagesHwnd());
    }
    if (pet_.MenuHwnd()) {
      seat(pet_.MenuHwnd());
    }
    if (healed && IsWindowVisible(pet_.Hwnd()) && !IsIconic(pet_.Hwnd())) {
      pet_.RefreshLayer();
    }
  }
  const bool pet_hosts = pet_visible_ && pet_.ShowPageBubbles();
  const bool show_page_bar = !pet_hosts && prefs_.show_page_indicator;
  if (show_page_bar && page_bar_.Hwnd()) {
    if (seat(page_bar_.Hwnd()) && IsWindowVisible(page_bar_.Hwnd()) &&
        !IsIconic(page_bar_.Hwnd())) {
      page_bar_.RefreshLayer();
    }
  }
  if (public_visible_ && public_host_.Hwnd() && public_host_.HasItems()) {
    if (seat(public_host_.Hwnd()) && IsWindowVisible(public_host_.Hwnd()) &&
        !IsIconic(public_host_.Hwnd())) {
      public_host_.RefreshLayer();
    }
  }
  // Honor the user's show/hide-icons intent (Ctrl+Shift+H). Do not force
  // Explorer icons off on every seat — that undid the hotkey (Fences: seat
  // restacks chrome; it does not thrash the user's icon visibility).
  SetDesktopIconsVisible(icons_visible_intent_);
  // Do not sink notes into the desktop band — it is a normal app window.
  if (settings_.Hwnd() && settings_.IsOpen()) {
    SetWindowPos(settings_.Hwnd(), HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
}

AppState App::BuildAppState() const {
  AppState state;
  state.current_page = current_page_;
  state.page_names = page_names_;
  state.page_locked = page_locked_;
  state.dock_visible = dock_visible_;
  state.pet_visible = pet_visible_;
  state.last_display_fingerprint = display_fp_;
  state.fences.reserve(fences_.size());
  for (const auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    FenceLayout fl = fence->CurrentLayout();
    // Keep G3 page overrides aligned with the live HWND so reboot cannot
    // resurrect a stale page_geometries row after fences[] was updated.
    if (fl.page_geometries.count(current_page_)) {
      SyncPageGeometryFromTopLevel(&fl, current_page_);
    }
    if (fl.page_geometries.count(fl.page)) {
      SyncPageGeometryFromTopLevel(&fl, fl.page);
    }
    state.fences.push_back(std::move(fl));
  }
  return state;
}

void App::PersistLayouts() {
  SaveAppState(BuildAppState());
}

void App::SchedulePersistLayouts() {
  if (!message_hwnd_) {
    PersistLayouts();
    return;
  }
  // Coalesce rapid page clicks / geometry tweaks — avoid sync disk on UI path.
  KillTimer(message_hwnd_, kTimerPersistLayouts);
  SetTimer(message_hwnd_, kTimerPersistLayouts, 350, nullptr);
}

void App::ScheduleWarmFenceLayers() {
  if (!message_hwnd_) {
    return;
  }
  KillTimer(message_hwnd_, kTimerWarmFences);
  SetTimer(message_hwnd_, kTimerWarmFences, kWarmFencesTickMs, nullptr);
}

void App::ReflowPublicFloats() {
  if (!public_visible_) {
    return;
  }
  std::vector<RECT> blocked;
  blocked.reserve(fences_.size() + 4);
  constexpr int kFencePad = 12;
  auto push_hwnd_block = [&](HWND hwnd, int pad) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
      return;
    }
    RECT r{};
    if (!GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) {
      return;
    }
    if (pad) {
      InflateRect(&r, pad, pad);
    }
    blocked.push_back(r);
  };
  if (!fences_hidden_) {
    for (auto& f : fences_) {
      if (!f || !f->VisibleOnPage(current_page_)) {
        continue;
      }
      RECT r{};
      bool got = false;
      if (f->Hwnd() && IsWindow(f->Hwnd()) && GetWindowRect(f->Hwnd(), &r) &&
          r.right > r.left && r.bottom > r.top) {
        got = true;
      }
      if (!got) {
        const FencePageGeometry geo = f->GeometryForPage(current_page_);
        if (geo.w > 0 && geo.h > 0) {
          r = {geo.x, geo.y, geo.x + geo.w, geo.y + geo.h};
          got = true;
        }
      }
      if (!got) {
        continue;
      }
      InflateRect(&r, kFencePad, kFencePad);
      blocked.push_back(r);
    }
  }
  // AIGC START
  if (page_bar_.Hwnd() && IsWindowVisible(page_bar_.Hwnd())) {
    push_hwnd_block(page_bar_.Hwnd(), 4);
  }
  if (pet_visible_ && pet_.Hwnd()) {
    push_hwnd_block(pet_.Hwnd(), 4);
    if (pet_.PagesHwnd()) {
      push_hwnd_block(pet_.PagesHwnd(), 4);
    }
  }
  if (dock_visible_) {
    push_hwnd_block(dock_.Hwnd(), 4);
  }
  // AIGC END
  public_host_.SetBlockedRects(std::move(blocked));
  public_host_.Reflow();
}

void App::ApplyDockPrefs() {
  static const wchar_t* kEdges[] = {L"bottom", L"left", L"right"};
  int edge = prefs_.dock_edge;
  if (edge < 0 || edge > 2) {
    edge = 0;
  }
  dock_.SetEdge(kEdges[edge]);
  float opacity = static_cast<float>(prefs_.dock_opacity) / 100.f;
  if (opacity < 0.4f) {
    opacity = 0.4f;
  }
  if (opacity > 1.f) {
    opacity = 1.f;
  }
  dock_.SetOpacity(opacity);
}

void App::MaybeStartupUpdateCheck() {
  if (!prefs_.check_update_on_startup) {
    return;
  }
  // Async worker; result lands on kMsgUpdateCheckDone (never blocks startup).
  StartUpdateCheckAsync(message_hwnd_, /*force=*/false, /*interactive=*/false);
}

void App::PromptAvailableUpdate(const std::wstring& latest,
                                const std::wstring& current,
                                const std::wstring& html_url) {
  const std::wstring msg = L"发现新版本 " + latest + L"（当前 " + current +
                           L"）。\n\n是：下载并静默安装\n否：打开发布页\n取消：稍后";
  const MistMsgResult answer =
      MistMessage(message_hwnd_, L"检查更新", msg.c_str(),
                  MistMsgKind::YesNoCancel, MistMsgTone::Info);
  if (answer == MistMsgResult::No) {
    ShellExecuteW(nullptr, L"open",
                  html_url.empty()
                      ? L"https://github.com/Bowen-Yu0926/DeskTidy-Releases/releases/latest"
                      : html_url.c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
  } else if (answer == MistMsgResult::Yes) {
    update_progress_last_pct_ = -1;
    UpdateToastText(L"正在下载更新…", 5000);
    StartUpdateInstallAsync(message_hwnd_);
  }
}

void App::OnUpdateCheckDone(UpdateCheckResult* r) {
  if (!r) {
    return;
  }
  std::unique_ptr<UpdateCheckResult> own(r);
  LogLine(L"[update] check done status=%d latest=%s err=%s", r->status,
          r->latest.c_str(), r->error.c_str());
  if (r->status == 0) {
    if (r->interactive) {
      ShowToast(L"检查更新失败：" +
                (r->error.empty() ? L"网络不可用" : r->error));
    }
    return;
  }
  if (r->status == 1) {
    if (r->interactive) {
      ShowToast(L"已是最新版本（" + r->current + L"）");
    }
    return;
  }
  const std::wstring tag = L"v" + r->latest;
  const long long now =
      static_cast<long long>(std::time(nullptr));
  constexpr long long kDay = 24LL * 3600LL;
  if (!r->interactive && prefs_.update_notified_tag == tag &&
      prefs_.update_notified_at > 0 &&
      now - prefs_.update_notified_at < kDay) {
    return;  // same tag already soft-reminded within 24h
  }
  prefs_.update_notified_tag = tag;
  prefs_.update_notified_at = now;
  SaveAppPrefs(prefs_);

  if (r->interactive) {
    PromptAvailableUpdate(r->latest, r->current, r->html_url);
    return;
  }
  // Startup: non-modal soft tip (VS Code / Discord style). Click → confirm.
  const std::wstring latest = r->latest;
  const std::wstring current = r->current;
  const std::wstring html = r->html_url;
  ShowActionToast(L"发现新版本 " + latest + L" — 点击更新",
                  [this, latest, current, html]() {
                    PromptAvailableUpdate(latest, current, html);
                  });
}

void App::OnUpdateInstallDone(UpdateInstallResult* r) {
  if (!r) {
    return;
  }
  std::unique_ptr<UpdateInstallResult> own(r);
  LogLine(L"[update] install done ok=%d verify=%d digest=%d path=%s err=%s",
          r->ok, r->verify_ok, r->has_checksum, r->path.c_str(),
          r->error.c_str());
  update_progress_last_pct_ = -1;
  if (!r->ok) {
    std::wstring msg = r->error.empty() ? L"更新安装失败" : r->error;
    if (!r->path.empty()) {
      msg += L"\n安装包保留于：\n" + r->path;
    }
    ShowToast(msg);
    return;
  }
  UpdateToastText(L"下载完成，准备安装…", 3000);
  if (!MistConfirm(
          message_hwnd_, L"下载完成",
          L"安装包已下载并通过校验。\n\n点击「是」将关闭 DeskTidy 并开始安装"
          L"（安装过程中会显示进度窗口）。\n是否立即安装？",
          MistMsgTone::Info)) {
    return;
  }
  UpdateToastText(L"正在启动安装程序…", 10000);
  // Launch + WaitForInputIdle run on a worker; completion lands back on
  // kMsgUpdateInstallLaunched (never block the UI thread for 12s+).
  StartInstallerLaunchAsync(message_hwnd_, r->path);
}

void App::OnUpdateInstallLaunched(UpdateInstallResult* r) {
  if (!r) {
    return;
  }
  std::unique_ptr<UpdateInstallResult> own(r);
  if (!r->ok) {
    std::wstring msg = r->error.empty() ? L"启动安装程序失败" : r->error;
    ShowToast(msg + L"\n安装包路径：" + r->path);
    return;
  }
  UpdateToastText(L"正在安装新版本，请稍候…", 8000);
  LogLine(L"[update] installer launched, quitting");
  // Brief beat so the Inno progress UI can paint before we tear down.
  Sleep(400);
  PostQuitMessage(0);
}

void App::OnUpdateProgress(int pct) {
  if (pct < 0) {
    pct = 0;
  }
  if (pct > 100) {
    pct = 100;
  }
  // Skip tiny wobbles; still paint every 5% and always 100.
  if (pct < 100 && update_progress_last_pct_ >= 0 &&
      pct - update_progress_last_pct_ < 5) {
    return;
  }
  update_progress_last_pct_ = pct;
  UpdateToastText(L"正在下载 " + std::to_wstring(pct) + L"%", 6000);
}

void App::ApplyChromeTheme() {
  const std::string bar_style_id =
      prefs_.page_bar_style.empty() ? "navy" : prefs_.page_bar_style;
  const FenceStylePreset* bar_style = FenceStyleById(bar_style_id);
  if (!bar_style) {
    bar_style = &DefaultFenceStyle();
  }
  page_bar_.ApplyStyle(bar_style->background, bar_style->accent,
                       bar_style->title);

  const FenceStylePreset* style = FenceStyleById(prefs_.theme_id);
  if (!style) {
    style = &DefaultFenceStyle();
  }
  settings_.ApplyStyle(style->background, style->accent, style->title);
}

void App::SyncPageChrome() {
  NormalizePageNames(&page_names_);
  // Account chip follows the active chrome host (float / tabs / pet).
  // Enabled ⇒ logged-in (toggle forces login; logout clears enabled).
  const bool tool_record = prefs_.record_enabled && prefs_.record_on_bar;
  const bool tool_minutes = prefs_.minutes_enabled;
  const bool tool_calculator =
      prefs_.calculator_enabled && prefs_.calculator_on_bar;
  const bool tool_todo = prefs_.todos_enabled && prefs_.todos_on_bar;
  pet_.SetToolFlags(tool_record, tool_minutes, tool_calculator, tool_todo);
  page_bar_.SetToolFlags(tool_record, tool_minutes, tool_calculator, tool_todo);
  page_bar_.SetChromeStyle(prefs_.page_chrome_style == "tabs"
                               ? PageBar::ChromeStyle::Tabs
                               : PageBar::ChromeStyle::Float);

  std::vector<PageBar::FolderChip> folders;
  std::vector<PetWindow::FolderChip> pet_folders;
  for (const auto& f : LoadPageFolders()) {
    PageBar::FolderChip c{f.name, f.path};
    folders.push_back(c);
    pet_folders.push_back(PetWindow::FolderChip{f.name, f.path});
  }
  page_bar_.SetFolders(folders);
  pet_.SetFolders(pet_folders);

  // Python: pet visible + show_page_bubbles ⇒ float bar on pet; else edge peeks.
  // show_page_indicator gates the right-edge bar (not pet bubbles).
  pet_.SetPages(page_names_, current_page_);
  public_host_.SetPageNames(page_names_);
  const bool pet_hosts = pet_visible_ && pet_.ShowPageBubbles();
  const bool edge_ok = prefs_.show_page_indicator;
  if (pet_hosts) {
    page_bar_.Hide();
  } else if (edge_ok) {
    // Pet off, or pet on with bubbles off ⇒ edge bar owns paging.
    if (!pet_visible_) {
      pet_.SetVisible(false);
    }
    page_bar_.SetPageNames(page_names_);
    page_bar_.SetPageLocks(page_locked_);
    std::vector<std::wstring> fence_titles;
    fence_titles.reserve(fences_.size());
    for (auto& fence : fences_) {
      fence_titles.push_back(fence ? fence->CurrentLayout().title
                                   : std::wstring{});
    }
    page_bar_.SetFenceTitles(fence_titles);
    page_bar_.SetPagesVisible(true);
    page_bar_.SetPage(current_page_);
    page_bar_.Show();
    page_bar_.AttachToDesktopLayer();
  } else {
    page_bar_.Hide();
  }
  for (auto& fence : fences_) {
    if (fence) {
      fence->SetPageNames(page_names_);
    }
  }
  // Quiet: avoid PushState with a half-updated ExtState while toggling tools.
  settings_.SetPageNames(page_names_, false);
  settings_.SetCurrentPage(current_page_, false);
  ApplyChromeTheme();
}

void App::AddPage() {
  if (page_names_.size() >= 12) {
    ShowToast(L"最多 12 页");
    return;
  }
  page_names_.push_back(NextPageName(page_names_.size() + 1));
  page_locked_.resize(page_names_.size(), false);
  SyncPageChrome();
  SetPage(static_cast<int>(page_names_.size()) - 1);
  ShowToast(L"已添加页面");
}

void App::RemoveCurrentPage() {
  // Same remapper as page-bar delete (multi-page membership + float pages).
  DeletePageAt(current_page_);
}

void App::RenameCurrentPage() {
  if (page_names_.empty()) {
    page_names_ = DefaultPageNames();
  }
  if (current_page_ < 0 ||
      current_page_ >= static_cast<int>(page_names_.size())) {
    current_page_ = 0;
  }
  const std::wstring& current = page_names_[static_cast<size_t>(current_page_)];
  const std::wstring name =
      PromptText(settings_.Hwnd(), Tr("rename_page").c_str(),
                 Tr("rename_page_label").c_str(), current);
  if (name.empty()) {
    return;
  }
  page_names_[static_cast<size_t>(current_page_)] = name;
  SyncPageChrome();
  PersistLayouts();
  settings_.SetPageNames(page_names_, false);
  PushExtensionSettings();
  ShowToast(L"页面已重命名");
}

void App::CreateFenceOnPage(int page, bool switch_to_page) {
  const int max_page = (std::max)(0, static_cast<int>(page_names_.size()) - 1);
  if (page < 0 || page > max_page) {
    page = (std::min)(max_page, current_page_);
  }
  if (PageLockedForFences(page)) {
    ShowToast(L"该页面已锁定，无法新建分区");
    return;
  }
  const RECT work = PrimaryWorkArea();
  // Design default with content_min floor (two icon rows) and work clamp.
  const int gap = 24;
  const int content_min = MinFenceHeightForIconRows(2);
  const int w = (std::max)(
      kFenceMinW,
      (std::min)(kFenceDefaultW + 40,
                 static_cast<int>(work.right - work.left) - gap * 2));
  const int work_h =
      (std::max)(content_min, static_cast<int>(work.bottom - work.top) - gap * 2);
  int h = (std::max)(content_min, kFenceDefaultH);
  if (h > work_h) {
    h = work_h;
  }
  const int step = (static_cast<int>(fences_.size()) % 6) * gap;
  FenceLayout layout;
  layout.id = MakeFenceId();
  const std::wstring page_name =
      (page >= 0 && page < static_cast<int>(page_names_.size()))
          ? page_names_[static_cast<size_t>(page)]
          : L"新建分区";
  layout.title = page_name.empty() ? L"新建分区" : page_name;
  layout.x = work.left + 80 + step;
  layout.y = work.top + 80 + step;
  layout.w = w;
  layout.h = h;
  layout.page = page;
  layout.pages = {page};
  layout.organize_kinds = {"file"};
  layout.style_id = prefs_.theme_id.empty() ? "navy" : prefs_.theme_id; // 跟随当前分区样式，不联动全局主题
  if (!SpawnFence(layout)) {
    return;
  }
  SeatAllFencesUnderApps();
  if (switch_to_page) {
    SetPage(page);
  }
  PersistLayouts();
  RefreshSettingsList();
  LogLine(L"[page_bar] create fence on page %d", page);
}

void App::EnsureOrganizeFencesForEmptyPages() {
  // No-op (market / Fences): deleting a user fence must stay deleted.
  // Previously every OrganizeVirtual (incl. quiet watch) spawned a page-named
  // 「工作」file fence when page 0 had only 常用/其他 — so dissolve → recreate.
  // ResolveFenceByKind already falls back to other pages' file zones
  // (system_docs); leftover icons stay floats until the user creates a zone.
  (void)this;
}

void App::DeletePageAt(int page) {
  if (page == 0 || page == 1) {
    ShowToast(L"系统页面不可删除");
    return;
  }
  if (page < 0 || page >= static_cast<int>(page_names_.size())) {
    return;
  }
  // A locked page must not be deleted — otherwise the lock means nothing.
  if (page < static_cast<int>(page_locked_.size()) &&
      page_locked_[static_cast<size_t>(page)]) {
    ShowToast(L"该页面已锁定，请先解锁再删除");
    return;
  }
  if (page_names_.size() <= 2) {
    ShowToast(L"至少保留系统页");
    return;
  }
  int content = 0;
  for (const auto& fence : fences_) {
    // Multi-page membership: count fences that actually show on this page.
    if (fence && fence->OnPage(page)) {
      ++content;
    }
  }
  if (content > 0) {
    const std::wstring msg =
        L"该页有 " + std::to_wstring(content) +
        L" 个分区，删除后这些分区将离开本页（仅属本页的移至工作页）。\n"
        L"确定删除页面「" +
        page_names_[static_cast<size_t>(page)] + L"」？";
    if (!MistConfirm(settings_.Hwnd(), L"删除页面", msg.c_str(),
                     MistMsgTone::Danger)) {
      return;
    }
  }
  page_names_.erase(page_names_.begin() + page);
  // Keep lock flags aligned with page_names_ (same index space).
  if (page < static_cast<int>(page_locked_.size())) {
    page_locked_.erase(page_locked_.begin() + page);
  }
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    // Drop deleted index + renumber pages_/page_geometries (SetPage alone
    // appends and leaves stale multi-page membership).
    fence->RemapPagesAfterDelete(page);
  }
  // Float placements share the same page index space.
  for (auto& kv : float_placements_) {
    if (kv.second.page == page) {
      kv.second.page = 0;
    } else if (kv.second.page > page) {
      --kv.second.page;
    }
  }
  SaveFloatPlacements(float_placements_);
  if (current_page_ > page) {
    --current_page_;
  } else if (current_page_ >= static_cast<int>(page_names_.size())) {
    current_page_ = static_cast<int>(page_names_.size()) - 1;
  }
  SyncPageChrome();
  ApplyPageVisibility();
  PersistLayouts();
  RefreshDesktopIcons();
  LogLine(L"[page_bar] delete page %d (remaining=%d)", page,
          static_cast<int>(page_names_.size()));
  ShowToast(L"已删除页面");
}

void App::RenamePageAt(int page) {
  if (page == 0 || page == 1) {
    ShowToast(L"系统页面不可重命名");
    return;
  }
  if (page < 0 || page >= static_cast<int>(page_names_.size())) {
    return;
  }
  const std::wstring current = page_names_[static_cast<size_t>(page)];
  const std::wstring name =
      PromptText(settings_.Hwnd(), Tr("rename_page").c_str(),
                 Tr("rename_page_label").c_str(), current);
  if (name.empty() || name == current) {
    return;
  }
  for (int i = 0; i < static_cast<int>(page_names_.size()); ++i) {
    if (i != page && page_names_[static_cast<size_t>(i)] == name) {
      ShowToast(L"页面名称已存在");
      return;
    }
  }
  page_names_[static_cast<size_t>(page)] = name;
  SyncPageChrome();
  PersistLayouts();
  settings_.SetPageNames(page_names_, false);
  PushExtensionSettings();
  LogLine(L"[page_bar] rename page %d -> %s", page, name.c_str());
  ShowToast(L"页面已重命名");
}

void App::OpenLayoutEditor() {
  AppState state;
  state.current_page = current_page_;
  state.page_names = page_names_;
  state.dock_visible = dock_visible_;
  state.pet_visible = pet_visible_;
  for (const auto& fence : fences_) {
    if (fence) {
      state.fences.push_back(fence->CurrentLayout());
    }
  }
  if (!ShowLayoutEditor(settings_.Hwnd(), &state)) {
    return;
  }
  page_names_ = state.page_names;
  NormalizePageNames(&page_names_);
  current_page_ = state.current_page;
  for (auto& fence : fences_) {
    if (fence) {
      fence->Destroy();
    }
  }
  fences_.clear();
  for (const auto& layout : state.fences) {
    SpawnFence(layout);
  }
  PersistLayouts();
  SyncPageChrome();
  RefreshDesktopIcons();
  ApplyPageVisibility();
  RefreshSettingsList();
  ShowToast(L"布局已应用");
}

void App::ApplyGlobalThemeId(const std::string& theme_id) {
  ApplyUiThemeId(theme_id);
}

void App::ApplyThemeId(const std::string& theme_id) {
  ApplyUiThemeId(theme_id);
}

void App::ApplyUiThemeId(const std::string& ui_theme) {
  const std::string id = NormalizeUiTheme(ui_theme);
  prefs_.ui_theme = id;
  SaveAppPrefs(prefs_);
  settings_.ApplyUiThemeVars(UiThemeCssVars(id));
  todos_.ApplyUiTheme(id);
  ApplyTrayThemeIcon();
  ApplyChromeTheme();
  PushExtensionSettings();
  ShowToast(L"已切换主题");
}

COLORREF App::TrayAccentColor() const {
  const auto vars = UiThemeCssVars(prefs_.ui_theme);
  auto it = vars.find("accent");
  std::wstring hex = it != vars.end() ? WidenUtf8(it->second) : L"#1D4ED8";
  auto hx = [](wchar_t c) -> int {
    if (c >= L'0' && c <= L'9') {
      return c - L'0';
    }
    if (c >= L'a' && c <= L'f') {
      return c - L'a' + 10;
    }
    if (c >= L'A' && c <= L'F') {
      return c - L'A' + 10;
    }
    return 0;
  };
  if (hex.size() >= 7 && hex[0] == L'#') {
    const int r = hx(hex[1]) * 16 + hx(hex[2]);
    const int g = hx(hex[3]) * 16 + hx(hex[4]);
    const int b = hx(hex[5]) * 16 + hx(hex[6]);
    return RGB(r, g, b);
  }
  return RGB(29, 78, 216);
}

void App::ApplyTrayThemeIcon() {
  if (!tray_added_) {
    return;
  }
  HICON old = nid_.hIcon;
  nid_.hIcon = CreateBrandMarkIcon(32, TrayAccentColor());
  if (!nid_.hIcon) {
    nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  }
  nid_.uFlags = NIF_ICON;
  Shell_NotifyIconW(NIM_MODIFY, &nid_);
  if (old) {
    DestroyIcon(old);
  }
  LogLine(L"[tray] icon recolored for theme %s",
          WidenUtf8(prefs_.ui_theme).c_str());
}

void App::TrimMemory() {
  // Market/Python memory trim: keep parked-page icon caches warm (page switch
  // must not flash empty fences). Only drop HICONs when fences are hidden.
  // EmptyWorkingSet only when chrome is parked — doing it while fences/pet
  // paint forces page-in stutter every minute.
  if (!fences_hidden_ || settings_.IsOpen()) {
    return;
  }
  LogLine(L"[mem] trim fences_hidden=1 settings_open=0");
  for (auto& f : fences_) {
    if (f) {
      f->ClearIcons();
    }
  }
  SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                           static_cast<SIZE_T>(-1));
}

void App::ShowTrayBalloon(const std::wstring& title,
                          const std::wstring& text) {
  if (!tray_added_) {
    ShowToast(text);
    return;
  }
  NOTIFYICONDATAW nid{};
  nid.cbSize = sizeof(nid);
  nid.hWnd = message_hwnd_;
  nid.uID = 1;
  nid.uFlags = NIF_INFO;
  nid.dwInfoFlags = NIIF_INFO;
  wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
  wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void App::CycleTheme() {
  const auto& themes = AllUiThemes();
  if (themes.empty()) {
    return;
  }
  const std::string cur = NormalizeUiTheme(prefs_.ui_theme);
  size_t idx = 0;
  for (size_t i = 0; i < themes.size(); ++i) {
    if (themes[i].id == cur) {
      idx = (i + 1) % themes.size();
      break;
    }
  }
  ApplyUiThemeId(themes[idx].id);
}

void App::RefreshSettingsList() {
  std::vector<std::wstring> names;
  names.reserve(fences_.size());
  for (const auto& fence : fences_) {
    if (fence) {
      names.push_back(fence->Title() + L" · p" +
                      std::to_wstring(fence->Page() + 1));
    }
  }
  settings_.SetFenceNames(names);
  // Desktop scan (ScanDesktopItems) is expensive — only when organize page asks.
}

void App::RefreshOrganizeSettingsPage() {
  PushOrganizePage(&settings_, pins_, rules_, fences_, current_page_);
}

DWORD App::OnPathsDropped(FenceWindow* fence,
                          const std::vector<std::wstring>& paths) {
  // Drag: portal → Explorer default (same-drive move / cross-drive copy);
  // plain fence → pin only (no file movement), dedup via SetPin overwrite.
  return HandleIncomingPaths(fence, paths, /*paste=*/false, /*move=*/false);
}

void App::OnPathsCreated(FenceWindow* fence,
                         const std::vector<std::wstring>& paths) {
  if (!fence || paths.empty() || !fence->PortalPath().empty()) {
    return;
  }
  // Fences New: shell already created the Desktop item — pin that path only.
  // Ctrl-drag / Create-shortcut may have InsertPinOrder'd the drop seat already;
  // only append paths that are not yet listed.
  for (const auto& p : paths) {
    SuppressOrganizePath(p, 24.0 * 3600.0);
  }
  fence->SetSortBy("manual");
  const auto order = fence->PinOrder();
  std::vector<std::wstring> missing;
  missing.reserve(paths.size());
  for (const auto& p : paths) {
    bool listed = false;
    const std::wstring key = NormalizePinKey(p);
    for (const auto& o : order) {
      if (!o.empty() && NormalizePinKey(o) == key) {
        listed = true;
        break;
      }
    }
    if (!listed) {
      missing.push_back(p);
    }
  }
  if (!missing.empty()) {
    fence->InsertPinOrder(missing, -1);
  }
  PersistLayouts();
  // paste=true: paths already on Desktop (New / Ctrl-drag copy / .lnk) —
  // skip WantDropLinkEffect rematerialize while modifiers may still be held.
  HandleIncomingPaths(fence, paths, /*paste=*/true, /*move=*/false);
  fence->SelectPaths(paths);
}

void App::OnPathsPasted(FenceWindow* fence,
                        const std::vector<std::wstring>& paths, bool move,
                        bool as_link) {
  if (!fence || paths.empty()) {
    return;
  }
  HWND owner = settings_.Hwnd() ? settings_.Hwnd() : message_hwnd_;
  // Explorer: exactly one selected folder → paste into that directory.
  const std::wstring into_folder = fence->SelectedSingleDirectory();
  if (!into_folder.empty()) {
    std::vector<std::wstring> landed;
    landed.reserve(paths.size());
    if (as_link) {
      for (const auto& src : paths) {
        std::wstring dest;
        if (!CreateShortcutIntoDir(owner, src, into_folder, &dest) ||
            dest.empty()) {
          continue;
        }
        SuppressOrganizePath(dest);
        landed.push_back(dest);
      }
      if (landed.empty()) {
        ShowToast(L"无法创建快捷方式");
        return;
      }
      ShowToast(L"已创建快捷方式 " + std::to_wstring(landed.size()) + L" 项");
    } else {
      for (const auto& src : paths) {
        std::wstring dest;
        if (!MoveOrCopyFileIntoDir(owner, src, into_folder, move, &dest) ||
            dest.empty()) {
          continue;
        }
        SuppressOrganizePath(dest);
        landed.push_back(dest);
      }
      if (landed.empty()) {
        ShowToast(L"无法粘贴到所选文件夹");
        return;
      }
      if (move) {
        ClipboardConsumeCut();
      }
      ShowToast(L"已粘贴 " + std::to_wstring(landed.size()) + L" 项");
    }
    // Contents live inside the folder — do not pin each child into the fence.
    // Portal may be browsing the destination (or its parent) — force rescan.
    InvalidateDesktopScanCache();
    InvalidatePortalScanCache();
    RefreshDesktopIcons();
    return;
  }
  // Explorer 「粘贴快捷方式」: land .lnk then pin (virtual) / into portal cwd.
  if (as_link) {
    std::wstring dest_dir = fence->PortalBrowsePath();
    if (dest_dir.empty()) {
      PWSTR desk_raw = nullptr;
      if (SUCCEEDED(
              SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) &&
          desk_raw) {
        dest_dir = desk_raw;
        CoTaskMemFree(desk_raw);
      }
    }
    if (dest_dir.empty()) {
      ShowToast(L"无法创建快捷方式");
      return;
    }
    std::vector<std::wstring> landed;
    landed.reserve(paths.size());
    for (const auto& src : paths) {
      std::wstring dest;
      if (!CreateShortcutIntoDir(owner, src, dest_dir, &dest) || dest.empty()) {
        continue;
      }
      SuppressOrganizePath(dest);
      landed.push_back(dest);
    }
    if (landed.empty()) {
      ShowToast(L"无法创建快捷方式");
      return;
    }
    if (fence->PortalPath().empty()) {
      fence->SetSortBy("manual");
      fence->InsertPinOrder(landed, -1);
      PersistLayouts();
      // paste=true: .lnk already landed — skip WantDropLinkEffect rematerialize.
      HandleIncomingPaths(fence, landed, /*paste=*/true, /*move=*/false);
      fence->SelectPaths(landed);
    } else {
      InvalidateDesktopScanCache();
      RefreshDesktopIcons();
      fence->SelectPaths(landed);
    }
    ShowToast(L"已创建快捷方式 " + std::to_wstring(landed.size()) + L" 项");
    return;
  }
  // Portal: real import into mirrored folder (HandleIncomingPaths).
  if (!fence->PortalPath().empty()) {
    const DWORD eff =
        HandleIncomingPaths(fence, paths, /*paste=*/true, move);
    if (move && eff != DROPEFFECT_NONE) {
      ClipboardConsumeCut();
    }
    return;
  }
  // Virtual fence paste:
  //  - Cut + already on Desktop → pin-transfer (Desktop self-MOVE is a no-op)
  //  - Copy + already on Desktop → FO_COPY duplicate, then pin the copy
  //  - Off-Desktop → land on Desktop (MOVE/COPY) then pin
  // Same-fence Cut→Paste: Explorer no-op — keep seats, just clear cut mark.
  if (move) {
    bool all_here = true;
    for (const auto& p : paths) {
      const std::string* cur = FindPin(pins_, p);
      if (!cur || *cur != fence->Id()) {
        all_here = false;
        break;
      }
    }
    if (all_here) {
      ClipboardConsumeCut();
      return;
    }
  }
  PWSTR desk_raw = nullptr;
  std::wstring desk;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) &&
      desk_raw) {
    desk = desk_raw;
    CoTaskMemFree(desk_raw);
  }
  std::vector<std::wstring> landed;
  landed.reserve(paths.size());
  for (const auto& src : paths) {
    if (src.empty()) {
      continue;
    }
    const bool on_desk = PathIsDesktopChild(src, desk);
    if (on_desk && move) {
      landed.push_back(src);
      continue;
    }
    if (on_desk && !move) {
      // Fences/Explorer: Copy→Paste duplicates even when already on Desktop.
      std::wstring dest;
      if (!MoveOrCopyFileIntoDir(owner, src, desk, /*move=*/false, &dest) ||
          dest.empty()) {
        continue;
      }
      SuppressOrganizePath(dest);
      landed.push_back(dest);
      continue;
    }
    if (desk.empty()) {
      landed.push_back(src);
      continue;
    }
    std::wstring dest;
    if (!MoveOrCopyFileIntoDir(owner, src, desk, move, &dest) ||
        dest.empty()) {
      continue;
    }
    SuppressOrganizePath(src);
    SuppressOrganizePath(dest);
    // Cut from another virtual fence: clear the old pin before re-pin.
    if (move && NormalizePinKey(src) != NormalizePinKey(dest)) {
      if (const std::string* cur = FindPin(pins_, src)) {
        for (auto& f : fences_) {
          if (f && f->Id() == *cur) {
            f->RemovePinOrder({src});
            break;
          }
        }
        ClearPin(&pins_, src);
      }
    }
    landed.push_back(dest);
  }
  if (landed.empty()) {
    ShowToast(L"无法粘贴到分区");
    return;
  }
  // Match New / DropAt: paste appends seats and switches to manual so
  // name/date sort cannot scramble freshly pasted icons (Fences).
  fence->SetSortBy("manual");
  fence->InsertPinOrder(landed, -1);
  PersistLayouts();
  HandleIncomingPaths(fence, landed, /*paste=*/true, /*move=*/false);
  // Explorer: selection follows the items just pasted.
  fence->SelectPaths(landed);
  if (move) {
    ClipboardConsumeCut();
  }
}

DWORD App::HandleIncomingPaths(FenceWindow* fence,
                               const std::vector<std::wstring>& paths,
                               bool paste, bool move) {
  if (!fence || paths.empty()) {
    return DROPEFFECT_NONE;
  }
  const std::wstring portal = fence->PortalPath();
  if (portal.empty()) {
    // Plain fence: pin Desktop paths (Explorer→fence pin-only stays).
    // Fallback Alt-drop (no DropAt): materialize .lnk then pin those paths.
    // Portal-folder sources: land on Desktop first (DropAt usually did this).
    std::vector<std::wstring> to_pin = paths;
    DWORD land_eff = DROPEFFECT_COPY;
    const bool want_link = !paste && WantDropLinkEffect();
    if (want_link) {
      PWSTR desk_raw = nullptr;
      std::wstring desk;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr,
                                         &desk_raw)) &&
          desk_raw) {
        desk = desk_raw;
        CoTaskMemFree(desk_raw);
      }
      to_pin.clear();
      if (!desk.empty()) {
        HWND owner = settings_.Hwnd() ? settings_.Hwnd() : message_hwnd_;
        to_pin.reserve(paths.size());
        for (const auto& src : paths) {
          std::wstring dest;
          if (!CreateShortcutIntoDir(owner, src, desk, &dest) || dest.empty()) {
            continue;
          }
          SuppressOrganizePath(dest);
          to_pin.push_back(dest);
        }
      }
      if (to_pin.empty()) {
        return DROPEFFECT_NONE;
      }
      fence->SetSortBy("manual");
      fence->InsertPinOrder(to_pin, -1);
      land_eff = DROPEFFECT_LINK;
    } else if (!paste) {
      HWND owner = settings_.Hwnd() ? settings_.Hwnd() : message_hwnd_;
      std::vector<std::wstring> resolved;
      land_eff =
          LandPortalSourcesOnDesktop(owner, fences_, paths, &resolved);
      if (resolved.empty()) {
        return DROPEFFECT_NONE;
      }
      to_pin = std::move(resolved);
      fence->SetSortBy("manual");
      fence->InsertPinOrder(to_pin, -1);
    }
    const std::string dst_id = fence->Id();
    for (const auto& p : to_pin) {
      SuppressOrganizePath(p);
      // Re-pin from another virtual fence: drop stale pin_order there first.
      if (const std::string* cur = FindPin(pins_, p)) {
        if (*cur != dst_id) {
          for (auto& f : fences_) {
            if (f && f->Id() == *cur) {
              f->RemovePinOrder({p});
              break;
            }
          }
        }
      }
      SetPin(&pins_, p, dst_id);
      EraseFloatPlacement(&float_placements_, p);
    }
    BumpPinsEpoch();
    SavePins(pins_);
    SaveFloatPlacements(float_placements_);
    // Fresh scan so the new pin is not racing a warm 2.5s desktop cache
    // (empty fence flash → later twin with a resurrected float).
    InvalidateDesktopScanCache();
    RefreshDesktopIcons();
    if (want_link) {
      PersistLayouts();
      ShowToast(L"已创建快捷方式 " + std::to_wstring(to_pin.size()) + L" 项");
      return DROPEFFECT_LINK;
    }
    if (land_eff == DROPEFFECT_MOVE || land_eff == DROPEFFECT_COPY) {
      return land_eff;
    }
    return DROPEFFECT_COPY;
  }

  // Portal fence: real file ops into the current browse folder (Fences).
  const std::wstring dest_dir = fence->PortalBrowsePath();
  std::vector<std::wstring> failed;
  std::vector<std::wstring> landed;
  int moved_ok = 0;
  int linked_ok = 0;
  bool pins_cleared = false;
  bool floats_cleared = false;
  const bool want_link = !paste && WantDropLinkEffect();
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  for (const auto& src : paths) {
    // Explorer: refuse dropping a folder into itself or its descendant.
    if (src.empty() || _wcsicmp(src.c_str(), dest_dir.c_str()) == 0 ||
        IsStrictPathPrefix(src, dest_dir)) {
      continue;
    }
    std::wstring dest;
    if (want_link) {
      if (CreateShortcutIntoDir(settings_.Hwnd(), src, dest_dir, &dest)) {
        if (!dest.empty()) {
          SuppressOrganizePath(dest);
        }
        landed.push_back(dest.empty() ? src : dest);
        ++linked_ok;
      } else {
        failed.push_back(src);
        LogLine(L"[portal] link failed (%s) -> %s", src.c_str(),
                dest_dir.c_str());
      }
      continue;
    }
    // Drag (non-paste): Explorer modifiers + same-volume default MOVE.
    bool should_move = paste ? move : SameVolume(src, dest_dir);
    if (!paste) {
      if (ctrl) {
        should_move = false;
      }
      if (shift) {
        should_move = true;
      }
    }
    if (MoveOrCopyFileIntoDir(settings_.Hwnd(), src, dest_dir, should_move,
                              &dest)) {
      if (!dest.empty() && _wcsicmp(dest.c_str(), src.c_str()) != 0) {
        SuppressOrganizePath(src);
        SuppressOrganizePath(dest);
      }
      landed.push_back(dest.empty() ? src : dest);
      if (should_move) {
        ++moved_ok;
        // CF_HDROP / Cut→Paste MOVE into Folder Portal: leave the virtual
        // fence (pin-drag OnPinDropped already did this; Explorer drop did not).
        const std::wstring final_dest = dest.empty() ? src : dest;
        const bool relocated =
            _wcsicmp(final_dest.c_str(), src.c_str()) != 0 ||
            GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES;
        if (relocated) {
          if (const std::string* cur = FindPin(pins_, src)) {
            for (auto& f : fences_) {
              if (f && f->Id() == *cur) {
                f->RemovePinOrder({src});
                break;
              }
            }
            ClearPin(&pins_, src);
            pins_cleared = true;
          }
          // Unpinned floats also leave the desktop — drop their seat now
          // (do not wait for sticky prune ghosts).
          if (float_placements_.find(NormalizePinKey(src)) !=
              float_placements_.end()) {
            EraseFloatPlacement(&float_placements_, src);
            floats_cleared = true;
          }
        }
      }
    } else {
      failed.push_back(src);
      LogLine(L"[portal] %s failed (%s) -> %s", src.c_str(),
              should_move ? L"move" : L"copy", dest_dir.c_str());
    }
  }
  if (pins_cleared) {
    BumpPinsEpoch();
    SavePins(pins_);
    PersistLayouts();
  }
  if (pins_cleared || floats_cleared) {
    SaveFloatPlacements(float_placements_);
  }

  InvalidateDesktopScanCache();
  InvalidatePortalScanCache();
  RefreshDesktopIcons();

  if (failed.empty()) {
    const wchar_t* verb = want_link ? L"已创建快捷方式"
                                    : (paste ? (move ? L"已移动" : L"已复制")
                                             : L"已导入");
    ShowToast(std::wstring(verb) + L" " + std::to_wstring(landed.size()) +
              L" 项");
  } else {
    std::wstring msg = L"成功 " + std::to_wstring(landed.size()) + L" 项，失败 " +
                       std::to_wstring(failed.size()) + L" 项：\n";
    for (size_t i = 0; i < failed.size() && i < 12; ++i) {
      msg += failed[i];
      msg += L"\n";
    }
    if (failed.size() > 12) {
      msg += L"…";
    }
    MistAlert(settings_.Hwnd(), L"导入失败", msg.c_str(), MistMsgTone::Warn);
  }
  if (landed.empty()) {
    return DROPEFFECT_NONE;
  }
  if (want_link && linked_ok == static_cast<int>(landed.size()) &&
      failed.empty()) {
    return DROPEFFECT_LINK;
  }
  if (moved_ok == static_cast<int>(landed.size()) && failed.empty()) {
    return DROPEFFECT_MOVE;
  }
  return DROPEFFECT_COPY;
}

void App::UnpinPath(const std::wstring& path, const POINT* drop) {
  UnpinPaths(std::vector<std::wstring>{path}, drop);
}

void App::ScheduleRevealPublicFloats() {
  if (!message_hwnd_) {
    RevealPublicFloatsNow();
    return;
  }
  // Post-OLE remount once; timer only re-blits (no Hide/layered toggle).
  KillTimer(message_hwnd_, kTimerRevealPublic);
  reveal_public_left_ = 1;
  PostMessageW(message_hwnd_, kMsgRevealPublic, 0, 0);
  SetTimer(message_hwnd_, kTimerRevealPublic, 120, nullptr);
}

void App::RevealPublicFloatsNow() {
  if (icons_visible_intent_) {
    public_host_.EndDeferPresent();
    return;
  }
  public_visible_ = true;
  if (!public_host_.HasItems()) {
    public_host_.EndDeferPresent();
    InvalidateDesktopScanCache();
    RefreshDesktopIcons();
  }
  if (!public_host_.HasItems()) {
    public_host_.EndDeferPresent();
    LogLine(L"[public] reveal: still no float items after refresh");
    return;
  }
  // Hidden layered reset + one Show+Present (see ForceRemapPresent).
  public_host_.ForceRemapPresent();
}

void App::UnpinPaths(const std::vector<std::wstring>& paths,
                     const POINT* drop) {
  if (paths.empty()) {
    return;
  }
  // Fences-class: when `drop` is set, seat floats at the release point
  // (cascade multi-unpin along the icon grid). Menu unpin with drop==null
  // still auto-packs via PublicHost::Layout (has_pos=false).
  // Do NOT Present on this stack: ULW during OLE teardown stays blank.
  //
  // Pin-only model may hold off-Desktop paths (Downloads/…). Floats only
  // render Desktop-scan items — land onto Desktop first (Explorer FO
  // semantics: same-volume MOVE, Ctrl=COPY, Shift=MOVE, Alt=.lnk).
  PWSTR desk_raw = nullptr;
  std::wstring desk;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) &&
      desk_raw) {
    desk = desk_raw;
    CoTaskMemFree(desk_raw);
  }
  const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool want_link = WantDropLinkEffect();
  HWND owner = message_hwnd_ ? message_hwnd_ : settings_.Hwnd();

  bool pins_changed = false;
  bool floats_changed = false;
  bool order_changed = false;
  std::vector<std::pair<std::wstring, int>> icon_jobs;
  icon_jobs.reserve(paths.size());
  const IconGridMetrics grid = QueryIconGridMetrics();
  const int step_x = (std::max)(48, grid.cell_w);
  int cascade = 0;
  for (const auto& path : paths) {
    if (path.empty()) {
      continue;
    }
    // Keep the float out of auto-organize for a long window — 12s was too short
    // and watch ticks re-pinned icons the user just dragged back to the desktop.
    SuppressOrganizePath(path, 24.0 * 3600.0);
    // Centralize pin_order cleanup so dock/search/menu unpin match fence drag.
    if (const std::string* cur = FindPin(pins_, path)) {
      for (auto& f : fences_) {
        if (f && f->Id() == *cur) {
          f->RemovePinOrder({path});
          order_changed = true;
          break;
        }
      }
    }
    ClearPin(&pins_, path);
    pins_changed = true;

    std::wstring float_path = path;
    const bool src_missing =
        GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (src_missing) {
      EraseFloatPlacement(&float_placements_, path);
      floats_changed = true;
      continue;
    }
    if (!desk.empty() && !PathIsDesktopChild(path, desk)) {
      // Namespace / shell pseudo-paths cannot FO onto Desktop.
      const bool shell_ns =
          path.size() >= 3 && path[0] == L':' && path[1] == L':';
      if (shell_ns) {
        EraseFloatPlacement(&float_placements_, path);
        floats_changed = true;
        continue;
      }
      std::wstring dest;
      bool ok = false;
      if (want_link) {
        ok = CreateShortcutIntoDir(owner, path, desk, &dest);
      } else {
        bool do_move = SameVolume(path, desk);
        if (ctrl) {
          do_move = false;
        }
        if (shift) {
          do_move = true;
        }
        ok = MoveOrCopyFileIntoDir(owner, path, desk, do_move, &dest);
        if (ok) {
          SuppressOrganizePath(path);
        }
      }
      if (!ok || dest.empty()) {
        EraseFloatPlacement(&float_placements_, path);
        floats_changed = true;
        LogLine(L"[public] unpin land-failed: %s", path.c_str());
        continue;
      }
      SuppressOrganizePath(dest, 24.0 * 3600.0);
      EraseFloatPlacement(&float_placements_, path);
      float_path = dest;
      LogLine(L"[public] unpin landed off-desk -> %s", float_path.c_str());
    }

    if (drop) {
      POINT seat{*drop};
      seat.x += cascade * step_x;
      UpsertFloatPlacement(&float_placements_, float_path, current_page_, seat);
      auto it = float_placements_.find(NormalizePinKey(float_path));
      if (it != float_placements_.end()) {
        it->second.page_local = false;
        it->second.path = float_path;
      }
      ++cascade;
    } else {
      SetFloatPage(&float_placements_, float_path, current_page_,
                   /*page_local=*/false);
      auto it = float_placements_.find(NormalizePinKey(float_path));
      if (it != float_placements_.end()) {
        it->second.has_pos = false;
        it->second.path = float_path;
      }
    }
    floats_changed = true;
    icon_jobs.emplace_back(float_path, grid.icon_size);
  }
  if (pins_changed) {
    BumpPinsEpoch();
    SavePins(pins_);
  }
  if (order_changed) {
    PersistLayouts();
  }
  if (floats_changed) {
    SaveFloatPlacements(float_placements_);
  }
  HidePinDragHint();
  if (!icon_jobs.empty()) {
    PreloadIconsParallel(icon_jobs);
  }
  public_host_.BeginDeferPresent();
  InvalidateDesktopScanCache();
  RefreshDesktopIcons();
  if (public_host_.HasItems()) {
    public_host_.PersistSlotPlacements();
  }
  LogLine(L"[public] unpin defer-present paths=%d has_items=%d",
          static_cast<int>(paths.size()), public_host_.HasItems() ? 1 : 0);
  ScheduleRevealPublicFloats();
}

void App::MaybeSuppressDesktopShellNew(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  PWSTR desk_raw = nullptr;
  std::wstring desk;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) &&
      desk_raw) {
    desk = desk_raw;
    CoTaskMemFree(desk_raw);
  }
  PWSTR pub_raw = nullptr;
  std::wstring pub;
  if (SUCCEEDED(
          SHGetKnownFolderPath(FOLDERID_PublicDesktop, 0, nullptr, &pub_raw)) &&
      pub_raw) {
    pub = pub_raw;
    CoTaskMemFree(pub_raw);
  }
  if (!PathIsDesktopChild(path, desk) && !PathIsDesktopChild(path, pub)) {
    return;
  }
  ULONGLONG tick = 0;
  POINT pt{};
  const bool recent_rclick =
      GetLastDesktopRightClick(&pt, &tick) && tick != 0 &&
      (GetTickCount64() - tick) < 15000ull;
  // Shell New default names (zh/en) even if RMB was slightly older.
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring name =
      slash == std::wstring::npos ? path : path.substr(slash + 1);
  const bool looks_new =
      name.rfind(L"新建", 0) == 0 || name.rfind(L"New folder", 0) == 0 ||
      name.rfind(L"New Folder", 0) == 0 ||
      name.rfind(L"New Text Document", 0) == 0 ||
      name.rfind(L"New Microsoft", 0) == 0;
  if (!recent_rclick && !looks_new) {
    return;
  }
  SuppressOrganizePath(path, 24.0 * 3600.0);
  LogLine(L"[organize] suppress DefView New: %s", path.c_str());
}

void App::ForgetDeletedDesktopPath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  const std::wstring key = NormalizePinKey(path);
  bool pins_changed = false;
  bool order_changed = false;
  bool floats_changed = false;
  if (const std::string* fid = FindPin(pins_, path)) {
    RememberPinUndo(path, *fid);
    for (auto& f : fences_) {
      if (!f) {
        continue;
      }
      const auto before = f->PinOrder();
      f->RemovePinOrder({path});
      if (f->PinOrder() != before) {
        order_changed = true;
      }
    }
    ClearPin(&pins_, path);
    pins_changed = true;
  }
  if (float_placements_.erase(key) > 0) {
    floats_changed = true;
  }
  pin_missing_since_.erase(key);
  if (!pins_changed && !floats_changed) {
    return;
  }
  if (pins_changed) {
    BumpPinsEpoch();
    SavePins(pins_);
  }
  if (order_changed) {
    PersistLayouts();
  }
  if (floats_changed) {
    SaveFloatPlacements(float_placements_);
  }
  LogLine(L"[pin] shell delete forgot: %s", path.c_str());
}

void App::RetargetPin(const std::wstring& from, const std::wstring& to) {
  const std::string* id = FindPin(pins_, from);
  const std::string fence_id = id ? *id : std::string();
  ClearPin(&pins_, from);
  if (!fence_id.empty()) {
    SetPin(&pins_, to, fence_id);
  }
  // Fences: rename keeps the same seat — rewrite pin_order in every fence
  // that still lists `from` (usually the owning fence).
  bool order_changed = false;
  for (auto& f : fences_) {
    if (!f) {
      continue;
    }
    const auto before = f->PinOrder();
    f->RetargetPinOrder(from, to);
    if (f->PinOrder() != before) {
      order_changed = true;
    }
  }
  RetargetFloatPlacement(&float_placements_, from, to);
  SaveFloatPlacements(float_placements_);
  BumpPinsEpoch();
  SavePins(pins_);
  if (order_changed) {
    PersistLayouts();
  }
  RefreshDesktopIcons();
}

bool App::TransferPins(const std::wstring& src, const std::wstring& dst,
                       const std::vector<std::wstring>& paths, int at) {
  if (src.empty() || dst.empty() || src == dst || paths.empty()) {
    return false;
  }
  FenceWindow* src_fence = nullptr;
  FenceWindow* dst_fence = nullptr;
  for (auto& f : fences_) {
    if (!f) {
      continue;
    }
    if (WidenUtf8(f->Id()) == src) {
      src_fence = f.get();
    }
    if (WidenUtf8(f->Id()) == dst) {
      dst_fence = f.get();
    }
  }
  if (!src_fence || !dst_fence) {
    return false;
  }
  // Portal fences mirror a real folder — pins don't apply, reject the transfer.
  if (!dst_fence->PortalPath().empty() || !src_fence->PortalPath().empty()) {
    return false;
  }
  // Dragging icons between fences changes BOTH sets: the source loses members
  // and the target gains them, so either lock must block the transfer.
  if (src_fence->IsContentLocked() || dst_fence->IsContentLocked()) {
    ShowToast(L"分区内容已锁定，请先解锁");
    return false;
  }

  // Only move paths that are actually pinned to src (avoids hijacking a file
  // that lives in two fences). A path not pinned to src is skipped.
  std::vector<std::wstring> move_list;
  move_list.reserve(paths.size());
  for (const auto& p : paths) {
    const std::string* cur = FindPin(pins_, p);
    if (cur && *cur == NarrowUtf8(src)) {
      move_list.push_back(p);
    }
  }
  if (move_list.empty()) {
    return false;
  }

  bool changed = false;
  for (const auto& p : move_list) {
    ClearPin(&pins_, p);
    SetPin(&pins_, p, NarrowUtf8(dst));
    changed = true;
  }
  if (!changed) {
    return false;
  }

  src_fence->RemovePinOrder(move_list);
  // Drop seat → pin_order insert; force manual so SetItems honors the seat.
  dst_fence->SetSortBy("manual");
  dst_fence->InsertPinOrder(move_list, at);

  BumpPinsEpoch();
  SavePins(pins_);
  PersistLayouts();
  RefreshDesktopIcons();
  return true;
}

DWORD App::OnPinDropped(FenceWindow* fence, const std::wstring& src_fence_id,
                        const std::vector<std::wstring>& paths, bool commit,
                        int at) {
  if (!fence) {
    return DROPEFFECT_NONE;
  }
  const std::wstring dst = WidenUtf8(fence->Id());
  if (src_fence_id.empty() || dst.empty() || src_fence_id == dst) {
    return DROPEFFECT_NONE;
  }
  for (const auto& p : paths) {
    if (p.find(L"::") != std::wstring::npos) {
      return DROPEFFECT_NONE;
    }
  }
  // Fences: drop onto a Folder Portal imports into the mirrored folder
  // (real FO), then clears the source virtual pins. Pins alone don't apply.
  if (!fence->PortalPath().empty()) {
    bool src_ok = false;
    for (auto& f : fences_) {
      if (f && WidenUtf8(f->Id()) == src_fence_id && f->PortalPath().empty()) {
        src_ok = true;
        break;
      }
    }
    if (!src_ok) {
      return DROPEFFECT_NONE;
    }
    for (const auto& p : paths) {
      const std::string* cur = FindPin(pins_, p);
      if (!cur || *cur != NarrowUtf8(src_fence_id)) {
        return DROPEFFECT_NONE;
      }
    }
    if (!commit) {
      // Match Explorer/Fences modifiers on Folder Portal import.
      if (WantDropLinkEffect()) {
        return DROPEFFECT_LINK;
      }
      if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        return DROPEFFECT_COPY;
      }
      if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        return DROPEFFECT_MOVE;
      }
      const std::wstring dest = fence->PortalBrowsePath();
      if (!paths.empty() && !dest.empty() && SameVolume(paths.front(), dest)) {
        return DROPEFFECT_MOVE;
      }
      return DROPEFFECT_COPY;
    }
    const DWORD eff =
        HandleIncomingPaths(fence, paths, /*paste=*/false, /*move=*/false);
    if (eff == DROPEFFECT_NONE) {
      return DROPEFFECT_NONE;
    }
    // Only MOVE leaves the virtual fence; COPY/LINK keep the source pin.
    if (eff == DROPEFFECT_MOVE) {
      for (const auto& p : paths) {
        ClearPin(&pins_, p);
        EraseFloatPlacement(&float_placements_, p);
      }
      for (auto& f : fences_) {
        if (f && WidenUtf8(f->Id()) == src_fence_id) {
          f->RemovePinOrder(paths);
          break;
        }
      }
      BumpPinsEpoch();
      SavePins(pins_);
      SaveFloatPlacements(float_placements_);
      PersistLayouts();
      RefreshDesktopIcons();
    }
    return eff;
  }
  if (commit) {
    return TransferPins(src_fence_id, dst, paths, at) ? DROPEFFECT_MOVE
                                                      : DROPEFFECT_NONE;
  }
  // Preview: destination exists and paths are still pinned to src.
  bool src_ok = false;
  for (auto& f : fences_) {
    if (f && WidenUtf8(f->Id()) == src_fence_id && f->PortalPath().empty()) {
      src_ok = true;
      break;
    }
  }
  if (!src_ok) {
    return DROPEFFECT_NONE;
  }
  for (const auto& p : paths) {
    const std::string* cur = FindPin(pins_, p);
    if (!cur || *cur != NarrowUtf8(src_fence_id)) {
      return DROPEFFECT_NONE;
    }
  }
  return DROPEFFECT_MOVE;
}

void App::UpdatePinDragHint(POINT cursor, const PinDragHint& hint) {
  std::wstring text;
  if (hint.invalid) {
    text = L"无法放置";
  } else if (hint.over_target) {
    // Portal import honors modifiers; virtual↔virtual transfer stays MOVE.
    if (WantDropLinkEffect()) {
      text = L"松开创建快捷方式";
    } else if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
      text = L"松开复制到目标";
    } else {
      text = L"松开移到目标分区";
    }
  } else if (hint.over_desktop) {
    text = L"松开移回桌面";
  } else {
    // WeChat / Explorer copy / other app — Fences keeps the pin.
    text = L"松开分享（保留在分区）";
  }
  ShowPinHintWindow(instance_, text, cursor);
}

void App::HidePinDragHint() { HidePinHintWindow(); }

void App::MoveDesktopItemsToFence(const std::vector<std::wstring>& paths,
                                  const std::string& fence_id) {
  if (fence_id.empty()) {
    return;
  }
  FenceWindow* target = nullptr;
  for (auto& f : fences_) {
    if (f && f->Id() == fence_id) {
      target = f.get();
      break;
    }
  }
  if (!target) {
    return;
  }
  // A content-locked fence must not accept new members from any entry point —
  // the menu path is as much a membership change as a drag-drop.
  if (target->IsContentLocked()) {
    ShowToast(L"分区内容已锁定，请先解锁");
    return;
  }
  // Fences Folder Portal: 「移动到分区」= MOVE into the mirrored folder.
  if (!target->PortalPath().empty()) {
    HandleIncomingPaths(target, paths, /*paste=*/true, /*move=*/true);
    return;
  }
  std::vector<std::wstring> moved;
  moved.reserve(paths.size());
  for (const auto& p : paths) {
    // Skip system namespace paths (not pinnable).
    if (p.find(L"::") != std::wstring::npos) {
      continue;
    }
    // Leave the previous virtual fence's pin_order (menu move ≠ TransferPins).
    if (const std::string* cur = FindPin(pins_, p)) {
      if (*cur != fence_id) {
        for (auto& f : fences_) {
          if (f && f->Id() == *cur) {
            f->RemovePinOrder({p});
            break;
          }
        }
      }
    }
    SetPin(&pins_, p, fence_id);
    EraseFloatPlacement(&float_placements_, p);
    moved.push_back(p);
  }
  if (moved.empty()) {
    return;
  }
  // Menu has no drop seat — append; force manual so seat order sticks (Fences).
  target->SetSortBy("manual");
  target->InsertPinOrder(moved, -1);
  BumpPinsEpoch();
  SavePins(pins_);
  SaveFloatPlacements(float_placements_);
  PersistLayouts();
  RefreshDesktopIcons();
}

void App::MoveDesktopItemsToPage(const std::vector<std::wstring>& paths,
                                 int page_index) {
  // Fences-class: unpin and park the float on the target page only (page_local).
  bool pins_changed = false;
  bool order_changed = false;
  for (const auto& p : paths) {
    if (const std::string* cur = FindPin(pins_, p)) {
      for (auto& f : fences_) {
        if (f && f->Id() == *cur) {
          f->RemovePinOrder({p});
          order_changed = true;
          break;
        }
      }
      ClearPin(&pins_, p);
      pins_changed = true;
    }
    SuppressOrganizePath(p, 24.0 * 3600.0);
    SetFloatPage(&float_placements_, p, page_index, /*page_local=*/true);
  }
  if (pins_changed) {
    BumpPinsEpoch();
    SavePins(pins_);
  }
  if (order_changed) {
    PersistLayouts();
  }
  SaveFloatPlacements(float_placements_);
  RefreshDesktopIcons();
}

void App::SetWhitelistForPaths(const std::vector<std::wstring>& paths,
                               bool add) {
  auto base = [](const std::wstring& p) {
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(slash + 1);
  };
  auto names = LoadWhitelist();
  for (const auto& p : paths) {
    const std::wstring b = base(p);
    if (b.empty()) {
      continue;
    }
    if (add) {
      bool exists = false;
      for (const auto& e : names) {
        if (_wcsicmp(e.c_str(), b.c_str()) == 0) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        names.push_back(b);
      }
    } else {
      names.erase(std::remove_if(names.begin(), names.end(),
                                 [&](const std::wstring& e) {
                                   return _wcsicmp(e.c_str(), b.c_str()) == 0;
                                 }),
                  names.end());
    }
  }
  SaveWhitelist(names);
}

void App::PruneStickyMissingPins() {
  constexpr ULONGLONG kStickyGraceMs = 45000;  // Python _STICKY_MISSING_S
  const ULONGLONG now = GetTickCount64();
  if (pin_prune_last_ms_ && now - pin_prune_last_ms_ < 30000) {
    return;  // 30s throttle; watch ticks every 2s — avoid full pin GetFileAttributes
  }
  pin_prune_last_ms_ = now;

  auto file_name = [](const std::wstring& path) -> std::wstring {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
  };
  auto link_stem = [](std::wstring name) -> std::wstring {
    if (name.size() > 4) {
      const wchar_t* ext = name.c_str() + name.size() - 4;
      if (_wcsicmp(ext, L".lnk") == 0 || _wcsicmp(ext, L".url") == 0) {
        name.resize(name.size() - 4);
      }
    }
    // Installers often use 「启动 Foo.lnk」 while Public Desktop has Foo.lnk.
    constexpr wchar_t kLaunch[] = L"启动 ";
    if (name.size() > 3 && _wcsnicmp(name.c_str(), kLaunch, 3) == 0) {
      name.erase(0, 3);
    }
    return name;
  };
  // One desktop scan (uses 2.5s cache) — most pins live on Desktop/Public so
  // membership check avoids N× GetFileAttributesW. Off-desktop pins still attr.
  const auto desk_items = ScanDesktopItems();
  std::unordered_set<std::wstring> on_desk;
  on_desk.reserve(desk_items.size() * 2);
  for (const auto& it : desk_items) {
    on_desk.insert(NormalizePinKey(it.path));
  }
  // Index the desktop once instead of rescanning it per missing pin: this runs
  // every 30 s on the UI thread and was O(missing pins x desktop items) with
  // two _wcsicmp per pair.
  std::unordered_map<std::wstring, std::wstring> by_name;
  std::unordered_map<std::wstring, std::wstring> link_stem_first;
  by_name.reserve(desk_items.size() * 2);
  link_stem_first.reserve(desk_items.size() * 2);
  for (const auto& it : desk_items) {
    const std::wstring bn = file_name(it.path);
    std::wstring lower = bn;
    for (auto& ch : lower) {
      ch = towlower(ch);
    }
    by_name.emplace(lower, it.path);
    if (it.is_link) {
      std::wstring stem = link_stem(bn);
      for (auto& ch : stem) {
        ch = towlower(ch);
      }
      link_stem_first.emplace(stem, it.path);
    }
  }
  auto find_desktop_replacement =
      [&](const std::wstring& missing) -> std::wstring {
    const std::wstring want_name = file_name(missing);
    std::wstring want_lower = want_name;
    for (auto& ch : want_lower) {
      ch = towlower(ch);
    }
    std::wstring want_stem = link_stem(want_name);
    if (want_stem.empty()) {
      return {};
    }
    for (auto& ch : want_stem) {
      ch = towlower(ch);
    }
    const auto exact = by_name.find(want_lower);
    if (exact != by_name.end()) {
      return exact->second;
    }
    // Soft fallback for「启动 Foo.lnk」↔ Foo.lnk (links only).
    const auto stem_hit = link_stem_first.find(want_stem);
    if (stem_hit != link_stem_first.end()) {
      return stem_hit->second;
    }
    return {};
  };

  int dropped = 0;
  int retargeted = 0;
  for (auto it = pins_.begin(); it != pins_.end();) {
    const std::wstring path = it->first;
    const std::string fence_id = it->second;
    if (path.empty()) {
      ++it;
      continue;
    }
    // pins_ keys are already NormalizePinKey'd (SetPin / LoadPins).
    const bool on_desktop = on_desk.find(path) != on_desk.end();
    if (on_desktop ||
        GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      pin_missing_since_.erase(path);
      ++it;
      continue;
    }
    auto miss = pin_missing_since_.find(path);
    if (miss == pin_missing_since_.end()) {
      pin_missing_since_[path] = now;
      ++it;
      continue;
    }
    if (now - miss->second < kStickyGraceMs) {
      ++it;
      continue;
    }
    pin_missing_since_.erase(miss);

    const std::wstring replacement = find_desktop_replacement(path);
    const std::wstring replacement_key =
        replacement.empty() ? std::wstring{} : NormalizePinKey(replacement);
    if (!replacement.empty() && replacement_key != path) {
      const std::string* existing = FindPin(pins_, replacement);
      if (!existing) {
        LogLine(L"[pin] sticky prune retarget: %s -> %s", path.c_str(),
                replacement.c_str());
        it = pins_.erase(it);
        SetPin(&pins_, replacement, fence_id);
        RetargetFloatPlacement(&float_placements_, path, replacement);
        for (auto& fence : fences_) {
          if (!fence) {
            continue;
          }
          auto order = fence->PinOrder();
          bool changed = false;
          for (auto& p : order) {
            if (NormalizePinKey(p) == path) {
              p = replacement;
              changed = true;
            }
          }
          if (changed) {
            fence->SetPinOrder(std::move(order));
          }
        }
        ++retargeted;
        continue;
      }
    }

    LogLine(L"[pin] sticky prune drop: %s", path.c_str());
    EraseFloatPlacement(&float_placements_, path);
    for (auto& fence : fences_) {
      if (fence && fence->Id() == fence_id) {
        fence->RemovePinOrder({path});
        break;
      }
    }
    it = pins_.erase(it);
    ++dropped;
  }

  // Same sticky window for orphan floats (e.g. 「启动 Foo.lnk」 after Public
  // reinstall) so a deleted desk icon does not keep a ghost seat forever.
  int float_dropped = 0;
  int float_retargeted = 0;
  int float_reclaimed = 0;
  std::vector<std::wstring> float_keys;
  float_keys.reserve(float_placements_.size());
  for (const auto& kv : float_placements_) {
    float_keys.push_back(kv.first);
  }
  for (const std::wstring& key : float_keys) {
    auto it = float_placements_.find(key);
    if (it == float_placements_.end()) {
      continue;
    }
    const std::wstring path =
        it->second.path.empty() ? it->first : it->second.path;
    const std::wstring path_key =
        it->second.path.empty() ? it->first : NormalizePinKey(path);
    const bool float_alive =
        !path.empty() &&
        (on_desk.find(path_key) != on_desk.end() ||
         GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (float_alive) {
      // Delete + re-copy (or Explorer replace) during the grace window used to
      // keep the old page and hide the icon on the page the user is viewing.
      // page_local (「移动到分页」) must stay on its page — do not yank to
      // current_page_ across a brief FS hole (Fences page-bound icons).
      auto miss = pin_missing_since_.find(path);
      if (miss != pin_missing_since_.end()) {
        if (!it->second.page_local && it->second.page != current_page_) {
          it->second.page = current_page_;
          it->second.has_pos = false;
          it->second.path = path;
          ++float_reclaimed;
        }
        pin_missing_since_.erase(miss);
      } else {
        pin_missing_since_.erase(path);
      }
      continue;
    }
    auto miss = pin_missing_since_.find(path);
    if (miss == pin_missing_since_.end()) {
      pin_missing_since_[path] = now;
      continue;
    }
    if (now - miss->second < kStickyGraceMs) {
      continue;
    }
    pin_missing_since_.erase(miss);

    const std::wstring replacement = find_desktop_replacement(path);
    const std::wstring replacement_key =
        replacement.empty() ? std::wstring{} : NormalizePinKey(replacement);
    if (!replacement.empty() && replacement_key != path_key &&
        float_placements_.find(replacement_key) == float_placements_.end() &&
        !FindPin(pins_, replacement)) {
      LogLine(L"[float] sticky prune retarget: %s -> %s", path.c_str(),
              replacement.c_str());
      RetargetFloatPlacement(&float_placements_, path, replacement);
      ++float_retargeted;
      continue;
    }

    LogLine(L"[float] sticky prune drop: %s", path.c_str());
    float_placements_.erase(key);
    ++float_dropped;
  }

  if (dropped > 0 || retargeted > 0 || float_dropped > 0 ||
      float_retargeted > 0 || float_reclaimed > 0) {
    if (dropped > 0 || retargeted > 0) {
      BumpPinsEpoch();
      SavePins(pins_);
    }
    if (retargeted > 0 || float_dropped > 0 || float_retargeted > 0 ||
        float_reclaimed > 0) {
      SaveFloatPlacements(float_placements_);
    }
    if (dropped > 0 || retargeted > 0) {
      PersistLayouts();
    }
    RefreshDesktopIcons();
    LogLine(L"[pin] sticky prune dropped=%d retargeted=%d float_dropped=%d "
            L"float_retargeted=%d float_reclaimed=%d",
            dropped, retargeted, float_dropped, float_retargeted,
            float_reclaimed);
  }
}

void App::RefreshDesktopIcons() {
  // Owning snapshot: RebuildDockItems may scan again, and the organize worker
  // can invalidate the cache underneath — a reference would dangle.
  // Still one scan (2.5s cache); RebuildPublicItems reuses &items (no 2nd scan).
  const std::vector<DesktopItem> items = ScanDesktopItems();
  // Pin-only fence membership (Python get_virtual_items_for_fence). Rule
  // matching is for 一键整理 / auto-pin — not live display — so unpinned
  // desktop files can surface as public/loose floats (P0-2/P0-3).
  // One NormalizePinKey per desktop item: build by_fence + scanned + float
  // icon paths together (was 2–3 passes of FindPin/normalize).
  std::unordered_set<std::string> live_fence_ids;
  live_fence_ids.reserve(fences_.size() * 2 + 4);
  for (const auto& f : fences_) {
    if (f && !f->Id().empty()) {
      live_fence_ids.insert(f->Id());
    }
  }
  std::unordered_map<std::string, std::vector<DesktopItem>> by_fence;
  std::unordered_set<std::wstring> scanned;
  scanned.reserve(items.size() * 2 + pins_.size() + 8);
  std::vector<std::wstring> unpinned_paths;
  unpinned_paths.reserve(items.size());
  std::vector<std::wstring> dead_pin_paths;
  for (const auto& it : items) {
    const std::wstring key = NormalizePinKey(it.path);
    scanned.insert(key);
    auto pit = pins_.find(key);
    if (pit == pins_.end() || pit->second.empty()) {
      unpinned_paths.push_back(it.path);
      continue;
    }
    std::string target = pit->second;
    if (target == "system_software") {
      target = "system_software_side";
    }
    // Stale fence_id (partition deleted): drop pin so the icon resurfaces as
    // a public float instead of vanishing from every layer (Fences).
    if (live_fence_ids.find(target) == live_fence_ids.end()) {
      dead_pin_paths.push_back(it.path);
      unpinned_paths.push_back(it.path);
      continue;
    }
    by_fence[target].push_back(it);
  }
  // Python get_virtual_items_for_fence walks the stored pin list directly, so
  // paths outside the desktop folder still render in a fence. Mirror that:
  // append pinned files the desktop scan cannot see (existing, non-namespace).
  for (const auto& kv : pins_) {
    const std::wstring& path = kv.first;
    if (path.empty() || kv.second.empty()) {
      continue;
    }
    if (path.find(L"::") != std::wstring::npos) {
      continue;  // shell namespace item (此电脑 / 回收站), not a file path
    }
    // pins_ keys are already NormalizePinKey'd (SetPin).
    if (scanned.find(path) != scanned.end()) {
      continue;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
      continue;
    }
    DesktopItem extra;
    extra.path = path;
    const size_t slash = path.find_last_of(L"\\/");
    extra.name = (slash != std::wstring::npos && slash + 1 < path.size())
                     ? path.substr(slash + 1)
                     : path;
    const size_t dot = extra.name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0 &&
        (_wcsicmp(extra.name.c_str() + dot, L".lnk") == 0 ||
         _wcsicmp(extra.name.c_str() + dot, L".url") == 0)) {
      extra.is_link = true;
      extra.name.resize(dot);
    }
    // Cached shell display name (same store as ScanDesktopItems).
    if (extra.is_link) {
      ULARGE_INTEGER mtime{};
      mtime.LowPart = fad.ftLastWriteTime.dwLowDateTime;
      mtime.HighPart = fad.ftLastWriteTime.dwHighDateTime;
      extra.name =
          ResolveCachedLinkDisplayName(path, extra.name, mtime.QuadPart);
    }
    std::string target = kv.second;
    if (target == "system_software") {
      target = "system_software_side";
    }
    if (live_fence_ids.find(target) == live_fence_ids.end()) {
      dead_pin_paths.push_back(path);
      continue;
    }
    by_fence[target].push_back(std::move(extra));
  }
  if (!dead_pin_paths.empty()) {
    for (const auto& p : dead_pin_paths) {
      ClearPin(&pins_, p);
    }
    BumpPinsEpoch();
    SavePins(pins_);
    LogLine(L"[pin] dropped %d stale fence_id pin(s)",
            static_cast<int>(dead_pin_paths.size()));
  }

  // Prefer in-memory prefs — LoadExcludePatterns() re-reads app_prefs.json
  // from disk (portal fence × every RefreshDesktopIcons).
  std::vector<std::wstring> excludes = prefs_.exclude_patterns;
  if (excludes.empty()) {
    excludes = DefaultExcludePatterns();
  }
  std::vector<std::pair<std::wstring, int>> icon_jobs;
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    // Portal must be a real folder. Page names / titles wrongly saved as
    // "portal" used to empty the fence and skip pin-based icons.
    const std::wstring& portal = fence->PortalPath();
    bool portal_ok = false;
    if (!portal.empty()) {
      const DWORD attr = GetFileAttributesW(portal.c_str());
      portal_ok = attr != INVALID_FILE_ATTRIBUTES &&
                  (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
      if (!portal_ok) {
        fence->SetPortalPath(L"");
      }
    }
    if (portal_ok) {
      // Browse cwd when drilled into a subfolder (Fences Folder Portal).
      const std::wstring browse = fence->PortalBrowsePath();
      auto portal_items = ScanPortalItems(
          browse.empty() ? portal : browse, excludes);
      const int sz = fence->IconSizePx();
      for (const auto& item : portal_items) {
        icon_jobs.emplace_back(item.path, sz);
      }
      fence->SetItems(std::move(portal_items));
      continue;
    }
    auto it = by_fence.find(fence->Id());
    if (it != by_fence.end()) {
      const int sz = fence->IconSizePx();
      for (const auto& item : it->second) {
        icon_jobs.emplace_back(item.path, sz);
      }
      fence->SetItems(std::move(it->second));
    } else {
      fence->SetItems({});
    }
  }
  RebuildDockItems();
  // One icon truth: floats only when Explorer icons are hidden (Fences).
  // enable_public_desktop controls cross-page sharing inside RebuildPublicItems.
  const bool want_floats = !icons_visible_intent_;
  public_visible_ = want_floats;
  if (want_floats) {
    RebuildPublicItems(&items);
    // RebuildPublicItems already hides/shows based on whether any float is
    // left; showing unconditionally resurrected an empty header plate.
    public_host_.SetVisible(public_host_.HasItems());
    // Loose floats share the Explorer icon grid size.
    const int pub_sz = QueryIconGridMetrics().icon_size;
    for (const auto& path : unpinned_paths) {
      icon_jobs.emplace_back(path, pub_sz);
    }
  } else {
    public_host_.SetItems({});
    public_host_.Hide();
  }
  // STA worker fills cold icons after fence+public SetItems (UI stays snappy).
  if (message_hwnd_) {
    PreloadIconsAsync(icon_jobs, message_hwnd_, kDeskTidyMsgIconsPreloaded);
  } else {
    PreloadIconsParallel(icon_jobs);
  }
  // Do not WarmLayerOffscreen here — Show/-32000/Hide races SetPage and can
  // leave off-page fences IsWindowVisible (ghost under apps). Hide instead.
  HideOffPageFences();
}

void App::RefreshIconGrids() {
  // P2-17: Explorer icon size / spacing changed — re-read the live grid metrics
  // and push them to every fence (public host re-queries inside SetItems).
  InvalidateIconGridMetricsCache();
  const IconGridMetrics m = QueryIconGridMetrics();
  for (auto& fence : fences_) {
    if (fence) {
      fence->SetIconGrid(m);
    }
  }
}

void App::RefreshOverlays() {
  // Throttle: WM_DWMCOMPOSITIONCHANGED fires in bursts on hybrid-GPU adapter
  // flapping — a full sweep of every overlay per event would peg the UI thread.
  const ULONGLONG now = GetTickCount64();
  if (now - last_overlay_refresh_ < 600) {
    return;
  }
  last_overlay_refresh_ = now;
  // Re-upload every visible layered overlay. Hybrid-GPU (Optimus/AMD switchable)
  // and DWM reconnects (RDP / driver reset) drop previously uploaded pixels
  // while the window stays "visible" — a fresh ULW blit recovers it.
  for (auto& fence : fences_) {
    if (fence) {
      fence->RefreshLayer();
    }
  }
  if (page_bar_.Hwnd() && IsWindowVisible(page_bar_.Hwnd())) {
    page_bar_.RefreshLayer();
  }
  if (dock_.Hwnd() && IsWindowVisible(dock_.Hwnd())) {
    dock_.RefreshLayer();
  }
  if (public_host_.Hwnd() && IsWindowVisible(public_host_.Hwnd())) {
    public_host_.RefreshLayer();
  }
}

void App::RebuildDockItems() {
  std::vector<DesktopItem> dock_items;
  // P1-10: prefer prefs_.dock_items (Python dock.items); legacy pin scan fallback.
  if (!prefs_.dock_items.empty()) {
    for (const auto& path : prefs_.dock_items) {
      if (path.empty() ||
          GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        continue;
      }
      DesktopItem it;
      it.path = path;
      it.name = path;
      const size_t slash = path.find_last_of(L"\\/");
      if (slash != std::wstring::npos && slash + 1 < path.size()) {
        it.name = path.substr(slash + 1);
      }
      const size_t dot = it.name.rfind(L'.');
      if (dot != std::wstring::npos && dot > 0) {
        const std::wstring ext = it.name.substr(dot);
        if (_wcsicmp(ext.c_str(), L".lnk") == 0 ||
            _wcsicmp(ext.c_str(), L".url") == 0) {
          it.is_link = true;
          it.name.resize(dot);
        }
      }
      if (it.is_link) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        ULONGLONG mtime_q = 0;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
          ULARGE_INTEGER mtime{};
          mtime.LowPart = fad.ftLastWriteTime.dwLowDateTime;
          mtime.HighPart = fad.ftLastWriteTime.dwHighDateTime;
          mtime_q = mtime.QuadPart;
        }
        it.name = ResolveCachedLinkDisplayName(path, it.name, mtime_q);
      }
      dock_items.push_back(std::move(it));
    }
  } else {
    const auto items = ScanDesktopItems();
    for (const auto& it : items) {
      if (!it.is_link) {
        continue;
      }
      const std::string target =
          ResolveFenceId(it.path, it.is_link, pins_, rules_, "system_software",
                         "system_docs");
      if (target != "system_software" && target != "system_software_side") {
        continue;
      }
      dock_items.push_back(it);
      if (dock_items.size() >= 8) {
        break;
      }
    }
  }
  // Skip SetItems when membership is unchanged (every RefreshDesktopIcons used
  // to re-LoadItemIcon the dock even with identical paths).
  if (DesktopItemListsIdentityEqual(dock_items, last_dock_items_)) {
    return;
  }
  last_dock_items_ = dock_items;
  dock_.SetItems(std::move(dock_items));
}

void App::SyncFenceTabGroups() {
  // Group members by tab_group, preserving layout order so the tab strip is
  // stable across reloads. Empty group = standalone fence (no tab strip).
  std::unordered_map<std::string, std::vector<size_t>> groups;
  for (size_t i = 0; i < fences_.size(); ++i) {
    if (!fences_[i] || fences_[i]->TabGroup().empty()) {
      continue;
    }
    groups[fences_[i]->TabGroup()].push_back(i);
  }
  for (auto& kv : groups) {
    auto& members = kv.second;
    if (members.size() < 2) {
      fences_[members[0]]->SetTabGroup("");
      fences_[members[0]]->SetTabActive(false);
      fences_[members[0]]->SetTabEntries({});
      continue;
    }
    // Exactly one active member; default to the first if the persisted flag is
    // missing or somehow set on several (older / hand-edited layout.json).
    int active_pos = -1;
    for (size_t k = 0; k < members.size(); ++k) {
      if (fences_[members[k]]->TabActive()) {
        active_pos = static_cast<int>(k);
        break;
      }
    }
    if (active_pos < 0) {
      active_pos = 0;
      fences_[members[0]]->SetTabActive(true);
    }
    std::vector<FenceWindow::TabEntry> entries;
    entries.reserve(members.size());
    for (size_t k = 0; k < members.size(); ++k) {
      FenceWindow* f = fences_[members[k]].get();
      // Non-active members must not claim the active flag — reconcile here so a
      // stale layout.json cannot show two panels at once.
      const bool is_active = static_cast<int>(k) == active_pos;
      if (f->TabActive() != is_active) {
        f->SetTabActive(is_active);
      }
      FenceWindow::TabEntry e;
      e.fence_id = f->Id();
      e.label = f->TabTitle().empty() ? f->Title() : f->TabTitle();
      e.active = is_active;
      entries.push_back(std::move(e));
    }
    // Every member gets the same strip so whichever one is visible (only the
    // active one is) still renders the group's tabs.
    for (size_t idx : members) {
      fences_[idx]->SetTabEntries(entries);
    }
  }
}

void App::JoinFenceTabGroup(FenceWindow* fence) {
  if (!fence) {
    return;
  }
  const std::string self_id = fence->Id();
  const std::wstring self_title = fence->Title();
  // Candidates: other fences visible on at least one of this fence's pages.
  // Grouping across pages would make the tab strip appear/disappear as the
  // user switches pages, so membership stays page-compatible.
  std::vector<std::string> candidate_ids;
  std::vector<std::wstring> labels;
  for (auto& f : fences_) {
    if (!f || f.get() == fence) {
      continue;
    }
    bool shared_page = false;
    for (int p : fence->Pages()) {
      if (f->OnPage(p)) {
        shared_page = true;
        break;
      }
    }
    if (!shared_page) {
      continue;
    }
    candidate_ids.push_back(f->Id());
    labels.push_back(f->Title());
  }
  if (candidate_ids.empty()) {
    ShowToast(L"没有可合并的其他分区（需在同一分页）");
    return;
  }

  std::string picked_id;
  MistWebDialog dlg;
  dlg.owner = settings_.Hwnd();
  dlg.user_data_subdir = L"webview2-tabgroup";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\pick_list.html");
  if (dlg.html_path.empty()) {
    ShowToast(L"找不到选择列表组件");
    return;
  }
  dlg.build_state = [&]() {
    std::wstring json = L"{\"type\":\"state\",\"title\":\"";
    json += MistJsonEscape(L"合并到标签组");
    json += L"\",\"hint\":\"";
    json += MistJsonEscape(L"选择要与「" + self_title + L"」合并的分区");
    json += L"\",\"open_label\":\"";
    json += MistJsonEscape(L"合并");
    json += L"\",\"allow_new\":false,\"sel\":0,\"items\":[";
    for (size_t i = 0; i < labels.size(); ++i) {
      if (i) {
        json += L',';
      }
      json += L'"';
      json += MistJsonEscape(labels[i]);
      json += L'"';
    }
    json += L"]}";
    return json;
  };
  dlg.on_message = [&](const std::wstring& json) -> bool {
    const std::wstring type = MistJsonStringField(json, L"type");
    if (type != L"action") {
      return false;
    }
    const std::wstring id = MistJsonStringField(json, L"id");
    if (id == L"ready") {
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"open") {
      const int index = MistJsonIntField(json, L"index", -1);
      if (index >= 0 && index < static_cast<int>(candidate_ids.size())) {
        picked_id = candidate_ids[static_cast<size_t>(index)];
        return true;
      }
      return false;
    }
    if (id == L"cancel") {
      return true;
    }
    return false;
  };
  MistWebDialogRun(&dlg, L"合并到标签组", 480, 400);
  if (picked_id.empty()) {
    return;
  }

  // Re-resolve both self and target in case `fences_` was rebuilt while the
  // modal dialog pumped messages (display change / snapshot restore).
  fence = nullptr;
  FenceWindow* target = nullptr;
  for (auto& f : fences_) {
    if (!f) {
      continue;
    }
    if (f->Id() == self_id) {
      fence = f.get();
    } else if (f->Id() == picked_id) {
      target = f.get();
    }
  }
  if (!fence || !target || fence == target) {
    return;
  }
  std::string group = target->TabGroup();
  if (group.empty()) {
    group = target->Id();
    target->SetTabGroup(group);
  }
  fence->SetTabGroup(group);
  // The target stays the visible member; the joining fence becomes a tab.
  fence->SetTabActive(false);
  if (target->TabGroup() == group && !target->TabActive()) {
    target->SetTabActive(true);
  }
  // Align the newcomer's geometry with the target so the group occupies one
  // seat — otherwise the hidden member would keep a stale rectangle.
  FenceLayout tl = target->CurrentLayout();
  FenceLayout fl = fence->CurrentLayout();
  fl.x = tl.x;
  fl.y = tl.y;
  fl.w = tl.w;
  fl.h = tl.h;
  fence->ApplyLayoutGeometry(fl);
  SyncFenceTabGroups();
  ApplyPageVisibility();
  PersistLayouts();
  PushExtensionSettings();
}

void App::LeaveFenceTabGroup(FenceWindow* fence) {
  if (!fence || fence->TabGroup().empty()) {
    return;
  }
  const std::string old_group = fence->TabGroup();
  fence->SetTabGroup("");
  fence->SetTabActive(false);
  // If the leaver was the active member, promote another remaining member so
  // the group never ends up with everything hidden.
  bool group_has_active = false;
  FenceWindow* first_left = nullptr;
  for (auto& f : fences_) {
    if (!f || f->TabGroup() != old_group) {
      continue;
    }
    if (!first_left) {
      first_left = f.get();
    }
    if (f->TabActive()) {
      group_has_active = true;
    }
  }
  if (!group_has_active && first_left) {
    first_left->SetTabActive(true);
  }
  SyncFenceTabGroups();
  ApplyPageVisibility();
  PersistLayouts();
  PushExtensionSettings();
}

void App::ActivateFenceTab(const std::string& fence_id) {
  if (fence_id.empty()) {
    return;
  }
  // Find the group of the clicked fence, then flip active flags within it.
  std::string group;
  FenceWindow* prev_active = nullptr;
  for (auto& f : fences_) {
    if (f && f->Id() == fence_id) {
      group = f->TabGroup();
      break;
    }
  }
  if (group.empty()) {
    return;
  }
  for (auto& f : fences_) {
    if (f && f->TabGroup() == group && f->TabActive()) {
      prev_active = f.get();
      break;
    }
  }
  FencePageGeometry active_geo{};
  const bool have_geo = prev_active != nullptr;
  if (have_geo) {
    active_geo = prev_active->GeometryForPage(current_page_);
  }
  for (auto& f : fences_) {
    if (!f || f->TabGroup() != group) {
      continue;
    }
    const bool now_active = (f->Id() == fence_id);
    f->SetTabActive(now_active);
    if (have_geo && f.get() != prev_active) {
      f->ApplyPageGeometry(active_geo, /*present=*/now_active);
    }
  }
  SyncFenceTabGroups();
  ApplyPageVisibility();
  PersistLayouts();
}

void App::ApplyPageVisibility() {
  if (peek_active_) {
    return;
  }
  // Tabbed fences: only the active member of a group may be shown. Compute the
  // suppressed set once so the Show/Hide loop below stays a single pass.
  std::unordered_set<FenceWindow*> tab_hidden;
  for (auto& fence : fences_) {
    if (fence && !fence->TabGroup().empty() && !fence->TabActive()) {
      tab_hidden.insert(fence.get());
    }
  }
  // Non-page-switch paths (peek end, show_fences toggle): still OK to Show/Hide
  // individually. Page clicks go through SetPage's OverlayWindowBatch.
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    // icons_visible_intent_: stock desktop — keep fences off DefView.
    if (!fences_hidden_ && !icons_visible_intent_ &&
        fence->VisibleOnPage(current_page_) &&
        tab_hidden.find(fence.get()) == tab_hidden.end()) {
      fence->Show();
    } else {
      fence->HideTransient();
    }
  }
  const bool pet_hosts = pet_visible_ && pet_.ShowPageBubbles();
  const bool show_page_bar =
      !icons_visible_intent_ && !pet_hosts && prefs_.show_page_indicator;
  if (show_page_bar) {
    page_bar_.SetPage(current_page_);
    page_bar_.Show();
    if (page_bar_.Hwnd()) {
      RaiseOverlayInDesktopBand(page_bar_.Hwnd());
      page_bar_.RefreshLayer();
    }
  } else {
    page_bar_.Hide();
  }
  if (!icons_visible_intent_ && dock_visible_ && dock_.HasItems() &&
      (!dock_.Hwnd() || !IsWindowVisible(dock_.Hwnd()))) {
    dock_.Show();
  } else if (icons_visible_intent_) {
    dock_.SetVisible(false);
  }
  if (!icons_visible_intent_ && pet_visible_ &&
      (!pet_.Hwnd() || !IsWindowVisible(pet_.Hwnd()))) {
    pet_.Show();
  } else if (icons_visible_intent_) {
    pet_.SetVisible(false);
  }
  if (!icons_visible_intent_ && public_visible_ && public_host_.HasItems() &&
      (!public_host_.Hwnd() || !IsWindowVisible(public_host_.Hwnd()))) {
    public_host_.Show();
  }
}

void App::SetPage(int page) {
  if (page < 0) {
    page = 0;
  }
  const int max_page = (std::max)(0, static_cast<int>(page_names_.size()) - 1);
  if (page > max_page) {
    page = max_page;
  }
  const bool pet_hosts = pet_visible_ && pet_.ShowPageBubbles();
  const bool show_page_bar = !pet_hosts && prefs_.show_page_indicator;
  if (page == current_page_) {
    if (show_page_bar) {
      page_bar_.SetPage(current_page_);
    }
    // AIGC START
    // Re-click current page (esp.「文档」): lift fences buried under DefView
    // after organize — same cure as desktop-FG restore after 微信截图.
    if (!DesktopChromeHealthy()) {
      ScheduleDesktopBandRestore();
    }
    // AIGC END
    return;
  }
  current_page_ = page;
  // Selection paint only — do not Attach/Relayout the edge bar (page flash).
  if (show_page_bar) {
    page_bar_.SetPage(current_page_);
  }
  pet_.SetPages(page_names_, current_page_);
  settings_.SetCurrentPage(current_page_, settings_.IsOpen());

  if (message_hwnd_) {
    KillTimer(message_hwnd_, kTimerWarmFences);
  }

  // Fences-class 零闪烁切页：先显后隐 + 预先就绪位图 + 同步刷新公共区，杜绝桌面底色裸露与48ms二次重绘跳变
  std::vector<FenceWindow*> leavers;
  std::vector<FenceWindow*> arrivals;
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd()) continue;
    const bool want = !fences_hidden_ && !icons_visible_intent_ &&
                      !peek_active_ && fence->VisibleOnPage(current_page_) &&
                      (fence->TabGroup().empty() || fence->TabActive());
    if (want) {
      arrivals.push_back(fence.get());
    } else {
      if (IsWindowVisible(fence->Hwnd()) || IsIconic(fence->Hwnd())) {
        leavers.push_back(fence.get());
      }
    }
  }

  // 1. 预先更新并显示入场分区（Arrivals）：位图在显存准备好后再映射，保证第一帧饱满
  for (FenceWindow* fence : arrivals) {
    const FencePageGeometry geo = fence->GeometryForPage(current_page_);
    fence->ApplyPageGeometry(geo, /*present=*/true);
    if (!fence->Show()) {
      fence->RefreshLayer();
    }
  }

  // 2. 同步更新公共区域浮层（取消48ms延迟计时器，彻底根除切页后的二次突兀闪烁）
  if (public_visible_) {
    RebuildPublicItems();
  }

  // 3. 入场分区全部就位后，再隐藏离场分区（Leavers）：新画面覆盖旧画面，桌面壁纸绝不暴露
  for (FenceWindow* fence : leavers) {
    ShowWindow(fence->Hwnd(), SW_HIDE);
  }

  // 4. 辅助控件状态平滑保持（无需频繁 Raise 强制 DWM 刷树）
  if (dock_visible_ && dock_.Hwnd() && dock_.HasItems() &&
      !IsWindowVisible(dock_.Hwnd())) {
    dock_.Show();
  }
  if (pet_visible_ && pet_.Hwnd() && !IsWindowVisible(pet_.Hwnd())) {
    pet_.Show();
  }
  if (show_page_bar) {
    if (!page_bar_.Hwnd() || !IsWindowVisible(page_bar_.Hwnd())) {
      page_bar_.Show();
    }
  } else {
    page_bar_.Hide();
  }

  if (message_hwnd_) {
    KillTimer(message_hwnd_, kTimerReflowPublic);
  }

  SchedulePersistLayouts();
}

void App::ScheduleDesktopBandRestore() {
  if (!message_hwnd_) {
    return;
  }
  if (session_locked_ || !DesktopChromeMayRestack()) {
    return;
  }
  // After 账号浮窗 close, ignore desktop-FG restore briefly (avoids Raise).
  if (suppress_band_restore_until_ != 0 &&
      GetTickCount64() < suppress_band_restore_until_) {
    return;
  }
  band_restore_force_ = true;
  // Coalesce like Python/Fences: short settle after WorkerW changes. Keep the
  // budget small — 6×100ms full Raise+ULW under many apps made drags stutter.
  if (band_restore_left_ < 3) {
    band_restore_left_ = 3;
  }
  // Stamp the burst start here, not on the first extension: the budget can be
  // partly spent before ExtendBandSettleIfTransitioning ever runs, and the cap
  // must measure the whole transition.
  if (band_settle_started_ms_ == 0) {
    band_settle_started_ms_ = GetTickCount64();
  }
  SetTimer(message_hwnd_, kTimerBandSettle, 100, nullptr);
}

void App::ExtendBandSettleIfTransitioning() {
  // Win+D is a multi-frame transition: the shell re-composites the desktop and
  // re-orders Z between frames, and IsShowDesktopActive() flaps while the
  // minimise animation is still running. The fixed 3-tick budget can be spent
  // entirely inside that window, after which nothing re-seats the fences until
  // the 4s keepalive — and that keepalive takes the "apps quiet" shortcut when
  // a restored window briefly owns the foreground. Net effect: fences stay
  // hidden until the user touches the desktop again.
  //
  // Keep the settle alive for as long as the band state disagrees with what we
  // last seated. Bounded by kBandSettleMaxMs so a permanently flapping
  // IsShowDesktopActive() cannot spin the timer forever.
  if (!message_hwnd_ || band_restore_left_ <= 0) {
    return;
  }
  if (GetTickCount64() - band_settle_started_ms_ > kBandSettleMaxMs) {
    // Cap reached: stop extending AND clear the burst stamp, so the next Win+D
    // gets a fresh budget instead of inheriting an already-expired stamp.
    band_settle_started_ms_ = 0;
    return;
  }
  const bool show_desktop = QueryDesktopBandState().show_desktop;
  if (show_desktop != show_desktop_seated_) {
    band_restore_left_ = (std::max)(band_restore_left_, 2);
    SetTimer(message_hwnd_, kTimerBandSettle, 100, nullptr);
  }
}

void App::StartForegroundHook() {
  if (fg_hook_) {
    return;
  }
  g_app_for_hook = this;
  g_band_hwnd = message_hwnd_;
  fg_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                             nullptr, &App::ForegroundWinEvent, 0, 0,
                             WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  // Win+D is implemented by minimising every top-level window; Windows exposes
  // no "show desktop" notification, so polling Z-order is a heuristic that
  // misfires mid-animation (a minimising window is still visible and
  // non-zero-sized for a few frames). Watching MINIMIZESTART gives a real
  // edge: a burst of minimises ending with the desktop in front is Win+D.
  min_hook_ = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART,
                              EVENT_SYSTEM_MINIMIZEEND, nullptr,
                              &App::MinimizeWinEvent, 0, 0,
                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  if (!g_kb_hook) {
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr,
                                  0);
  }
}

void CALLBACK App::MinimizeWinEvent(HWINEVENTHOOK, DWORD event, HWND /*hwnd*/,
                                    LONG, LONG, DWORD, DWORD) {
  if (!g_app_for_hook || !g_app_for_hook->message_hwnd_) {
    return;
  }
  // Stay non-blocking: OUTOFCONTEXT callbacks must not touch Z-order or the
  // shell. Just nudge the band restore; it re-evaluates on its own tick.
  if (event == EVENT_SYSTEM_MINIMIZESTART) {
    PostMessageW(g_app_for_hook->message_hwnd_, kMsgBandRestore, 0, 0);
  } else if (event == EVENT_SYSTEM_MINIMIZEEND) {
    PostMessageW(g_app_for_hook->message_hwnd_, kMsgBandRestore, 0, 0);
  }
}

void App::StopForegroundHook() {
  if (fg_hook_) {
    UnhookWinEvent(fg_hook_);
    fg_hook_ = nullptr;
  }
  if (min_hook_) {
    UnhookWinEvent(min_hook_);
    min_hook_ = nullptr;
  }
  if (g_kb_hook) {
    UnhookWindowsHookEx(g_kb_hook);
    g_kb_hook = nullptr;
  }
  if (g_app_for_hook == this) {
    g_app_for_hook = nullptr;
  }
  if (g_band_hwnd == message_hwnd_) {
    g_band_hwnd = nullptr;
  }
}

void App::StartDesktopShellNotify() {
  StopDesktopShellNotify();
  if (!message_hwnd_) {
    return;
  }
  PIDLIST_ABSOLUTE desk = nullptr;
  PIDLIST_ABSOLUTE pub = nullptr;
  if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &desk)) &&
      desk) {
    shell_notify_pidls_[shell_notify_pidl_count_++] = desk;
  }
  if (SUCCEEDED(
          SHGetKnownFolderIDList(FOLDERID_PublicDesktop, 0, nullptr, &pub)) &&
      pub) {
    shell_notify_pidls_[shell_notify_pidl_count_++] = pub;
  }
  if (shell_notify_pidl_count_ <= 0) {
    return;
  }
  SHChangeNotifyEntry entries[2]{};
  for (int i = 0; i < shell_notify_pidl_count_; ++i) {
    entries[i].pidl = shell_notify_pidls_[i];
    entries[i].fRecursive = FALSE;
  }
  // Fences/Explorer model: membership changes arrive as shell notifications,
  // then one debounced rescan. Do NOT subscribe to SHCNE_UPDATEITEM — Explorer
  // copy/drag onto Desktop floods UPDATEITEM as file size grows, and each
  // event used to schedule a full fence/public rebuild (UI hitch).
  shell_notify_id_ = SHChangeNotifyRegister(
      message_hwnd_,
      SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
      SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
          SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEDIR,
      kMsgShellNotify, shell_notify_pidl_count_, entries);
  if (!shell_notify_id_) {
    LogLine(L"[shell] SHChangeNotifyRegister failed err=%lu", GetLastError());
  }
}

void App::StopDesktopShellNotify() {
  if (shell_notify_id_) {
    SHChangeNotifyDeregister(shell_notify_id_);
    shell_notify_id_ = 0;
  }
  for (int i = 0; i < shell_notify_pidl_count_; ++i) {
    if (shell_notify_pidls_[i]) {
      CoTaskMemFree(shell_notify_pidls_[i]);
      shell_notify_pidls_[i] = nullptr;
    }
  }
  shell_notify_pidl_count_ = 0;
}

void App::ScheduleDesktopContentRefresh() {
  if (!message_hwnd_) {
    return;
  }
  // OnDesktopPaste already did an immediate RefreshDesktopIcons — suppress the
  // trailing SHChangeNotify rescan while Explorer's copy burst is still landing
  // (2-3 redundant full grid rescans made pasted icons flicker in/out). The
  // deadline is EXTENDED per suppressed notify, so it tracks the burst instead
  // of blindly dropping a fixed window: late CREATE/UPDATEDIR from a large
  // multi-file paste still trigger a rescan once the burst settles (they used
  // to be swallowed here and only showed up on the next ~10s PollChanged).
  const ULONGLONG now = GetTickCount64();
  if (now < g_paste_rescan_deadline) {
    g_paste_rescan_deadline = now + kPasteRescanSuppressMs;
    return;
  }
  // Coalesce multi-file CREATE/UPDATEDIR bursts from a folder→Desktop drop
  // (Fences-style: one rescan after the shell settles, not per-item).
  KillTimer(message_hwnd_, kTimerDesktopShell);
  SetTimer(message_hwnd_, kTimerDesktopShell, 750, nullptr);
}

void App::OnDesktopContentRefresh() {
  rules_ = LoadRules();
  InvalidateDesktopScanCache();
  InvalidatePortalScanCache();
  // Always refresh first (Fences / Explorer): portal & desktop icons must
  // update even when the organize worker is still busy. Watch organize is
  // best-effort on top — never block the FS refresh path.
  RefreshDesktopIcons();
  if (prefs_.auto_organize_watch) {
    OrganizeVirtual(true);
  }
  // Folder→Desktop copy churn can leave a mapped fence with an empty layered
  // buffer or buried under DefView. Only re-Present when dirty / buried —
  // SetItems already blitted membership changes (same-seat skip is the common
  // watch-refresh path).
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd()) {
      continue;
    }
    if (fences_hidden_ || peek_active_ ||
        !fence->VisibleOnPage(current_page_)) {
      continue;
    }
    if (!IsWindowVisible(fence->Hwnd()) || IsIconic(fence->Hwnd())) {
      continue;
    }
    if (fence->LayerDirty() || !IsOverlayAboveDefView(fence->Hwnd())) {
      fence->RefreshLayer();
    }
  }
  // Under normal apps, healthy chrome stays quiet (Fences/Rainmeter). Only
  // re-arm band restore when burial / show-desktop needs a real seat flip.
  //
  // Compare against show_desktop_seated_ using the SAME expression
  // RestoreDesktopChrome() stores there. It stores `show_desktop`, which is
  // QueryDesktopBandState().show_desktop. The old expression here also gated
  // on !HasVisibleFullscreenCover(), so the two could never match and this
  // re-armed the settle timer forever — an endless restore loop.
  if (!DesktopChromeHealthy() ||
      QueryDesktopBandState().show_desktop != show_desktop_seated_) {
    ScheduleDesktopBandRestore();
  }
}

void CALLBACK App::ForegroundWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG,
                                      DWORD, DWORD) {
  if (!g_app_for_hook || !g_app_for_hook->message_hwnd_ ||
      g_app_for_hook->session_locked_) {
    return;
  }
  // OUTOFCONTEXT WinEvent must stay non-blocking. Calling
  // IsExplorerDesktopForeground / IsShowDesktopActive here walks HWND Z-order
  // and can deadlock when Office/Word spins up (UI freeze → apparent crash).
  PostMessageW(g_app_for_hook->message_hwnd_, kMsgFgChanged, 0, 0);
}

bool App::DesktopChromeMayRestack() const {
  // Market: while any owned modal owns FG chrome, do not raise/seat fences.
  if (settings_.IsOpen()) {
    return false;
  }
  if (todos_.Hwnd() && IsWindowVisible(todos_.Hwnd())) {
    return false;
  }
  return true;
}

bool App::DesktopChromeHealthy() const {
  // Peek / user-hide: "healthy" means leave alone (do not resurrect here).
  if (peek_active_ || fences_hidden_) {
    return true;
  }
  // The desktop is not showing (app in front, Task View / Alt+Tab switcher):
  // overlays are supposed to be behind it, so "buried" is not a fault and must
  // not trigger a restore loop. This gate belongs here, in the repair decision
  // — IsOverlayAboveDefView() itself reports the real Z order.
  if (!IsShowDesktopActive()) {
    return true;
  }
  // One DefView host lookup per pass — IsAttached/IsOverlayAbove used to each
  // call FindDefViewHost (N overlays × 2s/4s timers).
  const DefViewHost host = FindDefViewHost();
  auto mapped_attached = [&host](HWND hwnd) -> bool {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd) || IsIconic(hwnd) ||
        !IsAttachedToDesktop(hwnd, &host)) {
      return false;
    }
    // Fences/DeskFrame: DefView children ride Win+D with the shell — no
    // self-TOPMOST required (that was the old Progman-owned popup seat).
    return true;
  };
  for (const auto& fence : fences_) {
    if (!fence || !fence->Hwnd() || !fence->VisibleOnPage(current_page_)) {
      continue;
    }
    // Tabbed fences: a non-active member is deliberately hidden and shares its
    // group's seat. Counting it here would report "unhealthy" on every pass,
    // so the settle timer would keep re-raising chrome — which is exactly the
    // kind of Z-order churn that made Win+D restores flaky.
    if (!fence->TabGroup().empty() && !fence->TabActive()) {
      continue;
    }
    if (!mapped_attached(fence->Hwnd())) {
      return false;
    }
    // Attached + visible but under DefView/wallpaper → icons "gone" until
    // SetPage Raises. Treat as unhealthy so band restore can lift them.
    if (!IsOverlayAboveDefView(fence->Hwnd(), &host)) {
      return false;
    }
  }
  if (dock_visible_ && dock_.Hwnd() && !mapped_attached(dock_.Hwnd())) {
    return false;
  }
  if (pet_visible_ && pet_.Hwnd() && !mapped_attached(pet_.Hwnd())) {
    return false;
  }
  if (pet_visible_ && pet_.PagesHwnd() &&
      !mapped_attached(pet_.PagesHwnd())) {
    return false;
  }
  const bool pet_hosts = pet_visible_ && pet_.ShowPageBubbles();
  const bool show_page_bar = !pet_hosts && prefs_.show_page_indicator;
  if (show_page_bar && page_bar_.Hwnd() &&
      !mapped_attached(page_bar_.Hwnd())) {
    return false;
  }
  if (public_visible_ && public_host_.Hwnd() && public_host_.HasItems() &&
      !mapped_attached(public_host_.Hwnd())) {
    return false;
  }
  // whole desktop chrome "unhealthy" and trigger a fence Raise over apps.
  return true;
}

void App::RestoreDesktopChrome() {
  // Always re-hide soft-parked pages first (even on the healthy early-out).
  // Off-page fences that stay mapped under DefView ghost when an app is FG.
  HideOffPageFences();

  // While settings/todos is open, do NOT re-seat fences.
  if (session_locked_ || !DesktopChromeMayRestack()) {
    if (settings_.IsOpen() && settings_.Hwnd()) {
      SetWindowPos(settings_.Hwnd(), HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return;
  }

  // Market/Python keepalive raise_band=False: already seated → no Invalidate /
  // Show / Raise (system-dialog dismiss used to schedule 12 settle ticks and
  // flash the desktop on every no-op pass).
  // Rainmeter CheckDesktopState: re-seat only when show-desktop band toggles
  // (DefView TOPMOST on/off) or chrome is unhealthy. Ignore DefView TOPMOST
  // while a fullscreen cover is still live (Win+D over game must not "lift").
  // Apps FG first: skip cover EnumWindows on the common multi-app path.
  const bool apps_quiet = IsAppsForegroundQuiet();
  // Resolve the band facts ONCE for this pass and reuse them for every decision
  // below. Previously `show_desktop` here was computed with its own cover gate
  // while the reattach loop queried again, so the early-out and the raise could
  // disagree about whether Win+D was active.
  DesktopBandState restore_band = QueryDesktopBandState();
  if (IsWinDRestoring()) {
    restore_band.show_desktop = true;
  }
  // IsAppsForegroundQuiet() already returns false whenever IsShowDesktopActive()
  // is true, so combining them with && double-negates and reports "not showing"
  // in exactly the case we care about. Trust the band state alone.
  const bool show_desktop = restore_band.show_desktop;
  // NOTE: no transition guard here. Fences already refuses SC_MINIMIZE
  // (FilterDesktopChromeSysCommand), so Win+D never hides them — they simply
  // stay seated above DefView the whole time. Waiting for the animation to
  // "settle" before re-seating is what produced the visible gap: for those
  // ~200ms the fence was left at its old Z position and the freshly-raised
  // DefView covered it. Keep the seat current instead.
  const bool band_changed = show_desktop != show_desktop_seated_;
  const bool healthy = DesktopChromeHealthy();
  // A normal app owning the foreground means the desktop is deliberately
  // covered — the fences SHOULD be behind it. Nothing to repair, and looping
  // band restores here only burns CPU and thrashes Z-order. This must come
  // before any `settling` logic: a stale force flag or leftover settle tick
  // would otherwise keep re-raising fences under a maximized Chrome window.
  if (apps_quiet && !show_desktop && !IsWinDRestoring()) {
    // A normal app owns the foreground and the desktop is not showing: the
    // fences are supposed to be behind it, so stop re-seating. Do NOT zero
    // band_restore_left_ here — the opening tick of a real show-desktop
    // transition can land in this branch before IsShowDesktopActive() flips,
    // and clearing the budget would leave no ticks to seat the fences.
    band_restore_force_ = false;
    return;
  }
  // Never early-out while the show-desktop band is active. `healthy` is derived
  // from an instantaneous Z-order snapshot, and the shell keeps reshuffling
  // during the Win+D transition — trusting it here is what made the fences
  // appear only on some ticks. Re-seating below is idempotent.
  const bool settling = band_restore_force_ || band_changed || show_desktop;
  if (!settling && !band_restore_force_ && band_restore_left_ <= 0 && healthy &&
      !band_changed) {
    return;
  }
  if (band_restore_left_ <= 1) {
    band_restore_force_ = false;
  }

  // Only drop DefView host cache when the band actually flipped or seating
  // failed — invalidating every settle tick forces WorkerW rescans.
  if (band_changed || !healthy) {
    InvalidateDefViewCache();
  }
  // Do NOT ApplyPageVisibility here — fence->Show Place+Raise+Present on every
  // settle tick flashes chrome (消失几秒又回来). Soft Raise/reattach only.
  // Seat into desktop band (normal: above DefView; show-desktop: TOPMOST with
  // DefView). Chrome refuses minimize — no PresentLayered "Win+D fix".
  // Returns true when SoftSeat/show likely wiped ULW and needs RefreshLayer.
  auto reattach = [band_changed, &restore_band](HWND hwnd) -> bool {
    if (!hwnd || !IsWindow(hwnd)) {
      return false;
    }
    if (!IsAttachedToDesktop(hwnd)) {
      if (!AttachOverlayToDesktop(hwnd, true)) {
        ForceAttachOverlayToDesktop(hwnd, true);
      }
      RaiseOverlayInDesktopBandWith(hwnd, restore_band);
      return true;
    }
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
      ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOWNOACTIVATE);
      RaiseOverlayInDesktopBandWith(hwnd, restore_band);
      return true;
    }
    // While the show-desktop band is active, always re-seat: the shell keeps
    // reshuffling Z-order during the Win+D transition, so a one-shot
    // IsOverlayAboveDefView() snapshot is a race — it reported "fine" on some
    // ticks and "buried" on others, which is why the fences appeared only
    // sometimes. Re-seating is idempotent (RaiseOverlay skips a no-op stack).
    if (restore_band.show_desktop || !IsOverlayAboveDefView(hwnd) ||
        band_changed) {
      RaiseOverlayInDesktopBandWith(hwnd, restore_band);
      // The Win+D transition re-composites the desktop and can drop the
      // overlay's ULW surface even though the window stays mapped and its
      // Z-order is correct. Report "needs present" so the caller re-uploads
      // pixels; otherwise the fence is correctly stacked yet renders blank.
      if (restore_band.show_desktop) {
        return true;
      }
    }
    return false;
  };
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    if (!fence->Hwnd()) {
      continue;
    }
    if (!fence->VisibleOnPage(current_page_)) {
      continue;
    }
    // Tabbed fences: a non-active member must stay hidden — its group sibling
    // owns the seat.
    if (!fence->TabGroup().empty() && !fence->TabActive()) {
      continue;
    }
    if (fences_hidden_ || peek_active_ || icons_visible_intent_) {
      continue;
    }
    if (!IsWindowVisible(fence->Hwnd()) || IsIconic(fence->Hwnd())) {
      // Show() blits the layered surface BEFORE mapping the window, so the
      // first frame is already painted. Calling ShowWindow directly here left
      // the window visible with an empty ULW surface — the fence only appeared
      // on the next settle tick, which is why the first Win+D looked broken.
      fence->Show();
      RaiseOverlayInDesktopBandWith(fence->Hwnd(), restore_band);
    } else {
      const bool need_present = reattach(fence->Hwnd());
      // SoftSeat FRAMECHANGED can blank ULW; Raise alone must not force a
      // full PresentLayered on every settle tick (N overlays × drag stutter).
      if (need_present || fence->LayerDirty()) {
        fence->RefreshLayer();
      }
    }
  }
  // Same invariant as the float host below: dock_visible_ is the user's
  // preference, not "the dock has icons". A dock whose configured paths all
  // vanished renders nothing, and reattach() would ShowWindow() that empty
  // host on every settle tick after SetItems had hidden it.
  if (dock_visible_ && dock_.Hwnd() && dock_.HasItems()) {
    reattach(dock_.Hwnd());
  } else if (dock_.Hwnd() && IsWindowVisible(dock_.Hwnd())) {
    dock_.Hide();
  }
  if (pet_visible_ && pet_.Hwnd()) {
    const bool need_present = reattach(pet_.Hwnd());
    if (pet_.PagesHwnd()) {
      reattach(pet_.PagesHwnd());
    }
    if (pet_.MenuHwnd()) {
      reattach(pet_.MenuHwnd());
    }
    if (need_present && IsWindowVisible(pet_.Hwnd()) &&
        !IsIconic(pet_.Hwnd())) {
      pet_.RefreshLayer();
    }
  }
  const bool pet_hosts = pet_visible_ && pet_.ShowPageBubbles();
  const bool show_page_bar = !pet_hosts && prefs_.show_page_indicator;
  if (show_page_bar && page_bar_.Hwnd()) {
    const bool need_present = reattach(page_bar_.Hwnd());
    if (need_present && IsWindowVisible(page_bar_.Hwnd()) &&
        !IsIconic(page_bar_.Hwnd())) {
      page_bar_.RefreshLayer();
    }
  }
  // public_visible_ is the organized-mode flag, not "the layer has content".
  // reattach() unconditionally ShowWindow()s, so seating an empty layer here
  // resurrected the float host that RebuildPublicItems had just hidden — the
  // icon stayed on screen after it was moved into a fence until the next
  // rebuild. Enforce the same invariant the other seat sites use: the float
  // layer is visible only while it has slots to draw.
  if (public_visible_ && public_host_.Hwnd() && public_host_.HasItems()) {
    const bool need_present = reattach(public_host_.Hwnd());
    if (need_present && IsWindowVisible(public_host_.Hwnd()) &&
        !IsIconic(public_host_.Hwnd())) {
      public_host_.RefreshLayer();
    }
  } else if (public_host_.Hwnd() && IsWindowVisible(public_host_.Hwnd())) {
    public_host_.Hide();
  }
  show_desktop_seated_ = show_desktop;
}

void App::OrganizeVirtual(bool quiet, bool reassign) {
  // P2-16: heavy scan + pin resolve off the UI thread (Python organize worker).
  if (g_organize_busy) {
    // Queue the strongest follow-up: reassign > manual > quiet watch.
    g_organize_pending = true;
    if (reassign) {
      g_organize_pending_reassign = true;
      g_organize_pending_quiet = false;
    } else if (!quiet) {
      g_organize_pending_quiet = false;
    }
    if (!quiet) {
      ShowToast(L"正在整理，请稍候…");
    }
    return;
  }
  if (!message_hwnd_) {
    return;
  }
  g_organize_busy = true;
  g_organize_pending = false;
  g_organize_pending_quiet = true;
  g_organize_pending_reassign = false;
  if (!quiet) {
    ShowToast(L"正在整理…");
  }
  // Namespace hosting touches the shell — keep on the UI thread.
  const int hosted = EnsureHostedSystemNamespaceIcons(&pins_);
  auto* job = new OrganizeJobResult{};
  job->pins = pins_;
  job->pins_at_start = pins_;
  job->hosted = hosted;
  job->quiet = quiet;
  job->reassign = reassign;
  job->pins_epoch = pins_epoch_;
  const RuleMap rules = rules_;
  std::vector<FenceLayout> layouts;
  layouts.reserve(fences_.size());
  for (const auto& f : fences_) {
    if (f) {
      layouts.push_back(f->CurrentLayout());
    }
  }
  HWND msg_hwnd = message_hwnd_;
  ReapFinishedWorkers();
  const int current_page = current_page_;
  const bool honor_suppress = quiet && !reassign;
  workers_.emplace_back([job, rules, msg_hwnd, layouts, current_page, reassign,
                         honor_suppress]() {
    const auto items = ScanDesktopItems();
    job->n = OrganizeApplyDefaultPins(&job->pins, items, rules,
                                      "system_software", "system_docs",
                                      &layouts, current_page, reassign,
                                      honor_suppress, &job->neu) +
             job->hosted;
    if (!PostMessageW(msg_hwnd, kMsgOrganizeDone, 0,
                      reinterpret_cast<LPARAM>(job))) {
      delete job;
      g_organize_busy = false;
      // Cannot reach OnOrganizeDone — drop pending rather than spin forever.
      g_organize_pending = false;
      g_organize_pending_quiet = true;
      g_organize_pending_reassign = false;
    }
  });
}

void App::OrganizeReassign() {
  // 与设置页一致：按当前规则重排全部（含已归入），只改钉选归属。
  // Busy → OrganizeVirtual queues a follow-up reassign after the current job.
  const std::wstring tip =
      L"按当前分区规则重新分配桌面上的全部项目？\n\n"
      L"已钉进分区的图标也会按新规则换区。\n"
      L"只改分区归属，不移动、不删除任何文件。";
  if (!MistConfirm(settings_.Hwnd() ? settings_.Hwnd() : message_hwnd_,
                   L"重新整理", tip.c_str(), MistMsgTone::Warn)) {
    return;
  }
  OrganizeVirtual(/*quiet=*/false, /*reassign=*/true);
}

void App::OnOrganizeDone(OrganizeJobResult* job) {
  g_organize_busy = false;
  const bool run_pending = g_organize_pending;
  const bool pending_quiet = g_organize_pending_quiet;
  const bool pending_reassign = g_organize_pending_reassign;
  g_organize_pending = false;
  g_organize_pending_quiet = true;
  g_organize_pending_reassign = false;
  auto drain_pending = [&]() {
    if (!run_pending) {
      return;
    }
    if (pending_reassign) {
      OrganizeVirtual(/*quiet=*/false, /*reassign=*/true);
      return;
    }
    if (!pending_quiet) {
      OrganizeVirtual(/*quiet=*/false, /*reassign=*/false);
      return;
    }
    if (prefs_.auto_organize_watch) {
      OrganizeVirtual(true);
    }
  };
  if (!job) {
    drain_pending();
    return;
  }
  const bool quiet = job->quiet;
  const bool reassign = job->reassign;
  int applied = 0;
  // path → previous fence id (reassign 时从旧分区 pin_order 摘掉).
  std::unordered_map<std::wstring, std::string> left_old;
  // Scan-order membership changes (filled by OrganizeApplyDefaultPins).
  std::vector<std::pair<std::wstring, std::string>> neu = std::move(job->neu);
  if (reassign) {
    // Full remap: only apply if pins_ was not edited while the worker ran.
    if (job->pins_epoch != pins_epoch_) {
      LogLine(L"[organize] reassign aborted (pins edited mid-job)");
      delete job;
      RefreshDesktopIcons();
      ShowToast(L"整理期间钉选有变动，请重试");
      if (run_pending && prefs_.auto_organize_watch) {
        OrganizeVirtual(true);
      }
      return;
    }
    for (const auto& kv : neu) {
      const auto it = job->pins_at_start.find(NormalizePinKey(kv.first));
      if (it != job->pins_at_start.end() && !it->second.empty() &&
          it->second != kv.second) {
        left_old[kv.first] = it->second;
      }
    }
    applied = job->n;
    pins_ = std::move(job->pins);
  } else {
    // Virtual organize only pins leftovers (Fences-class). Merge new pins onto
    // the live map — never replace pins_ wholesale, never retarget anything
    // already in a fence (已在分区的不参与整理).
    std::vector<std::pair<std::wstring, std::string>> accepted;
    accepted.reserve(neu.size());
    for (const auto& kv : neu) {
      if (FindPin(pins_, kv.first)) {
        continue;  // already pinned — leave seat alone
      }
      SetPin(&pins_, kv.first, kv.second);
      accepted.push_back(kv);
      ++applied;
    }
    neu = std::move(accepted);
  }
  delete job;
  LogLine(L"[organize] virtual quiet=%d reassign=%d pins_applied=%d",
          quiet ? 1 : 0, reassign ? 1 : 0, applied);
  if (applied > 0 || reassign) {
    SavePins(pins_);
  }
  // Fences-class: newly organized icons append to each fence's pin_order so
  // manual sort / next refresh keeps seat (boot reconcile used to be the only
  // writer — order was lost until restart). Keep scan encounter order.
  if (!neu.empty() || !left_old.empty()) {
    if (!left_old.empty()) {
      std::unordered_map<std::string, std::vector<std::wstring>> leave;
      for (const auto& kv : left_old) {
        leave[kv.second].push_back(kv.first);
      }
      for (auto& f : fences_) {
        if (!f) {
          continue;
        }
        const auto it = leave.find(f->Id());
        if (it != leave.end() && !it->second.empty()) {
          f->RemovePinOrder(it->second);
        }
      }
    }
    std::unordered_map<std::string, std::vector<std::wstring>> by_fence;
    for (const auto& kv : neu) {
      by_fence[kv.second].push_back(kv.first);
    }
    for (auto& f : fences_) {
      if (!f) {
        continue;
      }
      const auto it = by_fence.find(f->Id());
      if (it == by_fence.end() || it->second.empty()) {
        continue;
      }
      f->InsertPinOrder(it->second, -1);
    }
    PersistLayouts();
  }
  EnsureOrganizeFencesForEmptyPages();
  RefreshDesktopIcons();
  // AIGC START
  // Mirror OnDesktopContentRefresh: pin heal / SetItems can leave the current
  // page fence mapped but under DefView (blank until 微信截图 FG restore).
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd()) {
      continue;
    }
    if (fences_hidden_ || peek_active_ ||
        !fence->VisibleOnPage(current_page_)) {
      continue;
    }
    if (IsWindowVisible(fence->Hwnd()) && !IsIconic(fence->Hwnd())) {
      fence->RefreshLayer();
      RaiseOverlayInDesktopBand(fence->Hwnd());
    }
  }
  ScheduleDesktopBandRestore();
  // AIGC END
  PushOrganizePage(&settings_, pins_, rules_, fences_, current_page_);
  if (quiet) {
    if (applied > 0) {
      ShowToast(L"已自动钉选 " + std::to_wstring(applied) + L" 项");
    }
  } else {
    wchar_t buf[160];
    if (reassign) {
      swprintf_s(buf, L"重新整理完成：%d 项按规则换了分区。", applied);
    } else {
      swprintf_s(buf, L"虚拟整理完成：写入 %d 条钉选。", applied);
    }
    ShowToast(buf);
  }
  // Drain coalesced organize requests that arrived while this job ran.
  drain_pending();
}

void App::SetOrganizeStartup(bool on) {
  prefs_.auto_organize_on_startup = on;
  SaveAppPrefs(prefs_);
}

void App::SetOrganizeWatch(bool on) {
  prefs_.auto_organize_watch = on;
  SaveAppPrefs(prefs_);
}

void App::SetDockVisible(bool on) {
  dock_visible_ = on;
  prefs_.dock_visible = on;
  SaveAppPrefs(prefs_);
  dock_.SetVisible(on);
  PersistLayouts();
  LogLine(L"[dock] visible=%d", on ? 1 : 0);
}

void App::SetPublicVisible(bool on) {
  prefs_.enable_public_desktop = on;
  SaveAppPrefs(prefs_);
  // Toggle cross-page sharing; loose floats still need Explorer icons hidden.
  RefreshDesktopIcons();
  LogLine(L"[public] shared=%d floats=%d", on ? 1 : 0,
          public_visible_ ? 1 : 0);
}

void App::SetPetVisible(bool on) {
  pet_visible_ = on;
  pet_.SetVisible(pet_visible_);
  SyncPageChrome();
  PersistLayouts();
  LogLine(L"[pet] visible=%d", on ? 1 : 0);
}

void App::ToggleDock() { SetDockVisible(!dock_visible_); }

void App::TogglePet() { SetPetVisible(!pet_visible_); }

void App::SetAutostart(bool on) {
  if (!SetNativeAutostart(on)) {
    MistAlert(settings_.Hwnd(), L"DeskTidy", L"无法更新开机启动快捷方式。",
              MistMsgTone::Warn);
  }
  settings_.SetToggles(dock_visible_, pet_visible_, IsNativeAutostartEnabled());
}

void App::ToggleAutostart() { SetAutostart(!IsNativeAutostartEnabled()); }

void App::EditRules() {
  std::vector<std::pair<std::string, std::wstring>> hints;
  hints.reserve(fences_.size());
  for (const auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    hints.push_back({fence->Id(), fence->Title()});
  }
  if (EditRulesDialog(settings_.Hwnd(), &rules_, hints)) {
    rules_ = LoadRules();
    ShowToast(L"整理规则已保存");
  }
}

void App::AddFence() {
  if (PageLockedForFences(current_page_)) {
    ShowToast(L"该页面已锁定，无法新建分区");
    return;
  }
  if (!fence_region_.Begin(instance_, [this](const RECT& screen) {
        AddFenceAtRect(screen);
      })) {
    // Fallback: place a default-sized fence if overlay fails.
    RECT r{80 + static_cast<int>(fences_.size()) * 28,
           80 + static_cast<int>(fences_.size()) * 28, 0, 0};
    r.right = r.left + 260;
    r.bottom = r.top + 280;
    AddFenceAtRect(r);
  }
}

void App::AddFenceAtRect(const RECT& screen) {
  if (PageLockedForFences(current_page_)) {
    ShowToast(L"该页面已锁定，无法新建分区");
    return;
  }
  FenceLayout layout;
  layout.id = MakeFenceId();
  layout.title = L"新建分区";
  layout.x = screen.left;
  layout.y = screen.top;
  int drawn_w = static_cast<int>(screen.right - screen.left);
  int drawn_h = static_cast<int>(screen.bottom - screen.top);
  if (drawn_w < 200 || drawn_h < 160) {
    drawn_w = kFenceDefaultW;
    drawn_h = kFenceDefaultH;
  }
  layout.w = (std::max)(kFenceMinW, drawn_w);
  layout.h = (std::max)(kFenceMinH, drawn_h);
  layout.page = current_page_;
  layout.pages = {current_page_};
  // Align with page-bar create: default file landing zone (empty = 不参与整理).
  layout.organize_kinds = {"file"};
  layout.style_id = prefs_.theme_id.empty() ? "navy" : prefs_.theme_id; // 跟随当前分区样式，不联动全局主题
  if (!SpawnFence(layout)) {
    return;
  }
  // Fences: icons whose centers fall in the drawn rect join the new fence.
  auto point_in = [&](POINT pt) {
    return pt.x >= screen.left && pt.x < screen.right && pt.y >= screen.top &&
           pt.y < screen.bottom;
  };
  auto rect_hits = [&](const RECT& r) {
    const POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    return point_in(c);
  };
  std::unordered_set<std::wstring> scooped;
  // Encounter-order paths for pin_order (avoids N× ScanDesktopItems recovery).
  std::vector<std::wstring> scooped_paths;
  auto try_add = [&](const std::wstring& path) {
    if (path.empty() || FindPin(pins_, path)) {
      return;
    }
    const std::wstring key = NormalizePinKey(path);
    if (!scooped.insert(key).second) {
      return;
    }
    scooped_paths.push_back(path);
    SetPin(&pins_, path, layout.id);
    EraseFloatPlacement(&float_placements_, path);
  };
  for (const auto& kv : float_placements_) {
    if (!kv.second.has_pos || kv.second.page != current_page_) {
      continue;
    }
    if (point_in(FloatScreenPoint(kv.second))) {
      try_add(kv.second.path);
    }
  }
  for (const auto& pr : public_host_.SlotScreenRects()) {
    if (rect_hits(pr.second)) {
      try_add(pr.first);
    }
  }
  // When Explorer icons are visible, scoop by DefView positions (name match).
  if (icons_visible_intent_) {
    const auto icons = CaptureShellIconPositions();
    HWND lv = nullptr;
    const DefViewHost host = FindDefViewHost();
    if (host.defview) {
      lv = FindWindowExW(host.defview, nullptr, L"SysListView32", nullptr);
    }
    std::unordered_map<std::wstring, std::wstring> by_name;
    for (const auto& it : ScanDesktopItems()) {
      if (FindPin(pins_, it.path)) {
        continue;
      }
      by_name[it.name] = it.path;
    }
    for (const auto& icon : icons) {
      POINT pt{icon.x, icon.y};
      if (lv) {
        ClientToScreen(lv, &pt);
      }
      pt.x += 24;
      pt.y += 24;
      if (!point_in(pt)) {
        continue;
      }
      auto it = by_name.find(icon.name);
      if (it != by_name.end()) {
        try_add(it->second);
      }
    }
  }
  if (!scooped.empty()) {
    BumpPinsEpoch();
    SavePins(pins_);
    SaveFloatPlacements(float_placements_);
    // Fences: scoop preserves on-screen encounter order as manual seats
    // (default sort_by=name would ignore pin_order and scramble icons).
    if (!fences_.empty() && fences_.back() &&
        fences_.back()->Id() == layout.id) {
      fences_.back()->SetSortBy("manual");
      fences_.back()->InsertPinOrder(scooped_paths, -1);
    }
    RefreshDesktopIcons();
  }
  SeatAllFencesUnderApps();
  PersistLayouts();
  RefreshSettingsList();
  ApplyPageVisibility();
  if (settings_.IsOpen()) {
    settings_.Show();
  }
}

void App::RemoveFenceAt(int index) {
  if (index < 0 || index >= static_cast<int>(fences_.size())) {
    return;
  }
  // Settings ✕ uses the same path as desktop「解散」— confirm, protect system
  // fences, and clear pins (raw erase looked like a no-op when system fences
  // were recreated on refresh).
  DissolveFence(fences_[static_cast<size_t>(index)].get());
}

void App::DissolveFence(FenceWindow* fence) {
  if (!fence) {
    return;
  }
  const std::string id = fence->Id();
  // P2-13: system fences cannot be dissolved (Python protection tip).
  if (id.rfind("system_", 0) == 0) {
    MistAlert(settings_.Hwnd(), L"解散分区",
              L"系统分区不可删除。\n\n可清空内容或隐藏，但不会删除系统分区本身。",
              MistMsgTone::Info);
    return;
  }
  if (PageLockedForFences(fence->Page())) {
    ShowToast(L"该页面已锁定，无法删除分区");
    return;
  }
  const std::wstring tip =
      L"确定删除分区「" + fence->Title() +
      L"」？\n\n钉选图标将回到桌面，分区本身会被删除。";
  if (!MistConfirm(settings_.Hwnd(), L"删除分区", tip.c_str(),
                   MistMsgTone::Danger)) {
    return;
  }
  // Find and remove this fence from the live list.
  for (size_t i = 0; i < fences_.size(); ++i) {
    if (fences_[i].get() != fence) {
      continue;
    }
    // Snapshot pins before Destroy — icons become public floats.
    std::vector<std::wstring> to_clear;
    for (const auto& kv : pins_) {
      if (kv.second == id) {
        to_clear.push_back(kv.first);
      }
    }
    fences_[i]->Destroy();
    fences_.erase(fences_.begin() + static_cast<ptrdiff_t>(i));
    // Clear pins that pointed at the dissolved fence (icons fall back to the
    // real desktop, matching Python dissolve_fence_to_page). Suppress watch
    // organize so the next tick cannot suck them straight back into a fence.
    for (const auto& p : to_clear) {
      SuppressOrganizePath(p, 24.0 * 3600.0);
      ClearPin(&pins_, p);
      if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SetFloatPage(&float_placements_, p, current_page_,
                     /*page_local=*/false);
        auto it = float_placements_.find(NormalizePinKey(p));
        if (it != float_placements_.end()) {
          it->second.has_pos = false;
          it->second.path = p;
        }
      } else {
        EraseFloatPlacement(&float_placements_, p);
      }
    }
    if (!to_clear.empty()) {
      BumpPinsEpoch();
      SavePins(pins_);
      SaveFloatPlacements(float_placements_);
    }
    SyncFenceTabGroups();
    ApplyPageVisibility();
    PersistLayouts();
    RefreshSettingsList();
    RefreshDesktopIcons();
    return;
  }
}

void App::RenameFenceAt(int index, const std::wstring& title) {
  if (index < 0 || index >= static_cast<int>(fences_.size()) || title.empty()) {
    return;
  }
  fences_[static_cast<size_t>(index)]->SetTitle(title);
  SyncFenceTabGroups();
  PersistLayouts();
  RefreshSettingsList();
}

void App::SetFencePageAt(int index, int page) {
  if (index < 0 || index >= static_cast<int>(fences_.size())) {
    return;
  }
  FenceWindow* f = fences_[static_cast<size_t>(index)].get();
  if (!f) {
    return;
  }
  if (PageLockedForFences(page) || PageLockedForFences(f->Page())) {
    ShowToast(L"该页面已锁定，无法移动分区");
    return;
  }
  if (!f->TabGroup().empty()) {
    f->SetTabGroup("");
    f->SetTabActive(false);
    SyncFenceTabGroups();
  }
  f->SetPage(page);
  PersistLayouts();
  ApplyPageVisibility();
  RefreshSettingsList();
  PushExtensionSettings();
}

void App::CreateTrayIcon() {
  if (nid_.hIcon) {
    DestroyIcon(nid_.hIcon);
    nid_.hIcon = nullptr;
  }
  ZeroMemory(&nid_, sizeof(nid_));
  nid_.cbSize = sizeof(nid_);
  nid_.hWnd = message_hwnd_;
  nid_.uID = 1;
  nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid_.uCallbackMessage = kTrayMsg;
  // Prefer 16px for classic tray; shell may pick a larger source on HiDPI.
  nid_.hIcon = CreateBrandMarkIcon(32);
  if (!nid_.hIcon) {
    nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  }
  const std::wstring tip =
      IsDevRuntime() ? (L"DeskTidy [dev] v" + NativeDisplayVersion())
                     : (L"DeskTidy v" + NativeProductVersion());
  wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
  tray_added_ = Shell_NotifyIconW(NIM_ADD, &nid_) == TRUE;
  if (tray_added_) {
    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);
  }
}

void App::RemoveTrayIcon() {
  if (tray_added_) {
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    tray_added_ = false;
  }
  if (nid_.hIcon) {
    DestroyIcon(nid_.hIcon);
    nid_.hIcon = nullptr;
  }
}

LRESULT CALLBACK App::TrayWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam) {
  static const UINT s_msg_taskbar_created =
      RegisterWindowMessageW(L"TaskbarCreated");
  App* self =
      reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<App*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (s_msg_taskbar_created != 0 && msg == s_msg_taskbar_created) {
    self->CreateTrayIcon();
    self->ScheduleDesktopBandRestore();
    return 0;
  }

  switch (msg) {
    case kMsgBandRestore:
      // wParam=1 from Win+D LL hook (and elevated helper show_desktop): drop
      // fullscreen cover first so we don't TOPMOST-lift fences over a game.
      if (wparam) {
        MinimizeFullscreenCover();
        self->band_restore_force_ = true;
        self->win_d_restore_deadline_ms_ = GetTickCount64() + 1500;
      }
      self->ScheduleDesktopBandRestore();
      return 0;
    case kMsgHideOffPage:
      self->HideOffPageFences();
      return 0;
    case kMsgSeatUnderApps:
      self->SeatAllFencesUnderApps();
      return 0;
    case kMsgFgChanged: {
      // Drain burst (Word/Office can fire many FG events during splash).
      MSG drain{};
      while (PeekMessageW(&drain, hwnd, kMsgFgChanged, kMsgFgChanged,
                          PM_REMOVE)) {
      }
      if (IsExplorerDesktopForeground()) {
        if (IsShowDesktopActive() || self->IsWinDRestoring()) {
          self->ScheduleDesktopBandRestore();
        } else if (!self->IsWinDRestoring()) {
          self->SeatAllFencesUnderApps();
        }
      } else {
        self->HideOffPageFences();
      }
      return 0;
    }
    case kMsgRevealPublic:
      self->RevealPublicFloatsNow();
      return 0;
    case kMsgShellNotify: {
      // SHCNRF_NewDelivery: lock, then debounce a single membership refresh.
      // Defense in depth: ignore UPDATEITEM even if an older register mask
      // still delivers it (copy progress must not rebuild fences).
      long event = 0;
      PIDLIST_ABSOLUTE* pidls = nullptr;
      // SHCNRF_NewDelivery: wParam is the change handle and lParam is the
      // SENDING PROCESS ID — it is NOT a value to pass back as dwProcId.
      // Passing lParam made every lock fail, so all shell notifications were
      // dropped and desktop changes only surfaced on the ~10 s poll.
      HANDLE lock = SHChangeNotification_Lock(
          reinterpret_cast<HANDLE>(wparam), GetCurrentProcessId(), &pidls,
          &event);
      if (lock) {
        std::wstring rename_from;
        std::wstring rename_to;
        std::wstring deleted_path;
        std::wstring created_path;
        if ((event & (SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER)) != 0 && pidls) {
          wchar_t buf[MAX_PATH] = {};
          if (pidls[0] && SHGetPathFromIDListW(pidls[0], buf) && buf[0]) {
            rename_from = buf;
          }
          buf[0] = 0;
          if (pidls[1] && SHGetPathFromIDListW(pidls[1], buf) && buf[0]) {
            rename_to = buf;
          }
        } else if ((event & (SHCNE_DELETE | SHCNE_RMDIR)) != 0 && pidls &&
                   pidls[0]) {
          wchar_t buf[MAX_PATH] = {};
          if (SHGetPathFromIDListW(pidls[0], buf) && buf[0]) {
            deleted_path = buf;
          }
        } else if ((event & (SHCNE_CREATE | SHCNE_MKDIR)) != 0 && pidls &&
                   pidls[0]) {
          wchar_t buf[MAX_PATH] = {};
          if (SHGetPathFromIDListW(pidls[0], buf) && buf[0]) {
            created_path = buf;
          }
        }
        const long membership =
            SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
            SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEDIR;
        const bool refresh = (event & membership) != 0;
        SHChangeNotification_Unlock(lock);
        // Fences: external rename keeps the same fence seat immediately.
        if (!rename_from.empty() && !rename_to.empty() &&
            NormalizePinKey(rename_from) != NormalizePinKey(rename_to)) {
          self->RetargetPin(rename_from, rename_to);
        }
        // Explorer Cut/Delete: drop pin now — sticky 45s prune is too late and
        // can retarget a same-name Desktop arrival into the old fence.
        if (!deleted_path.empty()) {
          self->ForgetDeletedDesktopPath(deleted_path);
        }
        // DefView「新建」on blank wallpaper (float host is only a tight bbox):
        // keep as loose float — same SuppressOrganize as PublicHost New.
        if (!created_path.empty()) {
          self->MaybeSuppressDesktopShellNew(created_path);
        }
        if (refresh) {
          self->ScheduleDesktopContentRefresh();
        }
      }
      return 0;
    }
    case kMsgDesktopDblClick:
      self->OnDesktopBlankDoubleClick();
      return 0;
    case kMsgDesktopBlankDown:
      self->OnDesktopBlankLeftDown(lparam);
      return 0;
    case kMsgDesktopRubberMove: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      self->public_host_.UpdateRubberFromScreen(pt);
      return 0;
    }
    case kMsgDesktopRubberUp: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      self->public_host_.EndRubberFromScreen(pt);
      return 0;
    }
    case kMsgDesktopPaste:
      self->OnDesktopPaste(wparam != 0);
      return 0;
    case kMsgDesktopBlankRClick:
      self->OnDesktopBlankRightClick(lparam);
      return 0;
    case kDeskTidyMsgIconsPreloaded:
      for (auto& fence : self->fences_) {
        if (fence) {
          fence->FillMissingIcons();
        }
      }
      if (self->public_visible_) {
        self->public_host_.FillMissingIcons();
      }
      if (self->dock_visible_) {
        self->dock_.FillMissingIcons();
      }
      return 0;
    case WM_CLIPBOARDUPDATE:
      // Debounce cut-ghost refreshes: clipboard storms (Office/Explorer) used
      // to N× PresentLayered every visible fence and hitch the UI thread.
      KillTimer(hwnd, kTimerClipboard);
      SetTimer(hwnd, kTimerClipboard, 120, nullptr);
      return 0;
    case WM_TIMER:
      if (wparam == kTimerWebViewWarm) {
        KillTimer(hwnd, kTimerWebViewWarm);
        // Third-party runtime (not OS-version gated): log presence so Win10/Win11
        // support can degrade clearly when Edge WebView2 is missing.
        {
          auto probe_wv2 = [](HKEY root, const wchar_t* path) -> bool {
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
              return false;
            }
            RegCloseKey(key);
            return true;
          };
          const wchar_t* kWv2Wow =
              L"SOFTWARE\\WOW6432Node\\Microsoft\\EdgeUpdate\\Clients\\"
              L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
          const wchar_t* kWv2 =
              L"SOFTWARE\\Microsoft\\EdgeUpdate\\Clients\\"
              L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
          const bool present =
              probe_wv2(HKEY_LOCAL_MACHINE, kWv2Wow) ||
              probe_wv2(HKEY_LOCAL_MACHINE, kWv2) ||
              probe_wv2(HKEY_CURRENT_USER, kWv2);
          LogLine(L"[compat] webview2_runtime=%s (settings need it; "
                  L"fences/dock/pet do not)",
                  present ? L"present" : L"missing");
        }
        // Deferred warm of the two most-used WebView2 panels; todo/search stay
        // lazy (created on first Show) to keep the boot path browser-free.
        self->settings_.WarmWebView();
        return 0;
      }
      if (wparam == kTimerOverlayRefresh) {
        // Re-upload visible overlays a few times after boot so hybrid-GPU
        // (Optimus / AMD switchable) machines that lost the initial layered
        // blit recover without a desktop click.
        self->RefreshOverlays();
        if (--self->overlay_refresh_left_ > 0) {
          SetTimer(hwnd, kTimerOverlayRefresh, 1600, nullptr);
        } else {
          KillTimer(hwnd, kTimerOverlayRefresh);
        }
        return 0;
      }
      if (wparam == kTimerRestore) {
        bool apps_fg_quiet = false;
        if (self->DesktopChromeMayRestack()) {
          // Periodic keepalive (4s): restack when buried. SoftSeat only uses
          // FRAMECHANGED on real owner/style changes now — do NOT re-Present
          // every healthy fence (was N× full ULW blit every 4s).
          // Under open apps: Fences-style quiet — dirty ULW only, no Healthy /
          // Z-order walk (burial is irrelevant under APPWINDOWs; desk FG hook
          // restacks when the user returns to the desktop).
          apps_fg_quiet = IsAppsForegroundQuiet();
          // Show-desktop (Win+D) outranks the "apps quiet" shortcut: while the
          // desktop is what the user is looking at, a hidden/buried fence is
          // visible breakage, not an acceptable idle state. The quiet branch
          // only refreshes dirty ULW and never re-seats Z-order, so taking it
          // here would leave the fences missing until the next user action.
          const bool show_desktop_now = IsShowDesktopActive();
          if (apps_fg_quiet && !show_desktop_now) {
            for (auto& fence : self->fences_) {
              if (!fence || !fence->Hwnd() ||
                  !fence->VisibleOnPage(self->current_page_)) {
                continue;
              }
              if (!IsWindowVisible(fence->Hwnd()) || IsIconic(fence->Hwnd())) {
                continue;
              }
              if (fence->LayerDirty()) {
                fence->RefreshLayer();
              }
            }
          } else {
            const bool unhealthy = !self->DesktopChromeHealthy();
            for (auto& fence : self->fences_) {
              if (!fence || !fence->Hwnd() ||
                  !fence->VisibleOnPage(self->current_page_)) {
                continue;
              }
              // Tabbed fences: hidden group members are not "buried".
              if (!fence->TabGroup().empty() && !fence->TabActive()) {
                continue;
              }
              if (!IsWindowVisible(fence->Hwnd()) || IsIconic(fence->Hwnd())) {
                continue;
              }
              if (fence->LayerDirty() || unhealthy ||
                  !IsOverlayAboveDefView(fence->Hwnd())) {
                fence->RefreshLayer();
              }
            }
            self->RestoreDesktopChrome();
          }
        }
        // Stretch under apps FG (market: idle chrome nearly silent).
        // When settings/modals block restack, keep 4s so we resume promptly.
        const UINT want =
            (self->DesktopChromeMayRestack() && apps_fg_quiet) ? 10000u
                                                              : 4000u;
        if (want != self->restore_tick_ms_) {
          self->restore_tick_ms_ = want;
          SetTimer(hwnd, kTimerRestore, want, nullptr);
        }
        return 0;
      }
      if (wparam == kTimerBandSettle) {
        if (!self->DesktopChromeMayRestack()) {
          KillTimer(hwnd, kTimerBandSettle);
          self->band_restore_left_ = 0;
          self->band_restore_force_ = false;
          self->band_settle_started_ms_ = 0;
          return 0;
        }
        self->RestoreDesktopChrome();
        if (--self->band_restore_left_ <= 0) {
          KillTimer(hwnd, kTimerBandSettle);
          self->band_restore_left_ = 0;
          self->band_restore_force_ = false;
          self->band_settle_started_ms_ = 0;
        } else {
          // Budget still has ticks left — extend it if the band state has not
          // settled yet (the Win+D animation is longer than 3 × 100ms on some
          // systems, and spending the whole budget mid-animation left the
          // fences hidden until the next keepalive).
          self->ExtendBandSettleIfTransitioning();
        }
        return 0;
      }
      if (wparam == kTimerPet) {
        self->pet_.Tick();
        // P2-24: match Python idle throttle (33ms active / ~220ms idle+away).
        const UINT want =
            static_cast<UINT>((std::max)(16, self->pet_.WantedTickMs()));
        if (want != self->pet_tick_ms_) {
          self->pet_tick_ms_ = want;
          SetTimer(hwnd, kTimerPet, want, nullptr);
        }
        // Only poll ffmpeg when a session may be live — merge/prompt is heavy.
        if (ScreenRecordSessionLive()) {
          (void)IsScreenRecording();
        }
        return 0;
      }
      if (wparam == kTimerWatch) {
        if (self->session_locked_) {
          return 0;
        }
        self->OnDisplayTopologyMaybeChanged();
        // Watch portal root + drilled-in cwd (scan uses PortalBrowsePath).
        std::vector<std::wstring> portals;
        for (const auto& fence : self->fences_) {
          if (!fence || fence->PortalPath().empty()) {
            continue;
          }
          portals.push_back(fence->PortalPath());
          const std::wstring browse = fence->PortalBrowsePath();
          if (!browse.empty() &&
              _wcsicmp(browse.c_str(), fence->PortalPath().c_str()) != 0) {
            portals.push_back(browse);
          }
        }
        self->desktop_watch_.SetExtraFolders(portals);
        // Sticky-prune dead pins (Python prune_missing_virtual_items): runs on
        // the same 2s tick, throttled to 30s, 45s grace for save-then-rewrite.
        self->PruneStickyMissingPins();
        // Desktop/user folder changes come from SHChangeNotify. Poll is the
        // backup for portal folders + OneDrive quirks that skip shell events.
        if (self->desktop_watch_.PollChanged()) {
          self->ScheduleDesktopContentRefresh();
        }
        // Fences/Rainmeter keepalive: Explorer rebuild or burial under DefView.
        // Under open apps, skip Healthy walk (restore timer covers dirty); still
        // react to Explorer rebuild so a shell restart does not leave ghosts.
        const bool apps_fg_quiet = IsAppsForegroundQuiet();
        if (PollExplorerDesktopRebuild() ||
            (!apps_fg_quiet && !self->DesktopChromeHealthy())) {
          self->ScheduleDesktopBandRestore();
        }
        const UINT want = apps_fg_quiet ? 8000u : 2000u;
        if (want != self->watch_tick_ms_) {
          self->watch_tick_ms_ = want;
          SetTimer(hwnd, kTimerWatch, want, nullptr);
        }
        return 0;
      }
      if (wparam == kTimerDesktopShell) {
        KillTimer(hwnd, kTimerDesktopShell);
        self->OnDesktopContentRefresh();
        return 0;
      }
      if (wparam == kTimerUpdate) {
        KillTimer(hwnd, kTimerUpdate);
        self->update_timer_ = 0;
        self->MaybeStartupUpdateCheck();
        return 0;
      }
      if (wparam == kTimerOpenSettingsOrg) {
        KillTimer(hwnd, kTimerOpenSettingsOrg);
        if (self->settings_.IsOpen()) {
          PushOrganizePage(&self->settings_, self->pins_, self->rules_,
                           self->fences_, self->current_page_);
        }
        return 0;
      }
      if (wparam == kTimerIconsRefresh) {
        KillTimer(hwnd, kTimerIconsRefresh);
        self->RefreshIconGrids();
        self->RefreshDesktopIcons();
        return 0;
      }
      if (wparam == kTimerMemTrim) {
        // Periodic background trim when settings is not the active UI.
        if (!self->settings_.IsOpen()) {
          self->TrimMemory();
        }
        return 0;
      }
      if (wparam == kTimerPersistLayouts) {
        KillTimer(hwnd, kTimerPersistLayouts);
        self->PersistLayouts();
        return 0;
      }
      if (wparam == kTimerWarmFences) {
        KillTimer(hwnd, kTimerWarmFences);
        // Warm one dirty soft-parked fence per tick (keeps UI responsive).
        bool warmed = false;
        for (auto& fence : self->fences_) {
          if (!fence || !fence->Hwnd() || !fence->LayerDirty()) {
            continue;
          }
          if (IsWindowVisible(fence->Hwnd()) && !IsIconic(fence->Hwnd())) {
            continue;
          }
          const int page = fence->Page();
          fence->ApplyPageGeometry(fence->GeometryForPage(page),
                                   /*present=*/false);
          fence->WarmLayerOffscreen();
          warmed = true;
          break;
        }
        if (warmed) {
          for (const auto& fence : self->fences_) {
            if (fence && fence->LayerDirty() && fence->Hwnd() &&
                (!IsWindowVisible(fence->Hwnd()) ||
                 IsIconic(fence->Hwnd()))) {
              SetTimer(hwnd, kTimerWarmFences, kWarmFencesTickMs, nullptr);
              break;
            }
          }
        }
        return 0;
      }
      if (wparam == kTimerClipboard) {
        KillTimer(hwnd, kTimerClipboard);
        // Only repaint when cut-ghost state may have changed (cut on, or
        // clearing a previous cut). Plain text/Office clipboard noise skips.
        bool cut = false;
        std::vector<std::wstring> files;
        ClipboardGetFilesWithEffect(&files, &cut);
        static bool had_cut_ghost = false;
        if (!cut && !had_cut_ghost) {
          return 0;
        }
        had_cut_ghost = cut;
        for (auto& fence : self->fences_) {
          if (!fence || !fence->Hwnd()) {
            continue;
          }
          if (IsWindowVisible(fence->Hwnd()) && !IsIconic(fence->Hwnd())) {
            fence->RefreshLayer();
          }
        }
        // Public floats share Explorer cut-ghost dimming.
        if (self->public_host_.HasItems()) {
          self->public_host_.RefreshLayer();
        }
        return 0;
      }
      if (wparam == kTimerRevealPublic) {
        KillTimer(hwnd, kTimerRevealPublic);
        // Quiet re-blit only — full ForceRemapPresent here would flash.
        if (!self->icons_visible_intent_ && self->public_host_.HasItems()) {
          self->public_host_.RefreshLayer();
        }
        self->reveal_public_left_ = 0;
        return 0;
      }
      if (wparam == kTimerReflowPublic) {
        KillTimer(hwnd, kTimerReflowPublic);
        // Post-page-switch: pet clear + float reflow (kept off the click path).
        if (self->pet_visible_) {
          std::vector<RECT> fence_rects;
          for (const auto& fence : self->fences_) {
            if (!fence || !fence->Hwnd() || !IsWindowVisible(fence->Hwnd())) {
              continue;
            }
            if (!fence->VisibleOnPage(self->current_page_)) {
              continue;
            }
            RECT wr{};
            if (GetWindowRect(fence->Hwnd(), &wr)) {
              fence_rects.push_back(wr);
            }
          }
          self->pet_.NudgeClearOfFenceBottoms(fence_rects);
        }
        if (self->public_visible_) {
          // Page membership lives in float_placements — rebuild, don't just
          // reflow the previous page's slots.
          self->RebuildPublicItems();
        }
        return 0;
      }
      if (wparam == kTimerDisplayChange) {
        KillTimer(hwnd, kTimerDisplayChange);
        self->display_change_timer_ = 0;
        self->OnDisplayChangeTimerFired();
        return 0;
      }
      break;
    case kMsgOrganizeDone:
      self->OnOrganizeDone(reinterpret_cast<OrganizeJobResult*>(lparam));
      return 0;
    case kMsgUpdateCheckDone:
      self->OnUpdateCheckDone(reinterpret_cast<UpdateCheckResult*>(lparam));
      return 0;
    case kMsgUpdateInstallDone:
      self->OnUpdateInstallDone(reinterpret_cast<UpdateInstallResult*>(lparam));
      return 0;
    case kMsgUpdateInstallLaunched:
      self->OnUpdateInstallLaunched(
          reinterpret_cast<UpdateInstallResult*>(lparam));
      return 0;
    case kMsgUpdateProgress:
      self->OnUpdateProgress(static_cast<int>(wparam));
      return 0;
    case kTrayMsg: {
      // NOTIFYICON_VERSION_4 packs the event in LOWORD(lparam).
      const UINT tray_ev = LOWORD(lparam);
      if (tray_ev == WM_LBUTTONDBLCLK || tray_ev == WM_LBUTTONUP ||
          tray_ev == NIN_SELECT || tray_ev == NIN_KEYSELECT) {
        // Market/tray apps: if settings is already up (or minimized), always
        // raise it — debounce only coalesces closed→open click storms
        // (VERSION_4 NIN_SELECT + WM_LBUTTONUP).
        if (self->settings_.IsOpen() ||
            (self->settings_.Hwnd() && IsIconic(self->settings_.Hwnd()))) {
          self->OpenSettings();
        } else if (self->TryConsumeTrayOpen()) {
          self->OpenSettings();
        }
        return 0;
      }
      if (tray_ev == WM_RBUTTONUP || tray_ev == WM_CONTEXTMENU) {
        // Market/Python tray: short primary list + 扩展/帮助 submenus (not a
        // kitchen-sink of every settings action).
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        const std::wstring ver_hdr =
            IsDevRuntime()
                ? (L"DeskTidy v" + NativeDisplayVersion() + L" [源码版]")
                : (L"DeskTidy v" + NativeProductVersion());
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, ver_hdr.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 1, L"打开设置");
        AppendMenuW(menu, MF_STRING, 17, Tr("hk_organize").c_str());
        AppendMenuW(menu, MF_STRING, 13, Tr("peek").c_str());
        AppendMenuW(menu, MF_STRING, 7, Tr("screenshot").c_str());
        if (IsScreenRecording() || self->prefs_.record_enabled) {
          AppendMenuW(
              menu, MF_STRING, 8,
              IsScreenRecording()
                  ? (IsScreenRecordPaused() ? L"继续录屏" : L"停止录屏")
                  : Tr("record").c_str());
        }

        HMENU ext = CreatePopupMenu();
        if (self->prefs_.todos_enabled) {
          AppendMenuW(ext, MF_STRING, 3, Tr("todos").c_str());
        }
        // DeskNote is a separate exe; tray entry stays (shortcuts gated by prefs).
        AppendMenuW(ext, MF_STRING, 2, Tr("notes").c_str());
        if (self->prefs_.minutes_enabled) {
          AppendMenuW(ext, MF_STRING, 6, Tr("minutes").c_str());
        }
        if (self->prefs_.calculator_enabled) {
          AppendMenuW(ext, MF_STRING, 32, Tr("calculator").c_str());
        }
        AppendMenuW(ext, MF_STRING, 4, Tr("search").c_str());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(ext),
                    L"扩展功能");

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        HMENU help = CreatePopupMenu();
        AppendMenuW(help, MF_STRING, 34, L"使用说明");
        AppendMenuW(help, MF_STRING, 10, Tr("updates").c_str());
        AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(help, MF_STRING, 41, L"关于");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help),
                    Tr("help").c_str());

        AppendMenuW(menu, MF_STRING, 29, Tr("exit").c_str());
        SetForegroundWindow(hwnd);
        const int cmd =
            TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                           pt.x, pt.y, 0, hwnd, nullptr);
        // Required so the menu dismisses correctly next click.
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        switch (cmd) {
          case 1:
            self->OpenSettings();
            break;
          case 2:
            self->OpenNotes();
            break;
          case 3:
            self->OpenTodos();
            break;
          case 4:
            self->OpenSearch();
            break;
          case 6:
            self->OpenMinutes();
            break;
          case 7:
            self->StartRegionCapture();
            break;
          case 8:
            if (IsScreenRecording() && IsScreenRecordPaused()) {
              PauseScreenRecord(false);
            } else {
              StartScreenRecord(self->settings_.Hwnd());
            }
            break;
          case 10:
            OpenUpdatePage(self->settings_.Hwnd());
            break;
          case 13:
            self->TogglePeek();
            break;
          case 14:
            self->ToggleFencesVisible();
            break;
          case 17:
            self->OrganizeVirtual();
            break;
          case 32:
            OpenCalculator();
            break;
          case 34:
            ShowAppHelp(self->settings_.Hwnd());
            break;
          case 41: {
            const std::wstring channel_tag =
                IsDevRuntime() ? L"（源码开发版）" : L"（正式版）";
            const std::wstring about_msg =
                L"DeskTidy 桌面整理" + channel_tag +
                L"\n版本 " + NativeDisplayVersion() +
                L"\n目录 " + NativeExeDir() +
                L"\n\n官网 / 发布页：\ngithub.com/Bowen-Yu0926/DeskTidy-Releases\n\n"
                L"「是」打开发布页\n「否」打开使用说明";
            if (MistConfirm(hwnd, L"关于 DeskTidy", about_msg.c_str(),
                            MistMsgTone::Info)) {
              ShellExecuteW(
                  nullptr, L"open",
                  L"https://github.com/Bowen-Yu0926/DeskTidy-Releases/releases/latest",
                  nullptr, nullptr, SW_SHOWNORMAL);
            } else {
              ShowAppHelp(self->settings_.Hwnd());
            }
            break;
          }
          case 29:
            self->RequestQuit();
            break;
          default:
            break;
        }
        return 0;
      }
      return 0;
    }
    case WM_DPICHANGED: {
      const int new_dpi = static_cast<int>(HIWORD(wparam));
      const int old_dpi = self->app_dpi_ > 0 ? self->app_dpi_ : 96;
      if (new_dpi > 0) {
        self->app_dpi_ = new_dpi;
      }
      LogLine(L"[dpi] WM_DPICHANGED %d -> %d (chrome refresh, no fence frame scale)",
              old_dpi, new_dpi);
      // Fences-class / PerMonitorV2: fence frames are screen-space; chrome
      // paints via GetDpiForWindow. Globally ScaleGeometryForDpi every fence
      // from the message window's DPI delta mis-scales secondary monitors.
      const IconGridMetrics grid = QueryIconGridMetrics();
      for (auto& fence : self->fences_) {
        if (fence) {
          fence->SetIconGrid(grid);
        }
      }
      auto window_dpi = [](HWND hwnd) -> int {
        if (!hwnd || !IsWindow(hwnd)) {
          return SystemDpi();
        }
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        POINT c{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
        return DpiAtPoint(c);
      };
      // Page bar / dock: re-apply design metrics for the monitor they sit on.
      self->page_bar_.ScaleForDpi(96, window_dpi(self->page_bar_.Hwnd()));
      self->dock_.RelayoutForDpi();
      // Pet / public: only rescale geometry when that window's monitor DPI
      // actually matches the notification (avoid cross-monitor stomps).
      auto maybe_rescale = [new_dpi, old_dpi, &window_dpi](HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) {
          return;
        }
        if (window_dpi(hwnd) != new_dpi) {
          return;
        }
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        int x = rc.left;
        int y = rc.top;
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        ScaleRectForDpi(&x, &y, &w, &h, old_dpi, new_dpi);
        SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
      };
      maybe_rescale(self->pet_.Hwnd());
      maybe_rescale(self->public_host_.Hwnd());
      // Caption font + icon bitmaps are baked at the old DPI; drop them so the
      // next paint (RebuildPublicItems below) rebuilds at the new one.
      self->public_host_.InvalidateDpiCaches();
      self->RebuildDockItems();
      self->RebuildPublicItems();
      self->pet_.SetPages(self->page_names_, self->current_page_);
      self->PersistLayouts();
      return 0;
    }
    case WM_DWMCOMPOSITIONCHANGED: {
      // DWM reconnect (Optimus/AMD adapter switch, RDP, driver reset) drops
      // previously uploaded layered pixels while windows stay "visible".
      // Re-upload on a short delay so the compositor finishes re-initializing.
      // Use max() so a DWM event never truncates the boot recovery budget.
      self->overlay_refresh_left_ =
          (std::max)(self->overlay_refresh_left_, 2);
      SetTimer(hwnd, kTimerOverlayRefresh, 400, nullptr);
      return 0;
    }
    case WM_SETTINGCHANGE: {
      // P2-17: Explorer icon size / spacing changes → refresh fence grids.
      if (wparam == SPI_SETICONMETRICS || wparam == SPI_SETICONS ||
          wparam == SPI_SETNONCLIENTMETRICS ||
          (lparam &&
           (_wcsicmp(reinterpret_cast<LPCWSTR>(lparam), L"WindowMetrics") ==
                0 ||
            _wcsicmp(reinterpret_cast<LPCWSTR>(lparam), L"Environment") ==
                0))) {
        SetTimer(hwnd, kTimerIconsRefresh, 300, nullptr);
      }
      if (wparam == SPI_SETDESKWALLPAPER ||
          (lparam &&
           _wcsicmp(reinterpret_cast<LPCWSTR>(lparam), L"ImmersiveColorSet") ==
               0)) {
        RefreshChameleonPalette();
        for (auto& fence : self->fences_) {
          if (fence && fence->StyleId() == kChameleonStyleId) {
            fence->RefreshStyleFromPreset();
          }
        }
        if (self->prefs_.page_bar_style == kChameleonStyleId) {
          self->ApplyChromeTheme();
        }
        self->PushExtensionSettings();
      }
      break;
    }
    case WM_WTSSESSION_CHANGE: {
      if (wparam == WTS_SESSION_LOCK) {
        self->session_locked_ = true;
        LogLine(L"[session] locked - pausing animations, watch and display handling");
        if (self->pet_timer_) {
          KillTimer(hwnd, kTimerPet);
          self->pet_timer_ = 0;
        }
        if (self->restore_timer_) {
          KillTimer(hwnd, kTimerRestore);
          self->restore_timer_ = 0;
        }
        self->band_restore_left_ = 0;
        if (self->display_change_timer_) {
          KillTimer(hwnd, kTimerDisplayChange);
          self->display_change_timer_ = 0;
        }
        return 0;
      }
      if (wparam == WTS_SESSION_UNLOCK) {
        self->session_locked_ = false;
        LogLine(L"[session] unlocked - resuming");
        if (self->pet_visible_ && !self->pet_timer_) {
          self->pet_timer_ =
              SetTimer(hwnd, kTimerPet, self->pet_tick_ms_, nullptr);
        }
        self->ScheduleDesktopBandRestore();
        // Give the desktop shell 600ms to recover after unlock, then check topology
        if (!self->display_change_timer_) {
          self->display_change_stage_ = 1;
          self->display_change_timer_ =
              SetTimer(hwnd, kTimerDisplayChange, 600, nullptr);
        }
        return 0;
      }
      break;
    }
    case WM_DISPLAYCHANGE: {
      if (self->session_locked_) {
        LogLine(L"[display] WM_DISPLAYCHANGE ignored during session lock");
        return 0;
      }
      // Never perform synchronous driver DC probes (CreateDCW) or disk IO inside
      // the broadcast handler. Instead, coalesce all display changes with a debounce timer.
      if (self->display_change_timer_) {
        KillTimer(hwnd, kTimerDisplayChange);
        self->display_change_timer_ = 0;
      }
      self->display_change_stage_ = 1;
      self->display_change_timer_ =
          SetTimer(hwnd, kTimerDisplayChange, 500, nullptr);
      LogLine(L"[display] WM_DISPLAYCHANGE received, debounce timer scheduled");
      return 0;
    }
    case WM_COPYDATA: {
      const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lparam);
      if (cds && cds->dwData == kCliActionMsg && cds->lpData &&
          cds->cbData >= sizeof(wchar_t)) {
        const auto* text = static_cast<const wchar_t*>(cds->lpData);
        self->HandleCliAction(text);
        return TRUE;
      }
      break;
    }
    case kMsgRelayHotkey: {
      auto* action = reinterpret_cast<std::string*>(lparam);
      if (action) {
        self->OnRelayHotkeyAction(*action);
        delete action;
      }
      return 0;
    }
    case kMsgLlHotkey:
    case WM_HOTKEY: {
      int idx = -1;
      if (msg == kMsgLlHotkey) {
        idx = static_cast<int>(wparam);
      } else {
        idx = static_cast<int>(wparam) - static_cast<int>(kHotkeyBase);
      }
      if (idx >= 0 && idx < static_cast<int>(self->hotkeys_.size())) {
        const std::string& id = self->hotkeys_[static_cast<size_t>(idx)].id;
        const ULONGLONG now = GetTickCount64();
        const auto sit = self->hotkey_suppress_until_.find(id);
        if (sit != self->hotkey_suppress_until_.end() && now < sit->second) {
          return 0;  // just-bound chord still held — drop
        }
        const auto lit = self->hotkey_last_fire_.find(id);
        if (lit != self->hotkey_last_fire_.end() && now - lit->second < 300) {
          return 0;  // 300ms debounce against double-fire / LL+RegisterHotKey
        }
        self->hotkey_last_fire_[id] = now;
        self->DispatchHotkey(id);
        return 0;
      }
      break;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}
