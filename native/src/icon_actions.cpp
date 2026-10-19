#include "icon_actions.h"

#include "prompt_text.h"
#include "shell_clipboard.h"
#include "toast.h"

#include <Shellapi.h>

namespace {
std::wstring FileNameOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring ParentOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}
}  // namespace

void OpenShellPath(const std::wstring& path) {
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void RevealShellPath(const std::wstring& path) {
  const std::wstring arg = L"/select,\"" + path + L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr,
                SW_SHOWNORMAL);
}

bool CopyOrCutShellPath(HWND owner, const std::wstring& path, bool cut) {
  if (!ClipboardSetFiles(owner, {path}, cut)) {
    return false;
  }
  ShowToast(cut ? L"已剪切到剪贴板" : L"已复制到剪贴板");
  return true;
}

bool CopyShellPathText(HWND owner, const std::wstring& path) {
  if (!ClipboardSetText(owner, path)) {
    return false;
  }
  ShowToast(L"路径已复制");
  return true;
}

IconFsResult RenameShellPath(HWND owner, const std::wstring& path,
                             std::wstring* dest) {
  const std::wstring name = FileNameOf(path);
  const std::wstring neu = PromptText(owner, L"重命名", L"新名称：", name);
  if (neu.empty() || neu == name) {
    return IconFsResult::Cancelled;
  }
  if (neu.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
    ShowToast(L"名称包含非法字符");
    return IconFsResult::Failed;
  }
  const std::wstring parent = ParentOf(path);
  const std::wstring next = parent.empty() ? neu : parent + L"\\" + neu;
  if (!MoveFileW(path.c_str(), next.c_str())) {
    ShowToast(L"无法重命名，文件可能被占用");
    return IconFsResult::Failed;
  }
  if (dest) {
    *dest = next;
  }
  ShowToast(L"已重命名");
  return IconFsResult::Ok;
}

bool ShowShellPathProperties(HWND owner, const std::wstring& path) {
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_INVOKEIDLIST;
  sei.hwnd = owner;
  sei.lpVerb = L"properties";
  sei.lpFile = path.c_str();
  sei.nShow = SW_SHOW;
  if (!ShellExecuteExW(&sei)) {
    ShowToast(L"无法打开属性");
    return false;
  }
  return true;
}

IconFsResult RecycleShellPath(HWND owner, const std::wstring& path) {
  if (MessageBoxW(owner, L"将此项移到回收站？", L"删除",
                  MB_YESNO | MB_ICONQUESTION) != IDYES) {
    return IconFsResult::Cancelled;
  }
  std::wstring from = path;
  from.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.hwnd = owner;
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  const int rc = SHFileOperationW(&op);
  if (rc != 0 || op.fAnyOperationsAborted) {
    ShowToast(L"未能移到回收站");
    return IconFsResult::Failed;
  }
  return IconFsResult::Ok;
}

int TrackIconShellMenu(HWND owner, POINT screen, bool include_unpin) {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1, L"打开");
  AppendMenuW(menu, MF_STRING, 2, L"在资源管理器中显示");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 4, L"复制");
  AppendMenuW(menu, MF_STRING, 5, L"剪切");
  AppendMenuW(menu, MF_STRING, 6, L"复制路径");
  AppendMenuW(menu, MF_STRING, 7, L"重命名…");
  AppendMenuW(menu, MF_STRING, 8, L"属性");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 9, L"删除");
  if (include_unpin) {
    AppendMenuW(menu, MF_STRING, 3, L"取消钉选");
  }
  SetForegroundWindow(owner);
  const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x,
                                 screen.y, 0, owner, nullptr);
  DestroyMenu(menu);
  return cmd;
}
