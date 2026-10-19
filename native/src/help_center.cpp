#include "help_center.h"

#include "paths_util.h"

#include <Shellapi.h>

#include <fstream>
#include <string>

namespace {

constexpr char kHelpHtml[] =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<title>DeskTidy Native 帮助</title>"
    "<style>body{font-family:Segoe UI,Microsoft YaHei,sans-serif;max-width:720px;"
    "margin:32px auto;padding:0 20px;line-height:1.55;color:#1f2937}"
    "h1{font-size:1.6rem}h2{font-size:1.15rem;margin-top:1.6em;border-bottom:1px solid #e5e7eb}"
    "code{background:#f3f4f6;padding:1px 6px;border-radius:4px}"
    "ul{padding-left:1.2em}</style></head><body>"
    "<h1>DeskTidy Native</h1>"
    "<p>桌面分区与工具的原生 Win32 主机。数据目录："
    "<code>%LocalAppData%\\DeskTidyNative\\</code></p>"
    "<h2>分区</h2><ul>"
    "<li>拖动边角缩放；把桌面图标拖入即可钉选</li>"
    "<li>设置 →「框选添加」拖出区域新建分区</li>"
    "<li>右键：重命名、换页、样式；图标右键：打开 / 复制剪切 / 取消钉选</li>"
    "<li>Ctrl+V 或右键「粘贴文件」从资源管理器剪贴板钉入</li>"
    "</ul><h2>整理</h2><ul>"
    "<li>虚拟整理：按规则写入钉选；物理整理：移入 Desktop\\DeskTidy</li>"
    "<li>rules.json 支持扩展名与文件名通配（如 Invoice*）</li>"
    "<li>可开启动自动整理与监视新文件；取消钉选有短暂抑制窗口</li>"
    "</ul><h2>工具</h2><ul>"
    "<li>区域截图：单击取窗或拖拽选区；全屏截图；录屏开/关与角标停止</li>"
    "<li>便签：查找替换、笔记库搜索、Markdown 浏览器预览</li>"
    "<li>保险库：DPAPI 本地存储、密码生成、复制 30 秒清空、云同步</li>"
    "<li>底部页条：1/2 页 + 记/搜/截快捷入口</li>"
    "</ul><h2>快捷键</h2><ul>"
    "<li>Ctrl+Shift+S 设置 · N 便签 · F 分区 · O 整理 · V 保险库 · T 待办</li>"
    "<li>Ctrl+Space Peek · Ctrl+Alt+1/2 换页 · Ctrl+Alt+4 搜索</li>"
    "<li>Ctrl+Shift+1 区域截图 · 2 全屏 · R 录屏</li>"
    "<li>托盘 →「修改快捷键」可改绑</li>"
    "</ul><h2>更新</h2>"
    "<p>托盘「检查更新」会对比 GitHub DeskTidy-Releases 最新版本。</p>"
    "</body></html>";

}  // namespace

void ShowAppHelpCenter(HWND owner) {
  const std::wstring path = JoinPath(NativeDataDir(), L"help.html");
  {
    std::ofstream out(path, std::ios::binary);
    if (out) {
      out.write(kHelpHtml, sizeof(kHelpHtml) - 1);
    }
  }
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }
  MessageBoxW(owner, L"无法写入帮助文件。", L"帮助", MB_OK | MB_ICONWARNING);
}
