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
      {"notes", L"记事本"},
      {"todos", L"待办"},
      {"search", L"文件搜索"},
      {"minutes", L"会议纪要"},
      {"screenshot", L"截图"},
      {"record", L"录屏 开/关"},
      {"wallpaper", L"壁纸…"},
      {"updates", L"检查更新"},
      {"peek", L"Peek 显隐分区"},
      {"toggle_fences", L"显示/隐藏分区"},
      {"page1", L"工作"},
      {"page2", L"文档"},
      {"whitelist", L"整理白名单…"},
      {"autostart", L"开机启动"},
      {"restore_chrome", L"恢复桌面层"},
      {"hotkey_help", L"快捷键说明"},
      {"exit", L"退出"},
      {"language", L"语言 / Language"},
      {"calculator", L"计算器"},
      {"page_folders", L"分页文件夹…"},
      {"help", L"帮助"},
      {"bold", L"粗体"},
      {"italic", L"斜体"},
      {"underline", L"下划线"},
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
      {"fence_rename", L"重命名分区…"},
      {"fence_collapse", L"折叠分区"},
      {"fence_expand", L"展开分区"},
      {"fence_paste", L"粘贴文件"},
      {"fence_style", L"分区样式"},
      {"fence_opacity", L"透明度"},
      {"fence_view", L"视图"},
      {"fence_sort", L"图标排序"},
      {"view_grid", L"网格"},
      {"view_list", L"列表"},
      {"opacity_follow", L"跟随样式"},
      {"opacity_opaque", L"不透明"},
      {"wallpaper_title", L"壁纸"},
      {"wallpaper_prompt",
       L"壁纸：\n\n是 = 选择图片\n否 = 从库目录随机\n取消 = 恢复原始壁纸"},
      {"wallpaper_no_images", L"库目录中没有 jpg/png/bmp。"},
      {"wallpaper_no_original", L"没有可恢复的原始壁纸记录。"},
      {"record_title", L"录屏"},
      {"record_settings", L"录屏设置"},
      {"record_saved", L"录屏设置已保存。"},
      {"record_pause", L"暂停"},
      {"record_resume", L"继续"},
      {"record_stop", L"停止"},
      {"layout_editor", L"布局编辑"},
      {"layout_apply", L"应用"},
      {"layout_add", L"添加分区"},
      {"layout_delete", L"删除选中"},
      {"layout_keep_one", L"至少保留一个分区。"},
      {"layout_no_system", L"系统分区不能删除。"},
      {"search_title", L"文件搜索"},
      {"search_hint", L"文件搜索  ·  输入即搜 · Esc 关闭"},
      {"search_open", L"打开"},
      {"search_all_drives", L"搜索所有固定磁盘"},
      {"tool_record", L"录屏"},
      {"tool_minutes", L"会议纪要"},
      {"tool_calculator", L"计算器"},
      {"tool_todo", L"待办"},
      {"notes_paste_html", L"粘贴 HTML 为 Markdown"},
      {"notes_template", L"插入模板"},
      {"hk_capture_title", L"按下新快捷键…"},
      {"hk_capture_hint", L"请按下组合键（Esc 取消）"},
      {"hk_help_title", L"快捷键说明"},
      {"hk_help_intro", L"DeskTidy 全局快捷键\n\n"},
      {"hk_help_footer",
       L"\n在「设置 → 快捷键」中点「修改」可重新绑定。\n"
       L"配置文件：%LocalAppData%\\DeskTidyNative\\hotkeys.json"},
      {"hk_unbound", L"（未绑定）"},
      {"hk_organize", L"一键整理"},
      {"hk_toggle_fences", L"显示/隐藏分区"},
      {"hk_toggle_icons", L"显示/隐藏桌面图标"},
      {"hk_peek_fences", L"临时查看分区"},
      {"hk_page_next", L"下一页"},
      {"hk_page_prev", L"上一页"},
      {"hk_screenshot", L"区域截图"},
      {"hk_show_screenshot", L"显示截图贴图"},
      {"hk_screen_record", L"录屏（开始/结束）"},
      {"hk_file_search", L"文件搜索"},
      {"hk_calculator", L"计算器"},
      {"hk_settings", L"打开设置"},
      {"hk_notes", L"打开记事本"},
      {"hk_todos", L"打开待办"},
      {"rename_fence", L"重命名分区"},
      {"rename_fence_label", L"新标题："},
      {"rename_page", L"重命名页面"},
      {"rename_page_label", L"页面名称："},
      {"rename_item", L"重命名"},
      {"rename_item_label", L"新名称："},
      {"todo_sample", L"示例待办"},
      {"snap_mgr_title", L"管理布局快照"},
      {"snap_delete", L"删除"},
      {"snap_restore_ask", L"恢复快照？当前分区布局将被替换。"},
      {"snap_restore_title", L"恢复快照"},
      {"snap_read_fail", L"读取快照失败。"},
      {"snap_delete_ask", L"删除此快照？"},
      {"snap_delete_title", L"删除快照"},
      {"snap_delete_fail", L"删除失败。"},
      {"snap_empty", L"还没有快照。请先「保存布局快照」。"},
      {"snap_title", L"布局快照"},
      {"minutes_title", L"会议纪要"},
      {"minutes_recent", L"最近纪要"},
      {"minutes_empty", L"还没有纪要文件。"},
      {"minutes_folder_set", L"纪要文件夹已设置"},
      {"minutes_browse", L"选择会议纪要保存文件夹"},
      {"fence_refresh", L"刷新分区"},
      {"fence_dissolve", L"解散分区"},
      {"fence_hide", L"隐藏分区"},
      {"menu_unpin", L"移出分区"},
      {"wl_add_item", L"加入整理白名单"},
      {"wl_remove_item", L"移出整理白名单"},
      {"menu_move_page", L"移动到分页"},
      {"notes_open_in_notepad", L"用记事本打开"},
  };
  return m;
}

