#include "hotkey_help.h"

void ShowHotkeyHelp(HWND owner) {
  MessageBoxW(owner,
              L"DeskTidy Native 快捷键\n\n"
              L"Ctrl+Shift+S　　设置\n"
              L"Ctrl+Shift+N　　便签\n"
              L"Ctrl+Shift+F　　显示/隐藏分区\n"
              L"Ctrl+Space　　　Peek（临时隐藏桌面层）\n"
              L"Ctrl+Alt+1/2　　分页\n"
              L"Ctrl+Shift+1　　区域截图\n"
              L"Ctrl+Alt+4　　　文件搜索\n\n"
              L"分区右键：重命名 / 换页\n"
              L"图标右键：打开 / 资源管理器 / 取消钉选\n"
              L"宠物：拖入文件 → 回收站\n"
              L"贴图：右键关闭",
              L"快捷键说明", MB_OK | MB_ICONINFORMATION);
}
