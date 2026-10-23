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
#include "prompt_text.h"
#include "cli_ipc.h"
#include "fence_keys.h"
#include "fence_style.h"
#include "layout_editor.h"
#include "python_layout_sync.h"
#include "shell_clipboard.h"
#include "brand_mark.h"
#include "namespace_icons.h"
#include "shell_icons.h"
#include "shell_verbs.h"
#include "desknote_launch.h"
#include "file_assoc.h"
#include "snapshot.h"
#include "tool_paths.h"
#include "tools_misc.h"
#include "ui_theme.h"
#include "update_check.h"
#include "vault_http.h"
#include "rules_editor.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <cstring>
#include <Shellapi.h>
#include <ShlObj.h>
#include <gdiplus.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
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
constexpr UINT kTimerUpdate = 44;
constexpr UINT kTimerVault = 45;
constexpr UINT kTimerOpenSettingsVault = 46;
constexpr UINT kTimerOpenSettingsOrg = 47;
constexpr UINT kTimerDisplayChange = 49;
constexpr UINT kTimerIconsRefresh = 50;
constexpr UINT kTimerMemTrim = 51;
constexpr UINT kMsgVaultPullDone = WM_APP + 48;
constexpr UINT kMsgOrganizeDone = WM_APP + 49;
constexpr DWORD kTrayOpenDebounceMs = 450;
constexpr wchar_t kMsgClass[] = L"DeskTidyNativeMsg";
bool g_organize_busy = false;
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
App* g_app_for_hook = nullptr;
HWND g_band_hwnd = nullptr;
HHOOK g_kb_hook = nullptr;
// Avoid overlapping WinHttp vault pulls from the periodic timer.
bool g_vault_pull_busy = false;

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
  const int x = cursor.x + 12;
  const int y = cursor.y + 12;

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
  POINT origin{x, y};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateLayeredWindow(g_pin_hint_hwnd, screen, &origin, &size, mem, &src, 0,
                      &blend, ULW_ALPHA);
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
  int n = 0;
  int hosted = 0;
  bool quiet = false;
};

