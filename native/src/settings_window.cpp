#include "settings_window.h"

#include <CommCtrl.h>

#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeSettings";
constexpr int kBtnAdd = 1001;
constexpr int kBtnRemove = 1002;
constexpr int kBtnSeat = 1003;
constexpr int kBtnApply = 1004;
constexpr int kBtnOrgV = 1005;
constexpr int kBtnOrgP = 1006;
constexpr int kBtnRules = 1007;
constexpr int kBtnWhitelist = 1008;
constexpr int kBtnFolders = 1009;
constexpr int kBtnHelp = 1010;
constexpr int kChkOrgStartup = 1011;
constexpr int kChkOrgWatch = 1012;
constexpr int kBtnRecordCfg = 1013;
constexpr int kBtnOrgPreview = 1014;
constexpr int kListOrgPreview = 1110;
constexpr int kListId = 1100;
constexpr int kEditTitle = 1101;
constexpr int kComboPage = 1102;
constexpr int kChkDock = 1103;
constexpr int kChkPet = 1104;
constexpr int kChkAutostart = 1105;
constexpr int kTabs = 1200;
}  // namespace

bool SettingsWindow::Create(HINSTANCE instance) {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES};
  InitCommonControlsEx(&icc);

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
      CW_USEDEFAULT, 540, 680, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

  tabs_ = CreateWindowExW(0, WC_TABCONTROLW, L"",
                          WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 12, 8, 500,
                          36, hwnd_,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabs)),
                          instance, nullptr);
  TCITEMW ti{};
  ti.mask = TCIF_TEXT;
  ti.pszText = const_cast<wchar_t*>(L"分区");
  TabCtrl_InsertItem(tabs_, 0, &ti);
  ti.pszText = const_cast<wchar_t*>(L"整理");
  TabCtrl_InsertItem(tabs_, 1, &ti);
  ti.pszText = const_cast<wchar_t*>(L"工具");
  TabCtrl_InsertItem(tabs_, 2, &ti);

  lbl_hint_ = CreateWindowW(
      L"STATIC",
      L"桌面层分区（在应用窗口之下）· Ctrl+Shift+S 打开本窗口",
      WS_CHILD | WS_VISIBLE, 20, 52, 480, 24, hwnd_, nullptr, instance, nullptr);

  lbl_list_ = CreateWindowW(L"STATIC", L"分区列表", WS_CHILD | WS_VISIBLE, 20, 80,
                            100, 20, hwnd_, nullptr, instance, nullptr);
  list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                          WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 20,
                          102, 480, 150, hwnd_,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
                          instance, nullptr);

  lbl_title_ = CreateWindowW(L"STATIC", L"标题", WS_CHILD | WS_VISIBLE, 20, 266,
                             40, 20, hwnd_, nullptr, instance, nullptr);
  edit_title_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 70,
      262, 220, 24, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditTitle)), instance,
      nullptr);
  lbl_page_ = CreateWindowW(L"STATIC", L"页面", WS_CHILD | WS_VISIBLE, 310, 266,
                            40, 20, hwnd_, nullptr, instance, nullptr);
  combo_page_ = CreateWindowW(
      L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 350, 262, 160,
      240, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboPage)),
      instance, nullptr);
  SetPageNames({L"第 1 页", L"第 2 页"});

  btn_apply_ = CreateWindowW(
      L"BUTTON", L"应用标题/页面", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20,
      300, 140, 28, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnApply)), instance,
      nullptr);
  btn_add_ = CreateWindowW(L"BUTTON", L"框选添加",
                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 170, 300, 100,
                           28, hwnd_,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnAdd)),
                           instance, nullptr);
  btn_remove_ = CreateWindowW(
      L"BUTTON", L"删除选中", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 280, 300,
      100, 28, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnRemove)),
      instance, nullptr);
  btn_seat_ = CreateWindowW(L"BUTTON", L"沉底",
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 390, 300, 90,
                            28, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnSeat)),
                            instance, nullptr);

  chk_dock_ = CreateWindowW(
      L"BUTTON", L"显示 Dock", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 350,
      120, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChkDock)),
      instance, nullptr);
  chk_pet_ = CreateWindowW(
      L"BUTTON", L"显示宠物", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 150, 350,
      120, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChkPet)),
      instance, nullptr);
  chk_autostart_ = CreateWindowW(
      L"BUTTON", L"开机启动", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 280, 350,
      120, 24, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChkAutostart)), instance,
      nullptr);

  btn_org_v_ = CreateWindowW(
      L"BUTTON", L"虚拟整理(钉选)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20,
      120, 160, 32, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnOrgV)), instance,
      nullptr);
  btn_org_p_ = CreateWindowW(
      L"BUTTON", L"物理整理(移文件夹)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      200, 120, 180, 32, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnOrgP)), instance,
      nullptr);
  btn_rules_ = CreateWindowW(
      L"BUTTON", L"编辑扩展名规则…", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20,
      170, 160, 32, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnRules)), instance,
      nullptr);
  btn_whitelist_ = CreateWindowW(
      L"BUTTON", L"整理白名单…", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 200,
      170, 160, 32, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnWhitelist)), instance,
      nullptr);
  chk_org_startup_ = CreateWindowW(
      L"BUTTON", L"启动时自动整理（按规则钉选）",
      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 220, 280, 24, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChkOrgStartup)), instance,
      nullptr);
  chk_org_watch_ = CreateWindowW(
      L"BUTTON", L"监视新文件并自动钉选", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
      20, 250, 280, 24, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChkOrgWatch)), instance,
      nullptr);
  lbl_org_preview_ = CreateWindowW(
      L"STATIC", L"待虚拟整理预览：", WS_CHILD | WS_VISIBLE, 20, 285, 200, 20,
      hwnd_, nullptr, instance, nullptr);
  list_org_preview_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"LISTBOX", L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 20, 310, 480,
      140, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListOrgPreview)), instance,
      nullptr);
  btn_org_preview_ = CreateWindowW(
      L"BUTTON", L"刷新预览", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 360, 250,
      100, 28, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnOrgPreview)), instance,
      nullptr);

  btn_folders_ = CreateWindowW(
      L"BUTTON", L"分页文件夹…", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 120,
      160, 32, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnFolders)), instance,
      nullptr);
  btn_record_cfg_ = CreateWindowW(
      L"BUTTON", L"录屏设置…", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 200, 120,
      120, 32, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnRecordCfg)), instance,
      nullptr);
  btn_help_ = CreateWindowW(
      L"BUTTON", L"帮助", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 340, 120, 100,
      32, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnHelp)),
      instance, nullptr);

  ShowTab(0);
  return true;
}

