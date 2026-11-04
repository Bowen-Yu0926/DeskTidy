#include "layout_editor.h"

#include "display_geom.h"
#include "fence_style.h"
#include "i18n.h"
#include "mist_msg.h"
#include "prompt_text.h"
#include "toast.h"

#include <ShlObj.h>
#include <windowsx.h>

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kClass[] = L"DeskTidyNativeLayoutEditor";
constexpr int kHandle = 8;
constexpr int kMinW = kFenceMinW;
constexpr int kMinH = kFenceMinH;
constexpr int kGridPx = 40;   // grid snap pitch
constexpr int kSnapPx = 24;   // edge snap (Python EDGE_SNAP_THRESHOLD)
constexpr int kMaxPages = 12; // Python 分页上限

AppState* g_state = nullptr;
int g_edit_page = 0;
std::vector<int> g_sel;                  // selected fence indices (into fences)
int g_drag = -1;                         // primary drag fence index
int g_resize = -1;
bool g_opacity_drag = false;
bool g_grid_on = false;
bool g_snap_on = true;
std::vector<std::pair<int, POINT>> g_drag_origins;
POINT g_grab{};
SIZE g_start_size{};
bool g_ok = false;
HWND g_hwnd = nullptr;

RECT g_footer_btns[7]{};
RECT g_toggle_btns[3]{};
RECT g_opacity_slider{};
RECT g_new_page_tab{};
std::vector<RECT> g_page_tabs;
std::vector<RECT> g_style_chips;

// ---------------------------------------------------------------------------
// Page membership / geometry (G3 model from layout.h).
// ---------------------------------------------------------------------------
bool FenceOnPage(const FenceLayout& f, int page) {
  if (f.page_geometries.count(page)) {
    return true;
  }
  return f.page_geometries.empty() && f.page == page;
}

std::vector<int> PageFenceIndices() {
  std::vector<int> out;
  if (!g_state) {
    return out;
  }
  for (int i = 0; i < static_cast<int>(g_state->fences.size()); ++i) {
    if (FenceOnPage(g_state->fences[static_cast<size_t>(i)], g_edit_page)) {
      out.push_back(i);
    }
  }
  return out;
}

FencePageGeometry GeomOf(int idx) {
  return FenceGeometryForPage(g_state->fences[static_cast<size_t>(idx)],
                              g_edit_page);
}

void SetGeom(int idx, const FencePageGeometry& geo) {
  SetFenceGeometryForPage(&g_state->fences[static_cast<size_t>(idx)],
                          g_edit_page, geo);
}

bool HasSelection() {
  for (int idx : g_sel) {
    if (idx >= 0 && idx < static_cast<int>(g_state->fences.size())) {
      return true;
    }
  }
  return false;
}

bool IsSelected(int idx) {
  return std::find(g_sel.begin(), g_sel.end(), idx) != g_sel.end();
}

int PrimarySelected() {
  return g_sel.empty() ? -1 : g_sel.front();
}

// ---------------------------------------------------------------------------
// Chrome layout (tabs / chips / slider / toggles / footer).
// ---------------------------------------------------------------------------
int TextWidth(HDC hdc, const std::wstring& s) {
  SIZE sz{};
  GetTextExtentPoint32W(hdc, s.c_str(), static_cast<int>(s.size()), &sz);
  return sz.cx;
}

RECT CanvasRect(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  rc.left = 12;
  rc.top = 58;
  rc.right -= 12;
  rc.bottom -= 112;
  return rc;
}

void LayoutChrome(HWND hwnd) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  HDC hdc = GetDC(hwnd);

  // Page tabs.
  g_page_tabs.clear();
  int tx = 12;
  const int ty = 30;
  const int th = 22;
  if (g_state) {
    for (size_t i = 0; i < g_state->page_names.size(); ++i) {
      const std::wstring label =
          (static_cast<int>(i) == g_edit_page ? L"● " : L"") +
          g_state->page_names[i];
      const int w = TextWidth(hdc, label) + 20;
      g_page_tabs.push_back({tx, ty, tx + w, ty + th});
      tx += w + 4;
    }
  }
  g_new_page_tab = {tx, ty, tx + 24, ty + th};
  ReleaseDC(hwnd, hdc);

  // Upper footer row: style chips + opacity slider + toggles.
  const int cy = rc.bottom - 96;
  const int chh = 26;
  g_style_chips.clear();
  int cx = 12;
  const auto styles = AllFenceStyles();
  for (size_t i = 0; i < styles.size(); ++i) {
    g_style_chips.push_back({cx, cy, cx + 46, cy + chh});
    cx += 46 + 6;
  }
  g_opacity_slider = {cx + 8, cy, cx + 8 + 150, cy + chh};
  cx = g_opacity_slider.right + 12;
  g_toggle_btns[0] = {cx, cy, cx + 72, cy + chh};
  cx += 72 + 6;
  g_toggle_btns[1] = {cx, cy, cx + 48, cy + chh};
  cx += 48 + 6;
  g_toggle_btns[2] = {cx, cy, cx + 48, cy + chh};

  // Footer buttons (+ P1-4 整理规则).
  const int by = rc.bottom - 56;
  const int bh = 30;
  int bx = 12;
  const int bw[7] = {72, 64, 64, 64, 72, 72, 88};
  for (int i = 0; i < 7; ++i) {
    g_footer_btns[i] = {bx, by, bx + bw[i], by + bh};
    bx += bw[i] + 8;
  }
}

