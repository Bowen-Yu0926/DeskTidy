#include "page_bar.h"
#include "desktop_dblclick.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "i18n.h"
#include "fence_style.h"

#include <CommCtrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativePageBar";
// Float (mist card) @ 96 DPI.
constexpr int kFloatChipW = 112;
constexpr int kFloatChipH = 32;
constexpr int kFloatSlot = 36;
constexpr int kFloatPadX = 10;
constexpr int kFloatPadY = 10;
constexpr int kFloatCardMargin = 6;
constexpr int kSepGap = 10;  // page block ↔ tool block (float only)
// Tabs (discrete bookmark chips) @ 96 DPI — Python PageIndicatorWidget.
constexpr int kTabsChipW = 92;
constexpr int kTabsChipH = 32;
constexpr int kTabsSlot = 38;
constexpr int kTabsPadX = 14;  // Reserve room for active tab pop-out animation
constexpr int kTabsPadY = 8;
constexpr int kEdgeMargin = 0;
constexpr int kDragThreshold = 6;
// Collapsed: thin card nose; hover / click slides the full float out.
constexpr int kPeekW = 16;
constexpr int kTabsPeekW = 28;       // Tabs mode collapsed peek width @ 96 DPI
constexpr int kTabsPopMargin = 10;   // Active tab extra pop-out distance on expand
constexpr UINT_PTR kTimerExpand = 77;
constexpr UINT_PTR kTimerCollapseDelay = 78;
constexpr UINT kExpandTickMs = 16;
constexpr UINT kCollapseDelayMs = 320;
constexpr float kExpandStep = 0.14f;

int Scaled(int px, float scale) {
  return (std::max)(1, static_cast<int>(px * scale + 0.5f));
}

BYTE MixChannel(BYTE a, BYTE b, float t) {
  const float v = static_cast<float>(a) * (1.f - t) + static_cast<float>(b) * t;
  return static_cast<BYTE>((std::max)(0, (std::min)(255, static_cast<int>(v + 0.5f))));
}

float EaseOutCubic(float t) {
  t = (std::max)(0.f, (std::min)(1.f, t));
  const float u = 1.f - t;
  return 1.f - u * u * u;
}

bool g_registered = false;

void EnsureGdiplus() {
  static ULONG_PTR token = 0;
  if (!token) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
  }
}
}  // namespace

RECT PageBar::HostWorkArea() const {
  if (hwnd_ && IsWindow(hwnd_)) {
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    // Anchor inside the visible peek strip on the left edge so a collapsed
    // bar never crosses into a right-hand secondary monitor.
    POINT c{rc.left + 2, (rc.top + rc.bottom) / 2};
    return WorkAreaAtPoint(c);
  }
  return PrimaryWorkArea();
}

bool PageBar::Create(HINSTANCE instance) {
  if (!g_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PageBar::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    g_registered = true;
  }
  RebuildSlots();
  font_scale_ = 1.f;
  ApplyDesignMetrics(96);
  const int w = CardMargin() + PadX() * 2 + chip_w_;
  const int h = ContentHeight();
  const RECT work = HostWorkArea();
  // Both float and tabs start collapsed at the right edge
  const int x = work.right - VisibleWidth() - kEdgeMargin;
  const int y = work.top + (work.bottom - work.top - h) / 2;
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName,
      L"Pages", WS_POPUP, x, y, w, h, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  // Per-monitor chrome: apply absolute dpi/96 once at create (WM_DPICHANGED
  // alone never fires on a steady HiDPI boot — labels stayed soft/undersized).
  ScaleForDpi(96, SystemDpi());
  return true;
}

void PageBar::Destroy() {
  if (hwnd_) {
    KillTimer(hwnd_, kTimerExpand);
    KillTimer(hwnd_, kTimerCollapseDelay);
  }
  if (tip_) {
    DestroyWindow(tip_);
    tip_ = nullptr;
  }
  delete font_on_;
  delete font_page_;
  delete font_util_;
  delete font_family_;
  delete fmt_;
  font_on_ = font_page_ = font_util_ = nullptr;
  font_family_ = nullptr;
  fmt_ = nullptr;
  fonts_scale_ = -1.f;
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  ReleaseLayerBuffer();
}

int PageBar::CardMargin() const {
  return IsFloatStyle() ? Scaled(kFloatCardMargin, font_scale_) : 0;
}

int PageBar::PadX() const {
  return IsFloatStyle() ? Scaled(kFloatPadX, font_scale_)
                        : Scaled(kTabsPadX, font_scale_);
}

int PageBar::PadY() const {
  return IsFloatStyle() ? Scaled(kFloatPadY, font_scale_)
                        : Scaled(kTabsPadY, font_scale_);
}

void PageBar::ApplyDesignMetrics(int dpi) {
  if (dpi <= 0) {
    dpi = 96;
  }
  font_scale_ = static_cast<float>(dpi) / 96.f;
  if (IsFloatStyle()) {
    chip_w_ = (std::max)(1, MulDiv(kFloatChipW, dpi, 96));
    chip_h_ = (std::max)(1, MulDiv(kFloatChipH, dpi, 96));
    slot_h_ = (std::max)(chip_h_, MulDiv(kFloatSlot, dpi, 96));
  } else {
    chip_w_ = (std::max)(1, MulDiv(kTabsChipW, dpi, 96));
    chip_h_ = (std::max)(1, MulDiv(kTabsChipH, dpi, 96));
    slot_h_ = (std::max)(chip_h_, MulDiv(kTabsSlot, dpi, 96));
  }
}

void PageBar::SetChromeStyle(ChromeStyle style) {
  if (chrome_style_ == style) {
    return;
  }
  chrome_style_ = style;
  if (hwnd_) {
    KillTimer(hwnd_, kTimerExpand);
    KillTimer(hwnd_, kTimerCollapseDelay);
  }
  expand_t_ = 0.f;
  expand_target_ = 0.f;
  ApplyDesignMetrics(SystemDpi());
  Relayout();
}

int PageBar::PeekWidth() const {
  if (IsFloatStyle()) {
    return (std::max)(10, static_cast<int>(kPeekW * font_scale_ + 0.5f));
  }
  return (std::max)(16, static_cast<int>(kTabsPeekW * font_scale_ + 0.5f));
}

