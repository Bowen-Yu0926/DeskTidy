#include "icon_actions.h"

#include "namespace_icons.h"

#include "i18n.h"
#include "mist_msg.h"
#include "prompt_text.h"
#include "shell_clipboard.h"
#include "toast.h"

#include <ShlObj.h>
#include <Shellapi.h>

namespace {
std::wstring FileNameOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Trim leading/trailing spaces (and tabs).
std::wstring TrimName(const std::wstring& s) {
  size_t b = 0;
  while (b < s.size() && (s[b] == L' ' || s[b] == L'\t')) {
    ++b;
  }
  size_t e = s.size();
  while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) {
    --e;
  }
  return s.substr(b, e - b);
}

bool IsReservedName(const std::wstring& stem) {
  static const wchar_t* kReserved[] = {
      L"CON",  L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3",
      L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1",
      L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8",
      L"LPT9"};
  for (const auto* r : kReserved) {
    if (_wcsicmp(stem.c_str(), r) == 0) {
      return true;
    }
  }
  return false;
}
}  // namespace

std::wstring UniqueDestPath(const std::wstring& dest_dir,
                            const std::wstring& name) {
  std::wstring dest = dest_dir + L"\\" + name;
  if (GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return dest;
  }
  // Split into stem + extension (last dot after the last separator).
  const size_t dot = name.find_last_of(L'.');
  std::wstring stem = name;
  std::wstring ext;
  if (dot != std::wstring::npos && dot > 0) {
    stem = name.substr(0, dot);
    ext = name.substr(dot);
  }
  for (int i = 2;; ++i) {
    dest = dest_dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
    if (GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return dest;
    }
  }
}

bool SameVolume(const std::wstring& a, const std::wstring& b) {
  wchar_t va[MAX_PATH] = {};
  wchar_t vb[MAX_PATH] = {};
  if (!GetVolumePathNameW(a.c_str(), va, MAX_PATH)) {
    wcscpy_s(va, a.c_str());
  }
  if (!GetVolumePathNameW(b.c_str(), vb, MAX_PATH)) {
    wcscpy_s(vb, b.c_str());
  }
  return _wcsicmp(va, vb) == 0;
}

int PasteClipboardToDesktopFolder(HWND owner,
                                  std::vector<std::wstring>* landed_paths) {
  if (landed_paths) {
    landed_paths->clear();
  }
  std::vector<std::wstring> paths;
  bool cut = false;
  if (!ClipboardGetFilesWithEffect(&paths, &cut) || paths.empty()) {
    return 0;
  }
  PWSTR desk_raw = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk_raw)) ||
      !desk_raw) {
    return 0;
  }
  const std::wstring desk = desk_raw;
  CoTaskMemFree(desk_raw);
  int ok = 0;
  for (const auto& src : paths) {
    std::wstring dest;
    if (MoveOrCopyFileIntoDir(owner, src, desk, cut, &dest)) {
      ++ok;
      if (landed_paths && !dest.empty()) {
        landed_paths->push_back(dest);
      }
    }
  }
  return ok;
}

bool MoveOrCopyFileIntoDir(HWND owner, const std::wstring& src,
                           const std::wstring& dest_dir, bool move,
                           std::wstring* landed) {
  if (src.empty() || dest_dir.empty()) {
    return false;
  }
  CreateDirectoryW(dest_dir.c_str(), nullptr);
  const std::wstring name = FileNameOf(src);
  if (name.empty()) {
    return false;
  }
  // Already inside dest_dir: return the source itself. Without this,
  // UniqueDestPath sees the file occupying its own name and hands back
  // "name (2)", so re-dropping a file onto its own portal folder silently
  // renamed it (Python shutil path returns early the same way).
  {
    const size_t slash = src.find_last_of(L"\\/");
    const std::wstring parent =
        slash == std::wstring::npos ? std::wstring() : src.substr(0, slash);
    if (!parent.empty() && _wcsicmp(parent.c_str(), dest_dir.c_str()) == 0) {
      if (landed) {
        *landed = src;
      }
      return true;
    }
  }
  std::wstring dest = UniqueDestPath(dest_dir, name);
  if (_wcsicmp(src.c_str(), dest.c_str()) == 0) {
    if (landed) {
      *landed = dest;
    }
    return true;  // already in place
  }
  std::wstring from = src;
  from.push_back(L'\0');
  std::wstring to = dest;
  to.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.hwnd = owner;
  op.wFunc = move ? FO_MOVE : FO_COPY;
  op.pFrom = from.c_str();
  op.pTo = to.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  const int rc = SHFileOperationW(&op);
  const bool ok = rc == 0 && !op.fAnyOperationsAborted &&
                  GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES;
  if (!ok) {
    return false;
  }
  if (landed) {
    *landed = dest;
  }
  SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, dest_dir.c_str(), nullptr);
  return true;
}