int HitPageTab(POINT pt) {
  if (PtInRect(&g_new_page_tab, pt)) {
    return -2;  // new page
  }
  for (size_t i = 0; i < g_page_tabs.size(); ++i) {
    if (PtInRect(&g_page_tabs[i], pt)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int HitToggle(POINT pt) {
  for (int i = 0; i < 3; ++i) {
    if (PtInRect(&g_toggle_btns[i], pt)) {
      return i;
    }
  }
  return -1;
}

int HitStyleChip(POINT pt) {
  for (size_t i = 0; i < g_style_chips.size(); ++i) {
    if (PtInRect(&g_style_chips[i], pt)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int HitFooter(POINT pt) {
  for (int i = 0; i < 7; ++i) {
    if (PtInRect(&g_footer_btns[i], pt)) {
      return i + 1;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Canvas coordinate mapping (virtual screen <-> canvas).
// ---------------------------------------------------------------------------
void RectToCanvas(HWND hwnd, int x, int y, int w, int h, RECT* out) {
  const RECT canvas = CanvasRect(hwnd);
  const RECT work = VirtualScreenRect();
  const int vw = (std::max)(1, static_cast<int>(work.right - work.left));
  const int vh = (std::max)(1, static_cast<int>(work.bottom - work.top));
  const int cw = (std::max)(1, static_cast<int>(canvas.right - canvas.left));
  const int ch = (std::max)(1, static_cast<int>(canvas.bottom - canvas.top));
  out->left = canvas.left + (x - work.left) * cw / vw;
  out->top = canvas.top + (y - work.top) * ch / vh;
  out->right = canvas.left + (x + w - work.left) * cw / vw;
  out->bottom = canvas.top + (y + h - work.top) * ch / vh;
}

bool HitSEHandle(const RECT& r, int x, int y) {
  return x >= r.right - kHandle && x <= r.right + 2 && y >= r.bottom - kHandle &&
         y <= r.bottom + 2;
}

int HitFence(HWND hwnd, int x, int y, bool* on_se) {
  if (on_se) {
    *on_se = false;
  }
  const auto idxs = PageFenceIndices();
  for (auto it = idxs.rbegin(); it != idxs.rend(); ++it) {
    FencePageGeometry geo = GeomOf(*it);
    RECT r{};
    RectToCanvas(hwnd, geo.x, geo.y, geo.w, geo.h, &r);
    if (HitSEHandle(r, x, y)) {
      if (on_se) {
        *on_se = true;
      }
      return *it;
    }
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
      return *it;
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Mutations.
// ---------------------------------------------------------------------------
void SwitchPage(int page) {
  if (!g_state) {
    return;
  }
  if (page < 0 || page >= static_cast<int>(g_state->page_names.size())) {
    return;
  }
  g_edit_page = page;
  g_sel.clear();
  g_drag = -1;
  g_resize = -1;
}

void SetFencePos(int idx, int x, int y) {
  FencePageGeometry geo = GeomOf(idx);
  geo.x = x;
  geo.y = y;
  SetGeom(idx, geo);
}

void SetFenceSize(int idx, int w, int h) {
  FencePageGeometry geo = GeomOf(idx);
  geo.w = (std::max)(kMinW, w);
  geo.h = (std::max)(kMinH, h);
  SetGeom(idx, geo);
}

int SnapGrid(int v, int origin) {
  if (!g_grid_on) {
    return v;
  }
  const int off = v - origin;
  return origin + (off / kGridPx) * kGridPx;
}

void ApplyEdgeSnap(int idx) {
  if (!g_snap_on || !g_state) {
    return;
  }
  FencePageGeometry geo = GeomOf(idx);
  LayoutRect moving{geo.x, geo.y, geo.w, geo.h};
  std::vector<LayoutRect> others;
  for (int j : PageFenceIndices()) {
    if (j == idx) {
      continue;
    }
    FencePageGeometry g = GeomOf(j);
    others.push_back({g.x, g.y, g.w, g.h});
  }
  const RECT work = VirtualScreenRect();
  const LayoutRect wr{work.left, work.top, work.right - work.left,
                      work.bottom - work.top};
  const LayoutPoint p = SnapFencePosition(moving, others, wr, kSnapPx);
  geo.x = p.x;
  geo.y = p.y;
  SetGeom(idx, geo);
}

void ApplyStyleToSelection(int chip) {
  const auto styles = AllFenceStyles();
  if (chip < 0 || chip >= static_cast<int>(styles.size()) || !g_state) {
    return;
  }
  for (int idx : g_sel) {
    if (idx >= 0 && idx < static_cast<int>(g_state->fences.size())) {
      g_state->fences[static_cast<size_t>(idx)].style_id = styles[chip]->id;
    }
  }
}

void CycleSelectedStyle() {
  if (!g_state || g_sel.empty()) {
    return;
  }
  const auto styles = AllFenceStyles();
  if (styles.empty()) {
    return;
  }
  const int idx = PrimarySelected();
  if (idx < 0 || idx >= static_cast<int>(g_state->fences.size())) {
    return;
  }
  auto& f = g_state->fences[static_cast<size_t>(idx)];
  size_t cur = 0;
  for (size_t i = 0; i < styles.size(); ++i) {
    if (styles[i]->id == f.style_id) {
      cur = (i + 1) % styles.size();
      break;
    }
  }
  f.style_id = styles[cur]->id;
}

int SelectedOpacity() {
  const int idx = PrimarySelected();
  if (idx < 0 || idx >= static_cast<int>(g_state->fences.size())) {
    return 255;
  }
  const auto& f = g_state->fences[static_cast<size_t>(idx)];
  if (f.opacity >= 0) {
    return f.opacity;
  }
  const auto* st = FenceStyleById(f.style_id);
  return st ? st->alpha : 255;
}

void SetSelectedOpacity(int v) {
  if (!g_state) {
    return;
  }
  v = (std::max)(0, (std::min)(255, v));
  for (int idx : g_sel) {
    if (idx >= 0 && idx < static_cast<int>(g_state->fences.size())) {
      g_state->fences[static_cast<size_t>(idx)].opacity = v;
    }
  }
}

void SetOpacityFromX(int x) {
  const int span =
      (std::max)(1, static_cast<int>(g_opacity_slider.right -
                                     g_opacity_slider.left));
  const int v = (x - g_opacity_slider.left) * 255 / span;
  SetSelectedOpacity(v);
}

void ToggleVisible() {
  if (!g_state || g_sel.empty()) {
    return;
  }
  for (int idx : g_sel) {
    if (idx < 0 || idx >= static_cast<int>(g_state->fences.size())) {
      continue;
    }
    FencePageGeometry geo = GeomOf(idx);
    geo.visible = !geo.visible;
    SetGeom(idx, geo);
  }
}

void AddFence() {
  if (!g_state) {
    return;
  }
  FenceLayout fl;
  fl.id = MakeFenceId();
  fl.title = L"新分区";
  const RECT work = PrimaryWorkArea();
  const int count = static_cast<int>(PageFenceIndices().size());
  fl.x = work.left + 80 + count * 24;
  fl.y = work.top + 60 + count * 24;
  fl.w = kFenceDefaultW;
  fl.h = (std::max)(kFenceDefaultH, MinFenceHeightForIconRows(2));
  fl.page = g_edit_page;
  fl.style_id = "navy";
  g_state->fences.push_back(fl);
  g_sel = {static_cast<int>(g_state->fences.size()) - 1};
}

void DeleteSelected() {
  if (!g_state || g_sel.empty()) {
    return;
  }
  // Keep at least one fence per page.
  if (PageFenceIndices().size() <= 1) {
    ShowToast(Tr("layout_keep_one"));
    return;
  }
  for (int idx : g_sel) {
    if (idx < 0 || idx >= static_cast<int>(g_state->fences.size())) {
      continue;
    }
    if (g_state->fences[static_cast<size_t>(idx)].id.rfind("system_", 0) == 0) {
      ShowToast(Tr("layout_no_system"));
      return;
    }
  }
  // Erase from high to low so indices stay valid.
  std::sort(g_sel.begin(), g_sel.end(), std::greater<int>());
  for (int idx : g_sel) {
    g_state->fences.erase(g_state->fences.begin() + idx);
  }
  g_sel.clear();
  g_drag = -1;
  g_resize = -1;
}

void BrowsePortal() {
  if (!g_state || g_sel.empty()) {
    return;
  }
  // P2-15: if any selected fence is already a portal, offer clear / replace.
  bool any_portal = false;
  for (int idx : g_sel) {
    if (idx >= 0 && idx < static_cast<int>(g_state->fences.size()) &&
        !g_state->fences[static_cast<size_t>(idx)].portal_path.empty()) {
      any_portal = true;
      break;
    }
  }
  if (any_portal) {
    const MistMsgResult r = MistMessage(
        g_hwnd, L"门户文件夹",
        L"所选分区已是门户。\n\n是 = 更换门户文件夹\n否 = 取消门户（转回普通分区）\n取消 = 返回",
        MistMsgKind::YesNoCancel, MistMsgTone::Warn);
    if (r == MistMsgResult::Cancel) {
      return;
    }
    if (r == MistMsgResult::No) {
      for (int idx : g_sel) {
        if (idx >= 0 && idx < static_cast<int>(g_state->fences.size())) {
          g_state->fences[static_cast<size_t>(idx)].portal_path.clear();
        }
      }
      return;
    }
  }
  BROWSEINFOW bi{};
  bi.hwndOwner = g_hwnd;
  bi.lpszTitle = L"门户文件夹";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
  if (!pidl) {
    return;
  }
  wchar_t path[MAX_PATH] = {};
  if (SHGetPathFromIDListW(pidl, path)) {
    for (int idx : g_sel) {
      if (idx >= 0 && idx < static_cast<int>(g_state->fences.size())) {
        g_state->fences[static_cast<size_t>(idx)].portal_path = path;
      }
    }
  }
  CoTaskMemFree(pidl);
}

void MoveFenceToPage(int idx, int from, int to) {
  FencePageGeometry geo = FenceGeometryForPage(
      g_state->fences[static_cast<size_t>(idx)], from);
  auto& f = g_state->fences[static_cast<size_t>(idx)];
  f.page_geometries.erase(from);
  f.page_geometries[to] = geo;
  f.page = to;
}

void EditOrganizeKindsMenu(HWND hwnd) {
  if (!g_state || g_sel.empty()) {
    return;
  }
  // Seed from first selection.
  bool has_icon = false;
  bool has_file = false;
  for (int idx : g_sel) {
    if (idx < 0 || idx >= static_cast<int>(g_state->fences.size())) {
      continue;
    }
    const auto& kinds =
        g_state->fences[static_cast<size_t>(idx)].organize_kinds;
    for (const auto& k : kinds) {
      if (k == "icon") {
        has_icon = true;
      }
      if (k == "file") {
        has_file = true;
      }
    }
    break;
  }
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | (has_icon ? MF_CHECKED : 0), 1, L"图标（.lnk/.exe）");
  AppendMenuW(m, MF_STRING | (has_file ? MF_CHECKED : 0), 2, L"文档（其他文件）");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, 3, L"两者皆可");
  AppendMenuW(m, MF_STRING, 4, L"清除（沿用扩展名规则）");
  POINT pt{};
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  const int cmd =
      TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x,
                     pt.y, 0, hwnd, nullptr);
  DestroyMenu(m);
  PostMessageW(hwnd, WM_NULL, 0, 0);
  if (cmd < 1) {
    return;
  }
  std::vector<std::string> kinds;
  if (cmd == 1) {
    kinds = {"icon"};
  } else if (cmd == 2) {
    kinds = {"file"};
  } else if (cmd == 3) {
    kinds = {"icon", "file"};
  }  // cmd == 4 → empty
  for (int idx : g_sel) {
    if (idx >= 0 && idx < static_cast<int>(g_state->fences.size())) {
      g_state->fences[static_cast<size_t>(idx)].organize_kinds = kinds;
    }
  }
}

void MoveToPageMenu(HWND hwnd) {
  if (!g_state || g_sel.empty()) {
    return;
  }
  HMENU m = CreatePopupMenu();
  for (size_t i = 0; i < g_state->page_names.size(); ++i) {
    AppendMenuW(m, MF_STRING, static_cast<UINT_PTR>(i + 1),
                g_state->page_names[i].c_str());
  }
  POINT pt{};
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                 pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(m);
  PostMessageW(hwnd, WM_NULL, 0, 0);
  if (cmd < 1) {
    return;
  }
  const int target = cmd - 1;
  if (target == g_edit_page) {
    return;
  }
  for (int idx : g_sel) {
    if (idx >= 0 && idx < static_cast<int>(g_state->fences.size())) {
      MoveFenceToPage(idx, g_edit_page, target);
    }
  }
  g_sel.clear();
  g_drag = -1;
  g_resize = -1;
}

void AddPage() {
  if (!g_state) {
    return;
  }
  if (g_state->page_names.size() >= static_cast<size_t>(kMaxPages)) {
    ShowToast(L"最多 12 页。");
    return;
  }
  g_state->page_names.push_back(
      NextPageName(g_state->page_names.size() + 1));
  SwitchPage(static_cast<int>(g_state->page_names.size()) - 1);
}

void RenamePage(int page) {
  if (!g_state || page <= 1 ||
      page >= static_cast<int>(g_state->page_names.size())) {
    return;
  }
  const std::wstring name =
      PromptText(g_hwnd, Tr("rename_page").c_str(), L"页面名称",
                 g_state->page_names[static_cast<size_t>(page)]);
  if (!name.empty()) {
    g_state->page_names[static_cast<size_t>(page)] = name;
  }
}

void DeletePage(int page) {
  if (!g_state || page <= 1 ||
      page >= static_cast<int>(g_state->page_names.size())) {
    if (g_state && page <= 1) {
      ShowToast(Tr("layout_no_system"));
    }
    return;
  }
  // Reindex per-page geometry after removing the page; fences only on `page`
  // move to page 0.
  for (auto& f : g_state->fences) {
    std::map<int, FencePageGeometry> remap;
    for (const auto& kv : f.page_geometries) {
      if (kv.first == page) {
        if (f.page_geometries.size() == 1) {
          remap[0] = kv.second;
        }
      } else if (kv.first > page) {
        remap[kv.first - 1] = kv.second;
      } else {
        remap[kv.first] = kv.second;
      }
    }
    f.page_geometries = std::move(remap);
    if (f.page == page) {
      f.page = 0;
    } else if (f.page > page) {
      f.page -= 1;
    }
  }
  g_state->page_names.erase(g_state->page_names.begin() + page);
  if (g_state->current_page == page) {
    g_state->current_page = 0;
  } else if (g_state->current_page > page) {
    g_state->current_page -= 1;
  }
  g_edit_page = (std::min)(g_edit_page,
                           static_cast<int>(g_state->page_names.size()) - 1);
  g_sel.clear();
  g_drag = -1;
  g_resize = -1;
}

void PageTabContextMenu(HWND hwnd, int page) {
  if (!g_state) {
    return;
  }
  const bool locked = (page == 0 || page == 1);
  enum { CmdNew = 1, CmdRename, CmdDelete };
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, CmdNew, L"在此新建分区");
  AppendMenuW(m, MF_STRING | (locked ? MF_GRAYED : 0), CmdRename, L"重命名页面");
  AppendMenuW(m, MF_STRING | (locked ? MF_GRAYED : 0), CmdDelete, L"删除页面");
  POINT pt{};
  GetCursorPos(&pt);
  SetForegroundWindow(hwnd);
  const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                 pt.x, pt.y, 0, hwnd, nullptr);
  DestroyMenu(m);
  PostMessageW(hwnd, WM_NULL, 0, 0);
  if (cmd == CmdNew) {
    SwitchPage(page);
    AddFence();
  } else if (cmd == CmdRename && !locked) {
    RenamePage(page);
  } else if (cmd == CmdDelete && !locked) {
    DeletePage(page);
  }
  InvalidateRect(hwnd, nullptr, FALSE);
}

void RenameFence(int idx) {
  if (!g_state || idx < 0 || idx >= static_cast<int>(g_state->fences.size())) {
    return;
  }
  auto& f = g_state->fences[static_cast<size_t>(idx)];
  const std::wstring name =
      PromptText(g_hwnd, Tr("rename_fence").c_str(),
                 Tr("rename_fence_label").c_str(), f.title);
  if (!name.empty()) {
    f.title = name;
  }
}

// ---------------------------------------------------------------------------
// Painting.
// ---------------------------------------------------------------------------
void DrawMistBtn(HDC hdc, const RECT& rc, const wchar_t* text, bool primary) {
  const COLORREF fill = primary ? RGB(0x16, 0xA3, 0x4A) : RGB(0xFF, 0xFF, 0xFF);
  const COLORREF border =
      primary ? RGB(0x16, 0xA3, 0x4A) : RGB(0xC9, 0xD2, 0xDE);
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

COLORREF StyleColor(const std::string& style_id) {
  const auto* p = FenceStyleById(style_id);
  return p ? p->background : RGB(60, 70, 90);
}

void PaintEditor(HWND hwnd, HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  HBRUSH bg = CreateSolidBrush(RGB(0xEE, 0xF1, 0xF6));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(0x1A, 0x23, 0x32));
  RECT title{12, 6, rc.right - 12, 26};
  DrawTextW(hdc, L"拖动移动 · 右下角缩放 · 双击改名 · Ctrl点选多选 · 页签右键分页管理",
            -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  LayoutChrome(hwnd);

  // Page tabs.
  for (size_t i = 0; i < g_page_tabs.size(); ++i) {
    const bool cur = static_cast<int>(i) == g_edit_page;
    const RECT& r = g_page_tabs[i];
    HBRUSH br = CreateSolidBrush(cur ? RGB(0x16, 0xA3, 0x4A) : RGB(0xFF, 0xFF, 0xFF));
    FillRect(hdc, &r, br);
    DeleteObject(br);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xC9, 0xD2, 0xDE));
    HGDIOBJ oldp = SelectObject(hdc, pen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
    SelectObject(hdc, oldp);
    DeleteObject(pen);
    SetTextColor(hdc, cur ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1A, 0x23, 0x32));
    const std::wstring label =
        (cur ? L"● " : L"") + g_state->page_names[i];
    RECT tr = r;
    DrawTextW(hdc, label.c_str(), -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  {
    const RECT& r = g_new_page_tab;
    HBRUSH br = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    FillRect(hdc, &r, br);
    DeleteObject(br);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xC9, 0xD2, 0xDE));
    HGDIOBJ oldp = SelectObject(hdc, pen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
    SelectObject(hdc, oldp);
    DeleteObject(pen);
    SetTextColor(hdc, RGB(0x1A, 0x23, 0x32));
    RECT tr = r;
    DrawTextW(hdc, L"+", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  // Canvas background.
  const RECT canvas = CanvasRect(hwnd);
  HBRUSH desk = CreateSolidBrush(RGB(0xF4, 0xF6, 0xF9));
  FillRect(hdc, &canvas, desk);
  DeleteObject(desk);
  HPEN border = CreatePen(PS_SOLID, 1, RGB(0xC9, 0xD2, 0xDE));
  HGDIOBJ oldp = SelectObject(hdc, border);
  SelectObject(hdc, GetStockObject(NULL_BRUSH));
  Rectangle(hdc, canvas.left, canvas.top, canvas.right, canvas.bottom);
  SelectObject(hdc, oldp);
  DeleteObject(border);

  // Grid lines.
  if (g_grid_on && g_state) {
    const RECT work = VirtualScreenRect();
    const int vw = (std::max)(1, static_cast<int>(work.right - work.left));
    const int cw = (std::max)(1, static_cast<int>(canvas.right - canvas.left));
    HPEN gp = CreatePen(PS_DOT, 1, RGB(0xD6, 0xDC, 0xE6));
    HGDIOBJ oldg = SelectObject(hdc, gp);
    for (int vx = work.left; vx <= work.right; vx += kGridPx) {
      const int x = canvas.left + (vx - work.left) * cw / vw;
      MoveToEx(hdc, x, canvas.top, nullptr);
      LineTo(hdc, x, canvas.bottom);
    }
    const int vh = (std::max)(1, static_cast<int>(work.bottom - work.top));
    const int ch = (std::max)(1, static_cast<int>(canvas.bottom - canvas.top));
    for (int vy = work.top; vy <= work.bottom; vy += kGridPx) {
      const int y = canvas.top + (vy - work.top) * ch / vh;
      MoveToEx(hdc, canvas.left, y, nullptr);
      LineTo(hdc, canvas.right, y);
    }
    SelectObject(hdc, oldg);
    DeleteObject(gp);
  }

  // Fences on the current page.
  if (g_state) {
    for (int idx : PageFenceIndices()) {
      const auto& f = g_state->fences[static_cast<size_t>(idx)];
      FencePageGeometry geo = GeomOf(idx);
      RECT r{};
      RectToCanvas(hwnd, geo.x, geo.y, geo.w, geo.h, &r);
      HBRUSH br = CreateSolidBrush(StyleColor(f.style_id));
      FillRect(hdc, &r, br);
      DeleteObject(br);
      if (IsSelected(idx)) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0x1D, 0x4E, 0xD8));
        HGDIOBJ old = SelectObject(hdc, pen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, old);
        DeleteObject(pen);
        RECT hnd{r.right - kHandle, r.bottom - kHandle, r.right, r.bottom};
        HBRUSH hb = CreateSolidBrush(RGB(0x1D, 0x4E, 0xD8));
        FillRect(hdc, &hnd, hb);
        DeleteObject(hb);
      }
      std::wstring label = f.title;
      if (!geo.visible) {
        label += L" (隐藏)";
      }
      SetTextColor(hdc, RGB(255, 255, 255));
      DrawTextW(hdc, label.c_str(), -1, &r,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
  }

  // Style chips.
  const auto styles = AllFenceStyles();
  for (size_t i = 0; i < g_style_chips.size() && i < styles.size(); ++i) {
    const RECT& r = g_style_chips[i];
    const auto* st = styles[i];
    HBRUSH br = CreateSolidBrush(st->background);
    FillRect(hdc, &r, br);
    DeleteObject(br);
    HPEN pen = CreatePen(PS_SOLID, 1, st->accent);
    HGDIOBJ old = SelectObject(hdc, pen);
    SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, 6, 6);
    SelectObject(hdc, old);
    DeleteObject(pen);
    SetTextColor(hdc, st->light ? RGB(0x1A, 0x23, 0x32) : RGB(0xFF, 0xFF, 0xFF));
    RECT tr = r;
    DrawTextW(hdc, st->name, -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  // Opacity slider.
  {
    const RECT& r = g_opacity_slider;
    const int v = SelectedOpacity();
    HBRUSH track = CreateSolidBrush(RGB(0xD6, 0xDC, 0xE6));
    const RECT trackr{r.left, r.top + 11, r.right, r.top + 15};
    FillRect(hdc, &trackr, track);
    DeleteObject(track);
    HBRUSH fill = CreateSolidBrush(RGB(0x16, 0xA3, 0x4A));
    const int fx = r.left + (r.right - r.left) * v / 255;
    const RECT fillr{r.left, r.top + 11, fx, r.top + 15};
    FillRect(hdc, &fillr, fill);
    DeleteObject(fill);
    HBRUSH knob = CreateSolidBrush(RGB(0x1A, 0x23, 0x32));
    Ellipse(hdc, fx - 5, r.top + 8, fx + 5, r.top + 18);
    DeleteObject(knob);
    SetTextColor(hdc, RGB(0x1A, 0x23, 0x32));
    RECT tr{r.left, r.top - 2, r.right, r.top + 8};
    const std::wstring lbl = L"透明度 " + std::to_wstring(v);
    DrawTextW(hdc, lbl.c_str(), -1, &tr, DT_CENTER | DT_TOP | DT_SINGLELINE);
  }

  // Toggle buttons.
  const wchar_t* toggle_labels[3] = {L"显示/隐藏", L"网格", L"吸附"};
  const bool toggle_on[3] = {HasSelection(), g_grid_on, g_snap_on};
  for (int i = 0; i < 3; ++i) {
    DrawMistBtn(hdc, g_toggle_btns[i], toggle_labels[i], toggle_on[i]);
  }

  // Footer buttons.
  DrawMistBtn(hdc, g_footer_btns[0], Tr("layout_apply").c_str(), true);
  DrawMistBtn(hdc, g_footer_btns[1], Tr("cancel").c_str(), false);
  DrawMistBtn(hdc, g_footer_btns[2], Tr("layout_add").c_str(), false);
  DrawMistBtn(hdc, g_footer_btns[3], Tr("layout_delete").c_str(), false);
  DrawMistBtn(hdc, g_footer_btns[4], L"门户…", false);
  DrawMistBtn(hdc, g_footer_btns[5], L"移到页…", false);
  DrawMistBtn(hdc, g_footer_btns[6], L"整理规则…", false);
}

// ---------------------------------------------------------------------------
// Window proc.
// ---------------------------------------------------------------------------
LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      PaintEditor(hwnd, hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SETCURSOR: {
      if (LOWORD(lparam) == HTCLIENT && g_state) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        bool on_se = false;
        HitFence(hwnd, pt.x, pt.y, &on_se);
        SetCursor(LoadCursor(nullptr, on_se ? IDC_SIZENWSE : IDC_ARROW));
        return TRUE;
      }
      break;
    }
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      // Chrome hit-testing (priority: tabs > toggles > chips > slider > footer).
      const int tab = HitPageTab(pt);
      if (tab == -2) {
        AddPage();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (tab >= 0) {
        SwitchPage(tab);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      const int tg = HitToggle(pt);
      if (tg == 0 && HasSelection()) {
        ToggleVisible();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (tg == 1) {
        g_grid_on = !g_grid_on;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (tg == 2) {
        g_snap_on = !g_snap_on;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      const int chip = HitStyleChip(pt);
      if (chip >= 0 && HasSelection()) {
        ApplyStyleToSelection(chip);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (PtInRect(&g_opacity_slider, pt) && HasSelection()) {
        SetOpacityFromX(pt.x);
        g_opacity_drag = true;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      const int btn = HitFooter(pt);
      if (btn == 1) {
        g_ok = true;
        DestroyWindow(hwnd);
        return 0;
      }
      if (btn == 2) {
        g_ok = false;
        DestroyWindow(hwnd);
        return 0;
      }
      if (btn == 3) {
        AddFence();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (btn == 4) {
        DeleteSelected();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (btn == 5) {
        BrowsePortal();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (btn == 6) {
        MoveToPageMenu(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (btn == 7) {
        EditOrganizeKindsMenu(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }

      // Canvas fence selection / drag.
      bool on_se = false;
      const int hit = HitFence(hwnd, pt.x, pt.y, &on_se);
      if (hit >= 0 && g_state) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl) {
          auto it = std::find(g_sel.begin(), g_sel.end(), hit);
          if (it != g_sel.end()) {
            g_sel.erase(it);
          } else {
            g_sel.push_back(hit);
          }
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        if (!IsSelected(hit)) {
          g_sel = {hit};
        }
        g_grab = pt;
        if (on_se) {
          g_resize = hit;
          g_drag = -1;
          FencePageGeometry geo = GeomOf(hit);
          g_start_size = {geo.w, geo.h};
        } else {
          g_drag = hit;
          g_resize = -1;
          g_drag_origins.clear();
          for (int s : g_sel) {
            FencePageGeometry geo = GeomOf(s);
            g_drag_origins.push_back({s, {geo.x, geo.y}});
          }
        }
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      // Click on empty canvas clears selection.
      g_sel.clear();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if ((wparam & MK_LBUTTON) && g_state) {
        if (g_opacity_drag) {
          SetOpacityFromX(GET_X_LPARAM(lparam));
          InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        const RECT canvas = CanvasRect(hwnd);
        const RECT work = VirtualScreenRect();
        const int cw =
            (std::max)(1, static_cast<int>(canvas.right - canvas.left));
        const int ch =
            (std::max)(1, static_cast<int>(canvas.bottom - canvas.top));
        const int vw = (std::max)(1, static_cast<int>(work.right - work.left));
        const int vh = (std::max)(1, static_cast<int>(work.bottom - work.top));
        const int dx = GET_X_LPARAM(lparam) - g_grab.x;
        const int dy = GET_Y_LPARAM(lparam) - g_grab.y;
        if (g_resize >= 0) {
          const int w =
              (std::max)(kMinW, static_cast<int>(g_start_size.cx) + dx * vw / cw);
          const int h =
              (std::max)(kMinH, static_cast<int>(g_start_size.cy) + dy * vh / ch);
          FencePageGeometry geo = GeomOf(g_resize);
          geo.w = g_grid_on ? SnapGrid(w, 0) : w;
          geo.h = g_grid_on ? SnapGrid(h, 0) : h;
          if (geo.w < kMinW) {
            geo.w = kMinW;
          }
          if (geo.h < kMinH) {
            geo.h = kMinH;
          }
          SetGeom(g_resize, geo);
          InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g_drag >= 0) {
          const int vdx = dx * vw / cw;
          const int vdy = dy * vh / ch;
          for (const auto& o : g_drag_origins) {
            SetFencePos(o.first, SnapGrid(o.second.x + vdx, work.left),
                        SnapGrid(o.second.y + vdy, work.top));
          }
          if (!g_drag_origins.empty()) {
            ApplyEdgeSnap(g_drag_origins.front().first);
          }
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      g_drag = -1;
      g_resize = -1;
      g_opacity_drag = false;
      g_drag_origins.clear();
      ReleaseCapture();
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      const int hit =
          HitFence(hwnd, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), nullptr);
      if (hit >= 0 && g_state) {
        RenameFence(hit);
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int tab = HitPageTab(pt);
      if (tab >= 0) {
        PageTabContextMenu(hwnd, tab);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      const int hit = HitFence(hwnd, pt.x, pt.y, nullptr);
      if (hit >= 0) {
        if (!IsSelected(hit)) {
          g_sel = {hit};
        }
        CycleSelectedStyle();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
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
  g_resize = -1;
  g_opacity_drag = false;
  g_grid_on = false;
  g_snap_on = true;
  g_sel.clear();
  g_edit_page = g_state ? g_state->current_page : 0;
  if (g_state && !g_state->page_names.empty()) {
    g_edit_page = (std::min)(g_edit_page,
                             static_cast<int>(g_state->page_names.size()) - 1);
  }
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = EditorProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(0xEE, 0xF1, 0xF6));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  g_hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass,
                           Tr("layout_editor").c_str(),
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 80, 80, 860,
                           620, owner, nullptr, wc.hInstance, nullptr);
  if (!g_hwnd) {
    return false;
  }
  LayoutChrome(g_hwnd);
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
