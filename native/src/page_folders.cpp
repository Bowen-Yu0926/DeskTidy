#include "page_folders.h"

#include "mist_webview.h"
#include "paths_util.h"
#include "prompt_text.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include <algorithm>
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
  std::ostringstream out;
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
  return AtomicWriteUtf8File(CfgPath(), out.str());
}

namespace {

struct Ctx {
  std::vector<PageFolderItem> items;
  int sel = 0;
};

std::wstring BuildState(Ctx* ctx) {
  std::wstring json = L"{\"type\":\"state\",\"sel\":";
  json += std::to_wstring(ctx->sel);
  json += L",\"items\":[";
  for (size_t i = 0; i < ctx->items.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L"{\"name\":\"";
    json += MistJsonEscape(ctx->items[i].name);
    json += L"\",\"path\":\"";
    json += MistJsonEscape(ctx->items[i].path);
    json += L"\"}";
  }
  json += L"]}";
  return json;
}

bool BrowseAddFolder(HWND owner, Ctx* ctx) {
  BROWSEINFOW bi{};
  bi.hwndOwner = owner;
  bi.lpszTitle = L"选择文件夹";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
  if (!pidl) {
    return false;
  }
  wchar_t path[MAX_PATH] = {};
  SHGetPathFromIDListW(pidl, path);
  CoTaskMemFree(pidl);
  if (!path[0]) {
    return false;
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
  ctx->items.push_back(std::move(it));
  ctx->sel = static_cast<int>(ctx->items.size()) - 1;
  return true;
}

}  // namespace

void EditPageFoldersDialog(HWND owner) {
  Ctx ctx;
  ctx.items = LoadPageFolders();

  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.user_data_subdir = L"webview2-page-folders";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\page_folders.html");
  if (dlg.html_path.empty()) {
    MessageBoxW(owner, L"找不到 page_folders.html", L"分页文件夹",
                MB_OK | MB_ICONWARNING);
    return;
  }
  dlg.build_state = [&ctx]() { return BuildState(&ctx); };
  dlg.on_message = [&](const std::wstring& json) -> bool {
    const std::wstring type = MistJsonStringField(json, L"type");
    if (type != L"action") {
      return false;
    }
    const std::wstring id = MistJsonStringField(json, L"id");
    HWND parent = dlg.hwnd ? dlg.hwnd : owner;
    if (id == L"ready") {
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"add") {
      if (BrowseAddFolder(parent, &ctx)) {
        MistWebDialogPushState(&dlg);
      }
      return false;
    }
    if (id == L"rename") {
      const int index = MistJsonIntField(json, L"index", -1);
      if (index >= 0 && index < static_cast<int>(ctx.items.size())) {
        auto& it = ctx.items[static_cast<size_t>(index)];
        const std::wstring neu =
            PromptText(parent, L"重命名", L"显示名称：", it.name);
        if (!neu.empty()) {
          it.name = neu;
          ctx.sel = index;
          MistWebDialogPushState(&dlg);
        }
      }
      return false;
    }
    if (id == L"del") {
      const int index = MistJsonIntField(json, L"index", -1);
      if (index >= 0 && index < static_cast<int>(ctx.items.size())) {
        ctx.items.erase(ctx.items.begin() + index);
        ctx.sel = ctx.items.empty()
                      ? -1
                      : (std::min)(index, static_cast<int>(ctx.items.size()) - 1);
        MistWebDialogPushState(&dlg);
      }
      return false;
    }
    if (id == L"save") {
      SavePageFolders(ctx.items);
      return true;
    }
    if (id == L"cancel") {
      return true;
    }
    return false;
  };

  MistWebDialogRun(&dlg, L"分页文件夹", 560, 440);
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