RenamePlan PlanRename(const std::wstring& old_path,
                      const std::wstring& new_name) {
  RenamePlan plan;
  const std::wstring neu = TrimName(new_name);
  if (neu.empty()) {
    plan.error = L"名称不能为空";
    return plan;
  }
  if (neu.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) {
    plan.error = L"名称包含非法字符";
    return plan;
  }
  if (neu.back() == L'.' || neu.back() == L' ') {
    plan.error = L"名称不能以空格或点结尾";
    return plan;
  }
  const size_t slash = old_path.find_last_of(L"\\/");
  const std::wstring old_name =
      slash == std::wstring::npos ? old_path : old_path.substr(slash + 1);
  const std::wstring parent =
      slash == std::wstring::npos ? L"" : old_path.substr(0, slash);

  std::wstring final = neu;
  const size_t old_dot = old_name.find_last_of(L'.');
  const bool old_has_ext = old_dot != std::wstring::npos && old_dot > 0;
  const size_t new_dot = final.find_last_of(L'.');
  const bool new_has_ext = new_dot != std::wstring::npos && new_dot > 0;
  // Extension protection: stem-only edits keep the original extension.
  if (old_has_ext && !new_has_ext) {
    final += old_name.substr(old_dot);
  }
  if (final == old_name) {
    plan.error = L"名称未改变";
    return plan;
  }
  // Windows reserved device names (CON, PRN, ...) are invalid even with ext.
  const size_t sdot = final.find_first_of(L'.');
  const std::wstring stem = sdot == std::wstring::npos
                                ? final
                                : final.substr(0, sdot);
  if (IsReservedName(stem)) {
    plan.error = L"该名称是系统保留名称";
    return plan;
  }
  plan.dest = parent.empty() ? final : parent + L"\\" + final;
  plan.valid = true;
  return plan;
}

void OpenShellPath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  // Prefer ShellExecuteEx so .lnk / namespace shortcuts resolve like Explorer.
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_FLAG_DDEWAIT | SEE_MASK_INVOKEIDLIST;
  sei.lpVerb = L"open";
  sei.lpFile = path.c_str();
  sei.nShow = SW_SHOWNORMAL;
  if (ShellExecuteExW(&sei)) {
    return;
  }
  const HINSTANCE rc =
      ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(rc) <= 32) {
    ShowToast(L"无法打开该项");
  }
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
  const std::wstring neu = PromptText(owner, Tr("rename_item").c_str(),
                                      Tr("rename_item_label").c_str(), name);
  if (neu.empty() || neu == name) {
    return IconFsResult::Cancelled;
  }
  const RenamePlan plan = PlanRename(path, neu);
  if (!plan.valid) {
    ShowToast(plan.error);
    return IconFsResult::Failed;
  }
  if (GetFileAttributesW(plan.dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    ShowToast(L"该名称已存在");
    return IconFsResult::Failed;
  }
  if (!MoveFileW(path.c_str(), plan.dest.c_str())) {
    const DWORD err = GetLastError();
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
      ShowToast(L"文件正在使用中，请先关闭占用程序后重试");
    } else {
      ShowToast(L"无法重命名，文件可能被占用");
    }
    return IconFsResult::Failed;
  }
  SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW, path.c_str(),
                 plan.dest.c_str());
  if (dest) {
    *dest = plan.dest;
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

IconFsResult RecycleShellPaths(HWND owner,
                              const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    return IconFsResult::Cancelled;
  }
  if (paths.size() == 1) {
    return RecycleShellPath(owner, paths[0]);
  }
  const std::wstring prompt =
      L"将选中的 " + std::to_wstring(paths.size()) + L" 项移到回收站？";
  if (!MistConfirm(owner, L"删除", prompt.c_str(), MistMsgTone::Warn)) {
    return IconFsResult::Cancelled;
  }
  // P1-11: capture namespace CLSID before delete so Explorer icons can return.
  std::vector<std::wstring> ns_clsids;
  ns_clsids.reserve(paths.size());
  for (const auto& p : paths) {
    ns_clsids.push_back(GetLnkNamespaceClsid(p));
  }
  std::wstring from;
  for (const auto& p : paths) {
    from += p;
    from.push_back(L'\0');
  }
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
  for (const auto& clsid : ns_clsids) {
    if (!clsid.empty()) {
      ReturnNamespaceIconToDesktop(clsid);
    }
  }
  return IconFsResult::Ok;
}

IconFsResult RecycleShellPath(HWND owner, const std::wstring& path) {
  if (!MistConfirm(owner, L"删除", L"将此项移到回收站？", MistMsgTone::Warn)) {
    return IconFsResult::Cancelled;
  }
  const std::wstring ns_clsid = GetLnkNamespaceClsid(path);
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
  if (!ns_clsid.empty()) {
    ReturnNamespaceIconToDesktop(ns_clsid);
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
