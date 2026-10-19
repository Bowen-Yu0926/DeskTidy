#include "hotkey_help.h"

#include "hotkey_store.h"

#include <string>

void ShowHotkeyHelp(HWND owner) {
  const auto keys = LoadHotkeys();
  std::wstring body = L"DeskTidy Native 快捷键\n\n";
  for (const auto& k : keys) {
    body += FormatHotkey(k.modifiers, k.vk);
    body += L"　　";
    body += k.label;
    body += L"\n";
  }
  body +=
      L"\n托盘 →「修改快捷键」可重新绑定（写入 %LocalAppData%\\DeskTidyNative\\hotkeys.json）\n\n"
      L"分区右键：重命名 / 换页\n"
      L"图标右键：打开 / 资源管理器 / 取消钉选\n"
      L"宠物：拖入文件 → 回收站（可选 pet.png）\n"
      L"贴图：右键关闭";
  MessageBoxW(owner, body.c_str(), L"快捷键说明", MB_OK | MB_ICONINFORMATION);
}
