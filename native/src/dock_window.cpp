#include "dock_window.h"

#include "app_prefs.h"
#include "desktop_shell.h"
#include "display_geom.h"
#include "icon_actions.h"
#include "shell_context_menu.h"
#include "toast.h"

#include <CommCtrl.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <imm.h>
#include <windowsx.h>

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeDock";
// Match Python DockWidget design sheet @ 96 DPI.
constexpr int kBtn0 = 44;
constexpr int kIcon0 = 32;
constexpr int kGap0 = 8;
constexpr int kMarginX0 = 10;
constexpr int kMarginY0 = 6;
constexpr int kClear0 = 48;
constexpr int kRadius0 = 16;
bool g_registered = false;

void AddRoundRect(Gdiplus::GraphicsPath* path, int w, int h, int radius) {
  const int rr = radius * 2;
  const int rw = (std::max)(1, w - 1);
  const int rh = (std::max)(1, h - 1);
  path->AddArc(0, 0, rr, rr, 180, 90);
  path->AddArc(rw - rr, 0, rr, rr, 270, 90);
  path->AddArc(rw - rr, rh - rr, rr, rr, 0, 90);
  path->AddArc(0, rh - rr, rr, rr, 90, 90);
  path->CloseFigure();
}
}  // namespace

std::vector<std::wstring> SplitDockCsv(const std::wstring& csv) {
  std::vector<std::wstring> out;
  size_t start = 0;
  while (start <= csv.size()) {
    size_t comma = csv.find(L',', start);
    if (comma == std::wstring::npos) {
      comma = csv.size();
    }
    std::wstring part = csv.substr(start, comma - start);
    // Trim whitespace (paths may contain spaces but not leading/trailing).
    size_t a = 0;
    while (a < part.size() && (part[a] == L' ' || part[a] == L'\t' ||
                               part[a] == L'\r' || part[a] == L'\n')) {
      ++a;
    }
    size_t b = part.size();
    while (b > a && (part[b - 1] == L' ' || part[b - 1] == L'\t' ||
                     part[b - 1] == L'\r' || part[b - 1] == L'\n')) {
      --b;
    }
    part = part.substr(a, b - a);
    if (!part.empty()) {
      out.push_back(part);
    }
    if (comma >= csv.size()) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

void DockWindow::ApplyPaths(const std::vector<std::wstring>& paths) {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  std::vector<std::pair<std::wstring, int>> cold;
  const int icon_sz = HostMetrics().icon;
  for (const auto& path : paths) {
    if (slots_.size() >= 8) {
      break;
    }
    if (path.empty() ||
        GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      continue;
    }
    Slot s;
    s.item.path = path;
    s.item.name = path;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash + 1 < path.size()) {
      s.item.name = path.substr(slash + 1);
    }
    const size_t dot = s.item.name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) {
      const std::wstring ext = s.item.name.substr(dot);
      if (_wcsicmp(ext.c_str(), L".lnk") == 0 ||
          _wcsicmp(ext.c_str(), L".url") == 0) {
        s.item.is_link = true;
        s.item.name.resize(dot);
      }
    }
    if (s.item.is_link) {
      SHFILEINFOW sfi{};
      if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                         SHGFI_DISPLAYNAME) &&
          sfi.szDisplayName[0]) {
        s.item.name = sfi.szDisplayName;
      }
    }
    if (ItemIconCached(path, icon_sz)) {
      s.icon = LoadItemIcon(path, icon_sz);
    } else {
      cold.emplace_back(path, icon_sz);
    }
    slots_.push_back(std::move(s));
  }
  if (!cold.empty()) {
    PreloadIconsNotifyApp(cold);
  }
  LayoutSlots();
  ApplyVisibility();
}

void DockWindow::SetItemsFromCsv(const std::wstring& csv) {
  ApplyPaths(SplitDockCsv(csv));
}

void DockWindow::ReloadFromPrefs() {
  ApplyPaths(LoadAppPrefs().dock_items);
}