int PageBar::VisibleWidth() const {
  int full = CardMargin() + PadX() * 2 + chip_w_;
  if (hwnd_) {
    RECT rc{};
    if (GetWindowRect(hwnd_, &rc) && rc.right > rc.left) {
      full = static_cast<int>(rc.right - rc.left);
    }
  }
  const float e = EaseOutCubic(expand_t_);
  const int peek = PeekWidth();
  full = (std::max)(peek, full);
  return peek + static_cast<int>((full - peek) * e + 0.5f);
}

void PageBar::ApplyEdgeX() {
  if (!hwnd_) {
    return;
  }
  const RECT work = HostWorkArea();
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  int y = rc.top;
  y = (std::max)(static_cast<int>(work.top),
                 (std::min)(y, static_cast<int>(work.bottom) - h));
  // Slide: collapsed leaves only the peek portion on-screen; expanded seats flush.
  const int visible = VisibleWidth();
  const int x = static_cast<int>(work.right) - visible - kEdgeMargin;
  SetOverlayScreenPos(hwnd_, x, y, w, h, SWP_NOZORDER);
}

void PageBar::SetExpandTarget(float target) {
  target = (std::max)(0.f, (std::min)(1.f, target));
  expand_target_ = target;
  if (!hwnd_) {
    expand_t_ = target;
    return;
  }
  if (target >= 0.99f) {
    KillTimer(hwnd_, kTimerCollapseDelay);
  }
  if (std::fabs(expand_t_ - expand_target_) < 0.001f) {
    expand_t_ = expand_target_;
    ApplyEdgeX();
    return;
  }
  SetTimer(hwnd_, kTimerExpand, kExpandTickMs, nullptr);
}

void PageBar::TickExpandAnim() {
  if (!hwnd_) {
    return;
  }
  const float d = expand_target_ - expand_t_;
  if (std::fabs(d) < 0.008f) {
    expand_t_ = expand_target_;
    KillTimer(hwnd_, kTimerExpand);
    ApplyEdgeX();
    Paint(nullptr);
    return;
  }
  // Smooth spring-like easing: responsive launch with gentle deceleration
  float step = d * 0.28f;
  const float min_step = 0.035f;
  if (std::fabs(step) < min_step) {
    step = (d > 0.f ? min_step : -min_step);
  }
  expand_t_ += step;
  if ((d > 0.f && expand_t_ > expand_target_) ||
      (d < 0.f && expand_t_ < expand_target_)) {
    expand_t_ = expand_target_;
  }
  ApplyEdgeX();
  Paint(nullptr);
}

void PageBar::EnsureTooltip() {
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
}

void PageBar::UpdateHoverTip(int index) {
  // Expanded chips already show the label — a duplicate tip over the text
  // (see 文档) is noise. Tips only while tucked, so the peek still names pages.
  if (expand_t_ > 0.45f || index < 0 ||
      index >= static_cast<int>(slots_.size())) {
    if (tip_) {
      TOOLINFOW ti{};
      ti.cbSize = sizeof(ti);
      ti.hwnd = hwnd_;
      ti.uId = 1;
      SendMessageW(tip_, TTM_TRACKACTIVATE, FALSE,
                   reinterpret_cast<LPARAM>(&ti));
    }
    return;
  }
  EnsureTooltip();
  if (!tip_) {
    return;
  }
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.hwnd = hwnd_;
  ti.uId = 1;
  const Slot& s = slots_[static_cast<size_t>(index)];
  std::wstring text = s.label;
  if (s.kind == SlotKind::Tool) {
    if (s.tool_id == L"record") {
      text = Tr("tool_record");
    } else if (s.tool_id == L"note") {
      text = L"记事本";
    } else if (s.tool_id == L"minutes") {
      text = Tr("tool_minutes");
    } else if (s.tool_id == L"calculator") {
      text = Tr("tool_calculator");
    } else if (s.tool_id == L"todo") {
      text = Tr("tool_todo");
    }
  } else if (s.kind == SlotKind::Folder && !s.folder_path.empty()) {
    text = s.folder_path;
  }
  ti.lpszText = const_cast<wchar_t*>(text.c_str());
  SendMessageW(tip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
  POINT pt{};
  GetCursorPos(&pt);
  SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x - 12, pt.y + 16));
  SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
}

void PageBar::Show() {
  if (!hwnd_) {
    return;
  }
  // Page clicks call Show via ApplyPageVisibility — do not Relayout /
  // PlaceOverlay(HWND_BOTTOM) every time (that flashes the whole desktop).
  const bool need_map = !IsWindowVisible(hwnd_);
  if (need_map) {
    Relayout();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  if (!IsAttachedToDesktop(hwnd_)) {
    AttachToDesktopLayer();
  }
  if (need_map) {
    Paint(nullptr);
  }
}

void PageBar::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void PageBar::RefreshLayer() {
  // Re-upload visible page-bar pixels (hybrid-GPU / ULW-loss recovery).
  if (hwnd_ && IsWindowVisible(hwnd_)) {
    Paint(nullptr);
  }
}

void PageBar::SetPage(int page) {
  if (page_ == page) {
    return;
  }
  page_ = page;
  if (hwnd_) {
    Paint(nullptr);
  }
}

void PageBar::ApplyStyle(COLORREF background, COLORREF accent,
                          COLORREF title) {
  bg_ = background;
  accent_ = accent;
  title_ = title;
  if (hwnd_) {
    Paint(nullptr);
  }
}

int PageBar::FirstUtilityIndex() const {
  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    if (slots_[static_cast<size_t>(i)].kind != SlotKind::Page) {
      return i;
    }
  }
  return -1;
}

int PageBar::SeparatorGap() const {
  if (!IsFloatStyle()) {
    return 0;
  }
  const int util = FirstUtilityIndex();
  // Only when both a page block and a tool/folder block are present.
  if (util <= 0) {
    return 0;
  }
  return Scaled(kSepGap, font_scale_);
}

int PageBar::SlotTop(int index) const {
  const int pad_y = PadY();
  const int util = FirstUtilityIndex();
  const int gap = SeparatorGap();
  int y = pad_y + index * slot_h_;
  if (util > 0 && index >= util) {
    y += gap;
  }
  return y;
}

int PageBar::ContentHeight() const {
  const int n = static_cast<int>(slots_.size());
  const int pad_y = PadY();
  if (n <= 0) {
    return pad_y * 2 + slot_h_;
  }
  return pad_y * 2 + n * slot_h_ + SeparatorGap();
}