void SettingsWindow::ShowTab(int index) {
  const bool fences = index == 0;
  const bool organize = index == 1;
  const bool tools = index == 2;
  auto vis = [](HWND h, bool on) {
    if (h) {
      ShowWindow(h, on ? SW_SHOW : SW_HIDE);
    }
  };
  vis(lbl_hint_, fences);
  vis(lbl_list_, fences);
  vis(list_, fences);
  vis(lbl_title_, fences);
  vis(edit_title_, fences);
  vis(lbl_page_, fences);
  vis(combo_page_, fences);
  vis(btn_apply_, fences);
  vis(btn_add_, fences);
  vis(btn_remove_, fences);
  vis(btn_seat_, fences);
  vis(chk_dock_, fences);
  vis(chk_pet_, fences);
  vis(chk_autostart_, fences);

  vis(btn_org_v_, organize);
  vis(btn_org_p_, organize);
  vis(btn_rules_, organize);
  vis(btn_whitelist_, organize);
  vis(chk_org_startup_, organize);
  vis(chk_org_watch_, organize);
  vis(lbl_org_preview_, organize);
  vis(list_org_preview_, organize);
  vis(btn_org_preview_, organize);
  if (organize && on_org_preview_) {
    on_org_preview_();
  }

  vis(btn_folders_, tools);
  vis(btn_record_cfg_, tools);
  vis(btn_help_, tools);
}

void SettingsWindow::ApplyStyle(COLORREF background, COLORREF accent,
                                COLORREF title) {
  bg_ = background;
  accent_ = accent;
  text_ = title;
  if (brush_) {
    DeleteObject(brush_);
    brush_ = nullptr;
  }
  brush_ = CreateSolidBrush(bg_);
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
  (void)accent_;
}

