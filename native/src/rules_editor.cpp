#include "rules_editor.h"

#include "mist_msg.h"
#include "mist_webview.h"
#include "paths_util.h"

#include <Windows.h>
#include <Shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

struct Ctx {
  RuleMap* rules = nullptr;
  std::vector<std::pair<std::wstring, std::string>> rows;
  std::vector<std::pair<std::string, std::wstring>> fences;
  bool saved = false;
};

void LoadRows(Ctx* ctx) {
  ctx->rows.clear();
  if (!ctx->rules) {
    return;
  }
  for (const auto& kv : *ctx->rules) {
    ctx->rows.push_back({kv.first, kv.second});
  }
  std::sort(ctx->rows.begin(), ctx->rows.end(),
            [](const auto& a, const auto& b) {
              return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0;
            });
}

void CommitRows(Ctx* ctx) {
  if (!ctx->rules) {
    return;
  }
  ctx->rules->clear();
  for (const auto& row : ctx->rows) {
    if (!row.first.empty() && !row.second.empty()) {
      (*ctx->rules)[row.first] = row.second;
    }
  }
}

std::wstring FenceTitle(const Ctx* ctx, const std::string& id) {
  for (const auto& f : ctx->fences) {
    if (f.first == id) {
      return f.second;
    }
  }
  return {};
}

std::wstring BuildState(Ctx* ctx) {
  std::wstring json = L"{\"type\":\"state\",\"items\":[";
  for (size_t i = 0; i < ctx->rows.size(); ++i) {
    if (i) {
      json += L',';
    }
    const auto& row = ctx->rows[i];
    const std::wstring title = FenceTitle(ctx, row.second);
    json += L"{\"pattern\":\"";
    json += MistJsonEscape(row.first);
    json += L"\",\"fence\":\"";
    json += MistJsonEscape(WidenUtf8(row.second));
    json += L"\",\"fence_title\":\"";
    json += MistJsonEscape(title);
    json += L"\"}";
  }
  json += L"],\"fences\":[";
  for (size_t i = 0; i < ctx->fences.size(); ++i) {
    if (i) {
      json += L',';
    }
    json += L"{\"id\":\"";
    json += MistJsonEscape(WidenUtf8(ctx->fences[i].first));
    json += L"\",\"title\":\"";
    json += MistJsonEscape(ctx->fences[i].second);
    json += L"\"}";
  }
  json += L"]}";
  return json;
}

}  // namespace

bool EditRulesDialog(
    HWND owner, RuleMap* inout,
    const std::vector<std::pair<std::string, std::wstring>>& fence_hints) {
  if (!inout) {
    return false;
  }
  Ctx ctx;
  ctx.rules = inout;
  ctx.fences = fence_hints;
  LoadRows(&ctx);

  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.user = &ctx;
  dlg.user_data_subdir = L"webview2-rules";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\rules.html");
  if (dlg.html_path.empty()) {
    MistAlert(owner, L"整理规则", L"找不到 rules.html", MistMsgTone::Warn);
    return false;
  }
  dlg.build_state = [&ctx]() { return BuildState(&ctx); };
  constexpr WPARAM kDeferWarn = 1;
  constexpr WPARAM kDeferReset = 2;
  std::wstring pending_warn;
  dlg.on_deferred = [&](WPARAM op, LPARAM) {
    HWND parent = dlg.hwnd ? dlg.hwnd : owner;
    if (op == kDeferWarn) {
      if (!pending_warn.empty()) {
        MistAlert(parent, L"整理规则", pending_warn.c_str(), MistMsgTone::Warn);
        pending_warn.clear();
      }
      return;
    }
    if (op == kDeferReset) {
      if (MistConfirm(parent, L"整理规则", L"恢复为内置默认规则？",
                      MistMsgTone::Warn)) {
        *ctx.rules = DefaultRules();
        LoadRows(&ctx);
        MistWebDialogPushState(&dlg);
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
      const std::wstring pattern = MistJsonStringField(json, L"pattern");
      const std::wstring fence = MistJsonStringField(json, L"fence");
      const std::string reason = ValidateRule(pattern, NarrowUtf8(fence));
      if (!reason.empty()) {
        pending_warn = L"规则无效：" + WidenUtf8(reason);
        if (dlg.hwnd) {
          PostMessageW(dlg.hwnd, WM_APP + 81, kDeferWarn, 0);
        }
        return false;
      }
      if (!pattern.empty() && !fence.empty()) {
        ctx.rows.push_back({pattern, NarrowUtf8(fence)});
        MistWebDialogPushState(&dlg);
      }
      return false;
    }
    if (id == L"edit") {
      const int index = MistJsonIntField(json, L"index", -1);
      const std::wstring pattern = MistJsonStringField(json, L"pattern");
      const std::wstring fence = MistJsonStringField(json, L"fence");
      const std::string reason = ValidateRule(pattern, NarrowUtf8(fence));
      if (!reason.empty()) {
        pending_warn = L"规则无效：" + WidenUtf8(reason);
        if (dlg.hwnd) {
          PostMessageW(dlg.hwnd, WM_APP + 81, kDeferWarn, 0);
        }
        return false;
      }
      if (index >= 0 && index < static_cast<int>(ctx.rows.size()) &&
          !pattern.empty() && !fence.empty()) {
        ctx.rows[static_cast<size_t>(index)] = {pattern, NarrowUtf8(fence)};
        MistWebDialogPushState(&dlg);
      }
      return false;
    }
    if (id == L"del") {
      const int index = MistJsonIntField(json, L"index", -1);
      if (index >= 0 && index < static_cast<int>(ctx.rows.size())) {
        ctx.rows.erase(ctx.rows.begin() + index);
        MistWebDialogPushState(&dlg);
      }
      return false;
    }
    if (id == L"reset") {
      if (dlg.hwnd) {
        PostMessageW(dlg.hwnd, WM_APP + 81, kDeferReset, 0);
      }
      return false;
    }
    if (id == L"notepad") {
      CommitRows(&ctx);
      SaveRules(*ctx.rules);
      ShellExecuteW(dlg.hwnd ? dlg.hwnd : owner, L"open", L"notepad.exe",
                    RuleFilePath().c_str(), nullptr, SW_SHOWNORMAL);
      return false;
    }
    if (id == L"save") {
      CommitRows(&ctx);
      SaveRules(*ctx.rules);
      ctx.saved = true;
      return true;
    }
    if (id == L"cancel") {
      return true;
    }
    return false;
  };

  MistWebDialogRun(&dlg, L"整理规则", 560, 440);
  return ctx.saved;
}