void PageBar::RebuildSlots() {
  slots_.clear();
  if (pages_visible_) {
    for (int i = 0; i < page_count_; ++i) {
      Slot s;
      s.kind = SlotKind::Page;
      s.page_index = i;
      s.label = PageLabel(i);
      slots_.push_back(std::move(s));
    }
    // Python: folder peeks between pages and tools (edge bar only).
    for (const auto& f : folders_) {
      if (f.path.empty()) {
        continue;
      }
      Slot s;
      s.kind = SlotKind::Folder;
      s.folder_path = f.path;
      s.label = f.name.empty() ? L"文件夹" : f.name;
      slots_.push_back(std::move(s));
    }
  }
  // Python: record / minutes / calculator / todo (+ optional note).
  auto add_tool = [&](bool on, const wchar_t* id, const wchar_t* label) {
    if (!on) {
      return;
    }
    Slot s;
    s.kind = SlotKind::Tool;
    s.tool_id = id;
    s.label = label;
    slots_.push_back(std::move(s));
  };
  add_tool(tool_record_, L"record", Tr("tool_record").c_str());
  add_tool(tool_note_, L"note", L"记事本");
  add_tool(tool_minutes_, L"minutes", Tr("tool_minutes").c_str());
  add_tool(tool_calculator_, L"calculator", Tr("tool_calculator").c_str());
  add_tool(tool_todo_, L"todo", Tr("tool_todo").c_str());
}

void PageBar::DockToRightEdge(int preferred_y) {
  if (!hwnd_) {
    return;
  }
  const RECT work = HostWorkArea();
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  int y = preferred_y;
  if (y < 0) {
    y = work.top + (work.bottom - work.top - h) / 2;
  }
  y = (std::max)(static_cast<int>(work.top),
                 (std::min)(y, static_cast<int>(work.bottom) - h));
  const int visible = VisibleWidth();
  const int x = static_cast<int>(work.right) - visible - kEdgeMargin;
  SetOverlayScreenPos(hwnd_, x, y, w, h, SWP_NOZORDER);
  anchor_y_ = y - static_cast<int>(work.top);
  if (on_moved_) {
    on_moved_();
  }
}

void PageBar::Relayout() {
  if (!hwnd_) {
    return;
  }
  RebuildSlots();
  const int w = CardMargin() + PadX() * 2 + chip_w_;
  const int h = ContentHeight();
  const RECT work = HostWorkArea();
  int y = static_cast<int>(work.top) +
          (static_cast<int>(work.bottom - work.top) - h) / 2;
  if (anchor_y_ >= 0) {
    y = static_cast<int>(work.top) + anchor_y_;
  }
  y = (std::max)(static_cast<int>(work.top),
                 (std::min)(y, static_cast<int>(work.bottom) - h));
  const int visible = VisibleWidth();
  const int x = static_cast<int>(work.right) - visible - kEdgeMargin;
  SetOverlayScreenPos(hwnd_, x, y, w, h, SWP_NOZORDER);
  Paint(nullptr);
}

void PageBar::SetPageCount(int count) {
  if (count < 1) {
    count = 1;
  }
  // Python semantics: the caller prompts on overflow (kMaxPages); the bar only
  // reflects what it is given, never silently truncating the model.
  page_count_ = count;
  while (static_cast<int>(page_names_.size()) < page_count_) {
    page_names_.push_back(L"页" + std::to_wstring(page_names_.size() + 1));
  }
  if (static_cast<int>(page_names_.size()) > page_count_) {
    page_names_.resize(static_cast<size_t>(page_count_));
  }
  Relayout();
}

void PageBar::SetPageNames(const std::vector<std::wstring>& names) {
  page_names_ = names;
  if (!names.empty()) {
    page_count_ = static_cast<int>(names.size());
  }
  Relayout();
}

void PageBar::SetPageLocks(const std::vector<bool>& locks) {
  page_locked_ = locks;
  if (static_cast<int>(page_locked_.size()) < page_count_) {
    page_locked_.resize(static_cast<size_t>(page_count_), false);
  }
  Relayout();
}

bool PageBar::PageLockedAt(int index) const {
  if (index < 0) {
    return false;
  }
  // U7 fallback: pages 0/1 are the system pages (Python SYSTEM_WORK/DOCS page
  // ids) and stay locked unless explicitly unlocked.
  if (index >= static_cast<int>(page_locked_.size())) {
    return index == 0 || index == 1;
  }
  return page_locked_[static_cast<size_t>(index)];
}

void PageBar::SetPagesVisible(bool on) {
  pages_visible_ = on;
  Relayout();
}

void PageBar::SetToolFlags(bool record, bool minutes, bool calculator,
                           bool todo, bool note) {
  tool_record_ = record;
  tool_minutes_ = minutes;
  tool_calculator_ = calculator;
  tool_todo_ = todo;
  tool_note_ = note;
  Relayout();
}

void PageBar::SetFolders(const std::vector<FolderChip>& folders) {
  folders_ = folders;
  Relayout();
}

void PageBar::SetAnchorY(int y) {
  anchor_y_ = y;
  Relayout();
}

void PageBar::ScaleForDpi(int old_dpi, int new_dpi) {
  if (old_dpi <= 0) {
    old_dpi = 96;
  }
  if (new_dpi <= 0) {
    new_dpi = 96;
  }
  const float next_scale = static_cast<float>(new_dpi) / 96.f;
  if (old_dpi == new_dpi && std::fabs(font_scale_ - next_scale) < 0.001f) {
    return;
  }
  if (anchor_y_ >= 0 && old_dpi != new_dpi) {
    anchor_y_ = MulDiv(anchor_y_, new_dpi, old_dpi);
  }
  ApplyDesignMetrics(new_dpi);
  Relayout();
}

std::wstring PageBar::PageLabel(int index) const {
  if (index >= 0 && index < static_cast<int>(page_names_.size())) {
    const std::wstring& name = page_names_[static_cast<size_t>(index)];
    if (!name.empty()) {
      return name;  // Elide in Paint to match Python fontMetrics.elidedText.
    }
  }
  if (index == 0) {
    return L"工作";
  }
  if (index == 1) {
    return L"文档";
  }
  return L"页" + std::to_wstring(index + 1);
}

void PageBar::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  // Keepalive Raise is SWP_NOREDRAW and skips no-op restacks.
  if (!AttachOverlayToDesktop(hwnd_, true)) {
    ForceAttachOverlayToDesktop(hwnd_, true);
  }
  RaiseOverlayInDesktopBand(hwnd_);
}

