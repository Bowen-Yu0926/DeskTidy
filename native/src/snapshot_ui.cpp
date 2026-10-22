#include "snapshot_ui.h"

#include "i18n.h"
#include "mist_webview.h"
#include "prompt_text.h"
#include "snapshot.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::wstring BaseName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

std::wstring StripJsonExt(const std::wstring& path) {
  const std::wstring suf = L".json";
  if (path.size() >= suf.size() &&
      _wcsicmp(path.c_str() + (path.size() - suf.size()), suf.c_str()) == 0) {
    return path.substr(0, path.size() - suf.size());
  }
  return path;
}

// Non-destructive structured preview: per-fence geometry + icon (pinned) count
// + title. No PNG thumbnail — a clear text summary is shown instead.
std::wstring SnapshotPreviewText(const std::wstring& path) {
  SnapshotInfo info;
  info.path = path;
  info.name = StripJsonExt(BaseName(path));
  const SnapshotCardData card = BuildSnapshotCard(info);
  if (card.boxes.empty() && card.name.empty()) {
    return L"（无法读取预览）";
  }
  std::wstring out = L"页=" + std::to_wstring(card.focus_page) + L"  分区=" +
                     std::to_wstring(card.boxes.size()) + L"\n";
  for (size_t i = 0; i < card.boxes.size() && i < 12; ++i) {
    const auto& b = card.boxes[i];
    out += L" · " + b.title + L"  (" + std::to_wstring(b.w) + L"x" +
           std::to_wstring(b.h) + L") 图标=" + std::to_wstring(b.icon_count) +
           L"  p" + std::to_wstring(b.page) + L"\n";
  }
  if (card.boxes.size() > 12) {
    out += L" · …\n";
  }
  return out;
}

struct Ctx {
  std::vector<SnapshotInfo> list;
  std::vector<std::wstring> previews;
  AppState* applied = nullptr;
  bool restored = false;
  int sel = 0;
};

void EnsurePreviews(Ctx* ctx) {
  ctx->previews.resize(ctx->list.size());
  for (size_t i = 0; i < ctx->list.size(); ++i) {
    if (ctx->previews[i].empty()) {
      ctx->previews[i] = SnapshotPreviewText(ctx->list[i].path);
    }
  }
}

std::wstring BuildState(Ctx* ctx) {
  EnsurePreviews(ctx);
  if (ctx->sel < 0) {
    ctx->sel = 0;
  }
  if (ctx->sel >= static_cast<int>(ctx->list.size())) {
    ctx->sel = ctx->list.empty() ? -1 : 0;
  }
  std::wstring json = L"{\"type\":\"state\",\"sel\":";
  json += std::to_wstring(ctx->sel);
  json += L",\"items\":[";
  for (size_t i = 0; i < ctx->list.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L"{\"name\":\"";
    json += MistJsonEscape(ctx->list[i].name);
    json += L"\",\"preview\":\"";
    json += MistJsonEscape(ctx->previews[i]);
    json += L"\"}";
  }
  json += L"]}";
  return json;
}

}  // namespace

bool ManageSnapshotsDialog(HWND owner, AppState* applied) {
  if (!applied) {
    return false;
  }
  auto list = ListSnapshots();
  if (list.empty()) {
    MessageBoxW(owner, Tr("snap_empty").c_str(), Tr("snap_title").c_str(),
                MB_OK | MB_ICONINFORMATION);
    return false;
  }

  Ctx ctx;
  ctx.list = std::move(list);
  ctx.applied = applied;
  ctx.sel = 0;

  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.user = &ctx;
  dlg.user_data_subdir = L"webview2-snapshot";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\snapshot.html");
  if (dlg.html_path.empty()) {
    MessageBoxW(owner, L"找不到 snapshot.html", Tr("snap_title").c_str(),
                MB_OK | MB_ICONWARNING);
    return false;
  }
  dlg.build_state = [&ctx]() { return BuildState(&ctx); };
  dlg.on_message = [&](const std::wstring& json) -> bool {
    const std::wstring type = MistJsonStringField(json, L"type");
    if (type != L"action") {
      return false;
    }
    const std::wstring id = MistJsonStringField(json, L"id");
    if (id == L"ready" || id == L"select") {
      if (id == L"select") {
        ctx.sel = MistJsonIntField(json, L"index", ctx.sel);
      }
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"restore") {
      const int sel = MistJsonIntField(json, L"index", ctx.sel);
      if (sel < 0 || sel >= static_cast<int>(ctx.list.size())) {
        return false;
      }
      const auto& item = ctx.list[static_cast<size_t>(sel)];
      const std::wstring ask =
          Tr("snap_restore_ask") + L"\n「" + item.name + L"」";
      HWND parent = dlg.hwnd ? dlg.hwnd : owner;
      if (MessageBoxW(parent, ask.c_str(), Tr("snap_restore_title").c_str(),
                      MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return false;
      }
      if (!LoadSnapshotWithIcons(item.path, ctx.applied)) {
        MessageBoxW(parent, Tr("snap_read_fail").c_str(),
                    Tr("snap_title").c_str(), MB_OK | MB_ICONWARNING);
        return false;
      }
      ctx.restored = true;
      return true;
    }
    if (id == L"delete") {
      const int sel = MistJsonIntField(json, L"index", ctx.sel);
      if (sel < 0 || sel >= static_cast<int>(ctx.list.size())) {
        return false;
      }
      const auto& item = ctx.list[static_cast<size_t>(sel)];
      const std::wstring ask =
          Tr("snap_delete_ask") + L"\n「" + item.name + L"」";
      HWND parent = dlg.hwnd ? dlg.hwnd : owner;
      if (MessageBoxW(parent, ask.c_str(), Tr("snap_delete_title").c_str(),
                      MB_YESNO | MB_ICONWARNING) != IDYES) {
        return false;
      }
      if (DeleteSnapshot(item.path)) {
        ctx.list.erase(ctx.list.begin() + sel);
        if (sel < static_cast<int>(ctx.previews.size())) {
          ctx.previews.erase(ctx.previews.begin() + sel);
        }
        if (ctx.list.empty()) {
          return true;
        }
        ctx.sel = std::min(sel, static_cast<int>(ctx.list.size()) - 1);
        MistWebDialogPushState(&dlg);
      } else {
        MessageBoxW(parent, Tr("snap_delete_fail").c_str(),
                    Tr("snap_title").c_str(), MB_OK | MB_ICONWARNING);
      }
      return false;
    }
    if (id == L"rename") {
      const int sel = MistJsonIntField(json, L"index", ctx.sel);
      if (sel < 0 || sel >= static_cast<int>(ctx.list.size())) {
        return false;
      }
      HWND parent = dlg.hwnd ? dlg.hwnd : owner;
      const auto& item = ctx.list[static_cast<size_t>(sel)];
      const std::wstring nv =
          PromptText(parent, L"重命名快照", L"新名称", item.name);
      if (nv.empty() || nv == item.name) {
        return false;
      }
      if (!RenameSnapshot(item.path, nv)) {
        MessageBoxW(parent, Tr("snap_delete_fail").c_str(), L"重命名快照",
                    MB_OK | MB_ICONWARNING);
        return false;
      }
      ctx.list = ListSnapshots();
      ctx.previews.clear();
      ctx.sel = 0;
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"close") {
      return true;
    }
    return false;
  };

  MistWebDialogRun(&dlg, Tr("snap_mgr_title").c_str(), 580, 420);
  return ctx.restored;
}