void SettingsWindow::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (brush_) {
    DeleteObject(brush_);
    brush_ = nullptr;
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

void SettingsWindow::SetPageNames(const std::vector<std::wstring>& names) {
  page_names_ = names.empty() ? std::vector<std::wstring>{L"第 1 页", L"第 2 页"}
                              : names;
  if (!combo_page_) {
    return;
  }
  const int prev =
      static_cast<int>(SendMessageW(combo_page_, CB_GETCURSEL, 0, 0));
  SendMessageW(combo_page_, CB_RESETCONTENT, 0, 0);
  for (const auto& name : page_names_) {
    SendMessageW(combo_page_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(name.c_str()));
  }
  int sel = prev;
  if (sel < 0 || sel >= static_cast<int>(page_names_.size())) {
    sel = 0;
  }
  SendMessageW(combo_page_, CB_SETCURSEL, sel, 0);
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

void SettingsWindow::SetToggles(bool dock_on, bool pet_on, bool autostart_on) {
  if (chk_dock_) {
    SendMessageW(chk_dock_, BM_SETCHECK, dock_on ? BST_CHECKED : BST_UNCHECKED,
                 0);
  }
  if (chk_pet_) {
    SendMessageW(chk_pet_, BM_SETCHECK, pet_on ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  if (chk_autostart_) {
    SendMessageW(chk_autostart_, BM_SETCHECK,
                 autostart_on ? BST_CHECKED : BST_UNCHECKED, 0);
  }
}

void SettingsWindow::SetOrganizePrefs(bool startup_on, bool watch_on) {
  if (chk_org_startup_) {
    SendMessageW(chk_org_startup_, BM_SETCHECK,
                 startup_on ? BST_CHECKED : BST_UNCHECKED, 0);
  }
  if (chk_org_watch_) {
    SendMessageW(chk_org_watch_, BM_SETCHECK,
                 watch_on ? BST_CHECKED : BST_UNCHECKED, 0);
  }
}

void SettingsWindow::SetOrganizePreview(const std::vector<std::wstring>& names) {
  if (!list_org_preview_) {
    return;
  }
  SendMessageW(list_org_preview_, LB_RESETCONTENT, 0, 0);
  if (names.empty()) {
    SendMessageW(list_org_preview_, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"（没有待钉选项）"));
    return;
  }
  for (const auto& n : names) {
    SendMessageW(list_org_preview_, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(n.c_str()));
  }
}

void SettingsWindow::SetHandlers(VoidFn on_add, IndexFn on_remove, VoidFn on_seat,
                                 RenameFn on_rename, PageFn on_page,
                                 VoidFn on_org_virtual, VoidFn on_org_physical,
                                 BoolFn on_dock, BoolFn on_pet,
                                 VoidFn on_edit_rules, BoolFn on_autostart,
                                 VoidFn on_whitelist, VoidFn on_page_folders,
                                 VoidFn on_help, BoolFn on_org_startup,
                                 BoolFn on_org_watch, VoidFn on_record_cfg,
                                 VoidFn on_org_preview) {
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
  on_autostart_ = std::move(on_autostart);
  on_whitelist_ = std::move(on_whitelist);
  on_page_folders_ = std::move(on_page_folders);
  on_help_ = std::move(on_help);
  on_org_startup_ = std::move(on_org_startup);
  on_org_watch_ = std::move(on_org_watch);
  on_record_cfg_ = std::move(on_record_cfg);
  on_org_preview_ = std::move(on_org_preview);
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
    case WM_NOTIFY: {
      const auto* hdr = reinterpret_cast<LPNMHDR>(lparam);
      if (hdr && hdr->idFrom == kTabs && hdr->code == TCN_SELCHANGE) {
        ShowTab(TabCtrl_GetCurSel(tabs_));
        return 0;
      }
      break;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      const int code = HIWORD(wparam);
      if (id == kListId && code == LBN_SELCHANGE) {
        const int sel = SelectedIndex();
        if (sel >= 0 && edit_title_) {
          wchar_t buf[512] = {};
          SendMessageW(list_, LB_GETTEXT, sel, reinterpret_cast<LPARAM>(buf));
          const std::wstring full = buf;
          const size_t p = full.find(L"  p");
          SetWindowTextW(edit_title_,
                         (p == std::wstring::npos ? full : full.substr(0, p))
                             .c_str());
          const size_t bracket = full.rfind(L"  [");
          const size_t marker = full.rfind(
              L"  p", bracket == std::wstring::npos ? std::wstring::npos
                                                   : bracket);
          if (marker != std::wstring::npos && combo_page_) {
            const int page = _wtoi(full.c_str() + marker + 3) - 1;
            if (page >= 0) {
              SendMessageW(combo_page_, CB_SETCURSEL, page, 0);
            }
          }
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
      if (id == kBtnWhitelist && on_whitelist_) {
        on_whitelist_();
        return 0;
      }
      if (id == kBtnFolders && on_page_folders_) {
        on_page_folders_();
        return 0;
      }
      if (id == kBtnOrgPreview && on_org_preview_) {
        on_org_preview_();
        return 0;
      }
      if (id == kBtnRecordCfg && on_record_cfg_) {
        on_record_cfg_();
        return 0;
      }
      if (id == kBtnHelp && on_help_) {
        on_help_();
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
      if (id == kChkAutostart && code == BN_CLICKED && on_autostart_) {
        on_autostart_(SendMessageW(chk_autostart_, BM_GETCHECK, 0, 0) ==
                      BST_CHECKED);
        return 0;
      }
      if (id == kChkOrgStartup && code == BN_CLICKED && on_org_startup_) {
        on_org_startup_(SendMessageW(chk_org_startup_, BM_GETCHECK, 0, 0) ==
                        BST_CHECKED);
        return 0;
      }
      if (id == kChkOrgWatch && code == BN_CLICKED && on_org_watch_) {
        on_org_watch_(SendMessageW(chk_org_watch_, BM_GETCHECK, 0, 0) ==
                      BST_CHECKED);
        return 0;
      }
      break;
    }
    case WM_ERASEBKGND: {
      HDC hdc = reinterpret_cast<HDC>(wparam);
      RECT rc{};
      GetClientRect(hwnd_, &rc);
      HBRUSH br = brush_ ? brush_ : reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
      FillRect(hdc, &rc, br);
      return 1;
    }
    case WM_CTLCOLORSTATIC: {
      HDC hdc = reinterpret_cast<HDC>(wparam);
      SetTextColor(hdc, text_);
      SetBkColor(hdc, bg_);
      return reinterpret_cast<LRESULT>(brush_ ? brush_
                                             : GetStockObject(NULL_BRUSH));
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
