#include "todo_window.h"

#include "desktop_shell.h"
#include "paths_util.h"
#include "prompt_text.h"

#include <CommCtrl.h>

#include <fstream>
#include <sstream>
#include <string>

#pragma comment(lib, "comctl32.lib")

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeTodo";
constexpr int kList = 3001;
constexpr int kAdd = 3002;
constexpr int kDel = 3003;

std::wstring TodosPath() { return JoinPath(NativeDataDir(), L"todos.txt"); }
}  // namespace

bool TodoWindow::Create(HINSTANCE instance) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&icc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = TodoWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(250, 250, 252));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClass, L"待办",
      WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, 40, 200, 300, 360,
      nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 245, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

  list_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
      WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOCOLUMNHEADER |
          LVS_SHOWSELALWAYS,
      6, 6, 270, 280, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)), instance, nullptr);
  ListView_SetExtendedListViewStyle(
      list_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  LVCOLUMNW col{};
  col.mask = LVCF_WIDTH | LVCF_TEXT;
  col.cx = 250;
  col.pszText = const_cast<wchar_t*>(L"待办");
  ListView_InsertColumn(list_, 0, &col);

  btn_add_ = CreateWindowW(L"BUTTON", L"添加", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           6, 296, 80, 28, hwnd_,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAdd)),
                           instance, nullptr);
  btn_del_ =
      CreateWindowW(L"BUTTON", L"删除", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 96,
                    296, 80, 28, hwnd_,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDel)),
                    instance, nullptr);
  Load();
  RefreshList();
  return true;
}

void TodoWindow::Destroy() {
  Save();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void TodoWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

void TodoWindow::Hide() {
  Save();
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void TodoWindow::Load() {
  items_.clear();
  std::ifstream in(TodosPath(), std::ios::binary);
  if (!in) {
    items_.push_back({false, L"示例待办"});
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::wstring text = WidenUtf8(ss.str());
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find_first_of(L"\r\n", pos);
    if (eol == std::wstring::npos) {
      eol = text.size();
    }
    std::wstring line = text.substr(pos, eol - pos);
    pos = eol;
    while (pos < text.size() && (text[pos] == L'\r' || text[pos] == L'\n')) {
      ++pos;
    }
    if (line.empty()) {
      continue;
    }
    Item it;
    if (line.size() >= 4 &&
        (line.compare(0, 4, L"[x] ") == 0 || line.compare(0, 4, L"[X] ") == 0)) {
      it.done = true;
      it.text = line.substr(4);
    } else if (line.size() >= 4 && line.compare(0, 4, L"[ ] ") == 0) {
      it.done = false;
      it.text = line.substr(4);
    } else if (!line.empty() && line[0] == L'-') {
      it.text = line.substr(1);
      while (!it.text.empty() && it.text[0] == L' ') {
        it.text.erase(it.text.begin());
      }
    } else {
      it.text = line;
    }
    if (!it.text.empty()) {
      items_.push_back(std::move(it));
    }
  }
}

void TodoWindow::Save() {
  if (list_) {
    const int n = ListView_GetItemCount(list_);
    for (int i = 0; i < n && i < static_cast<int>(items_.size()); ++i) {
      items_[static_cast<size_t>(i)].done =
          ListView_GetCheckState(list_, i) != FALSE;
    }
  }
  std::ofstream out(TodosPath(), std::ios::binary);
  if (!out) {
    return;
  }
  for (const auto& it : items_) {
    out << (it.done ? "[x] " : "[ ] ") << NarrowUtf8(it.text) << "\n";
  }
}

void TodoWindow::RefreshList() {
  if (!list_) {
    return;
  }
  ListView_DeleteAllItems(list_);
  for (size_t i = 0; i < items_.size(); ++i) {
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(i);
    item.pszText = const_cast<wchar_t*>(items_[i].text.c_str());
    ListView_InsertItem(list_, &item);
    ListView_SetCheckState(list_, static_cast<int>(i), items_[i].done);
  }
}

void TodoWindow::AddItem() {
  const std::wstring text = PromptText(hwnd_, L"待办", L"新待办内容：", L"");
  if (text.empty()) {
    return;
  }
  items_.push_back({false, text});
  RefreshList();
  Save();
}

void TodoWindow::RemoveSelected() {
  if (!list_) {
    return;
  }
  const int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
  if (sel < 0 || sel >= static_cast<int>(items_.size())) {
    return;
  }
  items_.erase(items_.begin() + sel);
  RefreshList();
  Save();
}

void TodoWindow::ToggleSelected() {
  if (!list_) {
    return;
  }
  const int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
  if (sel < 0 || sel >= static_cast<int>(items_.size())) {
    return;
  }
  const BOOL on = ListView_GetCheckState(list_, sel);
  ListView_SetCheckState(list_, sel, !on);
  items_[static_cast<size_t>(sel)].done = !on;
  Save();
}

LRESULT CALLBACK TodoWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  TodoWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<TodoWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<TodoWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT TodoWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_COMMAND:
      if (LOWORD(wparam) == kAdd) {
        AddItem();
        return 0;
      }
      if (LOWORD(wparam) == kDel) {
        RemoveSelected();
        return 0;
      }
      break;
    case WM_NOTIFY: {
      const auto* hdr = reinterpret_cast<LPNMHDR>(lparam);
      if (hdr && hdr->idFrom == kList && hdr->code == NM_DBLCLK) {
        ToggleSelected();
        return 0;
      }
      if (hdr && hdr->idFrom == kList && hdr->code == LVN_ITEMCHANGED) {
        const auto* lv = reinterpret_cast<NMLISTVIEW*>(lparam);
        if (lv && (lv->uChanged & LVIF_STATE) && lv->iItem >= 0 &&
            lv->iItem < static_cast<int>(items_.size())) {
          items_[static_cast<size_t>(lv->iItem)].done =
              ListView_GetCheckState(list_, lv->iItem) != FALSE;
          Save();
        }
        return 0;
      }
      break;
    }
    case WM_SIZE:
      if (list_ && btn_add_ && btn_del_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        MoveWindow(list_, 6, 6, rc.right - 12, rc.bottom - 48, TRUE);
        ListView_SetColumnWidth(list_, 0, rc.right - 40);
        MoveWindow(btn_add_, 6, rc.bottom - 36, 80, 28, TRUE);
        MoveWindow(btn_del_, 96, rc.bottom - 36, 80, 28, TRUE);
      }
      return 0;
    case WM_CLOSE:
      Hide();
      return 0;
    case WM_DESTROY:
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}
