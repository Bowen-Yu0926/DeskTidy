#include "vault_window.h"

#include "paths_util.h"

#include <commdlg.h>
#include <wincrypt.h>

#include <fstream>
#include <sstream>
#include <string>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {
constexpr wchar_t kClass[] = L"DeskTidyNativeVault";
constexpr int kList = 5001;
constexpr int kTitle = 5002;
constexpr int kUser = 5003;
constexpr int kPass = 5004;
constexpr int kUrl = 5005;
constexpr int kAdd = 5006;
constexpr int kDel = 5007;
constexpr int kExport = 5008;
constexpr int kImport = 5009;

std::wstring VaultPath() { return JoinPath(NativeDataDir(), L"vault.dpapi"); }

bool ProtectBlob(const std::string& plain, std::vector<BYTE>* out) {
  DATA_BLOB in{};
  in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
  in.cbData = static_cast<DWORD>(plain.size());
  DATA_BLOB enc{};
  if (!CryptProtectData(&in, L"DeskTidyNativeVault", nullptr, nullptr, nullptr,
                        0, &enc)) {
    return false;
  }
  out->assign(enc.pbData, enc.pbData + enc.cbData);
  LocalFree(enc.pbData);
  return true;
}

bool UnprotectBlob(const std::vector<BYTE>& enc, std::string* plain) {
  DATA_BLOB in{};
  in.pbData = const_cast<BYTE*>(enc.data());
  in.cbData = static_cast<DWORD>(enc.size());
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
    return false;
  }
  plain->assign(reinterpret_cast<char*>(out.pbData), out.cbData);
  LocalFree(out.pbData);
  return true;
}
}  // namespace

bool VaultWindow::Create(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = VaultWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"账号保险库 (本地 DPAPI)",
                          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 560,
                          480, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                          WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY, 12, 12,
                          520, 200, hwnd_,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kList)),
                          instance, nullptr);
  CreateWindowW(L"STATIC", L"标题", WS_CHILD | WS_VISIBLE, 12, 230, 40, 20, hwnd_,
                nullptr, instance, nullptr);
  edit_title_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 60,
      228, 200, 24, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTitle)), instance, nullptr);
  CreateWindowW(L"STATIC", L"用户", WS_CHILD | WS_VISIBLE, 280, 230, 40, 20,
                hwnd_, nullptr, instance, nullptr);
  edit_user_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 320,
      228, 200, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUser)),
      instance, nullptr);
  CreateWindowW(L"STATIC", L"密码", WS_CHILD | WS_VISIBLE, 12, 270, 40, 20, hwnd_,
                nullptr, instance, nullptr);
  edit_pass_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD, 60, 268, 200, 24,
      hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPass)), instance,
      nullptr);
  CreateWindowW(L"STATIC", L"网址", WS_CHILD | WS_VISIBLE, 280, 270, 40, 20,
                hwnd_, nullptr, instance, nullptr);
  edit_url_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 320,
      268, 200, 24, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kUrl)),
      instance, nullptr);
  CreateWindowW(L"BUTTON", L"添加", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 12,
                320, 100, 30, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAdd)), instance,
                nullptr);
  CreateWindowW(L"BUTTON", L"删除选中", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 120,
                320, 100, 30, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDel)), instance,
                nullptr);
  CreateWindowW(L"BUTTON", L"导出备份", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 240,
                320, 100, 30, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kExport)), instance,
                nullptr);
  CreateWindowW(L"BUTTON", L"导入备份", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 350,
                320, 100, 30, hwnd_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kImport)), instance,
                nullptr);
  CreateWindowW(L"STATIC",
                L"本地 DPAPI 加密。导出为当前用户可解密的备份文件（可换机需同账号）。",
                WS_CHILD | WS_VISIBLE, 12, 370, 520, 40, hwnd_, nullptr,
                instance, nullptr);
  Load();
  RefreshList();
  return true;
}

void VaultWindow::Destroy() {
  Save();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void VaultWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
}

