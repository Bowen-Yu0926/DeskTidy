#include "page_folders.h"

#include "paths_util.h"
#include "prompt_text.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include <fstream>
#include <sstream>

namespace {

std::wstring CfgPath() {
  return JoinPath(NativeDataDir(), L"page_folders.json");
}

}  // namespace

std::vector<PageFolderItem> LoadPageFolders() {
  std::vector<PageFolderItem> out;
  std::ifstream in(CfgPath());
  if (!in) {
    return out;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  size_t pos = 0;
  while (true) {
    const size_t start = json.find('{', pos);
    if (start == std::string::npos) {
      break;
    }
    const size_t end = json.find('}', start);
    if (end == std::string::npos) {
      break;
    }
    const std::string obj = json.substr(start, end - start + 1);
    pos = end + 1;
    auto field = [&](const char* key) -> std::string {
      const std::string needle = std::string("\"") + key + "\"";
      size_t p = obj.find(needle);
      if (p == std::string::npos) {
        return {};
      }
      p = obj.find('"', obj.find(':', p) + 1);
      if (p == std::string::npos) {
        return {};
      }
      ++p;
      std::string v;
      while (p < obj.size() && obj[p] != '"') {
        if (obj[p] == '\\' && p + 1 < obj.size()) {
          ++p;
        }
        v.push_back(obj[p++]);
      }
      return v;
    };
    PageFolderItem it;
    it.path = WidenUtf8(field("path"));
    it.name = WidenUtf8(field("name"));
    if (it.path.empty()) {
      continue;
    }
    if (it.name.empty()) {
      const size_t slash = it.path.find_last_of(L"\\/");
      it.name = slash == std::wstring::npos ? it.path
                                            : it.path.substr(slash + 1);
    }
    out.push_back(std::move(it));
  }
  return out;
}

bool SavePageFolders(const std::vector<PageFolderItem>& items) {
  std::ofstream out(CfgPath(), std::ios::binary);
  if (!out) {
    return false;
  }
  out << "{\n  \"folders\": [\n";
  for (size_t i = 0; i < items.size(); ++i) {
    out << "    {\"name\":\"" << NarrowUtf8(items[i].name) << "\",\"path\":\""
        << NarrowUtf8(items[i].path) << "\"}";
    if (i + 1 < items.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
  return true;
}

void EditPageFoldersDialog(HWND owner) {
  auto items = LoadPageFolders();
  std::wstring msg = L"分页文件夹快捷方式：\n\n";
  if (items.empty()) {
    msg += L"（空）\n";
  } else {
    for (size_t i = 0; i < items.size(); ++i) {
      msg += std::to_wstring(i + 1) + L". " + items[i].name + L"\n   " +
             items[i].path + L"\n";
    }
  }
  msg += L"\n是 = 添加　否 = 删除　取消 = 关闭";
  const int choice =
      MessageBoxW(owner, msg.c_str(), L"分页文件夹", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDCANCEL) {
    return;
  }
  if (choice == IDYES) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) {
      return;
    }
    wchar_t path[MAX_PATH] = {};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!path[0]) {
      return;
    }
    PageFolderItem it;
    it.path = path;
    const size_t slash = it.path.find_last_of(L"\\/");
    it.name = slash == std::wstring::npos ? it.path : it.path.substr(slash + 1);
    const std::wstring custom =
        PromptText(owner, L"分页文件夹", L"显示名称：", it.name);
    if (!custom.empty()) {
      it.name = custom;
    }
    items.push_back(std::move(it));
    SavePageFolders(items);
    return;
  }
  if (items.empty()) {
    return;
  }
  const std::wstring ans = PromptText(
      owner, L"删除",
      (L"编号 (1-" + std::to_wstring(items.size()) + L")：").c_str(), L"1");
  const int idx = _wtoi(ans.c_str()) - 1;
  if (idx >= 0 && idx < static_cast<int>(items.size())) {
    items.erase(items.begin() + idx);
    SavePageFolders(items);
  }
}

void OpenPageFolderMenu(HWND owner) {
  auto items = LoadPageFolders();
  if (items.empty()) {
    if (MessageBoxW(owner, L"还没有分页文件夹。现在添加？", L"分页文件夹",
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
      EditPageFoldersDialog(owner);
    }
    return;
  }
  HMENU menu = CreatePopupMenu();
  for (size_t i = 0; i < items.size() && i < 20; ++i) {
    AppendMenuW(menu, MF_STRING, static_cast<UINT>(i + 1), items[i].name.c_str());
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 100, L"编辑列表…");
  POINT pt{};
  GetCursorPos(&pt);
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, owner,
                     nullptr);
  DestroyMenu(menu);
  if (cmd == 100) {
    EditPageFoldersDialog(owner);
    return;
  }
  if (cmd >= 1 && cmd <= static_cast<int>(items.size())) {
    const auto& it = items[static_cast<size_t>(cmd - 1)];
    CreateDirectoryW(it.path.c_str(), nullptr);
    ShellExecuteW(owner, L"open", it.path.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
  }
}