namespace {

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION &&
      (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
    const auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
    if (kb) {
      if (g_band_hwnd && kb->vkCode == 'D') {
        const bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                         (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
        if (win) {
          // Passthrough: still CallNextHookEx so Explorer gets Win+D.
          PostMessageW(g_band_hwnd, kMsgBandRestore, 0, 0);
        }
      }
      // P1-28 Tier A: when exclusive fullscreen swallows RegisterHotKey /
      // WM_HOTKEY, still match configured chords via LL hook.
      if (g_app_for_hook && g_app_for_hook->MessageHwnd()) {
        g_app_for_hook->TryDispatchLlHotkey(kb->vkCode);
      }
    }
  }
  return CallNextHookEx(g_kb_hook, code, wparam, lparam);
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

std::wstring FormatFileSize(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
    return L"—";
  }
  if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    return L"—";
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
  return buf;
}

std::wstring FileTypeLabel(const DesktopItem& it) {
  if (it.is_link) {
    return L"快捷方式";
  }
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExW(it.path.c_str(), GetFileExInfoStandard, &fad) &&
      (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return L"文件夹";
  }
  const size_t dot = it.name.find_last_of(L'.');
  if (dot != std::wstring::npos && dot + 1 < it.name.size()) {
    std::wstring ext = it.name.substr(dot + 1);
    for (auto& c : ext) {
      if (c >= L'a' && c <= L'z') {
        c = static_cast<wchar_t>(c - L'a' + L'A');
      }
    }
    return ext + L" 文件";
  }
  return L"文件";
}

void PushOrganizePage(SettingsWindow* settings, const PinMap& pins,
                      const RuleMap& rules,
                      const std::vector<std::unique_ptr<FenceWindow>>& fences) {
  if (!settings) {
    return;
  }
  const auto items = ScanDesktopItems();
  const auto preview =
      OrganizePreviewVirtualPins(pins, items, rules, FenceTitlesOf(fences));
  std::vector<SettingsWindow::OrgFileRow> files;
  files.reserve(preview.size());
  for (const auto& line : preview) {
    SettingsWindow::OrgFileRow row;
    const size_t arrow = line.find(L" → ");
    row.name = arrow == std::wstring::npos ? line : line.substr(0, arrow);
    row.type = L"文件";
    row.size = L"—";
    for (const auto& it : items) {
      const std::wstring n = it.name.empty() ? it.path : it.name;
      if (_wcsicmp(n.c_str(), row.name.c_str()) == 0) {
        row.type = FileTypeLabel(it);
        row.size = FormatFileSize(it.path);
        break;
      }
    }
    files.push_back(std::move(row));
  }
  int organized = 0;
  for (const auto& it : items) {
    if (FindPin(pins, it.path)) {
      ++organized;
    }
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
    if (f->Page() != current_page) {
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
  fence->SetOnMoved([this](FenceWindow*) { PersistLayouts(); });
  fence->SetOnGeometryMoved([this](FenceWindow* moved) {
    if (!moved) {
      return;
    }
    const int page = moved->Page();
    std::vector<FenceWindow*> list;
    std::vector<LayoutRect> rects;
    for (auto& f : fences_) {
      if (!f || f->Page() != page || f->IsCollapsed()) {
        continue;
      }
      RECT r{};
      GetWindowRect(f->Hwnd(), &r);
      list.push_back(f.get());
      rects.push_back({r.left, r.top, r.right - r.left, r.bottom - r.top});
    }
    RECT mr{};
    GetWindowRect(moved->Hwnd(), &mr);
    const POINT mpt{mr.left, mr.top};
    const RECT work = WorkAreaAtPoint(mpt);
    const LayoutRect work_r{work.left, work.top, work.right - work.left,
                            work.bottom - work.top};
    if (moved->IsCollapsed()) {
      // Collapsed chrome height is not a layout rect; just clamp + persist.
      ClampRectToWorkArea(&mr);
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
    ResolveFenceCollisions(&rects, 160, 120);
    for (size_t i = 0; i < rects.size(); ++i) {
      RECT rr{rects[i].x, rects[i].y, rects[i].x + rects[i].w,
              rects[i].y + rects[i].h};
      ClampRectToWorkArea(&rr);
      FenceWindow* fw = list[i];
      SetWindowPos(fw->Hwnd(), nullptr, rr.left, rr.top, rr.right - rr.left,
                   rr.bottom - rr.top, SWP_NOZORDER | SWP_NOACTIVATE);
      fw->AttachToDesktopLayer();
      FencePageGeometry geo;
      geo.x = rr.left;
      geo.y = rr.top;
      geo.w = rr.right - rr.left;
      geo.h = rr.bottom - rr.top;
      geo.visible = fw->VisibleOnPage(page);
      geo.collapsed = fw->IsCollapsed();
      fw->SetPageGeometry(page, geo);
    }
    PersistLayouts();
    NudgePetClearOfFences(pet_, fences_, current_page_);
  });
  fence->SetOnZoomChanged([this](FenceWindow* f, int zoom) {
    (void)f;
    (void)zoom;
    PersistLayouts();
    NudgePetClearOfFences(pet_, fences_, current_page_);
  });
  fence->SetOnReorder([this](FenceWindow* f,
                             const std::vector<std::wstring>& order) {
    if (f) {
      f->SetPinOrder(order);
    }
    PersistLayouts();
  });
  fence->SetOnPathsDroppedAt(
      [this](FenceWindow* f, const std::vector<std::wstring>& paths, int at) {
        // Must SetPin / portal-import (HandleIncomingPaths). InsertPinOrder alone
        // left membership empty — drop looked accepted then icons vanished (P0-1).
        if (f) {
          f->SetSortBy("manual");
          f->InsertPinOrder(paths, at);
        }
        OnPathsDropped(f, paths);
        PersistLayouts();
      });
  fence->SetOnPathsDropped(
      [this](FenceWindow* f, const std::vector<std::wstring>& paths) {
        OnPathsDropped(f, paths);
      });
  fence->SetOnDragDone([this](FenceWindow*) { RefreshDesktopIcons(); });
  fence->SetOnPinDrop([this](FenceWindow* f, const std::wstring& src,
                             const std::vector<std::wstring>& paths,
                             bool commit) {
    return OnPinDropped(f, src, paths, commit);
  });
  fence->SetOnPinDragEnd([this](FenceWindow*) { HidePinDragHint(); });
  fence->SetOnUnpin([this](FenceWindow*, const std::wstring& path) {
    UnpinPath(path);
  });
  fence->SetOnPathReplaced(
      [this](const std::wstring& from, const std::wstring& to) {
        RetargetPin(from, to);
      });
  fence->SetOnSetPage([this](FenceWindow* f, int page) {
    if (!f) {
      return;
    }
    f->SetPage(page);
    PersistLayouts();
    ApplyPageVisibility();
    RefreshSettingsList();
  });
  fence->SetOnRename([this](FenceWindow* f) {
    if (!f) {
      return;
    }
    const auto name =
        PromptText(settings_.Hwnd(), Tr("rename_fence").c_str(),
                   Tr("rename_fence_label").c_str(), f->Title());
    if (!name.empty()) {
      f->SetTitle(name);
      PersistLayouts();
      RefreshSettingsList();
    }
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
                            const std::vector<std::wstring>& paths, bool move) {
    OnPathsPasted(f, paths, move);
  });
  fence->SetOnRefresh([this](FenceWindow*) {
    InvalidateDesktopScanCache();
    RefreshDesktopIcons();
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
  fence->SetOnOpenNotes(
      [this](FenceWindow*, const std::vector<std::wstring>& paths) {
        OpenPathsInNotes(paths);
      });
  fence->SetQueryFences([this]() {
    std::vector<FenceMoveTarget> out;
    for (const auto& f : fences_) {
      if (f) {
        out.push_back({f->Id(), f->Title()});
      }
    }
    return out;
  });
  fence->SetPageNames(page_names_);
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
  AddClipboardFormatListener(message_hwnd_);
  // P1-12: drive the floating drop hint from the OLE drag loop.
  SetPinDragMoveFn([this](POINT cursor, const PinDragHint& hint) {
    UpdatePinDragHint(cursor, hint.over_target, hint.invalid);
  });

  // P1-29/P1-30: migrate Python settings/hotkeys/prefs/first_run BEFORE loading
  // native values, otherwise the first launch loads stale defaults and the
  // migrated values are overwritten by the in-memory copies.
  MigratePythonLayoutOnce();

  hotkeys_ = LoadHotkeys();
  SaveHotkeys(hotkeys_);

  if (!settings_.Create(instance)) {
    return false;
  }
  settings_.WarmWebView();
  PushHotkeyState();
  settings_.SetHandlers(
      [this]() { AddFence(); },
      [this](int index) { RemoveFenceAt(index); },
      [this]() {
        SeatAllFencesUnderApps();
        settings_.Show();
      },
      [this](int index, const std::wstring& title) {
        RenameFenceAt(index, title);
      },
      [this](int index, int page) { SetFencePageAt(index, page); },
      [this]() { OrganizeVirtual(); }, [this]() { OrganizePhysical(); },
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
      [this]() { PushOrganizePage(&settings_, pins_, rules_, fences_); });
  settings_.SetExtraHandlers(
      [this]() { SaveLayoutSnapshot(); },
      [this]() { RestoreLayoutSnapshot(); }, [this]() { OpenVault(); },
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
  settings_.SetOnFenceOpacity(
      [this](int index, int pct) { SetFenceOpacityPctAt(index, pct); });
  settings_.SetOnFencePage(
      [this](int index, int page) { SetFencePageAt(index, page); });
  settings_.SetOnExtToggle([this](const std::wstring& id, bool value) {
    if (id == L"minutes_on_bar") {
      prefs_.minutes_on_bar = value;
    } else if (id == L"notepad_enabled") {
      prefs_.notepad_enabled = value;
      SyncDeskNoteShortcuts(value);
    } else if (id == L"record_on_bar") {
      prefs_.record_on_bar = value;
    } else if (id == L"record_enabled") {
      prefs_.record_enabled = value;
    } else if (id == L"calculator_on_bar") {
      prefs_.calculator_on_bar = value;
    } else if (id == L"todos_on_bar") {
      prefs_.todos_on_bar = value;
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
      prefs_.auto_snapshot_on_change = false;
    } else if (id == L"desktop_right_click_menu") {
      prefs_.desktop_right_click_menu = value;
      SyncNativeShellVerbs(prefs_.desktop_right_click_menu);
    } else if (id == L"close_to_tray") {
      prefs_.close_to_tray = value;
    } else if (id == L"show_fences") {
      prefs_.show_fences = value;
      fences_hidden_ = !value;
      if (fences_hidden_) {
        for (auto& f : fences_) {
          if (f) {
            f->Hide();
          }
        }
      } else {
        ApplyPageVisibility();
      }
    } else if (id == L"double_click_hide") {
      prefs_.double_click_hide = value;
    } else if (id == L"enable_public_desktop") {
      SetPublicVisible(value);
    } else if (id == L"show_page_indicator") {
      prefs_.show_page_indicator = value;
    } else if (id == L"vault_enabled") {
      if (value) {
        if (!VaultHttpHealth()) {
          ShowToast(L"账号库服务器不可达，未启用");
          prefs_.vault_enabled = false;
          SaveAppPrefs(prefs_);
          SyncVaultLauncher();
          PushExtensionSettings();
          return;
        }
      }
      prefs_.vault_enabled = value;
      SyncVaultLauncher();
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
    } else if (id == L"vault_copy_user") {
      if (!value.empty() &&
          ClipboardSetText(settings_.Hwnd(), value)) {
        ShowToast(L"已复制用户名");
      }
      return;
    } else if (id == L"vault_copy_user_idx") {
      vault_.CopyUsernameAt(_wtoi(value.c_str()));
      return;
    } else if (id == L"vault_copy_pass") {
      vault_.CopyPasswordAt(_wtoi(value.c_str()));
      return;
    } else if (id == L"vault_copy_url") {
      vault_.CopyUrlAt(_wtoi(value.c_str()));
      return;
    } else if (id == L"vault_api_base") {
      std::wstring api = value;
      if (api.empty()) {
        api = VaultHttpGetApiBase();
      }
      if (api.empty()) {
        api = L"https://bowen-yu0926.byethost10.com/desktidy-vault/";
      }
      prefs_.vault_api_base = api;
      VaultHttpSetApiBase(prefs_.vault_api_base);
      SaveAppPrefs(prefs_);
    } else if (id == L"vault_refresh_hours") {
      int hours = value.empty() ? 2 : _wtoi(value.c_str());
      if (hours < 0) {
        hours = 0;
      }
      if (hours > 168) {
        hours = 168;
      }
      prefs_.vault_refresh_hours = hours;
      SaveAppPrefs(prefs_);
      RestartVaultRefreshTimer();
    } else if (id == L"vault_clipboard_clear_seconds") {
      int secs = value.empty() ? 30 : _wtoi(value.c_str());
      if (secs < 0) {
        secs = 0;
      }
      if (secs > 300) {
        secs = 300;
      }
      prefs_.vault_clipboard_clear_seconds = secs;
      vault_.SetClipboardClearSeconds(prefs_.vault_clipboard_clear_seconds);
      SaveAppPrefs(prefs_);
    } else {
      return;
    }
    PushExtensionSettings();
  });

  todos_.Create(instance);
  search_.Create(instance);
  vault_.Create(instance);
  // Prefetch WebView2 off the buoy-click path (cold start was hitching the UI).
  vault_.WarmWebView();
  vault_launcher_.Create(instance);
  vault_launcher_.SetOnActivate([this]() { OpenVault(); });
  vault_launcher_.SetOnMoved([this](int x, int y) {
    prefs_.vault_launcher_x = x;
    prefs_.vault_launcher_y = y;
    SaveAppPrefs(prefs_);
  });
  dock_.Create(instance);
  pet_.Create(instance);
  pet_.SetOnTrash([this](const std::vector<std::wstring>& paths) {
    if (SendPathsToRecycleBin(paths)) {
      RefreshDesktopIcons();
    } else {
      ShowToast(L"移入回收站失败");
    }
  });
  pet_.SetOnPage([this](int page) { SetPage(page); });
  pet_.SetOnCreateFence([this](int page) { CreateFenceOnPage(page); });
  pet_.SetOnHide([this]() { SetPetVisible(false); });
  auto activate_float_tool = [this](const std::wstring& id) {
    if (id == L"record") {
      StartScreenRecord(settings_.Hwnd());
    } else if (id == L"minutes") {
      OpenMinutes();
    } else if (id == L"calculator") {
      OpenCalculator();
    } else if (id == L"todo") {
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
  page_bar_.SetOnTool(
      [this](const std::wstring& id) {
        if (id == L"record") {
          StartScreenRecord(settings_.Hwnd());
        } else if (id == L"minutes") {
          OpenMinutes();
        } else if (id == L"calculator") {
          OpenCalculator();
        } else if (id == L"todo") {
          OpenTodos();
        } else if (id == L"vault") {
          OpenVault();
        } else if (id == L"note") {
          OpenNotes();
        }
      });
  page_bar_.SetOnFolder(open_folder);
  page_bar_.SetOnMoved([this]() {
    prefs_.page_bar_anchor_y = page_bar_.AnchorY();
    SaveAppPrefs(prefs_);
  });
  public_host_.Create(instance);
  dock_.SetOnPathReplaced(
      [this](const std::wstring& from, const std::wstring& to) {
        RetargetPin(from, to);
      });
  dock_.SetOnRemoved([this](const std::wstring& path) { UnpinPath(path); });
  public_host_.SetOnPathReplaced(
      [this](const std::wstring& from, const std::wstring& to) {
        RetargetPin(from, to);
      });
  public_host_.SetOnRemoved([this](const std::wstring& path) {
    UnpinPath(path);
    RebuildPublicItems();
  });
  public_host_.SetOnRefresh([this]() { RebuildPublicItems(); });
  search_.SetOnPathReplaced(
      [this](const std::wstring& from, const std::wstring& to) {
        RetargetPin(from, to);
      });
  search_.SetOnRemoved([this](const std::wstring& path) { UnpinPath(path); });
  search_.SetOnAllDrivesChanged([this](bool on) {
    prefs_.search_all_drives = on;
    SaveAppPrefs(prefs_);
    PushExtensionSettings();
  });

  prefs_ = LoadAppPrefs();
  prefs_.ui_theme = NormalizeUiTheme(prefs_.ui_theme);
  prefs_.session_active = true;
  SaveAppPrefs(prefs_);
  todos_.ApplyUiTheme(prefs_.ui_theme);
  page_bar_.SetAnchorY(prefs_.page_bar_anchor_y);
  ApplyDockPrefs();
  RegisterNoteFileAssociations();
  display_fp_ = DisplayFingerprint();
  display_work_ = PrimaryWorkArea();
  app_dpi_ = SystemDpi();
  fences_hidden_ = !prefs_.show_fences;
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
  // Remap pins that still point at pre-migration fence ids.
  for (auto& kv : pins_) {
    if (kv.second == "system_software") {
      kv.second = "system_software_side";
    } else if (kv.second == "system_page2") {
      kv.second = "system_docs";
    }
  }
  rules_ = LoadRules();
  SaveRules(rules_);

  for (const auto& layout : state.fences) {
    if (!SpawnFence(layout)) {
      return false;
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
  ApplyPageVisibility();
  InstallFenceKeyHook();
  dock_.SetVisible(dock_visible_);
  pet_.SetVisible(pet_visible_);
  public_host_.SetVisible(public_visible_);
  RebuildDockItems();
  RebuildPublicItems();
  desktop_watch_.Reset();
  SyncNativeShellVerbs(prefs_.desktop_right_click_menu);
  if (!prefs_.vault_api_base.empty()) {
    VaultHttpSetApiBase(prefs_.vault_api_base);
  }
  vault_.SetClipboardClearSeconds(prefs_.vault_clipboard_clear_seconds);
  RestartVaultRefreshTimer();
  vault_launcher_.SetPosition(prefs_.vault_launcher_x, prefs_.vault_launcher_y);
  SyncVaultLauncher();

  // Win+D settle is driven by the keyboard hook. A tight restore timer
  // re-layered the fences and flashed the desktop.
  restore_timer_ = SetTimer(message_hwnd_, kTimerRestore, 4000, nullptr);
  pet_timer_ = SetTimer(message_hwnd_, kTimerPet, 50, nullptr);
  watch_timer_ = SetTimer(message_hwnd_, kTimerWatch, 2000, nullptr);
  // P2-1: low-frequency background trim (Python ~60s while tray/background).
  SetTimer(message_hwnd_, kTimerMemTrim, 60000, nullptr);

  CreateTrayIcon();
  ReregisterHotkeys();
  // Refresh DeskNote.lnk → sibling deskNote.exe.
  SyncDeskNoteShortcuts(prefs_.notepad_enabled);
  // Organized mode: Explorer icons off so fence pins are not duplicated.
  ReconcileNamespaceIconsOnStartup(true);
  SetDesktopIconsVisible(false);
  icons_visible_intent_ = false;
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
  StartDesktopDblClickMonitor(message_hwnd_, kMsgDesktopDblClick);
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
    VaultHttpImportPythonSessionIfNeeded();
    if (prefs_.vault_api_base.empty()) {
      prefs_.vault_api_base = VaultHttpGetApiBase();
    }
    RefreshVaultListForSettings();
    PushExtensionSettings();
    PushHotkeyState();
  });
  ScheduleDesktopBandRestore();
  MigratePythonAutostartOnce();
  MaybeShowFirstRun(settings_.Hwnd());
  // U6: one-time first-run organize (independent of auto_organize_on_startup).
  // Startup auto-organize also requires auto_organize_watch (P1-29 / Python
  // _startup_deferred_organize). Manual 「一键整理」is unaffected.
  const bool first_organize = !prefs_.first_run_organize_done;
  if (first_organize) {
    OrganizeVirtual(true);
    prefs_.first_run_organize_done = true;
    SaveAppPrefs(prefs_);
  } else if (prefs_.auto_organize_on_startup && prefs_.auto_organize_watch) {
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
    // Primary occupied: try fallback chords (Python HOTKEY_FALLBACKS).
    bool fell_back = false;
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
        // P2-4: persist fallback so settings UI matches the live binding.
        hotkeys_[i].modifiers = m;
        hotkeys_[i].vk = v;
        fell_back = true;
        LogLine(L"[hotkey] %s primary occupied -> fallback %s (saved)",
                WidenUtf8(k.id).c_str(), alt.c_str());
        break;
      }
    }
    if (!fell_back) {
      conflicts.push_back(k.label + L"（" + FormatHotkey(k.modifiers, k.vk) +
                          L"）");
      LogLine(L"[hotkey] register FAILED %s (%s)", WidenUtf8(k.id).c_str(),
              FormatHotkey(k.modifiers, k.vk).c_str());
    }
  }
  SaveHotkeys(hotkeys_);
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
  if (hotkey_relay_.Running()) {
    hotkey_relay_.UpdateMapping(hotkeys_);
    return;
  }
  if (!hotkey_relay_.Start(message_hwnd_, kMsgRelayHotkey, hotkeys_)) {
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

void App::TryDispatchLlHotkey(UINT vk) {
  if (!message_hwnd_ || vk == 0) {
    return;
  }
  // Ignore pure modifier keys.
  if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN ||
      vk == VK_RWIN || vk == VK_LCONTROL || vk == VK_RCONTROL ||
      vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LMENU || vk == VK_RMENU) {
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
  if ((mods & MOD_WIN) && vk == 'D') {
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
  // Explorer applies the toggle asynchronously; defer the float refresh.
  if (message_hwnd_) {
    SetTimer(message_hwnd_, kTimerIconsRefresh, 400, nullptr);
  }
  if (toast) {
    ShowToast(visible ? L"已显示桌面图标" : L"已隐藏桌面图标");
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
    // Python: walk newest→oldest recent shots, one pin per press.
    if (!recent_shots_.empty()) {
      if (recent_shot_i_ >= recent_shots_.size()) {
        recent_shot_i_ = 0;
      }
      PinImage(recent_shots_[recent_shot_i_++]);
    } else if (!pins_img_.empty()) {
      for (auto& p : pins_img_) {
        if (p) {
          p->Show();
        }
      }
    } else {
      CaptureRegionToPng(settings_.Hwnd());
    }
  } else if (id == "screen_record" || id == "record") {
    StartScreenRecord(settings_.Hwnd());
  } else if (id == "file_search" || id == "search") {
    OpenSearch();
  } else if (id == "calculator") {
    OpenCalculator();
  } else if (id == "settings") {
    OpenSettings();
  } else if (id == "notes") {
    OpenNotes();
  } else if (id == "vault") {
    OpenVault();
  } else if (id == "todos") {
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
  // 1) stop all timers.
  RunShutdownStep("step 1 timers", [this]() {
    if (message_hwnd_) {
      KillTimer(message_hwnd_, kTimerRestore);
      KillTimer(message_hwnd_, kTimerPet);
      KillTimer(message_hwnd_, kTimerWatch);
      KillTimer(message_hwnd_, kTimerBandSettle);
      KillTimer(message_hwnd_, kTimerUpdate);
      KillTimer(message_hwnd_, kTimerVault);
      KillTimer(message_hwnd_, kTimerOpenSettingsVault);
      KillTimer(message_hwnd_, kTimerOpenSettingsOrg);
      KillTimer(message_hwnd_, kTimerMemTrim);
      KillTimer(message_hwnd_, kTimerDisplayChange);
      KillTimer(message_hwnd_, kTimerIconsRefresh);
      vault_timer_ = 0;
      open_settings_vault_timer_ = 0;
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

  // 9) destroy WebView2 windows (settings/vault/todo/search).
  RunShutdownStep("step 9 webview", [this]() {
    settings_.Destroy();
    vault_.Destroy();
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
    vault_launcher_.Destroy();
  });

  // 12) destroy pinned images.
  RunShutdownStep("step 12 pins", [this]() {
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
      DestroyWindow(message_hwnd_);
      message_hwnd_ = nullptr;
    }
  });

  // 15) finalize.
  RunShutdownStep("step 15 done", [this]() { InvalidateDefViewCache(); });
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
        }
        if (pin) {
          PinImage(path);
        }
      },
      auto_pin);
}

void App::PinImage(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  int max_pins = prefs_.screenshot_max_pins;
  if (max_pins < 1) {
    max_pins = 1;
  }
  if (max_pins > 5) {
    max_pins = 5;
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
    // the desktop. Host already pushed live fence cards via PushExtensionSettings.
    RefreshSettingsList();
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
      if (MessageBoxW(owner, ask.c_str(), L"恢复快照",
                      MB_YESNO | MB_ICONQUESTION) == IDYES) {
        AppState state;
        if (LoadSnapshotWithIcons(item.path, &state)) {
          ReloadFencesFromState(state);
          ShowToast(L"已恢复所选快照");
          PushExtensionSettings();
        } else {
          MessageBoxW(owner, L"无法读取该快照。", L"布局快照",
                      MB_OK | MB_ICONWARNING);
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
      if (MessageBoxW(owner, ask.c_str(), L"删除快照",
                      MB_YESNO | MB_ICONWARNING) == IDYES) {
        if (DeleteFileW(item.path.c_str())) {
          {
            std::wstring icons = item.path;
            const std::wstring suf = L".json";
            if (icons.size() >= suf.size() &&
                _wcsicmp(icons.c_str() + (icons.size() - suf.size()),
                         suf.c_str()) == 0) {
              icons.resize(icons.size() - suf.size());
            }
            icons += L".icons.json";
            DeleteFileW(icons.c_str());
          }
          ShowToast(L"已删除快照");
          PushExtensionSettings();
        } else {
          MessageBoxW(owner, L"删除失败。", L"布局快照",
                      MB_OK | MB_ICONWARNING);
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
    TogglePageLock(_wtoi(id.c_str() + 16));
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
  if (id.rfind(L"rename_fence:", 0) == 0) {
    const int index = _wtoi(id.c_str() + 13);
    if (index >= 0 && index < static_cast<int>(fences_.size()) &&
        fences_[static_cast<size_t>(index)]) {
      FenceWindow* f = fences_[static_cast<size_t>(index)].get();
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
    PushOrganizePage(&settings_, pins_, rules_, fences_);
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
  } else if (id == L"vault_login") {
    vault_.CloudLogin();
    SyncVaultSessionState();
    RefreshVaultListForSettings();
    PushExtensionSettings();
  } else if (id == L"vault_logout") {
    VaultHttpLogout();
    ShowToast(L"已退出账号库登录");
    SyncVaultSessionState();
    RefreshVaultListForSettings();
    PushExtensionSettings();
  } else if (id == L"vault_refresh") {
    VaultHttpImportPythonSessionIfNeeded();
    // Same async pull as the periodic timer — never block settings/UI.
    if (VaultHttpLoggedIn() && !g_vault_pull_busy && message_hwnd_) {
      g_vault_pull_busy = true;
      auto* remote =
          new std::vector<VaultHttpCredential>(vault_.SnapshotCredentials());
      HWND msg_hwnd = message_hwnd_;
      ReapFinishedWorkers();
      workers_.emplace_back([remote, msg_hwnd]() {
        const bool ok = VaultHttpPullMerge(remote);
        if (!PostMessageW(msg_hwnd, kMsgVaultPullDone, ok ? 1 : 0,
                          reinterpret_cast<LPARAM>(remote))) {
          delete remote;
          g_vault_pull_busy = false;
        }
      });
    } else {
      RefreshVaultListForSettings();
      PushExtensionSettings();
    }
  } else if (id == L"vault_export") {
    vault_.ExportBackup();
  } else if (id == L"vault_import") {
    vault_.ImportBackup();
    RefreshVaultListForSettings();
    PushExtensionSettings();
  } else if (id.rfind(L"vault_delete_item:", 0) == 0) {
    vault_.DeleteAt(_wtoi(id.c_str() + 18));
    RefreshVaultListForSettings();
    PushExtensionSettings();
  }
}

void App::HandleCliAction(const std::wstring& action) {
  if (action == L"--settings") {
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
  } else if (action == L"--vault") {
    OpenVault();
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
void App::OpenVault() {
  if (!prefs_.vault_enabled) {
    const int r = MessageBoxW(
        settings_.Hwnd(),
        L"账号库未启用。\n\n是否现在启用并打开？\n"
        L"（也可在设置 → 账号管理中打开开关）",
        L"账号库", MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) {
      return;
    }
    prefs_.vault_enabled = true;
    SaveAppPrefs(prefs_);
    RestartVaultRefreshTimer();
    SyncVaultLauncher();
    PushExtensionSettings();
  }
  // Doubao-style toggle: second click closes (Python _on_vault_launcher_activated).
  if (vault_.Hwnd() && IsWindowVisible(vault_.Hwnd()) && !IsIconic(vault_.Hwnd())) {
    vault_.Hide();
    return;
  }
  // Open panel only — no session import, no cloud pull on this path.
  // (Python: 打开面板时不再强制联网; sync is the periodic vault timer.)
  vault_.PlaceBeside(vault_launcher_.Hwnd());
  vault_.Show();
}

void App::TogglePeek() {
  peek_active_ = !peek_active_;
  if (peek_active_) {
    // Peek: raise current-page fences (and the dock) to topmost, interactive —
    // never hide them. Python detaches the overlay from DefView and sets
    // TOPMOST so the partitions float above all windows.
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
      if (!fence || !fence->Hwnd() || fence->Page() != current_page_) {
        continue;
      }
      raise_topmost(fence->Hwnd());
      fence->RefreshLayer();
    }
    if (dock_visible_) {
      raise_topmost(dock_.Hwnd());
    }
  } else {
    // Restore the desktop band (re-attach + page visibility).
    RestoreDesktopChrome();
  }
}

void App::ToggleFencesVisible() {
  fences_hidden_ = !fences_hidden_;
  prefs_.show_fences = !fences_hidden_;
  SaveAppPrefs(prefs_);
  if (fences_hidden_) {
    for (auto& f : fences_) {
      if (f) {
        f->Hide();
      }
    }
    TrimMemory();
  } else {
    RefreshDesktopIcons();
    ApplyPageVisibility();
  }
  PushExtensionSettings();
}

void App::SaveLayoutSnapshot() {
  AppState state;
  state.current_page = current_page_;
  state.page_names = page_names_;
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
  if (!ManageSnapshotsDialog(settings_.Hwnd(), &state)) {
    return;
  }
  ReloadFencesFromState(state);
  MessageBoxW(settings_.Hwnd(), L"已恢复所选快照。", L"布局快照",
              MB_OK | MB_ICONINFORMATION);
}

void App::ReloadFencesFromState(const AppState& state) {
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
  dock_visible_ = prefs_.dock_visible;
  pet_visible_ = state.pet_visible;
  AppState healed = state;
  HealFenceLayouts(&healed.fences);
  for (const auto& layout : healed.fences) {
    SpawnFence(layout);
  }
  PersistLayouts();
  SyncPageChrome();
  RefreshDesktopIcons();
  RefreshSettingsList();
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
  const int n = HealFenceLayouts(&layouts);
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
    SetWindowPos(fences_[i]->Hwnd(), nullptr, layouts[i].x, layouts[i].y,
                 layouts[i].w, layouts[i].h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }
  PersistLayouts();
  if (toast) {
    ShowToast(L"已修复 " + std::to_wstring(n) + L" 个分区位置");
  }
}

void App::OnDisplayTopologyMaybeChanged() {
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
  const RECT new_work = PrimaryWorkArea();
  PrepareDestinationProfileForDisplayChange(display_work_, new_work);
  ReloadFencesFromState(LoadAppState());
  display_work_ = new_work;
  ShowToast(L"显示器变化：已按配置恢复分区");
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
  const int choice = MessageBoxW(
      settings_.Hwnd(),
      L"会议纪要：\n\n"
      L"是 = 新建 Markdown\n"
      L"否 = 新建 RTF（可用 Word 打开）\n"
      L"取消 = 更多…",
      L"会议纪要", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDYES) {
    CreateAndOpenMinutes(settings_.Hwnd());
    return;
  }
  if (choice == IDNO) {
    CreateAndOpenMinutesRtf(settings_.Hwnd());
    return;
  }
  const int more = MessageBoxW(
      settings_.Hwnd(),
      L"更多：\n\n是 = 打开最近纪要\n否 = 打开文件夹\n取消 = 设置文件夹",
      L"会议纪要", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (more == IDYES) {
    OpenRecentMinutes(settings_.Hwnd());
  } else if (more == IDNO) {
    OpenMinutesFolder(settings_.Hwnd());
  } else if (more == IDCANCEL) {
    ConfigureMinutesFolder(settings_.Hwnd());
  }
}

void App::RebuildPublicItems() {
  // Python public_desktop: floats = desktop items NOT pinned to any fence.
  // (Not C:\Users\Public\Desktop — that was a semantic miss, P0-2/P0-3.)
  std::vector<DesktopItem> pub;
  const auto items = ScanDesktopItems();
  pub.reserve(items.size());
  for (const auto& it : items) {
    if (FindPin(pins_, it.path)) {
      continue;
    }
    pub.push_back(it);
  }
  std::sort(pub.begin(), pub.end(),
            [](const DesktopItem& a, const DesktopItem& b) {
              return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
            });
  public_host_.SetItems(std::move(pub));
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

void App::RestartVaultRefreshTimer() {
  if (message_hwnd_ && vault_timer_) {
    KillTimer(message_hwnd_, kTimerVault);
    vault_timer_ = 0;
  }
  if (!message_hwnd_ || prefs_.vault_refresh_hours <= 0) {
    return;
  }
  // Win32 SetTimer max practical period; cap at ~24h then fire repeatedly.
  UINT ms = static_cast<UINT>(prefs_.vault_refresh_hours) * 3600u * 1000u;
  if (ms < 60000u) {
    ms = 60000u;
  }
  vault_timer_ = SetTimer(message_hwnd_, kTimerVault, ms, nullptr);
}

void App::PushExtensionSettings() {
  SettingsWindow::ExtState ext;
  ext.minutes_on_bar = prefs_.minutes_on_bar;
  ext.notepad_enabled = prefs_.notepad_enabled;
  ext.record_on_bar = prefs_.record_on_bar;
  ext.calculator_on_bar = prefs_.calculator_on_bar;
  ext.todos_on_bar = prefs_.todos_on_bar;
  ext.screenshot_auto_pin = prefs_.screenshot_auto_pin;
  ext.screenshot_max_pins = prefs_.screenshot_max_pins;
  ext.record_quality = prefs_.record_quality;
  ext.minutes_folder = MinutesFolderOverride();
  ext.notepad_folder = prefs_.notepad_folder;
  ext.wallpaper_folder = WallpaperLibraryFolder();
  ext.record_dir = prefs_.record_output_dir;
  ext.record_mic = prefs_.record_mic;
  ext.record_enabled = prefs_.record_enabled;
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
  ext.check_update_on_startup = prefs_.check_update_on_startup;
  ext.auto_snapshot_on_change = prefs_.auto_snapshot_on_change;
  ext.desktop_right_click_menu = prefs_.desktop_right_click_menu;
  ext.close_to_tray = prefs_.close_to_tray && prefs_.close_behavior_prompted;
  ext.show_fences = prefs_.show_fences;
  ext.double_click_hide = prefs_.double_click_hide;
  ext.enable_public_desktop = prefs_.enable_public_desktop;
  ext.show_page_indicator = prefs_.show_page_indicator;
  ext.vault_enabled = prefs_.vault_enabled;
  ext.vault_api_base = prefs_.vault_api_base.empty() ? VaultHttpGetApiBase()
                                                     : prefs_.vault_api_base;
  if (ext.vault_api_base.empty()) {
    ext.vault_api_base =
        L"https://bowen-yu0926.byethost10.com/desktidy-vault/";
  }
  ext.vault_refresh_hours = prefs_.vault_refresh_hours > 0
                                ? prefs_.vault_refresh_hours
                                : 2;
  ext.vault_clipboard_clear_seconds = prefs_.vault_clipboard_clear_seconds;
  ext.vault_logged_in = VaultHttpLoggedIn();
  {
    const std::wstring login = VaultHttpSessionLogin();
    if (ext.vault_logged_in && !login.empty()) {
      ext.vault_session_label = L"已登录：" + login;
    } else if (ext.vault_logged_in) {
      ext.vault_session_label = L"已登录";
    } else {
      ext.vault_session_label = L"未登录 — 请先登录或注册账号";
    }
  }
  ext.lang = CurrentLang() ? CurrentLang() : "zh";
  ext.app_version = NativeProductVersion();
  ext.theme_id = prefs_.theme_id;
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
    card.style_id = f->StyleId();
    card.organize_kinds = f->OrganizeKinds();
    card.system_fence = f->Id().rfind("system_", 0) == 0;
    card.position_locked = f->IsPositionLocked();
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
  for (const auto& s : ListSnapshots()) {
    const SnapshotCardData data = BuildSnapshotCard(s);
    SettingsWindow::ExtState::SnapCard card;
    card.name = data.name;
    card.meta = data.meta;
    card.focus_page = data.focus_page;
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
      card.preview.push_back(std::move(box));
    }
    ext.snapshots.push_back(std::move(card));
  }
  ext.vault_items = vault_items_cache_;
  ext.vault_list_status = vault_list_status_;
  // Keep settings WebView page list in sync with live layout (避免仍显示「第 N 页」).
  NormalizePageNames(&page_names_);
  settings_.SetPageNames(page_names_, false);
  settings_.SetCurrentPage(current_page_, false);
  settings_.SetExtensionState(ext);
  settings_.ApplyUiThemeVars(UiThemeCssVars(ext.ui_theme));
  PushHotkeyState();
}

void App::SyncVaultLauncher() {
  // When the right-edge page bar is hosting the 账号 peek, hide the free-floating
  // circular buoy so it no longer sits off-axis from the peek column.
  const bool pet_hosts = pet_visible_ && pet_.ShowPageBubbles();
  const bool edge_bar =
      prefs_.show_page_indicator && !pet_hosts && page_bar_.Hwnd() &&
      IsWindowVisible(page_bar_.Hwnd());
  if (prefs_.vault_enabled && !edge_bar) {
    vault_launcher_.SetPosition(prefs_.vault_launcher_x, prefs_.vault_launcher_y);
    vault_launcher_.SetVisible(true);
  } else {
    vault_launcher_.SetVisible(false);
  }
}

void App::RefreshVaultListForSettings() {
  vault_items_cache_.clear();
  auto rows = vault_.ListRows();
  // Use in-memory vault rows — ReloadFromDisk (DPAPI) only when empty.
  if (rows.empty()) {
    vault_.ReloadFromDisk();
    rows = vault_.ListRows();
  }
  if (!VaultHttpLoggedIn() && rows.empty()) {
    vault_list_status_ = L"登录后可在此新增、编辑、复制账号信息。";
    return;
  }
  vault_items_cache_.reserve(rows.size());
  for (const auto& r : rows) {
    SettingsWindow::ExtState::VaultItem item;
    item.id = r.index + 1;
    item.index = r.index;
    item.title = r.title;
    item.username = r.username;
    item.url = r.url;
    item.note = r.note;
    item.pass_len = r.pass_len;
    vault_items_cache_.push_back(std::move(item));
  }
  if (VaultHttpLoggedIn()) {
    vault_list_status_ = L"共 " + std::to_wstring(vault_items_cache_.size()) +
                         L" 条 · 已同步";
  } else {
    vault_list_status_ = L"共 " + std::to_wstring(vault_items_cache_.size()) +
                         L" 条 · 仅本地";
  }
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

void App::SyncVaultSessionState() {
  // Session fields are pushed via PushExtensionSettings.
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

void App::DeferredOpenSettingsVaultSync() {
  open_settings_vault_timer_ = 0;
  if (!settings_.IsOpen()) {
    return;
  }
  // Runs after the window is already visible — keep this off the tray click.
  SeatAllFencesUnderApps();
  RefreshSettingsList();
  settings_.SetToggles(dock_visible_, pet_visible_, IsNativeAutostartEnabled());
  settings_.SetOrganizePrefs(prefs_.auto_organize_on_startup,
                             prefs_.auto_organize_watch);
  settings_.SetPageNames(page_names_, false);
  settings_.SetCurrentPage(current_page_, false);
  VaultHttpImportPythonSessionIfNeeded();
  if (prefs_.vault_api_base.empty()) {
    prefs_.vault_api_base = VaultHttpGetApiBase();
  }
  RefreshVaultListForSettings();
  PushExtensionSettings();
  PushHotkeyState();
  // Desktop scan for 整理 page — second tick so first paint stays snappy.
  if (message_hwnd_) {
    SetTimer(message_hwnd_, kTimerOpenSettingsOrg, 80, nullptr);
  }
}

void App::OpenSettings() {
  // Pause band-settle churn while settings is up (raise/sink was flashing).
  if (message_hwnd_ && band_restore_left_ > 0) {
    KillTimer(message_hwnd_, kTimerBandSettle);
    band_restore_left_ = 0;
  }
  // Tray click: show the HWND immediately; fill data on the next timer tick.
  settings_.Show();
  if (settings_.Hwnd()) {
    SetWindowPos(settings_.Hwnd(), HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(settings_.Hwnd());
  }
  if (message_hwnd_) {
    if (open_settings_vault_timer_) {
      KillTimer(message_hwnd_, kTimerOpenSettingsVault);
    }
    KillTimer(message_hwnd_, kTimerOpenSettingsOrg);
    open_settings_vault_timer_ =
        SetTimer(message_hwnd_, kTimerOpenSettingsVault, 1, nullptr);
  }
}

void App::OnDesktopBlankDoubleClick() {
  if (!prefs_.double_click_hide) {
    return;
  }
  // Python: icons visible → organized; icons hidden → stock.
  if (icons_visible_intent_) {
    ShowOrganizedDesktopView();
  } else {
    ShowStockDesktopView();
  }
}

void App::ShowStockDesktopView() {
  // Hide DeskTidy chrome without permanently clearing prefs (Python stock view).
  for (auto& f : fences_) {
    if (f) {
      f->Hide();
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
  fences_hidden_ = !prefs_.show_fences;
  SyncNativeShellVerbs(prefs_.desktop_right_click_menu);
  // Public/loose visibility is derived in RefreshDesktopIcons (pin-only + floats).
  RefreshDesktopIcons();
  if (fences_hidden_) {
    for (auto& f : fences_) {
      if (f) {
        f->Hide();
      }
    }
  } else {
    ApplyPageVisibility();
  }
  vault_.SetClipboardClearSeconds(prefs_.vault_clipboard_clear_seconds);
  if (!prefs_.vault_api_base.empty()) {
    VaultHttpSetApiBase(prefs_.vault_api_base);
  }
  RestartVaultRefreshTimer();
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
  if (MessageBoxW(settings_.Hwnd(), L"导入配置将覆盖当前设置，是否继续？",
                  L"确认导入", MB_YESNO | MB_ICONQUESTION) != IDYES) {
    return;
  }
  prefs_ = LoadAppPrefsFromPath(path);
  prefs_.ui_theme = NormalizeUiTheme(prefs_.ui_theme);
  SaveAppPrefs(prefs_);
  ApplyLoadedGeneralPrefs();
  ApplyUiThemeId(prefs_.ui_theme);
  settings_.SetOrganizePrefs(prefs_.auto_organize_on_startup,
                             prefs_.auto_organize_watch);
  MessageBoxW(settings_.Hwnd(),
              L"配置已导入，部分选项可能需要重启生效。", L"导入完成",
              MB_OK | MB_ICONINFORMATION);
}

void App::OpenNotes() { LaunchOrFocusDeskNote(); }

void App::OpenPathsInNotes(const std::vector<std::wstring>& paths) {
  LaunchOrFocusDeskNote(paths);
}

void App::RequestQuit() {
  // T4: confirm when a recording is in progress (exit would stop it).
  if (IsScreenRecording()) {
    const int r = MessageBoxW(
        settings_.Hwnd(),
        L"正在录屏，退出将停止录制并保存当前片段。\n是否继续退出？",
        L"退出确认", MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) {
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
    const int r = MessageBoxW(
        settings_.Hwnd(),
        L"关闭 DeskTidy 窗口后是否在系统托盘后台继续运行？\n\n"
        L"「是」：隐藏到托盘，桌面分区继续工作，此选择将被记住。\n"
        L"「否」：按退出流程处理（分区整理会保留）。",
        L"后台运行", MB_YESNO | MB_ICONQUESTION);
    const bool keep_background = (r == IDYES);
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

void App::SeatAllFencesUnderApps() {
  // HWND_BOTTOM alone buries layered fences under SHELLDLL_DefView / ListView,
  // so Explorer desktop icons paint on top of pins (ghosted labels). Seat under
  // APPWINDOWs but keep overlays DefView-adjacent — same as fence Show().
  auto seat = [](HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
      return;
    }
    AttachOverlayToDesktop(hwnd, true);
    PlaceOverlayInDesktopBand(hwnd, true);
    RaiseOverlayInDesktopBand(hwnd);
  };
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd()) {
      continue;
    }
    seat(fence->Hwnd());
    fence->RefreshLayer();
  }
  if (dock_.Hwnd()) {
    seat(dock_.Hwnd());
  }
  if (pet_.Hwnd()) {
    seat(pet_.Hwnd());
  }
  if (page_bar_.Hwnd()) {
    seat(page_bar_.Hwnd());
  }
  // Organized mode: keep Explorer icons off so they cannot bleed through.
  SetDesktopIconsVisible(false);
  icons_visible_intent_ = false;
  // Do not sink notes into the desktop band — it is a normal app window.
  if (settings_.Hwnd() && settings_.IsOpen()) {
    SetWindowPos(settings_.Hwnd(), HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
}

void App::PersistLayouts() {
  AppState state;
  state.current_page = current_page_;
  state.page_names = page_names_;
  state.page_locked = page_locked_;
  state.dock_visible = dock_visible_;
  state.pet_visible = pet_visible_;
  state.fences.reserve(fences_.size());
  for (const auto& fence : fences_) {
    if (fence) {
      state.fences.push_back(fence->CurrentLayout());
    }
  }
  SaveAppState(state);
  MaybeAutoSnapshot();
}

void App::MaybeAutoSnapshot() {
  // Auto-save-on-change removed from UI (Python snapshot page has no equivalent).
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
  if (!r->interactive && prefs_.update_notified_tag == tag) {
    return;  // already notified this tag this session
  }
  const std::wstring msg = L"发现新版本 " + r->latest + L"（当前 " + r->current +
                           L"）。\n\n是：下载并静默安装\n否：打开发布页\n取消：稍后";
  const int answer = MessageBoxW(message_hwnd_, msg.c_str(), L"检查更新",
                                 MB_YESNOCANCEL | MB_ICONQUESTION);
  if (answer == IDNO) {
    ShellExecuteW(nullptr, L"open",
                  r->html_url.empty()
                      ? L"https://github.com/Bowen-Yu0926/DeskTidy-Releases/releases/latest"
                      : r->html_url.c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
  } else if (answer == IDYES) {
    ShowToast(L"正在下载更新…");
    StartUpdateInstallAsync(message_hwnd_);
  }
  prefs_.update_notified_tag = tag;
  SaveAppPrefs(prefs_);
}

void App::OnUpdateInstallDone(UpdateInstallResult* r) {
  if (!r) {
    return;
  }
  std::unique_ptr<UpdateInstallResult> own(r);
  LogLine(L"[update] install done ok=%d verify=%d digest=%d path=%s err=%s",
          r->ok, r->verify_ok, r->has_checksum, r->path.c_str(),
          r->error.c_str());
  if (!r->ok) {
    std::wstring msg = r->error.empty() ? L"更新安装失败" : r->error;
    if (!r->path.empty()) {
      msg += L"\n安装包保留于：\n" + r->path;
    }
    ShowToast(msg);
    return;
  }
  const int answer = MessageBoxW(
      message_hwnd_,
      L"安装包已下载并通过校验。\n\n点击「是」将关闭 DeskTidy 并开始静默安装。"
      L"是否立即安装？",
      L"下载完成", MB_YESNO | MB_ICONQUESTION);
  if (answer != IDYES) {
    return;
  }
  std::wstring err;
  if (!LaunchInstallerSilent(r->path, &err)) {
    ShowToast(err + L"\n安装包路径：" + r->path);
    return;
  }
  LogLine(L"[update] installer launched, quitting");
  PostQuitMessage(0);
}

void App::OnUpdateProgress(int pct) {
  ShowToast(L"正在下载 " + std::to_wstring(pct) + L"%");
}

void App::ApplyChromeTheme() {
  const FenceStylePreset* style = FenceStyleById(prefs_.theme_id);
  if (!style) {
    style = &DefaultFenceStyle();
  }
  page_bar_.ApplyStyle(style->background, style->accent, style->title);
  settings_.ApplyStyle(style->background, style->accent, style->title);
}

void App::SyncPageChrome() {
  NormalizePageNames(&page_names_);
  // Python float_bar_tool_flags; vault peek lives on the edge page bar.
  const bool tool_record = prefs_.record_on_bar;
  const bool tool_minutes = prefs_.minutes_on_bar;
  const bool tool_calculator = prefs_.calculator_on_bar;
  const bool tool_todo = prefs_.todos_on_bar;
  const bool tool_vault = prefs_.vault_enabled;
  pet_.SetToolFlags(tool_record, tool_minutes, tool_calculator, tool_todo);
  page_bar_.SetToolFlags(tool_record, tool_minutes, tool_calculator, tool_todo,
                         tool_vault);

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
  SyncVaultLauncher();
}

void App::AddPage() {
  if (page_names_.size() >= 12) {
    ShowToast(L"最多 12 页");
    return;
  }
  page_names_.push_back(NextPageName(page_names_.size() + 1));
  SyncPageChrome();
  SetPage(static_cast<int>(page_names_.size()) - 1);
  ShowToast(L"已添加页面");
}

void App::RemoveCurrentPage() {
  if (page_names_.size() <= 1) {
    ShowToast(L"至少保留一页");
    return;
  }
  const int remove = current_page_;
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    if (fence->Page() == remove) {
      fence->SetPage(0);
    } else if (fence->Page() > remove) {
      fence->SetPage(fence->Page() - 1);
    }
  }
  page_names_.erase(page_names_.begin() + remove);
  if (current_page_ >= static_cast<int>(page_names_.size())) {
    current_page_ = static_cast<int>(page_names_.size()) - 1;
  }
  SyncPageChrome();
  ApplyPageVisibility();
  PersistLayouts();
  ShowToast(L"已删除当前页");
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
  const RECT work = PrimaryWorkArea();
  // Python DEFAULT_NEW_FENCE_WIDTH/HEIGHT = 320×360, clamped to the work area.
  const int gap = 24;
  const int w = (std::max)(
      160, (std::min)(320, static_cast<int>(work.right - work.left) - gap * 2));
  const int h = (std::max)(
      120, (std::min)(360, static_cast<int>(work.bottom - work.top) - gap * 2));
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
  layout.style_id = prefs_.theme_id.empty() ? "charcoal" : prefs_.theme_id;
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
  // Match Python ensure_organize_fence_for_page: prefer an existing file-capable
  // fence, else adopt a kinds-empty fence, else create a document fence so
  // virtual organize lands in a visible partition (not free floats).
  auto has_file_kind = [](const std::vector<std::string>& kinds) {
    for (const auto& k : kinds) {
      if (k == "file") {
        return true;
      }
    }
    return false;
  };
  int created = 0;
  int adopted = 0;
  for (int p = 0; p < static_cast<int>(page_names_.size()); ++p) {
    FenceWindow* adopt = nullptr;
    bool has_file = false;
    for (const auto& fence : fences_) {
      if (!fence || !fence->OnPage(p)) {
        continue;
      }
      const auto& kinds = fence->OrganizeKinds();
      if (has_file_kind(kinds)) {
        has_file = true;
        break;
      }
      if (kinds.empty() && adopt == nullptr) {
        adopt = fence.get();
      }
    }
    if (has_file) {
      continue;
    }
    if (adopt != nullptr) {
      adopt->SetOrganizeKinds({"file"});
      ++adopted;
      continue;
    }
    CreateFenceOnPage(p, /*switch_to_page=*/false);
    ++created;
  }
  if (created > 0 || adopted > 0) {
    PersistLayouts();
    RefreshSettingsList();
    LogLine(L"[organize] empty-page fences created=%d adopted=%d", created,
            adopted);
  }
}

void App::DeletePageAt(int page) {
  if (page == 0 || page == 1) {
    ShowToast(L"系统页面不可删除");
    return;
  }
  if (page < 0 || page >= static_cast<int>(page_names_.size())) {
    return;
  }
  if (page_names_.size() <= 2) {
    ShowToast(L"至少保留系统页");
    return;
  }
  int content = 0;
  for (const auto& fence : fences_) {
    if (fence && fence->Page() == page) {
      ++content;
    }
  }
  if (content > 0) {
    const std::wstring msg =
        L"该页有 " + std::to_wstring(content) +
        L" 个分区，删除后这些分区将移至工作页。\n确定删除页面「" +
        page_names_[static_cast<size_t>(page)] + L"」？";
    if (MessageBoxW(settings_.Hwnd(), msg.c_str(), L"删除页面",
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
      return;
    }
  }
  page_names_.erase(page_names_.begin() + page);
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    if (fence->Page() == page) {
      fence->SetPage(0);
    } else if (fence->Page() > page) {
      fence->SetPage(fence->Page() - 1);
    }
  }
  if (current_page_ > page) {
    --current_page_;
  } else if (current_page_ >= static_cast<int>(page_names_.size())) {
    current_page_ = static_cast<int>(page_names_.size()) - 1;
  }
  SyncPageChrome();
  ApplyPageVisibility();
  PersistLayouts();
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

void App::ApplyThemeId(const std::string& theme_id) {
  const FenceStylePreset* style = FenceStyleById(theme_id);
  if (!style) {
    return;
  }
  prefs_.theme_id = style->id;
  SaveAppPrefs(prefs_);
  for (auto& fence : fences_) {
    if (fence) {
      fence->SetStyleId(prefs_.theme_id);
    }
  }
  PersistLayouts();
  ApplyChromeTheme();
  PushExtensionSettings();
  ShowToast(std::wstring(L"分区样式：") + style->name);
}

void App::ApplyUiThemeId(const std::string& ui_theme) {
  prefs_.ui_theme = NormalizeUiTheme(ui_theme);
  SaveAppPrefs(prefs_);
  const auto* opt = UiThemeById(prefs_.ui_theme);
  settings_.ApplyUiThemeVars(UiThemeCssVars(prefs_.ui_theme));
  todos_.ApplyUiTheme(prefs_.ui_theme);
  PushExtensionSettings();
  ApplyTrayThemeIcon();
  if (opt) {
    ShowToast(std::wstring(L"界面主题：") + opt->name_zh);
  }
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
  LogLine(L"[mem] trim fences_hidden=%d settings_open=%d",
          fences_hidden_ ? 1 : 0, settings_.IsOpen() ? 1 : 0);
  if (fences_hidden_) {
    for (auto& f : fences_) {
      if (f) {
        f->ClearIcons();
      }
    }
  } else {
    // P2-1: non-current pages are invisible — lazily release their icon caches
    // (rebuilt on page switch). The current page keeps its shell HICONs; the
    // C++ path loads shell icons on demand (no persistent zoomed-pixmap cache),
    // so this page-scoped release is the effective "display cache halving".
    for (auto& f : fences_) {
      if (f && f->Page() != current_page_) {
        f->ClearIcons();
      }
    }
  }
  // When the main settings window is closed (tray / background), ask the
  // process to release unused pages — mirrors Python EmptyWorkingSet-ish trim.
  if (!settings_.IsOpen()) {
    SetProcessWorkingSetSize(GetCurrentProcess(),
                             static_cast<SIZE_T>(-1),
                             static_cast<SIZE_T>(-1));
  }
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
  PushOrganizePage(&settings_, pins_, rules_, fences_);
}

void App::OnPathsDropped(FenceWindow* fence,
                         const std::vector<std::wstring>& paths) {
  // Drag: portal → Explorer default (same-drive move / cross-drive copy);
  // plain fence → pin only (no file movement), dedup via SetPin overwrite.
  HandleIncomingPaths(fence, paths, /*paste=*/false, /*move=*/false);
}

void App::OnPathsPasted(FenceWindow* fence,
                        const std::vector<std::wstring>& paths, bool move) {
  // Paste follows the clipboard Preferred DropEffect (COPY/MOVE).
  HandleIncomingPaths(fence, paths, /*paste=*/true, move);
}

void App::HandleIncomingPaths(FenceWindow* fence,
                              const std::vector<std::wstring>& paths,
                              bool paste, bool move) {
  if (!fence || paths.empty()) {
    return;
  }
  const std::wstring portal = fence->PortalPath();
  if (portal.empty()) {
    // Plain fence: pin, never move the file (matches Python).
    for (const auto& p : paths) {
      SetPin(&pins_, p, fence->Id());
    }
    SavePins(pins_);
    RefreshDesktopIcons();
    return;
  }

  // Portal fence: real file operations into the mirrored folder.
  std::vector<std::wstring> failed;
  std::vector<std::wstring> landed;
  for (const auto& src : paths) {
    const bool should_move = paste ? move : SameVolume(src, portal);
    std::wstring dest;
    if (MoveOrCopyFileIntoDir(settings_.Hwnd(), src, portal, should_move,
                              &dest)) {
      if (!dest.empty() && _wcsicmp(dest.c_str(), src.c_str()) != 0) {
        SuppressOrganizePath(src);
        SuppressOrganizePath(dest);
      }
      landed.push_back(dest.empty() ? src : dest);
    } else {
      failed.push_back(src);
      LogLine(L"[portal] %s failed (%s) -> %s", src.c_str(),
              should_move ? L"move" : L"copy", portal.c_str());
    }
  }

  InvalidateDesktopScanCache();
  RefreshDesktopIcons();

  if (failed.empty()) {
    const wchar_t* verb = paste ? (move ? L"已移动" : L"已复制") : L"已导入";
    ShowToast(std::wstring(verb) + L" " + std::to_wstring(landed.size()) +
              L" 项");
    return;
  }
  std::wstring msg = L"成功 " + std::to_wstring(landed.size()) + L" 项，失败 " +
                     std::to_wstring(failed.size()) + L" 项：\n";
  for (size_t i = 0; i < failed.size() && i < 12; ++i) {
    msg += failed[i];
    msg += L"\n";
  }
  if (failed.size() > 12) {
    msg += L"…";
  }
  MessageBoxW(settings_.Hwnd(), msg.c_str(), L"导入失败", MB_OK | MB_ICONWARNING);
}

void App::UnpinPath(const std::wstring& path) {
  SuppressOrganizePath(path);
  ClearPin(&pins_, path);
  SavePins(pins_);
  RefreshDesktopIcons();
}

void App::RetargetPin(const std::wstring& from, const std::wstring& to) {
  const std::string* id = FindPin(pins_, from);
  const std::string fence_id = id ? *id : std::string();
  ClearPin(&pins_, from);
  if (!fence_id.empty()) {
    SetPin(&pins_, to, fence_id);
  }
  SavePins(pins_);
  RefreshDesktopIcons();
}

bool App::TransferPins(const std::wstring& src, const std::wstring& dst,
                       const std::vector<std::wstring>& paths) {
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
  dst_fence->InsertPinOrder(move_list, -1);

  SavePins(pins_);
  PersistLayouts();
  RefreshDesktopIcons();
  return true;
}

DWORD App::OnPinDropped(FenceWindow* fence, const std::wstring& src_fence_id,
                        const std::vector<std::wstring>& paths, bool commit) {
  if (!fence) {
    return DROPEFFECT_NONE;
  }
  const std::wstring dst = WidenUtf8(fence->Id());
  if (src_fence_id.empty() || dst.empty() || src_fence_id == dst) {
    return DROPEFFECT_NONE;
  }
  // Reject portal targets (pins don't apply) and same-file conflicts.
  if (!fence->PortalPath().empty()) {
    return DROPEFFECT_NONE;
  }
  for (const auto& p : paths) {
    if (p.find(L"::") != std::wstring::npos) {
      return DROPEFFECT_NONE;
    }
  }
  if (commit) {
    return TransferPins(src_fence_id, dst, paths) ? DROPEFFECT_MOVE
                                                  : DROPEFFECT_NONE;
  }
  // Preview: check the destination fence exists and the paths are still pinned
  // to src (a cheap validity probe without mutating state).
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

void App::UpdatePinDragHint(POINT cursor, bool over_target, bool invalid) {
  std::wstring text;
  if (invalid) {
    text = L"无法放置";
  } else if (over_target) {
    text = L"松开移到目标分区";
  } else {
    text = L"松开移回桌面";
  }
  ShowPinHintWindow(instance_, text, cursor);
}

void App::HidePinDragHint() { HidePinHintWindow(); }

void App::MoveDesktopItemsToFence(const std::vector<std::wstring>& paths,
                                  const std::string& fence_id) {
  if (fence_id.empty()) {
    return;
  }
  // Only allow targets that exist (not portal fences).
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
  bool changed = false;
  for (const auto& p : paths) {
    // Skip system namespace paths (not pinnable).
    if (p.find(L"::") != std::wstring::npos) {
      continue;
    }
    SetPin(&pins_, p, fence_id);
    changed = true;
  }
  if (changed) {
    SavePins(pins_);
    RefreshDesktopIcons();
  }
}

void App::MoveDesktopItemsToPage(const std::vector<std::wstring>& paths,
                                 int page_index) {
  // The C++ desktop is shared across fence pages (no per-page float host yet),
  // so "move to page" unpins the items — they fall back to the real desktop.
  (void)page_index;
  bool changed = false;
  for (const auto& p : paths) {
    if (FindPin(pins_, p)) {
      ClearPin(&pins_, p);
      changed = true;
    }
  }
  if (changed) {
    SavePins(pins_);
  }
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

void App::RefreshDesktopIcons() {
  const auto items = ScanDesktopItems();
  // Pin-only fence membership (Python get_virtual_items_for_fence). Rule
  // matching is for 一键整理 / auto-pin — not live display — so unpinned
  // desktop files can surface as public/loose floats (P0-2/P0-3).
  std::unordered_map<std::string, std::vector<DesktopItem>> by_fence;
  for (const auto& it : items) {
    const std::string* pin = FindPin(pins_, it.path);
    if (!pin || pin->empty()) {
      continue;
    }
    std::string target = *pin;
    if (target == "system_software") {
      target = "system_software_side";
    }
    by_fence[target].push_back(it);
  }

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
      fence->SetItems(ScanPortalItems(portal, LoadExcludePatterns()));
      continue;
    }
    auto it = by_fence.find(fence->Id());
    if (it != by_fence.end()) {
      fence->SetItems(std::move(it->second));
    } else {
      fence->SetItems({});
    }
  }
  RebuildDockItems();
  // Loose floats when Explorer icons are hidden; shared public when toggled.
  const bool want_floats =
      prefs_.enable_public_desktop || !icons_visible_intent_;
  public_visible_ = want_floats;
  if (want_floats) {
    RebuildPublicItems();
    public_host_.SetVisible(true);
  } else {
    public_host_.SetItems({});
    public_host_.Hide();
  }
}

void App::RefreshIconGrids() {
  // P2-17: Explorer icon size / spacing changed — re-read the live grid metrics
  // and push them to every fence (public host re-queries inside SetItems).
  const IconGridMetrics m = QueryIconGridMetrics();
  for (auto& fence : fences_) {
    if (fence) {
      fence->SetIconGrid(m);
    }
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
      SHFILEINFOW sfi{};
      if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                         SHGFI_DISPLAYNAME) &&
          sfi.szDisplayName[0]) {
        it.name = sfi.szDisplayName;
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
                         "system_common");
      if (target != "system_software" && target != "system_software_side") {
        continue;
      }
      dock_items.push_back(it);
      if (dock_items.size() >= 8) {
        break;
      }
    }
  }
  dock_.SetItems(std::move(dock_items));
}

void App::ApplyPageVisibility() {
  if (peek_active_) {
    return;
  }
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    // P2-11: show when membership includes current page (not only primary page_).
    if (!fences_hidden_ && fence->VisibleOnPage(current_page_)) {
      fence->Show();
    } else {
      fence->Hide();
    }
  }
  if (pet_visible_) {
    page_bar_.Hide();
  } else {
    page_bar_.SetPage(current_page_);
    page_bar_.Show();
  }
  if (dock_visible_) {
    dock_.Show();
  }
  if (pet_visible_) {
    pet_.Show();
  }
  if (public_visible_) {
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
  current_page_ = page;
  if (!pet_visible_) {
    page_bar_.SetPage(current_page_);
  }
  pet_.SetPages(page_names_, current_page_);
  settings_.SetCurrentPage(current_page_);
  // H3: apply each fence's per-page (or default) geometry for the new page.
  for (auto& fence : fences_) {
    if (fence) {
      fence->ApplyPageGeometry(fence->GeometryForPage(current_page_));
    }
  }
  ApplyPageVisibility();
  // P1-17: after page chrome settles, keep pet clear of fence bottoms.
  if (pet_visible_) {
    std::vector<RECT> fence_rects;
    for (const auto& fence : fences_) {
      if (!fence || !fence->Hwnd() || !IsWindowVisible(fence->Hwnd())) {
        continue;
      }
      if (fence->Page() != current_page_) {
        continue;
      }
      RECT wr{};
      if (GetWindowRect(fence->Hwnd(), &wr)) {
        fence_rects.push_back(wr);
      }
    }
    pet_.NudgeClearOfFenceBottoms(fence_rects);
  }
  PersistLayouts();
}

void App::ScheduleDesktopBandRestore() {
  if (!message_hwnd_) {
    return;
  }
  // Settings owns the foreground — skip settle churn (would flash fences).
  if (settings_.IsOpen()) {
    return;
  }
  // Coalesce like Python: bump settle budget; first tick after WorkerW settles.
  if (band_restore_left_ < 12) {
    band_restore_left_ = 12;
  }
  SetTimer(message_hwnd_, kTimerBandSettle, 280, nullptr);
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
  if (!g_kb_hook) {
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr,
                                  0);
  }
}

void App::StopForegroundHook() {
  if (fg_hook_) {
    UnhookWinEvent(fg_hook_);
    fg_hook_ = nullptr;
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

void CALLBACK App::ForegroundWinEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG,
                                      DWORD, DWORD) {
  if (!g_app_for_hook || !g_app_for_hook->message_hwnd_) {
    return;
  }
  // Coalesce: Win+D / Alt+Tab-to-desktop — settle overlays after Explorer FG.
  if (IsExplorerDesktopForeground()) {
    PostMessageW(g_app_for_hook->message_hwnd_, kMsgBandRestore, 0, 0);
  }
}

void App::RestoreDesktopChrome() {
  // While settings is open, never Show/Raise fences — that flashes the desktop
  // and steals clicks from the settings WebView ("页面无法选中").
  if (settings_.IsOpen()) {
    SeatAllFencesUnderApps();
    if (settings_.Hwnd()) {
      SetWindowPos(settings_.Hwnd(), HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return;
  }

  InvalidateDefViewCache();
  ApplyPageVisibility();
  auto reattach = [](HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
      return;
    }
    // Detach/reattach and HWND_BOTTOM flash the screen. Only force-attach
    // when the overlay fell off the DefView host (Win+D).
    if (!IsAttachedToDesktop(hwnd)) {
      ForceAttachOverlayToDesktop(hwnd, true);
    }
    RaiseOverlayInDesktopBand(hwnd);
  };
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd() || fence->Page() != current_page_) {
      continue;
    }
    if (fences_hidden_ || peek_active_) {
      continue;
    }
    if (!IsWindowVisible(fence->Hwnd())) {
      fence->Show();
    } else {
      reattach(fence->Hwnd());
    }
  }
  if (dock_visible_ && dock_.Hwnd()) {
    reattach(dock_.Hwnd());
  }
  if (pet_visible_ && pet_.Hwnd()) {
    reattach(pet_.Hwnd());
  }
  if (!pet_visible_ && page_bar_.Hwnd()) {
    reattach(page_bar_.Hwnd());
  }
  if (public_visible_ && public_host_.Hwnd()) {
    reattach(public_host_.Hwnd());
  }
  if (prefs_.vault_enabled && vault_launcher_.Hwnd()) {
    reattach(vault_launcher_.Hwnd());
  }
}

void App::OrganizeVirtual(bool quiet) {
  // P2-16: heavy scan + pin resolve off the UI thread (Python organize worker).
  if (g_organize_busy) {
    if (!quiet) {
      ShowToast(L"正在整理，请稍候…");
    }
    return;
  }
  if (!message_hwnd_) {
    return;
  }
  g_organize_busy = true;
  if (!quiet) {
    ShowToast(L"正在整理…");
  }
  // Namespace hosting touches the shell — keep on the UI thread.
  const int hosted = EnsureHostedSystemNamespaceIcons(&pins_);
  auto* job = new OrganizeJobResult{};
  job->pins = pins_;
  job->hosted = hosted;
  job->quiet = quiet;
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
  workers_.emplace_back([job, rules, msg_hwnd, layouts]() {
    const auto items = ScanDesktopItems();
    job->n = OrganizeApplyDefaultPins(&job->pins, items, rules,
                                      "system_software", "system_common",
                                      &layouts) +
             job->hosted;
    if (!PostMessageW(msg_hwnd, kMsgOrganizeDone, 0,
                      reinterpret_cast<LPARAM>(job))) {
      delete job;
      g_organize_busy = false;
    }
  });
}

void App::OnOrganizeDone(OrganizeJobResult* job) {
  g_organize_busy = false;
  if (!job) {
    return;
  }
  pins_ = std::move(job->pins);
  const int n = job->n;
  const bool quiet = job->quiet;
  delete job;
  LogLine(L"[organize] virtual quiet=%d pins_added=%d", quiet ? 1 : 0, n);
  SavePins(pins_);
  EnsureOrganizeFencesForEmptyPages();
  RefreshDesktopIcons();
  PushOrganizePage(&settings_, pins_, rules_, fences_);
  if (quiet) {
    if (n > 0) {
      ShowToast(L"已自动钉选 " + std::to_wstring(n) + L" 项");
    }
    return;
  }
  wchar_t buf[128];
  swprintf_s(buf, L"虚拟整理完成：写入 %d 条钉选。", n);
  MessageBoxW(settings_.Hwnd() ? settings_.Hwnd() : message_hwnd_, buf,
              L"DeskTidy Native", MB_OK | MB_ICONINFORMATION);
}

void App::SetOrganizeStartup(bool on) {
  prefs_.auto_organize_on_startup = on;
  SaveAppPrefs(prefs_);
}

void App::SetOrganizeWatch(bool on) {
  prefs_.auto_organize_watch = on;
  SaveAppPrefs(prefs_);
}

void App::OrganizePhysical() {
  const auto items = ScanDesktopItems();
  if (!OrganizeConfirmPhysical(settings_.Hwnd(), items)) {
    return;
  }
  const int n = OrganizeMoveIntoFolders(items);
  pins_.clear();
  SavePins(pins_);
  RefreshDesktopIcons();
  wchar_t buf[128];
  swprintf_s(buf, L"物理整理完成：移动 %d 个项目到 Desktop\\DeskTidy。", n);
  MessageBoxW(settings_.Hwnd(), buf, L"DeskTidy Native",
              MB_OK | MB_ICONINFORMATION);
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
  // Shared public toggle; loose floats still appear while Explorer icons hidden.
  RefreshDesktopIcons();
  LogLine(L"[public] enable=%d floats=%d", on ? 1 : 0,
          public_visible_ ? 1 : 0);
}

void App::SetPetVisible(bool on) {
  pet_visible_ = on;
  pet_.SetVisible(pet_visible_);
  SyncPageChrome();
  PersistLayouts();
}

void App::ToggleDock() { SetDockVisible(!dock_visible_); }

void App::TogglePet() { SetPetVisible(!pet_visible_); }

void App::SetAutostart(bool on) {
  if (!SetNativeAutostart(on)) {
    MessageBoxW(settings_.Hwnd(), L"无法更新开机启动快捷方式。",
                L"DeskTidy Native", MB_OK | MB_ICONWARNING);
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
  FenceLayout layout;
  layout.id = MakeFenceId();
  layout.title = L"新建分区";
  layout.x = screen.left;
  layout.y = screen.top;
  layout.w = (std::max)(160, static_cast<int>(screen.right - screen.left));
  layout.h = (std::max)(120, static_cast<int>(screen.bottom - screen.top));
  layout.page = current_page_;
  layout.style_id =
      prefs_.theme_id.empty() ? "charcoal" : prefs_.theme_id;
  if (!SpawnFence(layout)) {
    return;
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
    fences_[i]->Destroy();
    fences_.erase(fences_.begin() + static_cast<ptrdiff_t>(i));
    // Clear pins that pointed at the dissolved fence (icons fall back to the
    // real desktop, matching Python dissolve_fence_to_page).
    std::vector<std::wstring> to_clear;
    for (const auto& kv : pins_) {
      if (kv.second == id) {
        to_clear.push_back(kv.first);
      }
    }
    for (const auto& p : to_clear) {
      ClearPin(&pins_, p);
    }
    SavePins(pins_);
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
  PersistLayouts();
  RefreshSettingsList();
}

void App::SetFencePageAt(int index, int page) {
  if (index < 0 || index >= static_cast<int>(fences_.size())) {
    return;
  }
  fences_[static_cast<size_t>(index)]->SetPage(page);
  PersistLayouts();
  ApplyPageVisibility();
  RefreshSettingsList();
  PushExtensionSettings();
}

void App::CreateTrayIcon() {
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
  wcsncpy_s(nid_.szTip, L"DeskTidy", _TRUNCATE);
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

  switch (msg) {
    case kMsgBandRestore:
      self->ScheduleDesktopBandRestore();
      return 0;
    case kMsgDesktopDblClick:
      self->OnDesktopBlankDoubleClick();
      return 0;
    case WM_CLIPBOARDUPDATE:
      // Refresh cut-ghost opacity when CF_HDROP / Preferred DropEffect changes
      // (DeskTidy Ctrl+X or Explorer cut).
      for (auto& fence : self->fences_) {
        if (fence) {
          fence->RefreshLayer();
        }
      }
      return 0;
    case WM_TIMER:
      if (wparam == kTimerRestore) {
        self->RestoreDesktopChrome();
        return 0;
      }
      if (wparam == kTimerBandSettle) {
        self->RestoreDesktopChrome();
        if (--self->band_restore_left_ <= 0) {
          KillTimer(hwnd, kTimerBandSettle);
          self->band_restore_left_ = 0;
        }
        return 0;
      }
      if (wparam == kTimerPet) {
        self->pet_.Tick();
        // P2-24: match Python idle throttle (33ms active / 100ms idle+away).
        const UINT want =
            static_cast<UINT>((std::max)(16, self->pet_.WantedTickMs()));
        if (want != self->pet_tick_ms_) {
          self->pet_tick_ms_ = want;
          SetTimer(hwnd, kTimerPet, want, nullptr);
        }
        // Clear REC badge if ffmpeg exited (e.g. timed -t duration).
        (void)IsScreenRecording();
        return 0;
      }
      if (wparam == kTimerWatch) {
        self->OnDisplayTopologyMaybeChanged();
        std::vector<std::wstring> portals;
        for (const auto& fence : self->fences_) {
          if (fence && !fence->PortalPath().empty()) {
            portals.push_back(fence->PortalPath());
          }
        }
        self->desktop_watch_.SetExtraFolders(portals);
        if (self->desktop_watch_.PollChanged()) {
          self->rules_ = LoadRules();
          if (self->prefs_.auto_organize_watch) {
            self->OrganizeVirtual(true);
          } else {
            self->RefreshDesktopIcons();
          }
        }
        return 0;
      }
      if (wparam == kTimerUpdate) {
        KillTimer(hwnd, kTimerUpdate);
        self->update_timer_ = 0;
        self->MaybeStartupUpdateCheck();
        return 0;
      }
      if (wparam == kTimerVault) {
        // Snapshot on UI thread; WinHttp on worker; apply back via kMsgVaultPullDone.
        if (self->prefs_.vault_enabled && VaultHttpLoggedIn() &&
            !g_vault_pull_busy && self->message_hwnd_) {
          g_vault_pull_busy = true;
          auto* remote =
              new std::vector<VaultHttpCredential>(self->vault_.SnapshotCredentials());
          HWND msg_hwnd = self->message_hwnd_;
          self->ReapFinishedWorkers();
          self->workers_.emplace_back([remote, msg_hwnd]() {
            const bool ok = VaultHttpPullMerge(remote);
            if (!PostMessageW(msg_hwnd, kMsgVaultPullDone, ok ? 1 : 0,
                              reinterpret_cast<LPARAM>(remote))) {
              delete remote;
              g_vault_pull_busy = false;
            }
          });
        }
        return 0;
      }
      if (wparam == kTimerOpenSettingsVault) {
        KillTimer(hwnd, kTimerOpenSettingsVault);
        self->DeferredOpenSettingsVaultSync();
        return 0;
      }
      if (wparam == kTimerOpenSettingsOrg) {
        KillTimer(hwnd, kTimerOpenSettingsOrg);
        if (self->settings_.IsOpen()) {
          PushOrganizePage(&self->settings_, self->pins_, self->rules_,
                           self->fences_);
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
      if (wparam == kTimerDisplayChange) {
        KillTimer(hwnd, kTimerDisplayChange);
        self->display_change_timer_ = 0;
        self->ConvergeDisplayChange();
        if (self->display_change_stage_ == 2) {
          // Removal: schedule the second converge 1000ms later.
          self->display_change_stage_ = 3;
          self->display_change_timer_ =
              SetTimer(hwnd, kTimerDisplayChange, 1000, nullptr);
        } else {
          self->display_change_stage_ = 0;
        }
        return 0;
      }
      break;
    case kMsgVaultPullDone: {
      auto* remote =
          reinterpret_cast<std::vector<VaultHttpCredential>*>(lparam);
      g_vault_pull_busy = false;
      if (remote) {
        if (wparam) {
          self->vault_.ApplyPulledCredentials(*remote);
          self->RefreshVaultListForSettings();
          if (self->settings_.IsOpen()) {
            self->PushExtensionSettings();
          }
          ShowToast(L"账号库已后台同步");
        }
        delete remote;
      }
      return 0;
    }
    case kMsgOrganizeDone:
      self->OnOrganizeDone(reinterpret_cast<OrganizeJobResult*>(lparam));
      return 0;
    case kMsgUpdateCheckDone:
      self->OnUpdateCheckDone(reinterpret_cast<UpdateCheckResult*>(lparam));
      return 0;
    case kMsgUpdateInstallDone:
      self->OnUpdateInstallDone(reinterpret_cast<UpdateInstallResult*>(lparam));
      return 0;
    case kMsgUpdateProgress:
      self->OnUpdateProgress(static_cast<int>(wparam));
      return 0;
    case kTrayMsg: {
      // NOTIFYICON_VERSION_4 packs the event in LOWORD(lparam).
      const UINT tray_ev = LOWORD(lparam);
      if (tray_ev == WM_LBUTTONDBLCLK || tray_ev == WM_LBUTTONUP ||
          tray_ev == NIN_SELECT || tray_ev == NIN_KEYSELECT) {
        // VERSION_4 can fire NIN_SELECT and WM_LBUTTONUP for one click.
        if (self->TryConsumeTrayOpen()) {
          self->OpenSettings();
        }
        return 0;
      }
      if (tray_ev == WM_RBUTTONUP || tray_ev == WM_CONTEXTMENU) {
        POINT pt{};
        GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, Tr("settings").c_str());
        AppendMenuW(menu, MF_STRING, 2, Tr("notes").c_str());
        AppendMenuW(menu, MF_STRING, 3, Tr("todos").c_str());
        AppendMenuW(menu, MF_STRING, 4, Tr("search").c_str());
        AppendMenuW(menu, MF_STRING, 5, Tr("vault").c_str());
        AppendMenuW(menu, MF_STRING, 6, Tr("minutes").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 7, Tr("screenshot").c_str());
        if (IsScreenRecording() || self->prefs_.record_enabled) {
          AppendMenuW(
              menu, MF_STRING, 8,
              IsScreenRecording()
                  ? (IsScreenRecordPaused() ? L"继续录屏" : L"停止录屏")
                  : Tr("record").c_str());
        }
        AppendMenuW(menu, MF_STRING, 9, Tr("wallpaper").c_str());
        AppendMenuW(menu, MF_STRING, 10, Tr("updates").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 11, Tr("save_snapshot").c_str());
        AppendMenuW(menu, MF_STRING, 12, Tr("manage_snapshots").c_str());
        AppendMenuW(menu, MF_STRING, 13, Tr("peek").c_str());
        AppendMenuW(menu, MF_STRING, 14, Tr("toggle_fences").c_str());
        AppendMenuW(menu,
                    MF_STRING | (self->icons_visible_intent_ ? MF_CHECKED : 0),
                    40, L"显示桌面图标");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 15, Tr("page1").c_str());
        AppendMenuW(menu, MF_STRING, 16, Tr("page2").c_str());
        AppendMenuW(menu, MF_STRING, 17, Tr("organize_virtual").c_str());
        AppendMenuW(menu, MF_STRING, 18, Tr("organize_physical").c_str());
        AppendMenuW(menu, MF_STRING, 19, Tr("edit_rules").c_str());
        AppendMenuW(menu, MF_STRING, 30, Tr("whitelist").c_str());
        AppendMenuW(menu, MF_STRING, 20, Tr("refresh_icons").c_str());
        AppendMenuW(menu, MF_STRING, 21, Tr("add_fence").c_str());
        AppendMenuW(menu, MF_STRING, 23, Tr("toggle_pet").c_str());
        AppendMenuW(menu,
                    MF_STRING | (IsNativeAutostartEnabled() ? MF_CHECKED : 0),
                    24, L"开机启动");
        AppendMenuW(menu, MF_STRING, 25, L"恢复桌面层");
        AppendMenuW(menu, MF_STRING, 36, L"修复分区布局");
        AppendMenuW(menu, MF_STRING, 26, Tr("hotkey_help").c_str());
        AppendMenuW(menu, MF_STRING, 27, Tr("edit_hotkeys").c_str());
        AppendMenuW(menu, MF_STRING, 28, Tr("fullscreen_shot").c_str());
        AppendMenuW(menu, MF_STRING, 31, Tr("language").c_str());
        AppendMenuW(menu, MF_STRING, 32, Tr("calculator").c_str());
        AppendMenuW(menu, MF_STRING, 33, Tr("page_folders").c_str());
        AppendMenuW(menu, MF_STRING, 34, Tr("help").c_str());
        AppendMenuW(menu, MF_STRING, 41, L"关于");
        AppendMenuW(menu, MF_STRING, 35, L"卸载清理…");
        AppendMenuW(menu, MF_STRING, 38, L"布局编辑…");
        AppendMenuW(menu, MF_STRING, 39, L"切换主题");
        AppendMenuW(menu, MF_STRING, 37, L"复制数据目录路径");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
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
          case 5:
            self->OpenVault();
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
          case 9:
            WallpaperMenu(self->settings_.Hwnd());
            break;
          case 10:
            OpenUpdatePage(self->settings_.Hwnd());
            break;
          case 11:
            self->SaveLayoutSnapshot();
            break;
          case 12:
            self->RestoreLayoutSnapshot();
            break;
          case 13:
            self->TogglePeek();
            break;
          case 14:
            self->ToggleFencesVisible();
            break;
          case 40:
            self->SetIconsVisible(!self->icons_visible_intent_, true);
            break;
          case 15:
            self->SetPage(0);
            break;
          case 16:
            self->SetPage(1);
            break;
          case 17:
            self->OrganizeVirtual();
            break;
          case 18:
            self->OrganizePhysical();
            break;
          case 19:
            self->EditRules();
            break;
          case 30:
            EditWhitelistDialog(self->settings_.Hwnd());
            break;
          case 20:
            self->rules_ = LoadRules();
            self->RefreshDesktopIcons();
            break;
          case 21:
            self->AddFence();
            break;
          case 23:
            self->TogglePet();
            break;
          case 24:
            self->ToggleAutostart();
            break;
          case 25:
            self->RestoreDesktopChrome();
            break;
          case 36:
            self->HealLayoutsNow(true);
            break;
          case 26:
            self->ShowHotkeys();
            break;
          case 27:
            self->EditHotkeys();
            break;
          case 28:
            CaptureRegionToPng(self->settings_.Hwnd());
            break;
          case 31:
            self->ApplyUiLanguage();
            break;
          case 32:
            OpenCalculator();
            break;
          case 33:
            OpenPageFolderMenu(self->settings_.Hwnd());
            break;
          case 34:
            ShowAppHelp(self->settings_.Hwnd());
            break;
          case 41: {
            const std::wstring msg =
                L"DeskTidy 桌面整理\n版本 " + NativeProductVersion() +
                L"\n\n官网 / 发布页：\ngithub.com/Bowen-Yu0926/DeskTidy-Releases\n\n"
                L"「是」打开发布页\n「否」打开使用说明";
            if (MessageBoxW(hwnd, msg.c_str(), L"关于 DeskTidy",
                            MB_YESNO | MB_ICONINFORMATION) == IDYES) {
              ShellExecuteW(
                  nullptr, L"open",
                  L"https://github.com/Bowen-Yu0926/DeskTidy-Releases/releases/latest",
                  nullptr, nullptr, SW_SHOWNORMAL);
            } else {
              ShowAppHelp(self->settings_.Hwnd());
            }
            break;
          }
          case 35:
            RunNativeCleanup(self->settings_.Hwnd());
            break;
          case 37:
            if (ClipboardSetText(hwnd, NativeDataDir())) {
              ShowToast(L"数据目录路径已复制");
            }
            break;
          case 38:
            self->OpenLayoutEditor();
            break;
          case 39:
            self->CycleTheme();
            break;
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
      LogLine(L"[dpi] WM_DPICHANGED %d -> %d", old_dpi, new_dpi);
      for (auto& fence : self->fences_) {
        if (!fence) {
          continue;
        }
        fence->ScaleGeometryForDpi(old_dpi, new_dpi);
        IconGridMetrics m = QueryIconGridMetrics();
        ScaleIconGridForDpi(&m, old_dpi, new_dpi);
        fence->SetIconGrid(m);
      }
      // X2: page bar recalculates its chip height + font size.
      self->page_bar_.ScaleForDpi(old_dpi, new_dpi);
      // X2: pet / dock / public host have no dedicated scale API — rescale
      // their window geometry via ScaleRectForDpi, then re-layout internals.
      auto rescale_window = [old_dpi, new_dpi](HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) {
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
      rescale_window(self->pet_.Hwnd());
      rescale_window(self->dock_.Hwnd());
      rescale_window(self->public_host_.Hwnd());
      self->RebuildDockItems();
      self->RebuildPublicItems();
      self->pet_.SetPages(self->page_names_, self->current_page_);
      self->PersistLayouts();
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
      break;
    }
    case WM_DISPLAYCHANGE: {
      const std::string fp = DisplayFingerprint();
      if (FingerprintIsTransient(fp) || fp == self->display_fp_) {
        return 0;
      }
      // Persist the current geometry under the outgoing fingerprint, then
      // remember the new one so the watch timer won't re-fire.
      const int old_count = CountMonitorsInFingerprint(self->display_fp_);
      const int new_count = static_cast<int>(EnumMonitors().size());
      self->PersistLayouts();
      self->display_fp_ = fp;
      // Reuse/reset any pending converge timer — never stack multiple timers.
      if (self->display_change_timer_) {
        KillTimer(hwnd, kTimerDisplayChange);
        self->display_change_timer_ = 0;
      }
      // Removal: two-stage converge (500ms then 1000ms) so the taskbar /
      // workspace settle. Addition / resolution change: single 500ms converge.
      self->display_change_stage_ = (new_count < old_count) ? 2 : 1;
      self->display_change_timer_ =
          SetTimer(hwnd, kTimerDisplayChange, 500, nullptr);
      LogLine(L"[display] WM_DISPLAYCHANGE removed=%d (mon %d->%d)",
              (new_count < old_count) ? 1 : 0, old_count, new_count);
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