bool DockWindow::Create(HINSTANCE instance) {
  if (!g_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DockWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    g_registered = true;
  }

  metrics_ = HostMetrics();
  POINT cursor{};
  GetCursorPos(&cursor);
  const RECT work = WorkAreaAtPoint(cursor);
  const int bar_h = metrics_.my * 2 + metrics_.btn;
  const int w = metrics_.mx * 2 + metrics_.btn;
  const int x = work.left + (work.right - work.left - w) / 2;
  // Match Python DockWidget: clearance above work-area bottom (DPI-scaled).
  const int y = work.bottom - bar_h - metrics_.clear;
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName, L"Dock",
      WS_POPUP, x, y, w, bar_h, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void DockWindow::Destroy() {
  if (rename_edit_) {
    HWND edit = rename_edit_;
    rename_edit_ = nullptr;
    rename_index_ = -1;
    DestroyWindow(edit);
  }
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
      s.icon = nullptr;
    }
  }
  slots_.clear();
  if (tip_) {
    DestroyWindow(tip_);
    tip_ = nullptr;
  }
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  ReleaseLayerBuffer();
}

void DockWindow::EnsureTooltip() {
  if (tip_ || !hwnd_) {
    return;
  }
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
  InitCommonControlsEx(&icc);
  tip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                         WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                         hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!tip_) {
    return;
  }
  SetWindowPos(tip_, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
  ti.hwnd = hwnd_;
  ti.uId = 1;
  ti.lpszText = const_cast<wchar_t*>(L"");
  SendMessageW(tip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
  SendMessageW(tip_, TTM_SETMAXTIPWIDTH, 0, 280);
}

void DockWindow::UpdateHoverTip(int index) {
  EnsureTooltip();
  if (!tip_) {
    return;
  }
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.hwnd = hwnd_;
  ti.uId = 1;
  if (index < 0 || index >= static_cast<int>(slots_.size())) {
    SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
    return;
  }
  const auto& item = slots_[static_cast<size_t>(index)].item;
  std::wstring text = item.name.empty() ? item.path : item.name;
  ti.lpszText = const_cast<wchar_t*>(text.c_str());
  SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
  POINT pt{};
  GetCursorPos(&pt);
  SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x + 12, pt.y + 16));
  SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
}

void DockWindow::Show() { SetVisible(true); }

void DockWindow::Hide() { SetVisible(false); }

void DockWindow::ApplyVisibility() {
  if (!hwnd_) {
    return;
  }
  if (!want_visible_ || slots_.empty()) {
    ShowWindow(hwnd_, SW_HIDE);
    return;
  }
  if (!IsWindowVisible(hwnd_)) {
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  AttachToDesktopLayer();  // no-op when already seated (avoids page flash)
  Present();
}

void DockWindow::SetVisible(bool on) {
  want_visible_ = on;
  ApplyVisibility();
}

void DockWindow::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  if (!AttachOverlayToDesktop(hwnd_, true)) {
    ForceAttachOverlayToDesktop(hwnd_, true);
  }
  RaiseOverlayInDesktopBand(hwnd_);
}

void DockWindow::SetItems(std::vector<DesktopItem> items) {
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
    }
  }
  slots_.clear();
  slots_.reserve((std::min)(items.size(), size_t{8}));
  const int icon_sz = HostMetrics().icon;
  std::vector<std::pair<std::wstring, int>> cold;
  for (auto& item : items) {
    if (slots_.size() >= 8) {
      break;
    }
    // Match Python: skip paths that no longer exist.
    if (GetFileAttributesW(item.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      continue;
    }
    Slot s;
    s.item = std::move(item);
    if (ItemIconCached(s.item.path, icon_sz)) {
      s.icon = LoadItemIcon(s.item.path, icon_sz);
    } else if (!s.item.path.empty()) {
      cold.emplace_back(s.item.path, icon_sz);
    }
    slots_.push_back(std::move(s));
  }
  if (!cold.empty()) {
    PreloadIconsNotifyApp(cold);
  }
  LayoutSlots();
  ApplyVisibility();
}

