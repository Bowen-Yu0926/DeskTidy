#include "search_window.h"

#include "icon_actions.h"
#include "tool_paths.h"

#include <ShlObj.h>
#include <Shellapi.h>
#include <windowsx.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeSearch";
constexpr int kEdit = 4001;
constexpr int kList = 4002;
constexpr int kBtn = 4003;
constexpr int kOpen = 4004;
constexpr int kReveal = 4005;
WNDPROC g_old_edit = nullptr;

LRESULT CALLBACK EditSubclass(HWND hwnd, UINT msg, WPARAM wparam,
                              LPARAM lparam) {
  if (msg == WM_KEYDOWN && wparam == VK_RETURN) {
    HWND parent = GetParent(hwnd);
    if (parent) {
      SendMessageW(parent, WM_COMMAND, MAKEWPARAM(kBtn, BN_CLICKED),
                   reinterpret_cast<LPARAM>(hwnd));
    }
    return 0;
  }
  return CallWindowProcW(g_old_edit, hwnd, msg, wparam, lparam);
}

bool NameContains(const std::wstring& name, const std::wstring& needle) {
  if (needle.empty()) {
    return true;
  }
  for (size_t i = 0; i + needle.size() <= name.size(); ++i) {
    bool ok = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      wchar_t a = name[i + j];
      wchar_t b = needle[j];
      if (a >= L'A' && a <= L'Z') {
        a = static_cast<wchar_t>(a - L'A' + L'a');
      }
      if (b >= L'A' && b <= L'Z') {
        b = static_cast<wchar_t>(b - L'A' + L'a');
      }
      if (a != b) {
        ok = false;
        break;
      }
    }
    if (ok) {
      return true;
    }
  }
  return false;
}

void SearchRecursive(const std::wstring& dir, const std::wstring& needle,
                     std::vector<std::wstring>* out, int* budget) {
  if (!out || !budget || *budget <= 0 || needle.empty()) {
    return;
  }
  const std::wstring pattern = dir + L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == L'\0' ||
         (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
      continue;
    }
    const std::wstring name = fd.cFileName;
    const std::wstring full = dir + L"\\" + name;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (_wcsicmp(name.c_str(), L"Windows") == 0 ||
          _wcsicmp(name.c_str(), L"$Recycle.Bin") == 0 ||
          _wcsicmp(name.c_str(), L"AppData") == 0) {
        continue;
      }
      SearchRecursive(full, needle, out, budget);
    } else {
      if (NameContains(name, needle)) {
        out->push_back(full);
        --(*budget);
        if (*budget <= 0) {
          break;
        }
      }
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
}
}  // namespace

bool SearchWindow::Create(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = SearchWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(
      WS_EX_APPWINDOW, kClass, L"文件搜索", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
      CW_USEDEFAULT, 640, 480, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                          WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 12, 12, 480,
                          26, hwnd_,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEdit)),
                          instance, nullptr);
  btn_ = CreateWindowW(L"BUTTON", L"搜索", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON |
                                               BS_DEFPUSHBUTTON,
                       500, 10, 100, 30, hwnd_,
                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtn)),
                       instance, nullptr);
  list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                          WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 12, 50,
                          590, 320, hwnd_,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                          instance, nullptr);
  btn_open_ = CreateWindowW(
      L"BUTTON", L"打开", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 12, 380, 100,
      28, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOpen)), instance,
      nullptr);
  btn_reveal_ = CreateWindowW(
      L"BUTTON", L"在资源管理器中显示", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
      122, 380, 160, 28, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kReveal)), instance, nullptr);
  g_old_edit = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(edit_, GWLP_WNDPROC,
                        reinterpret_cast<LONG_PTR>(EditSubclass)));
  return true;
}

int SearchWindow::SelectedIndex() const {
  if (!list_) {
    return -1;
  }
  return static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
}

void SearchWindow::OpenSelected() {
  const int sel = SelectedIndex();
  if (sel >= 0 && sel < static_cast<int>(results_.size())) {
    ShellExecuteW(nullptr, L"open", results_[static_cast<size_t>(sel)].c_str(),
                  nullptr, nullptr, SW_SHOWNORMAL);
  }
}

void SearchWindow::RevealSelected() {
  const int sel = SelectedIndex();
  if (sel < 0 || sel >= static_cast<int>(results_.size())) {
    return;
  }
  const std::wstring arg =
      L"/select,\"" + results_[static_cast<size_t>(sel)] + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr,
                SW_SHOWNORMAL);
}

void SearchWindow::Destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void SearchWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
}

void SearchWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void SearchWindow::RunSearch() {
  if (!edit_ || !list_) {
    return;
  }
  wchar_t needle[256] = {};
  GetWindowTextW(edit_, needle, 256);
  results_.clear();
  SendMessageW(list_, LB_RESETCONTENT, 0, 0);
  if (needle[0] == L'\0') {
    return;
  }

  // Prefer fd.exe when available.
  const std::wstring fd = FindFdExe();
  if (!fd.empty()) {
    PWSTR profile = nullptr;
    std::wstring root = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) &&
        profile) {
      root = profile;
      CoTaskMemFree(profile);
    }
    std::wstring cmd = L"\"" + fd + L"\" -i -a --max-results 200 " + needle +
                       L" \"" + root + L"\"";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (CreatePipe(&rd, &wr, &sa, 0)) {
      SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
      STARTUPINFOW si{};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdOutput = wr;
      si.hStdError = wr;
      PROCESS_INFORMATION pi{};
      std::vector<wchar_t> buf(cmd.begin(), cmd.end());
      buf.push_back(L'\0');
      if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        wr = nullptr;
        CloseHandle(pi.hThread);
        std::string utf8;
        char chunk[4096];
        DWORD n = 0;
        while (ReadFile(rd, chunk, sizeof(chunk), &n, nullptr) && n) {
          utf8.append(chunk, chunk + n);
        }
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        size_t pos = 0;
        while (pos < utf8.size()) {
          size_t eol = utf8.find('\n', pos);
          if (eol == std::string::npos) {
            eol = utf8.size();
          }
          std::string line = utf8.substr(pos, eol - pos);
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          if (!line.empty()) {
            const int wn =
                MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, nullptr, 0);
            std::wstring wide(wn > 0 ? wn - 1 : 0, L'\0');
            if (wn > 1) {
              MultiByteToWideChar(CP_UTF8, 0, line.c_str(), -1, wide.data(), wn);
            }
            results_.push_back(wide);
            SendMessageW(list_, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(wide.c_str()));
          }
          pos = eol + 1;
        }
        CloseHandle(rd);
        return;
      }
      if (wr) {
        CloseHandle(wr);
      }
      CloseHandle(rd);
    }
  }

  int budget = 200;
  PWSTR profile = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) &&
      profile) {
    SearchRecursive(profile, needle, &results_, &budget);
    CoTaskMemFree(profile);
  }
  for (const auto& r : results_) {
    SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(r.c_str()));
  }
}

LRESULT CALLBACK SearchWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                       LPARAM lparam) {
  SearchWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<SearchWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<SearchWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT SearchWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_COMMAND:
      if (LOWORD(wparam) == kBtn) {
        RunSearch();
        return 0;
      }
      if (LOWORD(wparam) == kOpen) {
        OpenSelected();
        return 0;
      }
      if (LOWORD(wparam) == kReveal) {
        RevealSelected();
        return 0;
      }
      if (LOWORD(wparam) == kList && HIWORD(wparam) == LBN_DBLCLK) {
        OpenSelected();
        return 0;
      }
      if (LOWORD(wparam) == kEdit && HIWORD(wparam) == EN_CHANGE) {
        break;
      }
      break;
    case WM_CONTEXTMENU:
      if (reinterpret_cast<HWND>(wparam) == list_ ||
          GetFocus() == list_) {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (pt.x == -1 && pt.y == -1) {
          GetCursorPos(&pt);
        }
        const int sel = SelectedIndex();
        if (sel < 0 || sel >= static_cast<int>(results_.size())) {
          return 0;
        }
        const std::wstring path = results_[static_cast<size_t>(sel)];
        const int cmd = TrackIconShellMenu(hwnd_, pt, false);
        if (cmd == 1) {
          OpenSelected();
        } else if (cmd == 2) {
          RevealSelected();
        } else if (cmd == 4 || cmd == 5) {
          CopyOrCutShellPath(hwnd_, path, cmd == 5);
        } else if (cmd == 6) {
          CopyShellPathText(hwnd_, path);
        } else if (cmd == 7) {
          std::wstring dest;
          if (RenameShellPath(hwnd_, path, &dest) == IconFsResult::Ok) {
            results_[static_cast<size_t>(sel)] = dest;
            SendMessageW(list_, LB_DELETESTRING, static_cast<WPARAM>(sel), 0);
            SendMessageW(list_, LB_INSERTSTRING, static_cast<WPARAM>(sel),
                         reinterpret_cast<LPARAM>(dest.c_str()));
            SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(sel), 0);
            if (on_path_replaced_) {
              on_path_replaced_(path, dest);
            }
          }
        } else if (cmd == 8) {
          ShowShellPathProperties(hwnd_, path);
        } else if (cmd == 9) {
          if (RecycleShellPath(hwnd_, path) == IconFsResult::Ok) {
            results_.erase(results_.begin() + sel);
            SendMessageW(list_, LB_DELETESTRING, static_cast<WPARAM>(sel), 0);
            if (on_removed_) {
              on_removed_(path);
            }
          }
        }
        return 0;
      }
      break;
    case WM_KEYDOWN:
      if (wparam == VK_RETURN) {
        if (GetFocus() == edit_) {
          RunSearch();
        } else {
          OpenSelected();
        }
        return 0;
      }
      break;
    case WM_SIZE:
      if (edit_ && list_ && btn_ && btn_open_ && btn_reveal_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        MoveWindow(edit_, 12, 12, rc.right - 140, 26, TRUE);
        MoveWindow(btn_, rc.right - 116, 10, 100, 30, TRUE);
        MoveWindow(list_, 12, 50, rc.right - 24, rc.bottom - 90, TRUE);
        MoveWindow(btn_open_, 12, rc.bottom - 36, 100, 28, TRUE);
        MoveWindow(btn_reveal_, 122, rc.bottom - 36, 160, 28, TRUE);
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
