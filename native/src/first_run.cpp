#include "first_run.h"

#include "paths_util.h"

#include <fstream>
#include <string>

void MaybeShowFirstRun(HWND owner) {
  const std::wstring marker = JoinPath(NativeDataDir(), L"firstrun.done");
  if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return;
  }

  const wchar_t* steps[] = {
      L"欢迎使用 DeskTidy Native（1/3）\n\n"
      L"分区可贴在桌面层：拖动边角缩放，把桌面图标拖进分区即可钉选。\n"
      L"设置里「框选添加」可拖出一块区域新建分区。\n\n"
      L"是 = 下一步　否 = 跳过引导",

      L"快捷键与翻页（2/3）\n\n"
      L"· Ctrl+Alt+1 / 2：切换分区页\n"
      L"· Ctrl+Space：Peek 暂时隐藏分区\n"
      L"· Ctrl+Shift+S：设置　N：便签　F：显示/隐藏分区\n"
      L"· Ctrl+Shift+1：区域截图　R：录屏　O：整理\n\n"
      L"底部页条也可点选第 1 / 2 页。\n\n"
      L"是 = 下一步　否 = 跳过引导",

      L"托盘与工具（3/3）\n\n"
      L"右键托盘：搜索、保险库、壁纸、快照、检查更新…\n"
      L"设置 → 整理：启动自动钉选 / 监视新文件。\n"
      L"宠物右键可换角色与姿态；拖入文件进回收站。\n\n"
      L"是 = 打开设置　否 = 开始使用\n"
      L"（此引导只显示一次）",
  };

  bool open_settings = false;
  for (int i = 0; i < 3; ++i) {
    const UINT flags =
        (i < 2) ? (MB_YESNO | MB_ICONINFORMATION)
                : (MB_YESNO | MB_ICONINFORMATION);
    const int r =
        MessageBoxW(owner, steps[i], L"DeskTidy Native 引导", flags);
    if (r != IDYES) {
      break;
    }
    if (i == 2) {
      open_settings = true;
    }
  }

  std::ofstream out(marker);
  out << "1\n";
  if (open_settings && owner) {
    ShowWindow(owner, SW_SHOW);
    SetForegroundWindow(owner);
  }
}