bool DockWindow::FillMissingIcons() {
  const int icon_sz = metrics_.icon > 0 ? metrics_.icon : HostMetrics().icon;
  bool any = false;
  for (auto& s : slots_) {
    if (s.icon || s.item.path.empty()) {
      continue;
    }
    if (!ItemIconCached(s.item.path, icon_sz)) {
      continue;
    }
    s.icon = LoadItemIcon(s.item.path, icon_sz);
    if (s.icon) {
      any = true;
    }
  }
  if (!any) {
    return false;
  }
  if (hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    Present();
  }
  return true;
}

DockWindow::Metrics DockWindow::HostMetrics() const {
  POINT pt{};
  if (hwnd_ && IsWindow(hwnd_)) {
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    pt = {(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
  } else {
    GetCursorPos(&pt);
  }
  const int dpi = (std::max)(96, DpiAtPoint(pt));
  auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
  Metrics m;
  m.dpi = dpi;
  m.btn = S(kBtn0);
  m.icon = S(kIcon0);
  m.gap = S(kGap0);
  m.mx = S(kMarginX0);
  m.my = S(kMarginY0);
  m.clear = S(kClear0);
  m.radius = S(kRadius0);
  m.glow = S(4);
  return m;
}

RECT DockWindow::HostWorkArea() const {
  if (hwnd_ && IsWindow(hwnd_)) {
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    POINT c{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    return WorkAreaAtPoint(c);
  }
  POINT cursor{};
  GetCursorPos(&cursor);
  return WorkAreaAtPoint(cursor);
}

void DockWindow::RebuildIcons() {
  const int icon = metrics_.icon > 0 ? metrics_.icon : HostMetrics().icon;
  std::vector<std::pair<std::wstring, int>> cold;
  for (auto& s : slots_) {
    if (s.icon) {
      DestroyIcon(s.icon);
      s.icon = nullptr;
    }
    if (ItemIconCached(s.item.path, icon)) {
      s.icon = LoadItemIcon(s.item.path, icon);
    } else if (!s.item.path.empty()) {
      cold.emplace_back(s.item.path, icon);
    }
  }
  if (!cold.empty()) {
    PreloadIconsNotifyApp(cold);
  }
}

void DockWindow::RelayoutForDpi() {
  LayoutSlots();
}

void DockWindow::SetEdge(const std::wstring& edge) {
  if (edge == L"left" || edge == L"right" || edge == L"bottom") {
    edge_ = edge;
  } else {
    edge_ = L"bottom";
  }
  LayoutSlots();
}

void DockWindow::SetOpacity(float opacity) {
  if (opacity < 0.4f) {
    opacity = 0.4f;
  }
  if (opacity > 1.0f) {
    opacity = 1.0f;
  }
  fill_alpha_ = static_cast<BYTE>(opacity * 255.f + 0.5f);
  Present();
}

void DockWindow::LayoutSlots() {
  if (!hwnd_) {
    return;
  }
  if (slots_.empty()) {
    ApplyVisibility();
    return;
  }
  const Metrics prev = metrics_;
  metrics_ = HostMetrics();
  if (prev.icon != metrics_.icon) {
    RebuildIcons();
  }
  const int n = static_cast<int>(slots_.size());
  // Prefer the monitor hosting the dock (fall back to cursor).
  const RECT work = HostWorkArea();
  const int btn = metrics_.btn;
  const int icon = metrics_.icon;
  const int gap = metrics_.gap;
  const int mx = metrics_.mx;
  const int my = metrics_.my;
  const int icon_pad = (std::max)(0, (btn - icon) / 2);
  const int edge_pad = MulDiv(12, metrics_.dpi, 96);
  int x = 0, y = 0, w = 0, h = 0;
  if (edge_ == L"left" || edge_ == L"right") {
    w = my * 2 + btn;
    h = mx * 2 + n * btn + (std::max)(0, n - 1) * gap;
    y = work.top + ((work.bottom - work.top) - h) / 2;
    x = edge_ == L"left" ? work.left + edge_pad : work.right - w - edge_pad;
    SetOverlayScreenPos(hwnd_, x, y, w, h, SWP_NOZORDER);
    int top = mx;
    for (auto& s : slots_) {
      s.bounds = {my + icon_pad, top + icon_pad, my + icon_pad + icon,
                  top + icon_pad + icon};
      top += btn + gap;
    }
  } else {
    w = mx * 2 + n * btn + (std::max)(0, n - 1) * gap;
    h = my * 2 + btn;
    x = work.left + ((work.right - work.left) - w) / 2;
    y = work.bottom - h - metrics_.clear;
    SetOverlayScreenPos(hwnd_, x, y, w, h, SWP_NOZORDER);
    int left = mx;
    for (auto& s : slots_) {
      s.bounds = {left + icon_pad, my + icon_pad, left + icon_pad + icon,
                  my + icon_pad + icon};
      left += btn + gap;
    }
  }
  Present();
}

namespace {
bool DockRenameEditImeComposing(HWND hwnd) {
  HIMC himc = ImmGetContext(hwnd);
  if (!himc) {
    return false;
  }
  const LONG bytes =
      ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
  ImmReleaseContext(hwnd, himc);
  return bytes > 0;
}

LRESULT CALLBACK DockRenameEditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam) {
  auto* self =
      reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
    if (wparam == VK_RETURN && DockRenameEditImeComposing(hwnd)) {
      WNDPROC old = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"oldproc"));
      if (old) {
        return CallWindowProcW(old, hwnd, msg, wparam, lparam);
      }
      return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    if (self) {
      self->FinishInlineRename(wparam == VK_RETURN);
    }
    return 0;
  }
  if (msg == WM_KILLFOCUS) {
    if (self) {
      self->FinishInlineRename(true);
    }
    return 0;
  }
  WNDPROC old = reinterpret_cast<WNDPROC>(GetPropW(hwnd, L"oldproc"));
  if (old) {
    return CallWindowProcW(old, hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}  // namespace

void DockWindow::BeginInlineRename(int index) {
  if (!hwnd_ || index < 0 || index >= static_cast<int>(slots_.size())) {
    return;
  }
  if (rename_edit_) {
    DestroyWindow(rename_edit_);
    rename_edit_ = nullptr;
  }
  rename_index_ = index;
  const RECT& b = slots_[static_cast<size_t>(index)].bounds;
  POINT origin{b.left, b.bottom + 2};
  ClientToScreen(hwnd_, &origin);
  const int edit_w = (std::max)(96, static_cast<int>(b.right - b.left) + 40);
  const int edit_h = 22;
  rename_edit_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT",
      slots_[static_cast<size_t>(index)].item.name.c_str(),
      WS_POPUP | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, origin.x, origin.y,
      edit_w, edit_h, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!rename_edit_) {
    rename_index_ = -1;
    return;
  }
  rename_proc_ = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(rename_edit_, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(DockRenameEditProc)));
  SetPropW(rename_edit_, L"oldproc", reinterpret_cast<HANDLE>(rename_proc_));
  SetWindowLongPtrW(rename_edit_, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(this));
  const std::wstring& name = slots_[static_cast<size_t>(index)].item.name;
  const std::wstring& path = slots_[static_cast<size_t>(index)].item.path;
  const DWORD attr = GetFileAttributesW(path.c_str());
  const bool is_dir =
      attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
  int sel_end = -1;
  if (!is_dir) {
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
      sel_end = static_cast<int>(dot);
    }
  }
  SendMessageW(rename_edit_, EM_SETSEL, 0, sel_end);
  SetFocus(rename_edit_);
}

