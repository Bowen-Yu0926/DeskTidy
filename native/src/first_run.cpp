#include "first_run.h"

#include "paths_util.h"

#include <fstream>
#include <string>

void MaybeShowFirstRun(HWND owner) {
  const std::wstring marker = JoinPath(NativeDataDir(), L"firstrun.done");
  if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return;
  }
  const int r = MessageBoxW(
      owner,
      L"欢迎使用 DeskTidy Native\n\n"
      L"1. 分区可拖动 / 缩放；图标可拖入钉选\n"
      L"2. Ctrl+Alt+1/2 换页 · Ctrl+Space Peek\n"
      L"3. 托盘：截图 / 录屏 / 搜索 / 保险库…\n\n"
      L"是 = 打开设置　否 = 稍后再说\n"
      L"（此提示只显示一次）",
      L"DeskTidy Native", MB_YESNO | MB_ICONINFORMATION);
  std::ofstream out(marker);
  out << "1\n";
  if (r == IDYES && owner) {
    ShowWindow(owner, SW_SHOW);
    SetForegroundWindow(owner);
  }
}