int PageBar::HitTest(int x, int y) const {
  const int full_w = CardMargin() + PadX() * 2 + chip_w_;
  if (x < 0 || x >= full_w) {
    return -1;
  }
  for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
    const int top = SlotTop(i) + (slot_h_ - chip_h_) / 2;
    if (y >= top && y < top + chip_h_) {
      return i;
    }
  }
  return -1;
}

void PageBar::ActivateSlot(int index) {
  if (index < 0 || index >= static_cast<int>(slots_.size())) {
    return;
  }
  const Slot& s = slots_[static_cast<size_t>(index)];
  if (s.kind == SlotKind::Page) {
    if (on_page_) {
      on_page_(s.page_index);
    }
    return;
  }
  if (s.kind == SlotKind::Folder) {
    if (on_folder_) {
      on_folder_(s.folder_path);
    }
    return;
  }
  if (on_tool_) {
    on_tool_(s.tool_id);
  }
}

// R5: page-bar right-click menu. Pages 0/1 are the locked system pages
// (Python is_locked_page_id: page_id in {SYSTEM_WORK_PAGE_ID=0,
// SYSTEM_DOCS_PAGE_ID=1}) — delete/rename are disabled for them.
void PageBar::ShowPageContextMenu(int page_index) {
  if (page_index < 0) {
    return;
  }
  const bool locked = PageLockedAt(page_index);
  enum { CmdCreateFence = 1, CmdRename, CmdDelete, CmdNote, CmdToggleLock,
         CmdMoveBase = 100 };
  HMENU menu = CreatePopupMenu();
  // P0-11: locked system pages still allow create / rename / move — only the
  // page delete is gated (Python is_locked_page_id).
  AppendMenuW(menu, MF_STRING, CmdCreateFence, L"新建分区");
  AppendMenuW(menu, MF_STRING, CmdRename, L"重命名页面");
  // P1-16: "移动分区到该页" — list every fence, move the chosen one here.
  HMENU move_menu = CreatePopupMenu();
  int move_count = 0;
  for (size_t i = 0; i < fence_titles_.size(); ++i) {
    std::wstring label = fence_titles_[i];
    if (label.empty()) {
      label = L"分区 " + std::to_wstring(i + 1);
    }
    AppendMenuW(move_menu, MF_STRING, CmdMoveBase + static_cast<int>(i),
                label.c_str());
    ++move_count;
  }
  if (move_count > 0) {
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(move_menu),
                L"移动分区到该页");
  } else {
    DestroyMenu(move_menu);
  }
  AppendMenuW(menu, MF_STRING | (locked ? MF_GRAYED : 0), CmdDelete,
              L"删除页面");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, CmdToggleLock, locked ? L"解锁页面" : L"锁定页面");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, CmdNote, L"打开记事本");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  HMENU color_menu = CreatePopupMenu();
  enum { CmdColorBase = 300 };
  const auto all_styles = AllFenceStyles();
  for (size_t i = 0; i < all_styles.size(); ++i) {
    AppendMenuW(color_menu, MF_STRING, static_cast<UINT_PTR>(CmdColorBase + i),
                all_styles[i]->name);
  }
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(color_menu), L"页签颜色");
  POINT pt{};
  GetCursorPos(&pt);
  KillTimer(hwnd_, kTimerCollapseDelay);
  // Desktop-band chrome: no SetForegroundWindow — keeps page bar under apps.
  int cmd = 0;
  {
    ScopedDesktopPopupMenuGuard menu_guard(hwnd_);
    cmd = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
        hwnd_, nullptr);
  }
  DestroyMenu(menu);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  if (hwnd_ && IsWindow(hwnd_)) {
    POINT cur{};
    RECT wrc{};
    if (GetCursorPos(&cur) && GetWindowRect(hwnd_, &wrc) &&
        !PtInRect(&wrc, cur)) {
      SetTimer(hwnd_, kTimerCollapseDelay, kCollapseDelayMs, nullptr);
    }
  }
  if (cmd >= CmdColorBase && cmd < CmdColorBase + static_cast<int>(all_styles.size())) {
    const size_t idx = static_cast<size_t>(cmd - CmdColorBase);
    if (on_set_style_) {
      on_set_style_(all_styles[idx]->id);
    }
    return;
  }
  if (cmd >= CmdMoveBase) {
    const int fence = cmd - CmdMoveBase;
    if (on_move_fence_to_page_ && fence >= 0 &&
        fence < static_cast<int>(fence_titles_.size())) {
      on_move_fence_to_page_(page_index, fence);
    }
    return;
  }
  if (cmd == CmdCreateFence) {
    if (on_create_fence_) {
      on_create_fence_(page_index);
    }
  } else if (cmd == CmdRename) {
    if (on_rename_page_at_) {
      on_rename_page_at_(page_index);
    } else if (on_rename_page_) {
      on_rename_page_();
    }
  } else if (cmd == CmdDelete && !locked) {
    if (on_delete_page_) {
      on_delete_page_(page_index);
    }
  } else if (cmd == CmdToggleLock) {
    if (on_toggle_page_lock_) {
      on_toggle_page_lock_(page_index);
    }
  } else if (cmd == CmdNote) {
    if (on_tool_) {
      on_tool_(L"note");
    }
  }
}