void DockWindow::FinishInlineRename(bool commit) {
  if (!rename_edit_) {
    return;
  }
  HWND edit = rename_edit_;
  rename_edit_ = nullptr;
  wchar_t buf[260] = {};
  GetWindowTextW(edit, buf, 260);
  const int index = rename_index_;
  rename_index_ = -1;
  DestroyWindow(edit);
  if (!commit || index < 0 || index >= static_cast<int>(slots_.size())) {
    return;
  }
  const std::wstring old_path = slots_[static_cast<size_t>(index)].item.path;
  const RenamePlan plan = PlanRename(old_path, buf);
  if (!plan.valid) {
    if (buf[0]) {
      ShowToast(plan.error);
    }
    return;
  }
  if (GetFileAttributesW(plan.dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    ShowToast(L"该名称已存在");
    return;
  }
  if (!MoveFileW(old_path.c_str(), plan.dest.c_str())) {
    ShowToast(L"无法重命名，文件可能被占用");
    return;
  }
  SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW, old_path.c_str(),
                 plan.dest.c_str());
  if (on_path_replaced_) {
    on_path_replaced_(old_path, plan.dest);
  }
}

void DockWindow::ShowSlotMenu(int index, POINT screen) {
  if (index < 0 || index >= static_cast<int>(slots_.size())) {
    return;
  }
  const std::wstring path = slots_[static_cast<size_t>(index)].item.path;
  // Explorer/Fences: full IContextMenu; rename is host inline EDIT.
  bool rename_picked = false;
  bool verb_ran = false;
  ShellVerbFn on_verb = [&](const std::wstring& verb) {
    if (verb == L"rename") {
      rename_picked = true;
      return;
    }
    verb_ran = true;
  };
  const int shell_cmd =
      TrackShellContextMenu(hwnd_, screen, {path}, {}, on_verb, nullptr);
  if (shell_cmd == 0) {
    if (rename_picked) {
      BeginInlineRename(index);
      return;
    }
    if (!verb_ran) {
      return;
    }
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES &&
        on_removed_) {
      on_removed_(path);
    }
    return;
  }
  if (shell_cmd < 0) {
    const int cmd = TrackIconShellMenu(hwnd_, screen, false);
    if (cmd == 1) {
      OpenShellPath(path);
    } else if (cmd == 2) {
      RevealShellPath(path);
    } else if (cmd == 4 || cmd == 5) {
      CopyOrCutShellPath(hwnd_, path, cmd == 5);
    } else if (cmd == 6) {
      CopyShellPathText(hwnd_, path);
    } else if (cmd == 7) {
      BeginInlineRename(index);
    } else if (cmd == 8) {
      ShowShellPathProperties(hwnd_, path);
    } else if (cmd == 9) {
      if (RecycleShellPath(hwnd_, path) == IconFsResult::Ok && on_removed_) {
        on_removed_(path);
      }
    }
  }
}

