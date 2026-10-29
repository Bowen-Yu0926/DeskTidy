#include "page_folders.h"

#include "mist_msg.h"
#include "mist_webview.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "toast.h"
#include "tools_misc.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include <algorithm>
#include <sstream>

namespace {

std::wstring CfgPath() {
  return JoinPath(NativeDataDir(), L"page_folders.json");
}

std::vector<PageFolderItem> ParsePageFoldersJson(const std::string& json) {
  std::vector<PageFolderItem> out;
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

}  // namespace

std::vector<PageFolderItem> LoadPageFolders() {
  // Portable → LocalAppData switches used to leave page_folders behind in
  // <exe>\data; adopt before the first read so settings/page-bar stay filled.
  AdoptOrphanPortableSidecarOnce();

  std::string json;
  if (!ReadUtf8FileWithBak(CfgPath(), &json)) {
    return {};
  }
  return ParsePageFoldersJson(json);
}

bool SavePageFolders(const std::vector<PageFolderItem>& items) {
  std::ostringstream out;
  out << "{\n  \"folders\": [\n";
  for (size_t i = 0; i < items.size(); ++i) {
    out << "    {\"name\":\"" << EscapeJsonUtf8(NarrowUtf8(items[i].name))
        << "\",\"path\":\"" << EscapeJsonUtf8(NarrowUtf8(items[i].path))
        << "\"}";
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
  bool dirty = false;
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
  // Prefer the shared picker (same as wallpaper / record dir).
  const std::wstring path = BrowseFolderDialog(owner, L"选择文件夹");
  if (path.empty()) {
    return false;
  }
  PageFolderItem it;
  it.path = path;
  const size_t slash = it.path.find_last_of(L"\\/");
  it.name = slash == std::wstring::npos ? it.path : it.path.substr(slash + 1);
  // Called from on_deferred (off WebMessageReceived) — Mist prompt is safe.
  const std::wstring custom =
      PromptText(owner, L"分页文件夹", L"显示名称：", it.name);
  if (!custom.empty()) {
    it.name = custom;
  }
  ctx->items.push_back(std::move(it));
  ctx->sel = static_cast<int>(ctx->items.size()) - 1;
  ctx->dirty = true;
  return true;
}

bool PersistIfDirty(Ctx* ctx) {
  if (!ctx || !ctx->dirty) {
    return true;
  }
  if (!SavePageFolders(ctx->items)) {
    return false;
  }
  ctx->dirty = false;
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
    MistAlert(owner, L"分页文件夹", L"找不到 page_folders.html",
              MistMsgTone::Warn);
    return;
  }
  dlg.build_state = [&ctx]() { return BuildState(&ctx); };
  // Leave WebMessageReceived before folder picker / rename (nested browse or
  // Mist prompt from that stack freezes the page-folders dialog).
  constexpr WPARAM kDeferAdd = 1;
  constexpr WPARAM kDeferRename = 2;
  dlg.on_deferred = [&](WPARAM op, LPARAM index_lp) {
    HWND parent = dlg.hwnd ? dlg.hwnd : owner;
    if (op == kDeferAdd) {
      if (BrowseAddFolder(parent, &ctx)) {
        PersistIfDirty(&ctx);
        MistWebDialogPushState(&dlg);
      }
      return;
    }
    if (op == kDeferRename) {
      const int index = static_cast<int>(index_lp);
      if (index >= 0 && index < static_cast<int>(ctx.items.size())) {
        auto& it = ctx.items[static_cast<size_t>(index)];
        const std::wstring neu =
            PromptText(parent, L"重命名", L"显示名称：", it.name);
        if (!neu.empty()) {
          it.name = neu;
          ctx.sel = index;
          ctx.dirty = true;
          PersistIfDirty(&ctx);
          MistWebDialogPushState(&dlg);
        }
      }
    }
  };
  dlg.on_message = [&](const std::wstring& json) -> bool {
    const std::wstring type = MistJsonStringField(json, L"type");
    if (type != L"action") {
      return false;
    }
    const std::wstring id = MistJsonStringField(json, L"id");
    if (id == L"ready") {
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"add") {
      if (dlg.hwnd) {
        PostMessageW(dlg.hwnd, WM_APP + 81, kDeferAdd, 0);
      }
      return false;
    }
    if (id == L"rename") {
      const int index = MistJsonIntField(json, L"index", -1);
      if (dlg.hwnd && index >= 0) {
        PostMessageW(dlg.hwnd, WM_APP + 81, kDeferRename,
                     static_cast<LPARAM>(index));
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
        ctx.dirty = true;
        PersistIfDirty(&ctx);
        MistWebDialogPushState(&dlg);
      }
      return false;
    }
    if (id == L"save") {
      ctx.dirty = true;
      if (!PersistIfDirty(&ctx)) {
        // Toast only — MistAlert from WebMessageReceived can freeze the host.
        ShowToast(L"保存失败，请检查磁盘权限后重试");
        return false;
      }
      return true;
    }
    if (id == L"cancel") {
      // Dirty edits already flushed on each mutation; cancel just closes.
      return true;
    }
    return false;
  };

  MistWebDialogRun(&dlg, L"分页文件夹", 560, 440);
  // Closing the title-bar X used to discard unsaved adds — flush once more.
  PersistIfDirty(&ctx);
}

void OpenPageFolderMenu(HWND owner) {
  auto items = LoadPageFolders();
  if (items.empty()) {
    if (MistConfirm(owner, L"分页文件夹", L"还没有分页文件夹。现在添加？",
                    MistMsgTone::Info)) {
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
