#include "desknote_help.h"

#include "i18n.h"
#include "mist_msg.h"
#include "mist_webview.h"

#include <cstring>
#include <string>

void ShowDeskNoteHelp(HWND owner) {
  const char* lang = CurrentLang();
  const bool en = (lang && std::strcmp(lang, "en") == 0);
  const wchar_t* rel = en ? L"dialogs\\desknote_help_en.html"
                          : L"dialogs\\desknote_help_zh.html";
  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.user_data_subdir = L"webview2-desknote-help";
  dlg.html_path = MistFindAssetHtml(rel);
  if (dlg.html_path.empty()) {
    MistAlert(owner, L"帮助",
              L"找不到 DeskNote 帮助页（assets/dialogs/desknote_help*.html）。",
              MistMsgTone::Warn);
    return;
  }
  dlg.build_state = []() { return std::wstring(L"{\"type\":\"state\"}"); };
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
    return id == L"close";
  };
  MistWebDialogRun(&dlg, L"DeskNote 帮助", 760, 620);
}