int DockWindow::HitTest(int x, int y) const {
  for (size_t i = 0; i < slots_.size(); ++i) {
    const RECT& b = slots_[i].bounds;
    if (x >= b.left && x < b.right && y >= b.top && y < b.bottom) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

LRESULT CALLBACK DockWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  DockWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<DockWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<DockWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT DockWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_SYSCOMMAND && FilterDesktopChromeSysCommand(wparam)) {
    return 0;
  }
  if (msg == WM_WINDOWPOSCHANGING) {
    FilterDesktopChromeWindowPosChanging(reinterpret_cast<WINDOWPOS*>(lparam));
  }
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      Present();
      return 0;
    }
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme{};
      tme.cbSize = sizeof(tme);
      tme.dwFlags = TME_LEAVE;
      tme.hwndTrack = hwnd_;
      TrackMouseEvent(&tme);
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit != hover_index_) {
        hover_index_ = hit;
        Present();
        UpdateHoverTip(hit);
      } else if (hit >= 0 && tip_) {
        POINT pt{};
        GetCursorPos(&pt);
        SendMessageW(tip_, TTM_TRACKPOSITION, 0,
                     MAKELPARAM(pt.x + 12, pt.y + 16));
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hover_index_ >= 0) {
        hover_index_ = -1;
        Present();
        UpdateHoverTip(-1);
      }
      return 0;
    case WM_LBUTTONUP: {
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0) {
        OpenShellPath(slots_[static_cast<size_t>(hit)].item.path);
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0) {
        POINT pt{};
        GetCursorPos(&pt);
        ShowSlotMenu(hit, pt);
      }
      return 0;
    }
    case WM_DESTROY:
      tip_ = nullptr;
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void DockWindow::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int w = (std::max)(1, static_cast<int>(rc.right));
  const int h = (std::max)(1, static_cast<int>(rc.bottom));
  Gdiplus::Graphics g(hdc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  Gdiplus::GraphicsPath path;
  const int radius = (std::max)(4, metrics_.radius);
  const int btn = (std::max)(24, metrics_.btn);
  const int icon = (std::max)(16, metrics_.icon);
  const int glow = (std::max)(2, metrics_.glow);
  AddRoundRect(&path, w, h, radius);
  // Python default: rgba(22,27,34,0.85) + blue border.
  Gdiplus::SolidBrush fill(Gdiplus::Color(fill_alpha_, 22, 27, 34));
  Gdiplus::Pen edge(Gdiplus::Color(100, 88, 166, 255), 1.0f);
  g.FillPath(&fill, &path);
  g.DrawPath(&edge, &path);
  for (size_t i = 0; i < slots_.size(); ++i) {
    const auto& s = slots_[i];
    if (static_cast<int>(i) == hover_index_) {
      Gdiplus::SolidBrush glow_br(Gdiplus::Color(90, 120, 170, 255));
      g.FillRectangle(&glow_br, s.bounds.left - glow, s.bounds.top - glow, btn,
                      btn);
    }
    if (s.icon) {
      DrawIconEx(hdc, s.bounds.left, s.bounds.top, s.icon, icon, icon, 0,
                 nullptr, DI_NORMAL);
    }
  }
}

