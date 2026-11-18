#include "snapshot_ui.h"

#include "i18n.h"
#include "mist_msg.h"
#include "mist_webview.h"
#include "prompt_text.h"
#include "snapshot.h"
#include "toast.h"

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
  SnapshotCardOptions opts;
  opts.render_thumbs = false;
  const SnapshotCardData card = BuildSnapshotCard(info, opts);
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
  std::wstring* applied_path = nullptr;
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

bool ManageSnapshotsDialog(HWND owner, AppState* applied,
                           std::wstring* applied_path) {
  if (!applied) {
    return false;
  }
  auto list = ListSnapshots();
  if (list.empty()) {
    MistAlert(owner, Tr("snap_title").c_str(), Tr("snap_empty").c_str(),
              MistMsgTone::Info);
    return false;
  }

  Ctx ctx;
  ctx.list = std::move(list);
  ctx.applied = applied;
  ctx.applied_path = applied_path;
  ctx.sel = 0;

  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.user = &ctx;
  dlg.user_data_subdir = L"webview2-snapshot";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\snapshot.html");
  if (dlg.html_path.empty()) {
    MistAlert(owner, Tr("snap_title").c_str(), L"找不到 snapshot.html",
              MistMsgTone::Warn);
    return false;
  }
  dlg.build_state = [&ctx]() { return BuildState(&ctx); };
  // Nested Mist / MessageBox inside WebMessageReceived freezes — always defer.
  constexpr WPARAM kDeferRestore = 1;
  constexpr WPARAM kDeferDelete = 2;
  constexpr WPARAM kDeferRename = 3;
  dlg.on_deferred = [&](WPARAM op, LPARAM index_lp) {
    const int sel = static_cast<int>(index_lp);
    if (sel < 0 || sel >= static_cast<int>(ctx.list.size())) {
      return;
    }
    HWND parent = dlg.hwnd ? dlg.hwnd : owner;
    const auto& item = ctx.list[static_cast<size_t>(sel)];
    if (op == kDeferRestore) {
      const std::wstring ask =
          Tr("snap_restore_ask") + L"\n「" + item.name + L"」";
      if (!MistConfirm(parent, Tr("snap_restore_title").c_str(), ask.c_str(),
                       MistMsgTone::Warn)) {
        return;
      }
      if (!LoadSnapshotWithIcons(item.path, ctx.applied)) {
        MistAlert(parent, Tr("snap_title").c_str(), Tr("snap_read_fail").c_str(),
                  MistMsgTone::Warn);
        return;
      }
      if (ctx.applied_path) {
        *ctx.applied_path = item.path;
      }
      ctx.restored = true;
      MistWebDialogClose(&dlg);
      return;
    }
    if (op == kDeferDelete) {
      const std::wstring ask =
          Tr("snap_delete_ask") + L"\n「" + item.name + L"」";
      if (!MistConfirm(parent, Tr("snap_delete_title").c_str(), ask.c_str(),
                       MistMsgTone::Danger)) {
        return;
      }
      if (DeleteSnapshot(item.path)) {
        ctx.list.erase(ctx.list.begin() + sel);
        if (sel < static_cast<int>(ctx.previews.size())) {
          ctx.previews.erase(ctx.previews.begin() + sel);
        }
        if (ctx.list.empty()) {
          MistWebDialogClose(&dlg);
          return;
        }
        ctx.sel = std::min(sel, static_cast<int>(ctx.list.size()) - 1);
        MistWebDialogPushState(&dlg);
      } else {
        MistAlert(parent, Tr("snap_title").c_str(),
                  Tr("snap_delete_fail").c_str(), MistMsgTone::Warn);
      }
      return;
    }
    if (op == kDeferRename) {
      // Deferred: Mist prompt is OK (not on WebMessageReceived stack).
      const std::wstring nv =
          PromptText(parent, L"重命名快照", L"新名称", item.name);
      if (nv.empty() || nv == item.name) {
        return;
      }
      if (!RenameSnapshot(item.path, nv)) {
        MistAlert(parent, L"重命名快照", Tr("snap_delete_fail").c_str(),
                  MistMsgTone::Warn);
        return;
      }
      ctx.list = ListSnapshots();
      ctx.previews.clear();
      ctx.sel = 0;
      MistWebDialogPushState(&dlg);
    }
  };
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
    if (id == L"restore" || id == L"delete" || id == L"rename") {
      const int sel = MistJsonIntField(json, L"index", ctx.sel);
      if (!dlg.hwnd || sel < 0 || sel >= static_cast<int>(ctx.list.size())) {
        return false;
      }
      WPARAM op = kDeferRestore;
      if (id == L"delete") {
        op = kDeferDelete;
      } else if (id == L"rename") {
        op = kDeferRename;
      }
      PostMessageW(dlg.hwnd, WM_APP + 81, op, static_cast<LPARAM>(sel));
      return false;
    }
    if (id == L"close") {
      return true;
    }
    return false;
  };

  MistWebDialogRun(&dlg, Tr("snap_mgr_title").c_str(), 680, 500);
  return ctx.restored;
}
