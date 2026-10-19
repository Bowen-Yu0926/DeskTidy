#include "native_cleanup.h"

#include "autostart.h"
#include "paths_util.h"
#include "shell_verbs.h"

#include <Shellapi.h>

#include <string>

namespace {

bool WipeDirectory(const std::wstring& dir) {
  if (dir.empty() || dir.find(L"DeskTidyNative") == std::wstring::npos) {
    return false;
  }
  // SHFileOperation needs double-null terminated path.
  std::wstring from = dir;
  from.push_back(L'\0');
  from.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_ALLOWUNDO;
  return SHFileOperationW(&op) == 0;
}

}  // namespace

void RunNativeCleanup(HWND owner) {
  const int wipe = MessageBoxW(
      owner,
      L"卸载清理 DeskTidy Native：\n\n"
      L"将取消桌面右键动词、关闭开机启动。\n\n"
      L"是 = 同时删除数据目录（可进回收站）\n"
      L"否 = 仅清理注册项与开机启动\n"
      L"取消 = 返回",
      L"卸载清理", MB_YESNOCANCEL | MB_ICONWARNING);
  if (wipe == IDCANCEL) {
    return;
  }
  UnregisterNativeShellVerbs();
  SetNativeAutostart(false);
  if (wipe == IDYES) {
    WipeDirectory(NativeDataDir());
    MessageBoxW(owner, L"已清理注册项、开机启动，并尝试删除数据目录。",
                L"卸载清理", MB_OK | MB_ICONINFORMATION);
  } else {
    MessageBoxW(owner, L"已取消桌面右键动词并关闭开机启动。数据目录保留。",
                L"卸载清理", MB_OK | MB_ICONINFORMATION);
  }
}