void VaultWindow::Hide() {
  Save();
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void VaultWindow::RefreshList() {
  if (!list_) {
    return;
  }
  SendMessageW(list_, LB_RESETCONTENT, 0, 0);
  for (const auto& e : entries_) {
    std::wstring line = e.title + L"  <" + e.user + L">";
    SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
  }
}

void VaultWindow::AddFromFields() {
  Entry e;
  wchar_t buf[512] = {};
  GetWindowTextW(edit_title_, buf, 512);
  e.title = buf;
  GetWindowTextW(edit_user_, buf, 512);
  e.user = buf;
  GetWindowTextW(edit_pass_, buf, 512);
  e.pass = buf;
  GetWindowTextW(edit_url_, buf, 512);
  e.url = buf;
  if (e.title.empty()) {
    return;
  }
  entries_.push_back(std::move(e));
  SetWindowTextW(edit_title_, L"");
  SetWindowTextW(edit_user_, L"");
  SetWindowTextW(edit_pass_, L"");
  SetWindowTextW(edit_url_, L"");
  Save();
  RefreshList();
}

void VaultWindow::Save() {
  std::ostringstream ss;
  for (const auto& e : entries_) {
    ss << NarrowUtf8(e.title) << '\t' << NarrowUtf8(e.user) << '\t'
       << NarrowUtf8(e.pass) << '\t' << NarrowUtf8(e.url) << '\n';
  }
  std::vector<BYTE> enc;
  if (!ProtectBlob(ss.str(), &enc)) {
    return;
  }
  std::ofstream out(VaultPath(), std::ios::binary);
  if (out) {
    out.write(reinterpret_cast<const char*>(enc.data()),
              static_cast<std::streamsize>(enc.size()));
  }
}

void VaultWindow::Load() {
  entries_.clear();
  std::ifstream in(VaultPath(), std::ios::binary);
  if (!in) {
    return;
  }
  std::vector<BYTE> enc((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  std::string plain;
  if (!UnprotectBlob(enc, &plain)) {
    return;
  }
  std::istringstream ss(plain);
  std::string line;
  while (std::getline(ss, line)) {
    if (line.empty()) {
      continue;
    }
    Entry e;
    size_t p0 = 0;
    auto next = [&](std::wstring* field) {
      const size_t p1 = line.find('\t', p0);
      const std::string part =
          (p1 == std::string::npos) ? line.substr(p0) : line.substr(p0, p1 - p0);
      *field = WidenUtf8(part);
      p0 = (p1 == std::string::npos) ? line.size() : p1 + 1;
    };
    next(&e.title);
    next(&e.user);
    next(&e.pass);
    next(&e.url);
    entries_.push_back(std::move(e));
  }
}

LRESULT CALLBACK VaultWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
  VaultWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<VaultWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<VaultWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

void VaultWindow::ExportBackup() {
  Save();
  wchar_t file[MAX_PATH] = L"vault_backup.dpapi";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Vault backup\0*.dpapi\0All\0*.*\0";
  ofn.Flags = OFN_OVERWRITEPROMPT;
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  if (CopyFileW(VaultPath().c_str(), file, FALSE)) {
    MessageBoxW(hwnd_, L"已导出。", L"保险库", MB_OK | MB_ICONINFORMATION);
  }
}

void VaultWindow::ImportBackup() {
  wchar_t file[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Vault backup\0*.dpapi\0All\0*.*\0";
  ofn.Flags = OFN_FILEMUSTEXIST;
  if (!GetOpenFileNameW(&ofn)) {
    return;
  }
  if (!CopyFileW(file, VaultPath().c_str(), FALSE)) {
    MessageBoxW(hwnd_, L"导入失败。", L"保险库", MB_OK | MB_ICONWARNING);
    return;
  }
  Load();
  RefreshList();
  MessageBoxW(hwnd_, L"已导入。", L"保险库", MB_OK | MB_ICONINFORMATION);
}

LRESULT VaultWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_COMMAND:
      if (LOWORD(wparam) == kAdd) {
        AddFromFields();
        return 0;
      }
      if (LOWORD(wparam) == kDel && list_) {
        const int sel =
            static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(entries_.size())) {
          entries_.erase(entries_.begin() + sel);
          Save();
          RefreshList();
        }
        return 0;
      }
      if (LOWORD(wparam) == kExport) {
        ExportBackup();
        return 0;
      }
      if (LOWORD(wparam) == kImport) {
        ImportBackup();
        return 0;
      }
      break;
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
