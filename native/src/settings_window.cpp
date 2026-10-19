#include "settings_window.h"

#include <string>

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeSettings";
constexpr int kBtnAdd = 1001;
constexpr int kBtnRemove = 1002;
constexpr int kBtnSeat = 1003;
constexpr int kBtnApply = 1004;
constexpr int kBtnOrgV = 1005;
constexpr int kBtnOrgP = 1006;
constexpr int kBtnRules = 1007;
constexpr int kListId = 1100;
constexpr int kEditTitle = 1101;
constexpr int kComboPage = 1102;
constexpr int kChkDock = 1103;
constexpr int kChkPet = 1104;
}  // namespace

bool SettingsWindow::Create(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = SettingsWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(
      WS_EX_APPWINDOW, kClassName, L"DeskTidy Native - 设置",
      WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, CW_USEDEFAULT,
      CW_USEDEFAULT, 520, 560, nullptr, nullptr, instance, this);

  if (!hwnd_) {
    return false;
  }

  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

  CreateWindowW(L"STATIC",
                L"桌面层分区（在应用窗口之下）\r\n"
                L"热键: Ctrl+Shift+S 设置 · Ctrl+Alt+1/2 换页",
                WS_CHILD | WS_VISIBLE, 20, 12, 460, 40, hwnd_, nullptr, instance,
                nullptr);

  CreateWindowW(L"STATIC", L"分区列表", WS_CHILD | WS_VISIBLE, 20, 56, 100, 20,
                hwnd_, nullptr, instance, nullptr);

  list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                          WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 20,
                          78, 460, 160, hwnd_,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
                          instance, nullptr);

  CreateWindowW(L"STATIC", L"标题", WS_CHILD | WS_VISIBLE, 20, 250, 40, 20,
                hwnd_, nullptr, instance, nullptr);
  edit_title_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 70,
      246, 220, 24, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditTitle)), instance,
      nullptr);

  CreateWindowW(L"STATIC", L"页面", WS_CHILD | WS_VISIBLE, 300, 250, 40, 20,
                hwnd_, nullptr, instance, nullptr);
  combo_page_ = CreateWindowW(
      L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 340, 246, 80,
      120, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboPage)),
      instance, nullptr);
  SendMessageW(combo_page_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"第1页"));
  SendMessageW(combo_page_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"第2页"));
  SendMessageW(combo_page_, CB_SETCURSEL, 0, 0);

  CreateWindowW(L"BUTTON", L"应用标题/页面", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                20, 284, 140, 28, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnApply)),
                instance, nullptr);
  CreateWindowW(L"BUTTON", L"添加分区", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 170,
                284, 100, 28, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnAdd)), instance,
                nullptr);
  CreateWindowW(L"BUTTON", L"删除选中", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280,
                284, 100, 28, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnRemove)),
                instance, nullptr);
  CreateWindowW(L"BUTTON", L"沉底", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 390,
                284, 90, 28, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnSeat)),
                instance, nullptr);

  chk_dock_ = CreateWindowW(
      L"BUTTON", L"显示 Dock", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 330,
      120, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChkDock)),
      instance, nullptr);
  chk_pet_ = CreateWindowW(
      L"BUTTON", L"显示宠物", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 150, 330,
      120, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChkPet)),
      instance, nullptr);

  CreateWindowW(L"BUTTON", L"虚拟整理(钉选)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                20, 370, 140, 30, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnOrgV)),
                instance, nullptr);
  CreateWindowW(L"BUTTON", L"物理整理(移文件夹)",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 170, 370, 160, 30, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnOrgP)),
                instance, nullptr);
  CreateWindowW(L"BUTTON", L"编辑扩展名规则…",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 340, 370, 140, 30, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnRules)),
                instance, nullptr);

  CreateWindowW(L"STATIC",
                L"分区：拖边框缩放 · 滚轮滚动 · 右键菜单\r\n"
                L"图标：拖到分区钉选 · 拖出到资源管理器",
                WS_CHILD | WS_VISIBLE, 20, 420, 460, 50, hwnd_, nullptr,
                instance, nullptr);
  return true;
}