const std::unordered_map<std::string, std::wstring>& MapEn() {
  static const std::unordered_map<std::string, std::wstring> m = {
      {"settings", L"Settings"},
      {"notes", L"Notes"},
      {"todos", L"Todos"},
      {"search", L"File search"},
      {"minutes", L"Meeting notes"},
      {"screenshot", L"Screenshot"},
      {"record", L"Record on/off"},
      {"wallpaper", L"Wallpaper…"},
      {"updates", L"Check for updates"},
      {"peek", L"Peek fences"},
      {"toggle_fences", L"Show/hide fences"},
      {"page1", L"Page 1"},
      {"page2", L"Page 2"},
      {"whitelist", L"Organize whitelist…"},
      {"autostart", L"Start with Windows"},
      {"restore_chrome", L"Restore desktop layer"},
      {"hotkey_help", L"Hotkey help"},
      {"exit", L"Exit"},
      {"language", L"Language / 语言"},
      {"calculator", L"Calculator"},
      {"page_folders", L"Page folders…"},
      {"help", L"Help"},
      {"bold", L"Bold"},
      {"italic", L"Italic"},
      {"underline", L"Underline"},
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
      {"fence_rename", L"Rename fence…"},
      {"fence_collapse", L"Collapse"},
      {"fence_expand", L"Expand"},
      {"fence_paste", L"Paste files"},
      {"fence_style", L"Fence style"},
      {"fence_opacity", L"Opacity"},
      {"fence_view", L"View"},
      {"fence_sort", L"Sort icons"},
      {"view_grid", L"Grid"},
      {"view_list", L"List"},
      {"opacity_follow", L"Follow style"},
      {"opacity_opaque", L"Opaque"},
      {"wallpaper_title", L"Wallpaper"},
      {"wallpaper_prompt",
       L"Wallpaper:\n\nYes = pick image\nNo = random from library\nCancel = "
       L"restore original"},
      {"wallpaper_no_images", L"No jpg/png/bmp in the library folder."},
      {"wallpaper_no_original", L"No original wallpaper recorded."},
      {"record_title", L"Screen record"},
      {"record_settings", L"Recording settings"},
      {"record_saved", L"Recording settings saved."},
      {"record_pause", L"Pause"},
      {"record_resume", L"Resume"},
      {"record_stop", L"Stop"},
      {"layout_editor", L"Layout editor"},
      {"layout_apply", L"Apply"},
      {"layout_add", L"Add fence"},
      {"layout_delete", L"Delete selected"},
      {"layout_keep_one", L"Keep at least one fence."},
      {"layout_no_system", L"System fences cannot be deleted."},
      {"search_title", L"File search"},
      {"search_hint", L"File search  ·  type to search · Esc closes"},
      {"search_open", L"Open"},
      {"search_all_drives", L"Search all fixed drives"},
      {"tool_record", L"Record"},
      {"tool_minutes", L"Meeting notes"},
      {"tool_calculator", L"Calculator"},
      {"tool_todo", L"Todos"},
      {"notes_paste_html", L"Paste HTML as Markdown"},
      {"notes_template", L"Insert template"},
      {"hk_capture_title", L"Press a new shortcut…"},
      {"hk_capture_hint", L"Press a key combo (Esc cancels)"},
      {"hk_help_title", L"Hotkey help"},
      {"hk_help_intro", L"DeskTidy global hotkeys\n\n"},
      {"hk_help_footer",
       L"\nIn Settings → Hotkeys, click Edit to rebind.\n"
       L"Config: %LocalAppData%\\DeskTidyNative\\hotkeys.json"},
      {"hk_unbound", L"(unbound)"},
      {"hk_organize", L"Organize"},
      {"hk_toggle_fences", L"Show/hide fences"},
      {"hk_toggle_icons", L"Show/hide desktop icons"},
      {"hk_peek_fences", L"Peek fences"},
      {"hk_page_next", L"Next page"},
      {"hk_page_prev", L"Previous page"},
      {"hk_screenshot", L"Region screenshot"},
      {"hk_show_screenshot", L"Show pinned screenshots"},
      {"hk_screen_record", L"Screen record (toggle)"},
      {"hk_file_search", L"File search"},
      {"hk_calculator", L"Calculator"},
      {"hk_settings", L"Open settings"},
      {"hk_notes", L"Open DeskNote"},
      {"hk_todos", L"Open todos"},
      {"rename_fence", L"Rename fence"},
      {"rename_fence_label", L"New title:"},
      {"rename_page", L"Rename page"},
      {"rename_page_label", L"Page name:"},
      {"rename_item", L"Rename"},
      {"rename_item_label", L"New name:"},
      {"todo_sample", L"Sample todo"},
      {"snap_mgr_title", L"Manage layout snapshots"},
      {"snap_delete", L"Delete"},
      {"snap_restore_ask", L"Restore this snapshot? Current fence layout will be replaced."},
      {"snap_restore_title", L"Restore snapshot"},
      {"snap_read_fail", L"Failed to read snapshot."},
      {"snap_delete_ask", L"Delete this snapshot?"},
      {"snap_delete_title", L"Delete snapshot"},
      {"snap_delete_fail", L"Delete failed."},
      {"snap_empty", L"No snapshots yet. Save a layout snapshot first."},
      {"snap_title", L"Layout snapshots"},
      {"minutes_title", L"Meeting notes"},
      {"minutes_recent", L"Recent notes"},
      {"minutes_empty", L"No meeting notes yet."},
      {"minutes_folder_set", L"Minutes folder set"},
      {"minutes_browse", L"Choose meeting-notes folder"},
      {"fence_refresh", L"Refresh Fence"},
      {"fence_dissolve", L"Dissolve Fence"},
      {"fence_hide", L"Hide Fence"},
      {"menu_unpin", L"Remove from Fence"},
      {"wl_add_item", L"Add to Organize Whitelist"},
      {"wl_remove_item", L"Remove from Organize Whitelist"},
      {"menu_move_page", L"Move to Page"},
      {"notes_open_in_notepad", L"Open in Notepad"},
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
