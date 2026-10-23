#include "page_bar.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "i18n.h"

#include <CommCtrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <string>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativePageBar";
// Match Python PageIndicatorWidget: _PEEK_W=86, _BTN_H=34, spacing≈4.
constexpr int kChipW = 86;
constexpr int kChipH = 34;
constexpr int kSlot = 38;
constexpr int kPadX = 0;
constexpr int kPadY = 6;
constexpr int kEdgeMargin = 0;
constexpr int kDragThreshold = 6;
bool g_registered = false;

void EnsureGdiplus() {
  static ULONG_PTR token = 0;
  if (!token) {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
  }
}
}  // namespace

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
  const int n = (std::max)(1, static_cast<int>(slots_.size()));
  const int w = kChipW + kPadX * 2;
  const int h = kPadY * 2 + n * slot_h_;
  const RECT work = PrimaryWorkArea();
  const int x = work.right - w - kEdgeMargin;
  const int y = work.top + (work.bottom - work.top - h) / 2;
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName,
      L"Pages", WS_POPUP, x, y, w, h, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  return true;
}

void PageBar::Destroy() {
  if (tip_) {
    DestroyWindow(tip_);
    tip_ = nullptr;
  }
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
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
  SendMessageW(tip_, TTM_TRACKPOSITION, 0, MAKELPARAM(pt.x - 8, pt.y + 14));
  SendMessageW(tip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
}

void PageBar::Show() {
  if (!hwnd_) {
    return;
  }
  Relayout();
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
  Paint(nullptr);
}

void PageBar::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void PageBar::SetPage(int page) {
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
  // Python: record / minutes / calculator / todo + vault peek on edge bar.
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
  add_tool(tool_vault_, L"vault", L"账号");
}

void PageBar::DockToRightEdge(int preferred_y) {
  if (!hwnd_) {
    return;
  }
  const RECT work = PrimaryWorkArea();
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
  const int x = static_cast<int>(work.right) - w - kEdgeMargin;
  SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
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
  const int n = (std::max)(1, static_cast<int>(slots_.size()));
  const int w = kChipW + kPadX * 2;
  const int h = kPadY * 2 + n * slot_h_;
  const RECT work = PrimaryWorkArea();
  int y = static_cast<int>(work.top) +
          (static_cast<int>(work.bottom - work.top) - h) / 2;
  if (anchor_y_ >= 0) {
    y = static_cast<int>(work.top) + anchor_y_;
  }
  y = (std::max)(static_cast<int>(work.top),
                 (std::min)(y, static_cast<int>(work.bottom) - h));
  const int x = static_cast<int>(work.right) - w - kEdgeMargin;
  SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
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
                           bool todo, bool vault, bool note) {
  tool_record_ = record;
  tool_minutes_ = minutes;
  tool_calculator_ = calculator;
  tool_todo_ = todo;
  tool_vault_ = vault;
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
  if (old_dpi == new_dpi) {
    return;
  }
  const double k = static_cast<double>(new_dpi) / old_dpi;
  if (anchor_y_ >= 0) {
    anchor_y_ = static_cast<int>(anchor_y_ * k + 0.5);
  }
  chip_h_ = (std::max)(1, static_cast<int>(kChipH * k + 0.5));
  slot_h_ = (std::max)(chip_h_, static_cast<int>(kSlot * k + 0.5));
  font_scale_ = static_cast<float>(k);
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
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

int PageBar::HitTest(int x, int y) const {
  if (x < kPadX || x >= kPadX + kChipW) {
    return -1;
  }
  const int idx = (y - kPadY) / slot_h_;
  if (idx < 0 || idx >= static_cast<int>(slots_.size())) {
    return -1;
  }
  const int local_y = (y - kPadY) - idx * slot_h_;
  if (local_y < 0 || local_y >= chip_h_) {
    return -1;
  }
  return idx;
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
  POINT pt{};
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd_);
  const int cmd = TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0,
      hwnd_, nullptr);
  DestroyMenu(menu);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
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
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      Paint(nullptr);
      return 0;
    }
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
        const int hit = HitTest(x, y);
        if (hit != hover_index_) {
          hover_index_ = hit;
          Paint(nullptr);
          UpdateHoverTip(hit);
        } else if (hit >= 0 && tip_) {
          POINT pt{};
          GetCursorPos(&pt);
          SendMessageW(tip_, TTM_TRACKPOSITION, 0,
                       MAKELPARAM(pt.x - 8, pt.y + 14));
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
      const RECT work = PrimaryWorkArea();
      RECT rc{};
      GetWindowRect(hwnd_, &rc);
      const int w = rc.right - rc.left;
      const int h = rc.bottom - rc.top;
      int ny = screen.y - drag_grab_.y;
      ny = (std::max)(static_cast<int>(work.top),
                     (std::min)(ny, static_cast<int>(work.bottom) - h));
      // Keep X locked to the right edge while dragging (Python product rule).
      const int nx = static_cast<int>(work.right) - w - kEdgeMargin;
      SetWindowPos(hwnd_, nullptr, nx, ny, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hover_index_ >= 0) {
        hover_index_ = -1;
        Paint(nullptr);
        UpdateHoverTip(-1);
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
        return 0;
      }
      dragging_ = false;
      drag_moved_ = false;
      const int hit = HitTest(x, y);
      ActivateSlot(hit);
      return 0;
    }
    case WM_RBUTTONUP: {
      const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      if (hit >= 0 && hit < static_cast<int>(slots_.size()) &&
          slots_[static_cast<size_t>(hit)].kind == SlotKind::Page) {
        ShowPageContextMenu(slots_[static_cast<size_t>(hit)].page_index);
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

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
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

    Gdiplus::FontFamily family(L"Microsoft YaHei UI");
    const Gdiplus::FontFamily* face =
        family.IsAvailable() ? &family : Gdiplus::FontFamily::GenericSansSerif();
    Gdiplus::Font font_page(face, 12.f * font_scale_, Gdiplus::FontStyleRegular,
                            Gdiplus::UnitPixel);
    Gdiplus::Font font_util(face, 11.f * font_scale_, Gdiplus::FontStyleRegular,
                            Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    const BYTE ar = GetRValue(accent_);
    const BYTE ag = GetGValue(accent_);
    const BYTE ab = GetBValue(accent_);

    for (int i = 0; i < static_cast<int>(slots_.size()); ++i) {
      const Slot& s = slots_[static_cast<size_t>(i)];
      const bool on =
          s.kind == SlotKind::Page && s.page_index == page_;
      const bool hover = (i == hover_index_);
      // Python: folder/record/minutes/todo quieter; vault/page share dock identity.
      const bool utility =
          s.kind == SlotKind::Folder ||
          (s.kind == SlotKind::Tool && s.tool_id != L"vault");
      const float cx = static_cast<float>(kPadX);
      const float cy =
          static_cast<float>(kPadY + i * slot_h_ + (slot_h_ - chip_h_) / 2);
      const float bw = static_cast<float>(kChipW);
      const float bh = static_cast<float>(kChipH);

      // Soft indigo wash when active — match Python _palette().
      Gdiplus::Color top;
      Gdiplus::Color bottom;
      if (on) {
        top = Gdiplus::Color(236, 224, 231, 255);
        bottom = Gdiplus::Color(244, 210, 220, 252);
      } else if (hover) {
        top = Gdiplus::Color(236, 255, 255, 255);
        bottom = Gdiplus::Color(244, 232, 238, 255);
      } else if (utility) {
        top = Gdiplus::Color(214, 255, 255, 255);
        bottom = Gdiplus::Color(222, 248, 250, 252);
      } else {
        top = Gdiplus::Color(228, 255, 255, 255);
        bottom = Gdiplus::Color(236, 248, 250, 252);
      }
      Gdiplus::LinearGradientBrush body(
          Gdiplus::PointF(cx, cy), Gdiplus::PointF(cx, cy + bh), top, bottom);

      // Bookmark tab: generous left radius, flush right edge (screen edge).
      const float rad = (std::min)(10.f, bh * 0.42f);
      Gdiplus::GraphicsPath path;
      path.AddArc(cx, cy, rad * 2, rad * 2, 180, 90);
      path.AddLine(cx + rad, cy, cx + bw, cy);
      path.AddLine(cx + bw, cy, cx + bw, cy + bh);
      path.AddLine(cx + bw, cy + bh, cx + rad, cy + bh);
      path.AddArc(cx, cy + bh - rad * 2, rad * 2, rad * 2, 90, 90);
      path.CloseFigure();
      g.FillPath(&body, &path);

      if (on || hover) {
        Gdiplus::Pen rim(Gdiplus::Color(on ? 40 : 55, ar, ag, ab), 0.85f);
        g.DrawPath(&rim, &path);
      }
      if (on) {
        const float rail_h = (std::max)(11.f, bh - 16.f);
        Gdiplus::SolidBrush rail(Gdiplus::Color(255, ar, ag, ab));
        g.FillRectangle(&rail, cx + 5.f, cy + (bh - rail_h) * 0.5f, 2.2f,
                        rail_h);
      }

      // Active: extra left pad so text clears the rail (Python left_pad=13).
      const float left_pad = on ? 13.f : (utility ? 9.f : 8.f);
      const float right_pad = 6.f;
      Gdiplus::SolidBrush ink(
          on ? Gdiplus::Color(255, ar, ag, ab)
             : Gdiplus::Color(255, utility ? 100 : 15, utility ? 116 : 23,
                              utility ? 139 : 42));
      const Gdiplus::Font& font = utility ? font_util : font_page;
      g.DrawString(s.label.c_str(), -1, &font,
                   Gdiplus::RectF(cx + left_pad, cy, bw - left_pad - right_pad,
                                  bh),
                   &fmt, &ink);

      // U7 page.locked — draw a small padlock marker on locked pages.
      if (s.kind == SlotKind::Page && PageLockedAt(s.page_index)) {
        const float lw = 6.f;
        const float lx = cx + bw - 11.f;
        const float ly = cy + (bh - 9.f) * 0.5f;
        Gdiplus::Pen shackle(Gdiplus::Color(255, ar, ag, ab), 1.f);
        g.DrawArc(&shackle, Gdiplus::RectF(lx + 1.f, ly, lw - 2.f, 3.4f),
                  180.f, 180.f);
        Gdiplus::SolidBrush lock_body(Gdiplus::Color(255, ar, ag, ab));
        g.FillRectangle(&lock_body, Gdiplus::RectF(lx, ly + 2.8f, lw, 5.2f));
      }
    }
  }

  POINT origin{wr.left, wr.top};
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateLayeredWindow(hwnd_, screen, &origin, &size, mem, &src, 0, &blend,
                      ULW_ALPHA);
  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}