void DockWindow::Present() {
  if (!hwnd_) {
    return;
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  const int w = (std::max)(1, static_cast<int>(wr.right - wr.left));
  const int h = (std::max)(1, static_cast<int>(wr.bottom - wr.top));
  if (!EnsureLayerBuffer(w, h)) {
    return;
  }
  HDC mem = layer_dc_;
  void* bits = layer_bits_;
  std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
  Paint(mem);
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return;
  }
  POINT origin{wr.left, wr.top};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateOverlayLayered(hwnd_, screen, &origin, &size, mem, &src, 0, &blend,
                      ULW_ALPHA);
  ReleaseDC(nullptr, screen);
}

void DockWindow::RefreshLayer() {
  if (hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
    Present();
  }
}

void DockWindow::ReleaseLayerBuffer() {
  if (layer_dc_ && layer_old_) {
    SelectObject(layer_dc_, layer_old_);
    layer_old_ = nullptr;
  }
  if (layer_dib_) {
    DeleteObject(layer_dib_);
    layer_dib_ = nullptr;
  }
  if (layer_dc_) {
    DeleteDC(layer_dc_);
    layer_dc_ = nullptr;
  }
  layer_bits_ = nullptr;
  layer_buf_w_ = 0;
  layer_buf_h_ = 0;
}

bool DockWindow::EnsureLayerBuffer(int w, int h) {
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (layer_dc_ && layer_dib_ && layer_bits_ && layer_buf_w_ == w &&
      layer_buf_h_ == h) {
    return true;
  }
  ReleaseLayerBuffer();
  HDC screen = GetDC(nullptr);
  if (!screen) {
    return false;
  }
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
  ReleaseDC(nullptr, screen);
  if (!mem || !dib || !bits) {
    if (dib) {
      DeleteObject(dib);
    }
    if (mem) {
      DeleteDC(mem);
    }
    return false;
  }
  layer_old_ = SelectObject(mem, dib);
  layer_dc_ = mem;
  layer_dib_ = dib;
  layer_bits_ = bits;
  layer_buf_w_ = w;
  layer_buf_h_ = h;
  return true;
}