LRESULT CALLBACK PageBar::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam) {
  PageBar* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<PageBar*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<PageBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT PageBar::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_SYSCOMMAND && FilterDesktopChromeSysCommand(wparam)) {
    return 0;
  }
  if (msg == WM_WINDOWPOSCHANGING) {
    FilterDesktopChromeWindowPosChanging(reinterpret_cast<WINDOWPOS*>(lparam));
  }
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      Paint(nullptr);
      return 0;
    }
    case WM_TIMER:
      if (wparam == kTimerExpand) {
        TickExpandAnim();
        return 0;
      }
      if (wparam == kTimerCollapseDelay) {
        KillTimer(hwnd_, kTimerCollapseDelay);
        if (!dragging_) {
          SetExpandTarget(0.f);
        }
        return 0;
      }
      break;
    case WM_LBUTTONDOWN: {
      press_pt_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      POINT screen = press_pt_;
      ClientToScreen(hwnd_, &screen);
      RECT rc{};
      GetWindowRect(hwnd_, &rc);
      drag_grab_.x = screen.x - rc.left;
      drag_grab_.y = screen.y - rc.top;
      dragging_ = true;
      drag_moved_ = false;
      SetExpandTarget(1.f);
      SetCapture(hwnd_);
      return 0;
    }
    case WM_MOUSEMOVE: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      if (!(wparam & MK_LBUTTON) || !dragging_) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd_;
        TrackMouseEvent(&tme);
        KillTimer(hwnd_, kTimerCollapseDelay);
        SetExpandTarget(1.f);
        const int hit = HitTest(x, y);
        if (hit != hover_index_) {
          hover_index_ = hit;
          Paint(nullptr);
          UpdateHoverTip(hit);
        } else if (hit >= 0 && tip_ && expand_t_ <= 0.45f) {
          POINT pt{};
          GetCursorPos(&pt);
          SendMessageW(tip_, TTM_TRACKPOSITION, 0,
                       MAKELPARAM(pt.x - 12, pt.y + 16));
        }
        return 0;
      }
      if (!drag_moved_ &&
          abs(x - press_pt_.x) < kDragThreshold &&
          abs(y - press_pt_.y) < kDragThreshold) {
        return 0;
      }
      drag_moved_ = true;
      POINT screen{};
      GetCursorPos(&screen);
      // Follow the monitor under the pointer while dragging (multi-display).
      const RECT work = WorkAreaAtPoint(screen);
      RECT rc{};
      GetWindowRect(hwnd_, &rc);
      const int h = rc.bottom - rc.top;
      int ny = screen.y - drag_grab_.y;
      ny = (std::max)(static_cast<int>(work.top),
                     (std::min)(ny, static_cast<int>(work.bottom) - h));
      // Keep X locked to the (expanded) right-edge seat while dragging.
      const int nx =
          static_cast<int>(work.right) - VisibleWidth() - kEdgeMargin;
      MoveOverlayScreen(hwnd_, nx, ny);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hover_index_ >= 0) {
        hover_index_ = -1;
        Paint(nullptr);
        UpdateHoverTip(-1);
      }
      if (!dragging_) {
        SetTimer(hwnd_, kTimerCollapseDelay, kCollapseDelayMs, nullptr);
      }
      return 0;
    case WM_LBUTTONUP: {
      const int x = GET_X_LPARAM(lparam);
      const int y = GET_Y_LPARAM(lparam);
      ReleaseCapture();
      if (dragging_ && drag_moved_) {
        RECT rc{};
        GetWindowRect(hwnd_, &rc);
        DockToRightEdge(rc.top);
        dragging_ = false;
        drag_moved_ = false;
        AttachToDesktopLayer();
        SetTimer(hwnd_, kTimerCollapseDelay, kCollapseDelayMs, nullptr);
        return 0;
      }
      dragging_ = false;
      drag_moved_ = false;
      const int hit = HitTest(x, y);
      ActivateSlot(hit);
      return 0;
    }
    case WM_RBUTTONUP: {
      SetExpandTarget(1.f);
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0 && hit < static_cast<int>(slots_.size()) &&
          slots_[static_cast<size_t>(hit)].kind == SlotKind::Page) {
        ShowPageContextMenu(slots_[static_cast<size_t>(hit)].page_index);
      }
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (reinterpret_cast<HWND>(lparam) != hwnd_ && dragging_) {
        dragging_ = false;
        drag_moved_ = false;
        RECT rc{};
        GetWindowRect(hwnd_, &rc);
        DockToRightEdge(rc.top);
        AttachToDesktopLayer();
        SetTimer(hwnd_, kTimerCollapseDelay, kCollapseDelayMs, nullptr);
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd_, kTimerExpand);
      KillTimer(hwnd_, kTimerCollapseDelay);
      tip_ = nullptr;
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void PageBar::EnsureFonts() {
  // Fonts depend only on font_scale_ (not the per-frame expand ease), so keep
  // them across animation frames and rebuild only when the scale changes.
  if (font_on_ && std::fabs(fonts_scale_ - font_scale_) < 0.001f) {
    return;
  }
  delete font_on_;
  delete font_page_;
  delete font_util_;
  delete font_family_;
  font_on_ = font_page_ = font_util_ = nullptr;
  font_family_ = nullptr;

  Gdiplus::FontFamily family(L"Microsoft YaHei UI");
  const Gdiplus::FontFamily* face =
      family.IsAvailable() ? &family : Gdiplus::FontFamily::GenericSansSerif();
  font_family_ = face->Clone();
  const float em_on = std::round(13.f * font_scale_);
  const float em_page = std::round(12.5f * font_scale_);
  const float em_util = std::round(12.f * font_scale_);
  font_on_ = new Gdiplus::Font(font_family_, em_on, Gdiplus::FontStyleBold,
                               Gdiplus::UnitPixel);
  font_page_ = new Gdiplus::Font(font_family_, em_page, Gdiplus::FontStyleRegular,
                                 Gdiplus::UnitPixel);
  font_util_ = new Gdiplus::Font(font_family_, em_util,
                                 Gdiplus::FontStyleRegular,
                                 Gdiplus::UnitPixel);
  fonts_scale_ = font_scale_;
}