void SettingsWindow::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void SettingsWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

void SettingsWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

bool SettingsWindow::IsOpen() const {
  return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
}

void SettingsWindow::SetFenceNames(const std::vector<std::wstring>& names) {
  if (!list_) {
    return;
  }
  SendMessageW(list_, LB_RESETCONTENT, 0, 0);
  for (const auto& name : names) {
    SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
  }
}

void SettingsWindow::SetToggles(bool dock_on, bool pet_on) {
  if (chk_dock_) {
    SendMessageW(chk_dock_, BM_SETCHECK, dock_on ? BST_CHECKED : BST_UNCHECKED,
                 0);
  }
  if (chk_pet_) {
    SendMessageW(chk_pet_, BM_SETCHECK, pet_on ? BST_CHECKED : BST_UNCHECKED, 0);
  }
}

void SettingsWindow::SetHandlers(VoidFn on_add, IndexFn on_remove, VoidFn on_seat,
                                 RenameFn on_rename, PageFn on_page,
                                 VoidFn on_org_virtual, VoidFn on_org_physical,
                                 BoolFn on_dock, BoolFn on_pet,
                                 VoidFn on_edit_rules) {
  on_add_ = std::move(on_add);
  on_remove_ = std::move(on_remove);
  on_seat_ = std::move(on_seat);
  on_rename_ = std::move(on_rename);
  on_page_ = std::move(on_page);
  on_org_virtual_ = std::move(on_org_virtual);
  on_org_physical_ = std::move(on_org_physical);
  on_dock_ = std::move(on_dock);
  on_pet_ = std::move(on_pet);
  on_edit_rules_ = std::move(on_edit_rules);
}

int SettingsWindow::SelectedIndex() const {
  if (!list_) {
    return -1;
  }
  return static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam) {
  SettingsWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<SettingsWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT SettingsWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      const int code = HIWORD(wparam);
      if (id == kListId && code == LBN_SELCHANGE) {
        const int sel = SelectedIndex();
        if (sel >= 0 && edit_title_) {
          wchar_t buf[512] = {};
          SendMessageW(list_, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(buf));
          // Title is before "  p"
          std::wstring s = buf;
          const size_t p = s.find(L"  p");
          if (p != std::wstring::npos) {
            s.resize(p);
          }
          SetWindowTextW(edit_title_, s.c_str());
        }
        return 0;
      }
      if (id == kBtnAdd && on_add_) {
        on_add_();
        return 0;
      }
      if (id == kBtnRemove && on_remove_) {
        const int sel = SelectedIndex();
        if (sel >= 0) {
          on_remove_(sel);
        }
        return 0;
      }
      if (id == kBtnSeat && on_seat_) {
        on_seat_();
        return 0;
      }
      if (id == kBtnApply) {
        const int sel = SelectedIndex();
        if (sel >= 0) {
          wchar_t title[256] = {};
          GetWindowTextW(edit_title_, title, 256);
          if (on_rename_ && title[0]) {
            on_rename_(sel, title);
          }
          if (on_page_ && combo_page_) {
            const int page =
                static_cast<int>(SendMessageW(combo_page_, CB_GETCURSEL, 0, 0));
            if (page >= 0) {
              on_page_(sel, page);
            }
          }
        }
        return 0;
      }
      if (id == kBtnOrgV && on_org_virtual_) {
        on_org_virtual_();
        return 0;
      }
      if (id == kBtnOrgP && on_org_physical_) {
        on_org_physical_();
        return 0;
      }
      if (id == kBtnRules && on_edit_rules_) {
        on_edit_rules_();
        return 0;
      }
      if (id == kChkDock && code == BN_CLICKED && on_dock_) {
        on_dock_(SendMessageW(chk_dock_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        return 0;
      }
      if (id == kChkPet && code == BN_CLICKED && on_pet_) {
        on_pet_(SendMessageW(chk_pet_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        return 0;
      }
      break;
    }
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
