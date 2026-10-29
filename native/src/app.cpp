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
#include "float_placements.h"
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
constexpr UINT kTimerUpdate = 44;
constexpr UINT kTimerVault = 45;
constexpr UINT kTimerOpenSettingsVault = 46;
constexpr UINT kTimerOpenSettingsOrg = 47;
constexpr UINT kTimerDisplayChange = 49;
constexpr UINT kTimerIconsRefresh = 50;
constexpr UINT kTimerMemTrim = 51;
constexpr UINT kTimerPersistLayouts = 52;
constexpr UINT kTimerReflowPublic = 53;
constexpr UINT kMsgVaultPullDone = WM_APP + 48;
constexpr UINT kMsgOrganizeDone = WM_APP + 49;
constexpr UINT kMsgDesktopBlankDown = WM_APP + 50;
constexpr UINT kMsgDesktopPaste = WM_APP + 51;
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
  PinMap pins_at_start;
  int n = 0;
  int hosted = 0;
  bool quiet = false;
  bool reassign = false;
  // Reassign replaces the whole map — abort if the user edited pins mid-job.
  unsigned long long pins_epoch = 0;
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
                      const std::vector<std::unique_ptr<FenceWindow>>& fences,
                      int current_page) {
  if (!settings) {
    return;
  }
  const auto items = ScanDesktopItems();
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
    RebuildPublicItems();  // reflow floats outside moved fence frames
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
  fence->SetOnUnpin(
      [this](FenceWindow*, const std::wstring& path, POINT drop) {
        UnpinPath(path, &drop);
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

  if (!settings_.Create(instance)) {
    return false;
  }
  // Do NOT WarmWebView here — layout/page_names_/fences_ are still empty, and
  // SetOnReady is not wired yet. Prefetch after LoadAppState + SetOnReady.
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
  settings_.SetOnFenceStyleAll(
      [this](const std::wstring& style) { SetAllFenceStyle(style); });
  settings_.SetOnFenceOpacity(
      [this](int index, int pct) { SetFenceOpacityPctAt(index, pct); });
  settings_.SetOnFencePage(
      [this](int index, int page) { SetFencePageAt(index, page); });
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
    } else if (id == L"vault_enabled") {
      if (!value) {
        prefs_.vault_enabled = false;
        SyncVaultLauncher();
      } else if (VaultHttpLoggedIn()) {
        prefs_.vault_enabled = true;
        SyncVaultLauncher();
      } else {
        // Enable requires cloud login first — keep off until login succeeds.
        if (!VaultHttpHealth()) {
          ShowToast(L"账号库服务器不可达，未启用");
          prefs_.vault_enabled = false;
          SaveAppPrefs(prefs_);
          SyncVaultLauncher();
          PushExtensionSettings();
          SyncPageChrome();
          return;
        }
        prefs_.vault_enabled = false;
        SaveAppPrefs(prefs_);
        PushExtensionSettings();
        ShowToast(L"请先登录云端账号以启用账号管理");
        vault_.SetOnLoginResult([this](bool ok) {
          if (ok) {
            prefs_.vault_enabled = true;
            SaveAppPrefs(prefs_);
            RestartVaultRefreshTimer();
            SyncVaultLauncher();
            SyncPageChrome();
            PushExtensionSettings();
            ShowToast(L"账号管理已启用");
          } else {
            prefs_.vault_enabled = false;
            SaveAppPrefs(prefs_);
            SyncVaultLauncher();
            SyncPageChrome();
            PushExtensionSettings();
          }
        });
        vault_.CloudLogin();
        return;
      }
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
    } else if (id == L"page_chrome_style") {
      const std::string style = NarrowUtf8(value);
      prefs_.page_chrome_style =
          (style == "tabs" || style == "tab" || style == "chips") ? "tabs"
                                                                 : "float";
      SaveAppPrefs(prefs_);
      SyncPageChrome();
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
  vault_.SetOnHide([this]() { OnVaultHidden(); });
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
    } else if (id == L"vault") {
      OpenVault();
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
    EraseFloatPlacement(&float_placements_, path);
    SaveFloatPlacements(float_placements_);
    ClearPin(&pins_, path);
    BumpPinsEpoch();
    SavePins(pins_);
    RebuildPublicItems();
  });
  // Same path as fence refresh: drop scan TTL cache then rebuild fences + floats.
  // RebuildPublicItems alone kept a warm ScanDesktopItems cache and skipped
  // portal fences — paste/new via shell menu looked like a no-op until 刷新.
  public_host_.SetOnRefresh([this]() {
    InvalidateDesktopScanCache();
    RefreshDesktopIcons();
  });
  public_host_.SetOnPaste([this]() { OnDesktopPaste(); });
  public_host_.SetOnPathsDropped(
      [this](const std::vector<std::wstring>& paths) { OnDesktopDropPaths(paths); });
  public_host_.SetOnPlacementsChanged([this](const FloatPlacementMap& m) {
    float_placements_ = m;
    SaveFloatPlacements(float_placements_);
  });
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
  // 文档类文件曾被扩展名规则钉到 system_common（常用），迁回文档分区。
  bool pins_migrated = false;
  for (auto& kv : pins_) {
    if (kv.second == "system_software") {
      kv.second = "system_software_side";
      pins_migrated = true;
    } else if (kv.second == "system_page2") {
      kv.second = "system_docs";
      pins_migrated = true;
    } else if (kv.second == "system_common") {
      // 此电脑 / 回收站等命名空间项留在常用，不迁到文档。
      if (kv.first.find(L"::") != std::wstring::npos) {
        continue;
      }
      DesktopItem probe;
      probe.path = kv.first;
      const size_t slash = kv.first.find_last_of(L"\\/");
      probe.name = (slash == std::wstring::npos) ? kv.first
                                                 : kv.first.substr(slash + 1);
      const std::wstring ext = ExtOf(kv.first);
      probe.is_link =
          ext == L".lnk" || ext == L".url" ||
          (probe.name.size() >= 4 &&
           _wcsicmp(probe.name.c_str() + probe.name.size() - 4, L".lnk") == 0);
      if (OrganizeKindOfItem(probe) == "file") {
        kv.second = "system_docs";
        pins_migrated = true;
      }
    }
  }
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
  // Reconcile pin_order with pins.json: collapse corrupt "\\\\" paths and append
  // missing membership so manual sort / grid matches the real pin set.
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
  VaultHttpImportPythonSessionIfNeeded();
  // Not logged in ⇒ treat account management as off (no chrome entry).
  if (prefs_.vault_enabled && !VaultHttpLoggedIn()) {
    prefs_.vault_enabled = false;
    SaveAppPrefs(prefs_);
  }
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
  StartDesktopDblClickMonitor(message_hwnd_, kMsgDesktopDblClick,
                              kMsgDesktopBlankDown, kMsgDesktopPaste);
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
  // Prefetch WebView only after layout + on_ready_ exist so the first ready
  // push carries live pages / fence_cards (360-like settings never look empty).
  settings_.WarmWebView();
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
  } else if (id == "vault") {
    OpenVault();
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
      KillTimer(message_hwnd_, kTimerPersistLayouts);
      KillTimer(message_hwnd_, kTimerReflowPublic);
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
      if (MessageBoxW(owner, ask.c_str(), L"恢复快照",
                      MB_YESNO | MB_ICONQUESTION) == IDYES) {
        AppState state;
        if (LoadSnapshotWithIcons(item.path, &state)) {
          ReloadFencesFromState(state, /*heal_geometry=*/false);
          RestoreSnapshotShellIcons(item.path);
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
        if (DeleteSnapshot(item.path) ||
            GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
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
  } else if (id == L"vault_login") {
    vault_.SetOnLoginResult([this](bool /*ok*/) {
      SyncVaultSessionState();
      RefreshVaultListForSettings();
      PushExtensionSettings();
      SyncPageChrome();
    });
    vault_.CloudLogin();
  } else if (id == L"vault_logout") {
    VaultHttpLogout();
    prefs_.vault_enabled = false;
    SaveAppPrefs(prefs_);
    SyncVaultLauncher();
    SyncPageChrome();
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
  if (!prefs_.vault_enabled || !VaultHttpLoggedIn()) {
    const int r = MessageBoxW(
        settings_.Hwnd(),
        L"账号管理未启用或未登录。\n\n是否现在登录并启用？\n"
        L"（也可在设置 → 账号管理中操作）",
        L"账号库", MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) {
      return;
    }
    if (!VaultHttpHealth()) {
      ShowToast(L"账号库服务器不可达");
      return;
    }
    if (!VaultHttpLoggedIn()) {
      vault_.SetOnLoginResult([this](bool ok) {
        if (!ok) {
          prefs_.vault_enabled = false;
          SaveAppPrefs(prefs_);
          SyncVaultLauncher();
          SyncPageChrome();
          PushExtensionSettings();
          return;
        }
        prefs_.vault_enabled = true;
        SaveAppPrefs(prefs_);
        RestartVaultRefreshTimer();
        SyncVaultLauncher();
        SyncPageChrome();
        PushExtensionSettings();
        OpenVault();
      });
      vault_.CloudLogin();
      return;
    }
    prefs_.vault_enabled = true;
    SaveAppPrefs(prefs_);
    RestartVaultRefreshTimer();
    SyncVaultLauncher();
    PushExtensionSettings();
    SyncPageChrome();
  }
  // Second click on the same entry closes (common float-panel toggle).
  // Hide() → OnVaultHidden sinks fences (do not Raise on desktop FG).
  if (vault_.Hwnd() && IsWindowVisible(vault_.Hwnd()) && !IsIconic(vault_.Hwnd())) {
    vault_.Hide();
    return;
  }
  if (message_hwnd_ && band_restore_left_ > 0) {
    KillTimer(message_hwnd_, kTimerBandSettle);
    band_restore_left_ = 0;
  }
  HWND anchor = nullptr;
  const bool pet_hosts = pet_visible_ && pet_.ShowPageBubbles();
  if (pet_hosts && pet_.Hwnd()) {
    anchor = pet_.Hwnd();
  } else if (page_bar_.Hwnd() && IsWindowVisible(page_bar_.Hwnd())) {
    anchor = page_bar_.Hwnd();
  }
  vault_.PlaceBeside(anchor);
  vault_.Show();
  vault_.PlaceBeside(anchor);
  SeatAllFencesUnderApps();
  if (vault_.Hwnd()) {
    SetWindowPos(vault_.Hwnd(), HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  }
}

void App::OnVaultHidden() {
  // Hide returns FG to Progman/DefView → ForegroundWinEvent schedules band
  // restore. RaiseOverlay there seats fences above open apps — sink instead
  // (same contract as OpenVault / settings: modal chrome must not restack).
  if (message_hwnd_ && band_restore_left_ > 0) {
    KillTimer(message_hwnd_, kTimerBandSettle);
    band_restore_left_ = 0;
  }
  // Cover the async kMsgBandRestore posted from the FG hook after Hide.
  suppress_band_restore_until_ = GetTickCount64() + 900;
  SeatAllFencesUnderApps();
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
  ReloadFencesFromState(state, /*heal_geometry=*/false);
  if (!applied_path.empty()) {
    RestoreSnapshotShellIcons(applied_path);
  }
  MessageBoxW(settings_.Hwnd(), L"已恢复所选快照。", L"布局快照",
              MB_OK | MB_ICONINFORMATION);
}

void App::ReloadFencesFromState(const AppState& state, bool heal_geometry) {
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
  // Snapshot / layout file may have rewritten pins.json — reload or the UI
  // keeps the pre-apply pin map and looks unchanged.
  pins_ = LoadPins();
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
    SetWindowPos(fences_[i]->Hwnd(), nullptr, layouts[i].x, layouts[i].y,
                 layouts[i].w, layouts[i].h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    fences_[i]->SetPageGeometry(
        fences_[i]->Page(), FenceGeometryForPage(layouts[i], layouts[i].page));
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
      L"否 = 新建 Word (.docx)\n"
      L"取消 = 更多…",
      L"会议纪要", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDYES) {
    CreateAndOpenMinutes(settings_.Hwnd());
    return;
  }
  if (choice == IDNO) {
    CreateAndOpenMinutesDocx(settings_.Hwnd());
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
  // Page filter: placement.page == current_page_ (missing → claim current page).
  std::vector<DesktopItem> pub;
  const auto items = ScanDesktopItems();
  pub.reserve(items.size());
  bool placements_dirty = false;
  for (const auto& it : items) {
    if (FindPin(pins_, it.path)) {
      continue;
    }
    const std::wstring key = NormalizePinKey(it.path);
    auto fit = float_placements_.find(key);
    if (fit == float_placements_.end()) {
      FloatPlacement fp;
      fp.path = it.path;
      fp.page = current_page_;
      fp.has_pos = false;
      float_placements_[key] = fp;
      placements_dirty = true;
      pub.push_back(it);
      continue;
    }
    if (fit->second.page != current_page_) {
      continue;
    }
    pub.push_back(it);
  }
  if (placements_dirty) {
    SaveFloatPlacements(float_placements_);
  }
  // Prefer saved positions (top-left first) then name for unplaced items.
  std::sort(pub.begin(), pub.end(),
            [this](const DesktopItem& a, const DesktopItem& b) {
              const auto ia = float_placements_.find(NormalizePinKey(a.path));
              const auto ib = float_placements_.find(NormalizePinKey(b.path));
              const bool ha =
                  ia != float_placements_.end() && ia->second.has_pos;
              const bool hb =
                  ib != float_placements_.end() && ib->second.has_pos;
              if (ha != hb) {
                return ha && !hb;
              }
              if (ha && hb) {
                if (ia->second.ry != ib->second.ry) {
                  return ia->second.ry < ib->second.ry;
                }
                if (ia->second.rx != ib->second.rx) {
                  return ia->second.rx < ib->second.rx;
                }
              }
              return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
            });
  std::vector<RECT> blocked;
  blocked.reserve(fences_.size());
  constexpr int kFencePad = 12;
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
  ext.record_mic = prefs_.record_mic;
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
  ext.desktop_right_click_menu = prefs_.desktop_right_click_menu;
  ext.close_to_tray = prefs_.close_to_tray && prefs_.close_behavior_prompted;
  ext.show_fences = prefs_.show_fences;
  ext.double_click_hide = prefs_.double_click_hide;
  ext.enable_public_desktop = prefs_.enable_public_desktop;
  ext.show_page_indicator = prefs_.show_page_indicator;
  ext.page_chrome_style = prefs_.page_chrome_style == "tabs" ? "tabs" : "float";
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
  // Account entry lives on page chrome / pet — no standalone desktop buoy.
  vault_launcher_.SetVisible(false);
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

void App::SetAllFenceStyle(const std::wstring& style_id) {
  const std::string id = NormalizeFenceStyleId(NarrowUtf8(style_id));
  for (auto& fence : fences_) {
    if (fence) {
      fence->SetStyleId(id);
    }
  }
  // Global preset lives in prefs_.theme_id (not AppState), so persist it
  // explicitly; PersistLayouts() only writes the per-fence layouts.
  prefs_.theme_id = id;
  SaveAppPrefs(prefs_);
  PersistLayouts();
  PushExtensionSettings();
  ShowToast(L"已应用到全部分区样式");
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
  // Double-click hide/show desktop icons removed — keep organized chrome.
}

void App::OnDesktopBlankLeftDown() {
  // Same as Explorer / Fences: click outside a fence drops icon selection.
  for (auto& fence : fences_) {
    if (fence) {
      fence->ClearSelection();
    }
  }
}

void App::OnDesktopPaste() {
  // Stock view: Explorer DefView owns paste. Organized view hides the listview,
  // so we land CF_HDROP into the Desktop folder and show floats (Fences).
  if (icons_visible_intent_) {
    return;
  }
  std::vector<std::wstring> landed;
  const int n =
      PasteClipboardToDesktopFolder(message_hwnd_ ? message_hwnd_ : nullptr,
                                    &landed);
  if (n <= 0) {
    ShowToast(L"剪贴板里没有文件");
    return;
  }
  for (const auto& p : landed) {
    SuppressOrganizePath(p);
  }
  InvalidateDesktopScanCache();
  RefreshDesktopIcons();
  ShowToast(L"已粘贴 " + std::to_wstring(n) + L" 项到桌面");
}

void App::OnDesktopDropPaths(const std::vector<std::wstring>& paths) {
  if (icons_visible_intent_ || paths.empty()) {
    return;
  }
  PWSTR desk_raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) ||
      !desk_raw) {
    return;
  }
  const std::wstring desk = desk_raw;
  CoTaskMemFree(desk_raw);
  HWND owner = message_hwnd_ ? message_hwnd_ : nullptr;
  int ok = 0;
  for (const auto& src : paths) {
    std::wstring dest;
    if (MoveOrCopyFileIntoDir(owner, src, desk, /*move=*/false, &dest)) {
      ++ok;
      if (!dest.empty()) {
        SuppressOrganizePath(dest);
      }
    }
  }
  if (ok <= 0) {
    ShowToast(L"未能放到桌面");
    return;
  }
  InvalidateDesktopScanCache();
  RefreshDesktopIcons();
  ShowToast(L"已放入桌面 " + std::to_wstring(ok) + L" 项");
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
  // Market/Python `_keep_overlays_under_apps`: HWND_BOTTOM sink under APPWINDOWs.
  // Do NOT RaiseOverlay here — Raise after Place was lifting Progman-owned
  // fences over WeChat/VS Code when opening settings / 账号浮窗.
  //
  // Critical: AttachOverlay may ShowWindow when hidden. Only seat chrome that
  // should be visible — otherwise opening Settings resurrects a pet/dock the
  // user just turned off.
  auto seat = [](HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
      return;
    }
    if (!IsAttachedToDesktop(hwnd)) {
      AttachOverlayToDesktop(hwnd, true);
    }
    PlaceOverlayInDesktopBand(hwnd, true);
  };
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd()) {
      continue;
    }
    const bool want =
        !fences_hidden_ && !peek_active_ && fence->VisibleOnPage(current_page_);
    if (!want) {
      continue;
    }
    seat(fence->Hwnd());
  }
  if (dock_visible_ && dock_.Hwnd()) {
    seat(dock_.Hwnd());
  }
  if (pet_visible_ && pet_.Hwnd()) {
    seat(pet_.Hwnd());
  }
  const bool pet_hosts = pet_visible_ && pet_.ShowPageBubbles();
  if (!pet_hosts && prefs_.show_page_indicator && page_bar_.Hwnd()) {
    seat(page_bar_.Hwnd());
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
  // Keep 账号浮窗 above the sunk desktop band while it is open.
  if (vault_.Hwnd() && IsWindowVisible(vault_.Hwnd()) && !IsIconic(vault_.Hwnd())) {
    SetWindowPos(vault_.Hwnd(), HWND_TOP, 0, 0, 0, 0,
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
  SaveAppState(state);
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

void App::ReflowPublicFloats() {
  if (!public_visible_) {
    return;
  }
  std::vector<RECT> blocked;
  blocked.reserve(fences_.size());
  constexpr int kFencePad = 12;
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
  const int answer = MessageBoxW(message_hwnd_, msg.c_str(), L"检查更新",
                                 MB_YESNOCANCEL | MB_ICONQUESTION);
  if (answer == IDNO) {
    ShellExecuteW(nullptr, L"open",
                  html_url.empty()
                      ? L"https://github.com/Bowen-Yu0926/DeskTidy-Releases/releases/latest"
                      : html_url.c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
  } else if (answer == IDYES) {
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
  const FenceStylePreset* style = FenceStyleById(prefs_.theme_id);
  if (!style) {
    style = &DefaultFenceStyle();
  }
  page_bar_.ApplyStyle(style->background, style->accent, style->title);
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
  const bool tool_vault = prefs_.vault_enabled && VaultHttpLoggedIn();
  pet_.SetToolFlags(tool_record, tool_minutes, tool_calculator, tool_todo,
                    tool_vault);
  page_bar_.SetToolFlags(tool_record, tool_minutes, tool_calculator, tool_todo,
                         tool_vault);
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
  // After virtual organize: make sure there is somewhere for "file" items to
  // land. ResolveFenceByKind already falls through to other pages, so a global
  // file fence (system_docs) is enough — do NOT rewrite empty organize_kinds.
  // Empty kinds =「不参与整理」(UI / default「其他」); adopting them as file was
  // stomping user intent every organize (logs: empty-page fences adopted=1).
  auto has_file_kind = [](const std::vector<std::string>& kinds) {
    for (const auto& k : kinds) {
      if (k == "file") {
        return true;
      }
    }
    return false;
  };
  bool global_file = false;
  for (const auto& fence : fences_) {
    if (fence && has_file_kind(fence->OrganizeKinds())) {
      global_file = true;
      break;
    }
  }
  if (global_file) {
    return;
  }
  // No file landing zone anywhere — create one on the current page (or 0).
  const int page =
      (current_page_ >= 0 &&
       current_page_ < static_cast<int>(page_names_.size()))
          ? current_page_
          : 0;
  CreateFenceOnPage(page, /*switch_to_page=*/false);
  PersistLayouts();
  RefreshSettingsList();
  LogLine(L"[organize] empty-page fences created=1 (no global file zone)");
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
  // Market/Python memory trim: keep parked-page icon caches warm (page switch
  // must not flash empty fences). Only drop HICONs when fences are hidden.
  if (fences_hidden_) {
    for (auto& f : fences_) {
      if (f) {
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
  PushOrganizePage(&settings_, pins_, rules_, fences_, current_page_);
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
      SuppressOrganizePath(p);
      SetPin(&pins_, p, fence->Id());
      EraseFloatPlacement(&float_placements_, p);
    }
    BumpPinsEpoch();
    SavePins(pins_);
    SaveFloatPlacements(float_placements_);
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

void App::UnpinPath(const std::wstring& path, const POINT* drop) {
  // Keep the float out of auto-organize for a long window — 12s was too short
  // and watch ticks re-pinned icons the user just dragged back to the desktop.
  SuppressOrganizePath(path, 24.0 * 3600.0);
  ClearPin(&pins_, path);
  BumpPinsEpoch();
  SavePins(pins_);
  POINT pt{};
  if (drop) {
    pt = *drop;
  } else {
    GetCursorPos(&pt);
  }
  UpsertFloatPlacement(&float_placements_, path, current_page_, pt);
  SaveFloatPlacements(float_placements_);
  RefreshDesktopIcons();
  if (public_visible_ && public_host_.Hwnd()) {
    public_host_.AttachToDesktopLayer();
  }
}

void App::RetargetPin(const std::wstring& from, const std::wstring& to) {
  const std::string* id = FindPin(pins_, from);
  const std::string fence_id = id ? *id : std::string();
  ClearPin(&pins_, from);
  if (!fence_id.empty()) {
    SetPin(&pins_, to, fence_id);
  }
  RetargetFloatPlacement(&float_placements_, from, to);
  SaveFloatPlacements(float_placements_);
  BumpPinsEpoch();
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

  BumpPinsEpoch();
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

void App::UpdatePinDragHint(POINT cursor, const PinDragHint& hint) {
  std::wstring text;
  if (hint.invalid) {
    text = L"无法放置";
  } else if (hint.over_target) {
    text = L"松开移到目标分区";
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
    EraseFloatPlacement(&float_placements_, p);
    changed = true;
  }
  if (changed) {
    BumpPinsEpoch();
    SavePins(pins_);
    SaveFloatPlacements(float_placements_);
    RefreshDesktopIcons();
  }
}

void App::MoveDesktopItemsToPage(const std::vector<std::wstring>& paths,
                                 int page_index) {
  // Fences-class: unpin and attach the float to the target page.
  bool pins_changed = false;
  for (const auto& p : paths) {
    if (FindPin(pins_, p)) {
      ClearPin(&pins_, p);
      pins_changed = true;
    }
    SuppressOrganizePath(p, 24.0 * 3600.0);
    SetFloatPage(&float_placements_, p, page_index);
  }
  if (pins_changed) {
    BumpPinsEpoch();
    SavePins(pins_);
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
  if (pin_prune_last_ms_ && now - pin_prune_last_ms_ < 10000) {
    return;  // 10s throttle; watch timer ticks every 2s
  }
  pin_prune_last_ms_ = now;
  int dropped = 0;
  for (auto it = pins_.begin(); it != pins_.end();) {
    const std::wstring& path = it->first;
    if (!path.empty() &&
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
    LogLine(L"[pin] sticky prune drop: %s", path.c_str());
    it = pins_.erase(it);
    ++dropped;
  }
  if (dropped > 0) {
    BumpPinsEpoch();
    SavePins(pins_);
    RefreshDesktopIcons();
    LogLine(L"[pin] sticky prune dropped=%d", dropped);
  }
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
  // Python get_virtual_items_for_fence walks the stored pin list directly, so
  // paths outside the desktop folder still render in a fence. Mirror that:
  // append pinned files the desktop scan cannot see (existing, non-namespace).
  {
    std::unordered_set<std::wstring> scanned;
    scanned.reserve(items.size() + pins_.size());
    for (const auto& it : items) {
      scanned.insert(NormalizePinKey(it.path));
    }
    for (const auto& kv : pins_) {
      const std::wstring& path = kv.first;
      if (path.empty() || kv.second.empty()) {
        continue;
      }
      if (path.find(L"::") != std::wstring::npos) {
        continue;  // shell namespace item (此电脑 / 回收站), not a file path
      }
      if (scanned.find(NormalizePinKey(path)) != scanned.end()) {
        continue;
      }
      if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
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
      SHFILEINFOW sfi{};
      if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                         SHGFI_DISPLAYNAME) &&
          sfi.szDisplayName[0]) {
        extra.name = sfi.szDisplayName;
      }
      std::string target = kv.second;
      if (target == "system_software") {
        target = "system_software_side";
      }
      by_fence[target].push_back(std::move(extra));
    }
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
  // One icon truth: floats only when Explorer icons are hidden (Fences).
  // enable_public_desktop remains in prefs but no longer stacks a second set.
  const bool want_floats = !icons_visible_intent_;
  public_visible_ = want_floats;
  if (want_floats) {
    RebuildPublicItems();
    // RebuildPublicItems already hides/shows based on whether any float is
    // left; showing unconditionally resurrected an empty header plate.
    public_host_.SetVisible(public_host_.HasItems());
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
  dock_.SetItems(std::move(dock_items));
}

void App::ApplyPageVisibility() {
  if (peek_active_) {
    return;
  }
  // Non-page-switch paths (peek end, show_fences toggle): still OK to Show/Hide
  // individually. Page clicks go through SetPage's OverlayWindowBatch.
  for (auto& fence : fences_) {
    if (!fence) {
      continue;
    }
    if (!fences_hidden_ && fence->VisibleOnPage(current_page_)) {
      fence->Show();
    } else {
      fence->HideTransient();
    }
  }
  if (pet_visible_) {
    page_bar_.Hide();
  } else {
    page_bar_.SetPage(current_page_);
    page_bar_.Show();
  }
  if (dock_visible_ &&
      (!dock_.Hwnd() || !IsWindowVisible(dock_.Hwnd()))) {
    dock_.Show();
  }
  if (pet_visible_ &&
      (!pet_.Hwnd() || !IsWindowVisible(pet_.Hwnd()))) {
    pet_.Show();
  }
  if (public_visible_ && public_host_.HasItems() &&
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
  if (page == current_page_) {
    if (!pet_visible_) {
      page_bar_.SetPage(current_page_);
    }
    return;
  }
  current_page_ = page;
  // Selection paint only — do not Attach/Relayout the edge bar (page flash).
  if (!pet_visible_) {
    page_bar_.SetPage(current_page_);
  }
  pet_.SetPages(page_names_, current_page_);
  settings_.SetCurrentPage(current_page_, settings_.IsOpen());

  // Market: one DeferWindowPos for leavers+arrivers (Python OverlayWindowBatch).
  // Paint arrivals while still hidden, then SHOW with content ready.
  OverlayWindowBatch batch;
  std::vector<HWND> raise_after;
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd()) {
      continue;
    }
    const HWND hwnd = fence->Hwnd();
    const bool want =
        !fences_hidden_ && !peek_active_ && fence->VisibleOnPage(current_page_);
    const bool mapped =
        IsWindowVisible(hwnd) != FALSE && IsIconic(hwnd) == FALSE;
    if (want) {
      fence->ApplyPageGeometry(fence->GeometryForPage(current_page_),
                               /*present=*/false);
      if (!IsAttachedToDesktop(hwnd)) {
        AttachOverlayToDesktop(hwnd, /*show=*/false);
      }
      if (!mapped) {
        // Soft-park restore: paint off-map, then batch SWP_SHOWWINDOW.
        fence->RefreshLayer();
        batch.Show(hwnd);
        // Batch Show keeps SWP_NOZORDER — parked fences stay under DefView /
        // wallpaper unless raised (looks like a blank page: window "visible"
        // but not on the desktop band). Match FenceWindow::Show seating.
        raise_after.push_back(hwnd);
      }
      // Already mapped (multi-page): geometry-only; skip RefreshLayer flash.
    } else if (mapped) {
      // Soft hide via batch (keep layered buffer; no per-fence ShowWindow).
      batch.Hide(hwnd);
    }
  }
  batch.Commit();
  for (HWND hwnd : raise_after) {
    RaiseOverlayInDesktopBand(hwnd);
  }

  // Chrome: paint/selection only — never restack on page click.
  if (pet_visible_) {
    page_bar_.Hide();
  } else {
    page_bar_.Show();
  }

  // Defer pet nudge + float reflow off the click path (sync SetWindowPos flash).
  if (message_hwnd_ && (pet_visible_ || public_visible_)) {
    KillTimer(message_hwnd_, kTimerReflowPublic);
    SetTimer(message_hwnd_, kTimerReflowPublic, 48, nullptr);
  }
  SchedulePersistLayouts();
}

void App::ScheduleDesktopBandRestore() {
  if (!message_hwnd_) {
    return;
  }
  if (!DesktopChromeMayRestack()) {
    return;
  }
  // After 账号浮窗 close, ignore desktop-FG restore briefly (avoids Raise).
  if (suppress_band_restore_until_ != 0 &&
      GetTickCount64() < suppress_band_restore_until_) {
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

bool App::DesktopChromeMayRestack() const {
  // Market: while any owned modal owns FG chrome, do not raise/seat fences.
  if (settings_.IsOpen()) {
    return false;
  }
  if (vault_.Hwnd() && IsWindowVisible(vault_.Hwnd())) {
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
  auto mapped_attached = [](HWND hwnd) -> bool {
    return hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd) && !IsIconic(hwnd) &&
           IsAttachedToDesktop(hwnd);
  };
  for (const auto& fence : fences_) {
    if (!fence || !fence->Hwnd() || !fence->VisibleOnPage(current_page_)) {
      continue;
    }
    if (!mapped_attached(fence->Hwnd())) {
      return false;
    }
  }
  if (dock_visible_ && dock_.Hwnd() && !mapped_attached(dock_.Hwnd())) {
    return false;
  }
  if (pet_visible_ && pet_.Hwnd() && !mapped_attached(pet_.Hwnd())) {
    return false;
  }
  if (!pet_visible_ && page_bar_.Hwnd() &&
      !mapped_attached(page_bar_.Hwnd())) {
    return false;
  }
  if (public_visible_ && public_host_.Hwnd() && public_host_.HasItems() &&
      !mapped_attached(public_host_.Hwnd())) {
    return false;
  }
  // Intentionally ignore vault_launcher_: buoy Show/Place must not mark the
  // whole desktop chrome "unhealthy" and trigger a fence Raise over apps.
  return true;
}

void App::RestoreDesktopChrome() {
  // While settings/vault/todos is open, do NOT re-seat fences.
  if (!DesktopChromeMayRestack()) {
    if (settings_.IsOpen() && settings_.Hwnd()) {
      SetWindowPos(settings_.Hwnd(), HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return;
  }

  // Market/Python keepalive raise_band=False: already seated → no Invalidate /
  // Show / Raise (system-dialog dismiss used to schedule 12 settle ticks and
  // flash the desktop on every no-op pass).
  if (DesktopChromeHealthy()) {
    return;
  }

  InvalidateDefViewCache();
  ApplyPageVisibility();
  // Raise only when recovering iconic/unmapped (Win+D). Attached+visible must
  // stay put — HWND_TOP raise after 账号 buoy / DefView FG climbs over apps.
  auto reattach = [](HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
      return;
    }
    if (!IsAttachedToDesktop(hwnd)) {
      if (!AttachOverlayToDesktop(hwnd, true)) {
        ForceAttachOverlayToDesktop(hwnd, true);
      }
      RaiseOverlayInDesktopBand(hwnd);
      return;
    }
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
      ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOWNOACTIVATE);
      RaiseOverlayInDesktopBand(hwnd);
    }
    // Attached + mapped: no-op (Python raise_band=False keepalive).
  };
  for (auto& fence : fences_) {
    if (!fence || !fence->Hwnd() || fence->Page() != current_page_) {
      continue;
    }
    if (fences_hidden_ || peek_active_) {
      continue;
    }
    if (!IsWindowVisible(fence->Hwnd()) || IsIconic(fence->Hwnd())) {
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
}

void App::OrganizeVirtual(bool quiet, bool reassign) {
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
  workers_.emplace_back([job, rules, msg_hwnd, layouts, current_page,
                         reassign]() {
    const auto items = ScanDesktopItems();
    job->n = OrganizeApplyDefaultPins(&job->pins, items, rules,
                                      "system_software", "system_docs",
                                      &layouts, current_page, reassign) +
             job->hosted;
    if (!PostMessageW(msg_hwnd, kMsgOrganizeDone, 0,
                      reinterpret_cast<LPARAM>(job))) {
      delete job;
      g_organize_busy = false;
    }
  });
}

void App::OrganizeReassign() {
  if (g_organize_busy) {
    ShowToast(L"正在整理，请稍候…");
    return;
  }
  const std::wstring tip =
      L"按当前分区规则重新分配桌面上的全部项目？\n\n"
      L"已被你手动拖入某个分区的项目也会按规则重新归位。\n"
      L"只改分区归属，不移动、不删除任何文件。";
  if (!MistConfirm(settings_.Hwnd() ? settings_.Hwnd() : message_hwnd_,
                   L"重新整理", tip.c_str(), MistMsgTone::Warn)) {
    return;
  }
  OrganizeVirtual(/*quiet=*/false, /*reassign=*/true);
}

void App::OnOrganizeDone(OrganizeJobResult* job) {
  g_organize_busy = false;
  if (!job) {
    return;
  }
  const bool quiet = job->quiet;
  const bool reassign = job->reassign;
  int applied = 0;
  if (reassign) {
    // Full remap: only apply if pins_ was not edited while the worker ran.
    if (job->pins_epoch != pins_epoch_) {
      LogLine(L"[organize] reassign aborted (pins edited mid-job)");
      delete job;
      RefreshDesktopIcons();
      ShowToast(L"整理期间钉选有变动，请重试");
      return;
    }
    applied = job->n;
    pins_ = std::move(job->pins);
  } else {
    // Virtual organize only pins leftovers (Fences-class). Merge deltas onto
    // the live map — never replace pins_ wholesale (that stomped manual drops).
    for (const auto& kv : job->pins) {
      const auto it = job->pins_at_start.find(kv.first);
      if (it != job->pins_at_start.end() && it->second == kv.second) {
        continue;  // unchanged from job start
      }
      if (FindPin(pins_, kv.first)) {
        continue;  // already pinned live (manual drop / other)
      }
      SetPin(&pins_, kv.first, kv.second);
      ++applied;
    }
  }
  delete job;
  LogLine(L"[organize] virtual quiet=%d reassign=%d pins_applied=%d",
          quiet ? 1 : 0, reassign ? 1 : 0, applied);
  if (applied > 0 || reassign) {
    SavePins(pins_);
  }
  EnsureOrganizeFencesForEmptyPages();
  RefreshDesktopIcons();
  PushOrganizePage(&settings_, pins_, rules_, fences_, current_page_);
  if (quiet) {
    if (applied > 0) {
      ShowToast(L"已自动钉选 " + std::to_wstring(applied) + L" 项");
    }
    return;
  }
  wchar_t buf[160];
  if (reassign) {
    swprintf_s(buf, L"重新整理完成：%d 项按规则换了分区。", applied);
  } else {
    swprintf_s(buf, L"虚拟整理完成：写入 %d 条钉选。", applied);
  }
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
  LogLine(L"[pet] visible=%d", on ? 1 : 0);
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
  auto try_add = [&](const std::wstring& path) {
    if (path.empty() || FindPin(pins_, path)) {
      return;
    }
    const std::wstring key = NormalizePinKey(path);
    if (!scooped.insert(key).second) {
      return;
    }
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
    // Keep pin_order of the new fence in scoop encounter order.
    if (!fences_.empty() && fences_.back() &&
        fences_.back()->Id() == layout.id) {
      std::vector<std::wstring> order;
      order.reserve(scooped.size());
      for (const auto& key : scooped) {
        // scooped holds normalized keys; recover display path from pins scan.
        for (const auto& it : ScanDesktopItems()) {
          if (NormalizePinKey(it.path) == key) {
            order.push_back(it.path);
            break;
          }
        }
      }
      fences_.back()->InsertPinOrder(order, -1);
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
    case kMsgDesktopBlankDown:
      self->OnDesktopBlankLeftDown();
      return 0;
    case kMsgDesktopPaste:
      self->OnDesktopPaste();
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
        if (self->DesktopChromeMayRestack()) {
          self->RestoreDesktopChrome();
        }
        return 0;
      }
      if (wparam == kTimerBandSettle) {
        if (!self->DesktopChromeMayRestack()) {
          KillTimer(hwnd, kTimerBandSettle);
          self->band_restore_left_ = 0;
          return 0;
        }
        self->RestoreDesktopChrome();
        // Stop once chrome is seated — do not burn the remaining settle budget
        // (that was flashing after MessageBox / system dialog dismiss).
        if (self->DesktopChromeHealthy() ||
            --self->band_restore_left_ <= 0) {
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
        // Sticky-prune dead pins (Python prune_missing_virtual_items): runs on
        // the same 2s tick, throttled to 10s, 45s grace for save-then-rewrite.
        self->PruneStickyMissingPins();
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
        if (self->prefs_.vault_enabled) {
          AppendMenuW(ext, MF_STRING, 5, Tr("vault").c_str());
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