void PageBar::Paint(HDC) {
  if (!hwnd_) {
    return;
  }
  if (slots_.empty()) {
    RebuildSlots();
  }
  RECT wr{};
  GetWindowRect(hwnd_, &wr);
  const int w = (std::max)(1, static_cast<int>(wr.right - wr.left));
  const int h = (std::max)(1, static_cast<int>(wr.bottom - wr.top));
  EnsureGdiplus();
  if (!EnsureLayerBuffer(w, h)) {
    return;
  }
  HDC mem = layer_dc_;
  void* bits = layer_bits_;
  std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);

  {
    Gdiplus::Graphics g(mem);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeNone);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetTextContrast(4);

    EnsureFonts();
    if (!fmt_) {
      fmt_ = new Gdiplus::StringFormat();
      // Slightly centered in the chip (WeChat/dock feel); lock/rail keep side insets.
      fmt_->SetAlignment(Gdiplus::StringAlignmentCenter);
      fmt_->SetLineAlignment(Gdiplus::StringAlignmentCenter);
      fmt_->SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
      fmt_->SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    }

    const BYTE ar = GetRValue(accent_);
    const BYTE ag = GetGValue(accent_);
    const BYTE ab = GetBValue(accent_);
    const BYTE br = GetRValue(bg_);
    const BYTE bg = GetGValue(bg_);
    const BYTE bb = GetBValue(bg_);
    const bool is_light = (br * 299 + bg * 587 + bb * 114) / 1000 > 180;
    // Clip off-workarea overhang to transparent (alpha=0) so collapsed state
    // never intrudes into a right-docked taskbar or right secondary monitor.
    const int clip_w = (std::min)(w, VisibleWidth() + kEdgeMargin);
    g.SetClip(Gdiplus::Rect(0, 0, (std::max)(1, clip_w), h));
    const int pad_x = PadX();
    const int card_m = CardMargin();
    const bool float_style = IsFloatStyle();

    auto add_round_rect = [](Gdiplus::GraphicsPath* p, float x, float y,
                             float rw, float rh, float rad) {
      if (!p) {
        return;
      }
      p->Reset();
      const float d = rad * 2.f;
      p->AddArc(x, y, d, d, 180, 90);
      p->AddArc(x + rw - d, y, d, d, 270, 90);
      p->AddArc(x + rw - d, y + rh - d, d, d, 0, 90);
      p->AddArc(x, y + rh - d, d, d, 90, 90);
      p->CloseFigure();
    };

    if (float_style) {
      const float card_x = static_cast<float>(card_m);
      const float card_y = 1.f;
      const float card_w = static_cast<float>(w - card_m - 1);
      const float card_h = static_cast<float>((std::max)(2, h - 2));
      const float card_rad =
          (std::min)(14.f * font_scale_, (std::min)(card_w, card_h) * 0.22f);

      Gdiplus::GraphicsPath shade;
      for (int s = 3; s >= 1; --s) {
        const float o = static_cast<float>(s) * 1.4f;
        add_round_rect(&shade, card_x - o * 0.2f, card_y + o * 0.55f,
                       card_w + o * 0.35f, card_h + o * 0.25f,
                       card_rad + o * 0.15f);
        Gdiplus::SolidBrush brush(
            Gdiplus::Color(static_cast<BYTE>(10 + s * 8), 15, 23, 42));
        g.FillPath(&brush, &shade);
      }

      Gdiplus::GraphicsPath card;
      add_round_rect(&card, card_x, card_y, card_w, card_h, card_rad);
      auto clamp_b = [](int v) -> BYTE { return static_cast<BYTE>((std::max)(0, (std::min)(255, v))); };
      Gdiplus::Color card_top = is_light
          ? Gdiplus::Color(235, 246, 248, 252)
          : Gdiplus::Color(230, clamp_b(br + 12), clamp_b(bg + 12), clamp_b(bb + 15));
      Gdiplus::Color card_bot = is_light
          ? Gdiplus::Color(245, 230, 236, 246)
          : Gdiplus::Color(245, clamp_b(br - 4), clamp_b(bg - 4), clamp_b(bb - 4));
      Gdiplus::LinearGradientBrush fill(
          Gdiplus::PointF(card_x, card_y),
          Gdiplus::PointF(card_x, card_y + card_h),
          card_top, card_bot);
      g.FillPath(&fill, &card);
      Gdiplus::Pen rim(Gdiplus::Color(is_light ? 120 : 85, ar, ag, ab), 1.f);
      g.DrawPath(&rim, &card);
      if (expand_t_ < 0.55f) {
        const float nose_alpha = 140.f + expand_t_ * 100.f;
        Gdiplus::SolidBrush glow(Gdiplus::Color(
            static_cast<BYTE>(nose_alpha * 0.4f), ar, ag, ab));
        g.FillRectangle(&glow,
                        Gdiplus::RectF(card_x, card_y + card_rad - 2.f,
                                       5.f * font_scale_,
                                       card_h - card_rad * 2.f + 4.f));
        Gdiplus::SolidBrush nose(Gdiplus::Color(
            static_cast<BYTE>(nose_alpha), ar, ag, ab));
        g.FillRectangle(&nose,
                        Gdiplus::RectF(card_x + 1.f, card_y + card_rad,
                                       2.5f * font_scale_,
                                       card_h - card_rad * 2.f));
      }

      const int first_util = FirstUtilityIndex();
      if (first_util > 0 && expand_t_ > 0.4f) {
        const float dy =
            static_cast<float>(SlotTop(first_util)) - SeparatorGap() * 0.5f;
        Gdiplus::Pen sep(Gdiplus::Color(55, 148, 163, 184), 1.f);
        g.DrawLine(&sep, card_x + pad_x * 0.6f, dy,
                   card_x + card_w - pad_x * 0.6f, dy);
      }
    }

    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
      const Slot& s = slots_[static_cast<size_t>(i)];
      const bool on = s.kind == SlotKind::Page && s.page_index == page_;
      const bool hover = (i == hover_index_);
      const bool utility =
          s.kind == SlotKind::Folder || s.kind == SlotKind::Tool;
      const bool locked =
          s.kind == SlotKind::Page && PageLockedAt(s.page_index);

      const float cx = static_cast<float>(card_m + pad_x);
      const float cy = static_cast<float>(
          SlotTop(i) + (slot_h_ - chip_h_) / 2);
      const float bw = static_cast<float>(chip_w_);
      const float bh = static_cast<float>(chip_h_);

      if (float_style) {
        const float rad = (std::min)(9.f * font_scale_, bh * 0.42f);
        Gdiplus::Color fill_c;
        if (on) {
          fill_c = is_light
                       ? Gdiplus::Color(255, MixChannel(232, ar, 0.22f),
                                        MixChannel(236, ag, 0.22f),
                                        MixChannel(245, ab, 0.22f))
                       : Gdiplus::Color(235, MixChannel(30, ar, 0.28f),
                                        MixChannel(41, ag, 0.28f),
                                        MixChannel(59, ab, 0.28f));
        } else if (hover) {
          fill_c = is_light
                       ? Gdiplus::Color(255, MixChannel(241, ar, 0.10f),
                                        MixChannel(244, ag, 0.10f),
                                        MixChannel(250, ab, 0.10f))
                       : Gdiplus::Color(210, MixChannel(30, ar, 0.14f),
                                        MixChannel(41, ag, 0.14f),
                                        MixChannel(59, ab, 0.14f));
        } else {
          fill_c = Gdiplus::Color(0, 0, 0, 0);
        }
        Gdiplus::GraphicsPath row;
        add_round_rect(&row, cx, cy, bw, bh, rad);
        if (fill_c.GetA() > 0) {
          Gdiplus::SolidBrush body(fill_c);
          g.FillPath(&body, &row);
        }
        if (on || hover) {
          Gdiplus::Pen rim(Gdiplus::Color(on ? 110 : 55, ar, ag, ab), 1.f);
          g.DrawPath(&rim, &row);
        }
        if (on) {
          const float rail_w = (std::max)(2.f, 3.f * font_scale_);
          Gdiplus::SolidBrush rail(Gdiplus::Color(230, ar, ag, ab));
          g.FillRectangle(&rail,
                          Gdiplus::RectF(cx + 2.f * font_scale_,
                                         cy + 5.f * font_scale_, rail_w,
                                         bh - 10.f * font_scale_));
        }
        const float rail_reserve =
            on ? std::round(10.f * font_scale_) : 0.f;
        const float lock_reserve =
            locked ? std::round(14.f * font_scale_) : 0.f;
        const float side = std::round(8.f * font_scale_);
        const float text_x = cx + side + rail_reserve;
        const float text_w =
            bw - side * 2.f - rail_reserve - lock_reserve;
        const Gdiplus::Color ink_c =
            is_light ? (utility ? Gdiplus::Color(220, 71, 85, 105)
                                : Gdiplus::Color(255, 26, 35, 50))
                     : (utility ? Gdiplus::Color(210, 148, 163, 184)
                                : Gdiplus::Color(245, 241, 245, 249));
        Gdiplus::SolidBrush ink(ink_c);
        const Gdiplus::Font& font =
            on ? *font_on_ : (utility ? *font_util_ : *font_page_);
        if (expand_t_ > 0.35f && text_w > 8.f) {
          g.DrawString(s.label.c_str(), -1, &font,
                       Gdiplus::RectF(text_x, cy, text_w, bh), fmt_, &ink);
        }
        if (locked && expand_t_ > 0.45f) {
          const float lw = std::round(5.5f * font_scale_);
          const float lx = cx + bw - std::round(12.f * font_scale_);
          const float ly = cy + std::round((bh - 8.5f * font_scale_) * 0.5f);
          const Gdiplus::Color lock_c =
              is_light ? Gdiplus::Color(180, 71, 85, 105)
                       : Gdiplus::Color(190, 148, 163, 184);
          Gdiplus::Pen shackle(lock_c, (std::max)(1.f, font_scale_));
          g.DrawArc(&shackle,
                    Gdiplus::RectF(lx + font_scale_, ly, lw - 2.f * font_scale_,
                                   3.2f * font_scale_),
                    180.f, 180.f);
          Gdiplus::SolidBrush lock_body(lock_c);
          g.FillRectangle(&lock_body,
                          Gdiplus::RectF(lx, ly + 2.6f * font_scale_, lw,
                                         4.8f * font_scale_));
        }
      } else {
        // Discrete bookmark tabs (flush right edge) — Clean Frosted Pill Tabs
        const float pop_max = static_cast<float>(Scaled(kTabsPopMargin, font_scale_));
        const float e = EaseOutCubic(expand_t_);
        const float pop = on ? (pop_max * (0.35f + 0.65f * e))
                             : (hover ? (std::min)(pop_max * 0.40f, 4.5f * font_scale_ * e) : 0.f);
        const float tab_cx = static_cast<float>(card_m + pad_x) - pop;
        const float tab_cy = static_cast<float>(SlotTop(i) + (slot_h_ - chip_h_) / 2);
        const float tab_bw = static_cast<float>(chip_w_) + pop;
        const float tab_bh = static_cast<float>(chip_h_);
        const float rad = (std::min)(10.f * font_scale_, tab_bh * 0.46f);

        auto make_tab_path = [](Gdiplus::GraphicsPath* p, float x, float y,
                                float rw, float rh, float r, bool close_right) {
          p->Reset();
          const float d = r * 2.f;
          p->AddLine(x + rw, y, x + r, y);
          p->AddArc(x, y, d, d, 270.f, -90.f);
          p->AddLine(x, y + r, x, y + rh - r);
          p->AddArc(x, y + rh - d, d, d, 180.f, -90.f);
          p->AddLine(x + r, y + rh, x + rw, y + rh);
          if (close_right) {
            p->CloseFigure();
          }
        };

        // Multi-layer soft ambient drop shadow (eliminates hard dark rim)
        for (int layer = 2; layer >= 1; --layer) {
          const float ox = static_cast<float>(layer) * 0.9f * font_scale_;
          const float oy = static_cast<float>(layer) * 1.1f * font_scale_;
          Gdiplus::GraphicsPath shadow;
          make_tab_path(&shadow, tab_cx - ox * 0.6f, tab_cy + oy,
                        tab_bw + ox * 0.6f, tab_bh, rad + ox * 0.2f, true);
          const BYTE sa = static_cast<BYTE>((on ? 14 : 9) * (3 - layer));
          Gdiplus::SolidBrush shade(Gdiplus::Color(sa, 15, 23, 42));
          g.FillPath(&shade, &shadow);
        }

        Gdiplus::GraphicsPath path;
        make_tab_path(&path, tab_cx, tab_cy, tab_bw, tab_bh, rad, true);

        Gdiplus::Color top;
        Gdiplus::Color bottom;
        auto clamp_b = [](int v) -> BYTE {
          return static_cast<BYTE>((std::max)(0, (std::min)(255, v)));
        };
        if (is_light) {
          if (on) {
            top = Gdiplus::Color(252, 255, 255, 255);
            bottom = Gdiplus::Color(250, MixChannel(248, ar, 0.08f),
                                    MixChannel(250, ag, 0.08f),
                                    MixChannel(254, ab, 0.10f));
          } else if (hover) {
            top = Gdiplus::Color(242, 255, 255, 255);
            bottom = Gdiplus::Color(238, 246, 249, 254);
          } else {
            top = Gdiplus::Color(218, 250, 251, 254);
            bottom = Gdiplus::Color(212, 241, 245, 250);
          }
        } else {
          if (on) {
            top = Gdiplus::Color(246, MixChannel(clamp_b(br + 26), ar, 0.18f),
                                 MixChannel(clamp_b(bg + 26), ag, 0.18f),
                                 MixChannel(clamp_b(bb + 30), ab, 0.18f));
            bottom = Gdiplus::Color(244, MixChannel(clamp_b(br + 12), ar, 0.12f),
                                    MixChannel(clamp_b(bg + 12), ag, 0.12f),
                                    MixChannel(clamp_b(bb + 16), ab, 0.12f));
          } else if (hover) {
            top = Gdiplus::Color(228, clamp_b(br + 16), clamp_b(bg + 16), clamp_b(bb + 20));
            bottom = Gdiplus::Color(224, clamp_b(br + 6), clamp_b(bg + 6), clamp_b(bb + 10));
          } else {
            top = Gdiplus::Color(198, clamp_b(br + 4), clamp_b(bg + 4), clamp_b(bb + 6));
            bottom = Gdiplus::Color(192, clamp_b(br - 4), clamp_b(bg - 4), clamp_b(bb - 2));
          }
        }
        Gdiplus::LinearGradientBrush body(
            Gdiplus::PointF(tab_cx, tab_cy),
            Gdiplus::PointF(tab_cx, tab_cy + tab_bh), top, bottom);
        g.FillPath(&body, &path);

        // Open-right border path (no vertical seam along the screen edge)
        Gdiplus::GraphicsPath border_path;
        make_tab_path(&border_path, tab_cx, tab_cy, tab_bw, tab_bh, rad, false);
        if (on) {
          Gdiplus::Pen rim(
              Gdiplus::Color(is_light ? 135 : 165, ar, ag, ab), 1.1f);
          g.DrawPath(&rim, &border_path);
        } else if (hover) {
          Gdiplus::Pen rim(
              is_light ? Gdiplus::Color(95, ar, ag, ab)
                       : Gdiplus::Color(90, 255, 255, 255),
              1.0f);
          g.DrawPath(&rim, &border_path);
        } else {
          Gdiplus::Pen rim(
              is_light ? Gdiplus::Color(36, 15, 23, 42)
                       : Gdiplus::Color(38, 255, 255, 255),
              1.0f);
          g.DrawPath(&rim, &border_path);
        }

        // Crisp rounded pill indicator on the left of the active tab
        if (on) {
          const float pill_w = (std::max)(2.5f, std::round(3.f * font_scale_));
          const float pill_h = (std::max)(10.f, std::round(14.f * font_scale_));
          const float pill_x = tab_cx + std::round(6.5f * font_scale_);
          const float pill_y = tab_cy + (tab_bh - pill_h) * 0.5f;
          Gdiplus::GraphicsPath pill;
          add_round_rect(&pill, pill_x, pill_y, pill_w, pill_h, pill_w * 0.5f);
          Gdiplus::SolidBrush pill_brush(Gdiplus::Color(245, ar, ag, ab));
          g.FillPath(&pill_brush, &pill);
        }

        // Center label in the tab body; show subtle lock icon only on hover or when plenty of space
        const bool show_lock = locked && expand_t_ > 0.45f && (hover || on);
        const float rail_reserve = on ? std::round(11.f * font_scale_) : 0.f;
        const float lock_reserve = show_lock ? std::round(13.f * font_scale_) : 0.f;
        const float side = std::round(7.f * font_scale_);
        const float text_x = tab_cx + side + rail_reserve;
        const float text_w = tab_bw - side * 2.f - rail_reserve - lock_reserve;

        const float text_fade = (std::min)(1.f, (std::max)(0.f, (expand_t_ - 0.18f) / 0.62f));
        if (text_w > 8.f && text_fade > 0.05f) {
          const Gdiplus::Font& font =
              on ? *font_on_ : (utility ? *font_util_ : *font_page_);
          const BYTE ink_a = static_cast<BYTE>(255.f * text_fade);
          Gdiplus::Color ink_c;
          if (is_light) {
            if (on) {
              ink_c = Gdiplus::Color(ink_a, MixChannel(15, ar, 0.55f),
                                     MixChannel(23, ag, 0.55f),
                                     MixChannel(42, ab, 0.55f));
            } else if (hover) {
              ink_c = Gdiplus::Color(ink_a, 15, 23, 42);
            } else if (utility) {
              ink_c = Gdiplus::Color(static_cast<BYTE>(215.f * text_fade), 71, 85, 105);
            } else {
              ink_c = Gdiplus::Color(static_cast<BYTE>(235.f * text_fade), 45, 55, 72);
            }
          } else {
            if (on) {
              ink_c = Gdiplus::Color(ink_a, 255, 255, 255);
            } else if (hover) {
              ink_c = Gdiplus::Color(ink_a, 241, 245, 249);
            } else if (utility) {
              ink_c = Gdiplus::Color(static_cast<BYTE>(200.f * text_fade), 165, 180, 202);
            } else {
              ink_c = Gdiplus::Color(static_cast<BYTE>(225.f * text_fade), 214, 224, 238);
            }
          }
          Gdiplus::SolidBrush ink(ink_c);
          g.DrawString(s.label.c_str(), -1, &font,
                       Gdiplus::RectF(text_x, tab_cy, text_w, tab_bh), fmt_, &ink);
        }

        if (show_lock) {
          const float lw = std::round(6.f * font_scale_);
          const float lh = std::round(4.6f * font_scale_);
          const float lx = tab_cx + tab_bw - std::round(12.f * font_scale_);
          const float ly = tab_cy + (tab_bh - (lh + 3.2f * font_scale_)) * 0.5f;
          const Gdiplus::Color lock_c =
              on ? Gdiplus::Color(200, ar, ag, ab)
                 : (is_light ? Gdiplus::Color(150, 100, 116, 139)
                             : Gdiplus::Color(160, 165, 180, 202));
          Gdiplus::Pen shackle(lock_c, (std::max)(1.f, 1.1f * font_scale_));
          g.DrawArc(&shackle,
                    Gdiplus::RectF(lx + 1.f * font_scale_, ly,
                                   lw - 2.f * font_scale_, 3.6f * font_scale_),
                    180.f, 180.f);
          Gdiplus::GraphicsPath body_p;
          add_round_rect(&body_p, lx, ly + 2.8f * font_scale_, lw, lh,
                         1.2f * font_scale_);
          Gdiplus::SolidBrush lock_body(lock_c);
          g.FillPath(&lock_body, &body_p);
        }
      }
    }
  }

  // GDI+ writes straight alpha; ULW needs premultiplied — avoid dark fringes
  // on the mist card rim / soft shadow (same pass as pet Present).
  {
    auto* px = static_cast<BYTE*>(bits);
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    for (size_t i = 0; i < n; ++i) {
      BYTE* p = px + i * 4;
      const unsigned a = p[3];
      if (a == 0) {
        p[0] = p[1] = p[2] = 0;
      } else if (a < 255) {
        if (p[0] > a || p[1] > a || p[2] > a) {
          p[0] = static_cast<BYTE>((p[0] * a) / 255);
          p[1] = static_cast<BYTE>((p[1] * a) / 255);
          p[2] = static_cast<BYTE>((p[2] * a) / 255);
        }
      }
    }
  }

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

void PageBar::ReleaseLayerBuffer() {
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

bool PageBar::EnsureLayerBuffer(int w, int h) {
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
