#include "i18n.h"

#include "paths_util.h"

#include <Windows.h>

#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

namespace {

std::string g_lang = "zh";

std::wstring LangFile() {
  return JoinPath(NativeDataDir(), L"ui_lang.txt");
}

void Persist() {
  std::ofstream out(LangFile(), std::ios::binary);
  if (!out) {
    return;
  }
  out << g_lang << "\n";
}

const std::unordered_map<std::string, std::wstring>& MapZh() {
  static const std::unordered_map<std::string, std::wstring> m = {
      {"settings", L"设置"},
      {"notes", L"便签"},
      {"todos", L"待办"},
      {"search", L"文件搜索"},
      {"vault", L"账号保险库"},
      {"minutes", L"会议纪要"},
      {"screenshot", L"截图"},
      {"record", L"录屏 开/关"},
      {"wallpaper", L"壁纸…"},
      {"updates", L"检查更新"},
      {"save_snapshot", L"保存布局快照"},
      {"manage_snapshots", L"管理布局快照…"},
      {"peek", L"Peek 显隐分区"},
      {"toggle_fences", L"显示/隐藏分区"},
      {"page1", L"第 1 页"},
      {"page2", L"第 2 页"},
      {"organize_virtual", L"虚拟整理"},
      {"organize_physical", L"物理整理"},
      {"edit_rules", L"编辑规则"},
      {"whitelist", L"整理白名单…"},
      {"refresh_icons", L"刷新图标"},
      {"add_fence", L"添加分区"},
      {"toggle_dock", L"切换 Dock"},
      {"toggle_pet", L"切换宠物"},
      {"autostart", L"开机启动"},
      {"restore_chrome", L"恢复桌面层"},
      {"hotkey_help", L"快捷键说明"},
      {"edit_hotkeys", L"修改快捷键…"},
      {"fullscreen_shot", L"全屏截图"},
      {"exit", L"退出"},
      {"language", L"语言 / Language"},
      {"calculator", L"计算器"},
      {"page_folders", L"分页文件夹…"},
      {"help", L"帮助"},
      {"sync_cloud", L"云同步"},
      {"login", L"登录"},
      {"logout", L"退出登录"},
      {"bold", L"粗体"},
      {"italic", L"斜体"},
      {"underline", L"下划线"},
      {"cloud_sync_done", L"云同步完成"},
      {"cloud_sync_fail", L"云同步失败"},
      {"add", L"添加"},
      {"delete", L"删除"},
      {"save", L"保存"},
      {"cancel", L"取消"},
      {"ok", L"确定"},
      {"title", L"标题"},
      {"username", L"用户"},
      {"password", L"密码"},
      {"url", L"网址"},
      {"export", L"导出"},
      {"import", L"导入"},
      {"file", L"文件"},
      {"view", L"视图"},
      {"edit", L"编辑"},
      {"format", L"格式"},
  };
  return m;
}

const std::unordered_map<std::string, std::wstring>& MapEn() {
  static const std::unordered_map<std::string, std::wstring> m = {
      {"settings", L"Settings"},
      {"notes", L"Notes"},
      {"todos", L"Todos"},
      {"search", L"File search"},
      {"vault", L"Account vault"},
      {"minutes", L"Meeting notes"},
      {"screenshot", L"Screenshot"},
      {"record", L"Record on/off"},
      {"wallpaper", L"Wallpaper…"},
      {"updates", L"Check for updates"},
      {"save_snapshot", L"Save layout snapshot"},
      {"manage_snapshots", L"Manage snapshots…"},
      {"peek", L"Peek fences"},
      {"toggle_fences", L"Show/hide fences"},
      {"page1", L"Page 1"},
      {"page2", L"Page 2"},
      {"organize_virtual", L"Organize (virtual)"},
      {"organize_physical", L"Organize (physical)"},
      {"edit_rules", L"Edit rules"},
      {"whitelist", L"Organize whitelist…"},
      {"refresh_icons", L"Refresh icons"},
      {"add_fence", L"Add fence"},
      {"toggle_dock", L"Toggle dock"},
      {"toggle_pet", L"Toggle pet"},
      {"autostart", L"Start with Windows"},
      {"restore_chrome", L"Restore desktop layer"},
      {"hotkey_help", L"Hotkey help"},
      {"edit_hotkeys", L"Edit hotkeys…"},
      {"fullscreen_shot", L"Fullscreen screenshot"},
      {"exit", L"Exit"},
      {"language", L"Language / 语言"},
      {"calculator", L"Calculator"},
      {"page_folders", L"Page folders…"},
      {"help", L"Help"},
      {"sync_cloud", L"Cloud sync"},
      {"login", L"Sign in"},
      {"logout", L"Sign out"},
      {"bold", L"Bold"},
      {"italic", L"Italic"},
      {"underline", L"Underline"},
      {"cloud_sync_done", L"Cloud sync complete"},
      {"cloud_sync_fail", L"Cloud sync failed"},
      {"add", L"Add"},
      {"delete", L"Delete"},
      {"save", L"Save"},
      {"cancel", L"Cancel"},
      {"ok", L"OK"},
      {"title", L"Title"},
      {"username", L"Username"},
      {"password", L"Password"},
      {"url", L"URL"},
      {"export", L"Export"},
      {"import", L"Import"},
      {"file", L"File"},
      {"view", L"View"},
      {"edit", L"Edit"},
      {"format", L"Format"},
  };
  return m;
}

bool IsZh() { return g_lang != "en"; }

}  // namespace

void LoadLanguage() {
  g_lang = "zh";
  std::ifstream in(LangFile());
  if (!in) {
    return;
  }
  std::string line;
  std::getline(in, line);
  while (!line.empty() &&
         (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
    line.pop_back();
  }
  if (line == "en" || line == "zh") {
    g_lang = line;
  }
}

const char* CurrentLang() { return g_lang.c_str(); }

void SetLanguage(const char* lang) {
  if (lang && std::strcmp(lang, "en") == 0) {
    g_lang = "en";
  } else {
    g_lang = "zh";
  }
  Persist();
}

void ToggleLanguage() {
  SetLanguage(IsZh() ? "en" : "zh");
}

std::wstring Tr(const char* key) {
  if (!key || !*key) {
    return {};
  }
  const auto& map = IsZh() ? MapZh() : MapEn();
  const auto it = map.find(key);
  if (it != map.end()) {
    return it->second;
  }
  return WidenUtf8(key);
}
