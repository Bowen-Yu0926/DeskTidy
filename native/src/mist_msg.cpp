#include "mist_msg.h"

#include "mist_webview.h"
#include "prompt_text.h"

#include <string>

namespace {

const wchar_t* ToneClass(MistMsgTone tone) {
  switch (tone) {
    case MistMsgTone::Warn:
      return L"warn";
    case MistMsgTone::Danger:
      return L"danger";
    case MistMsgTone::Info:
    default:
      return L"";
  }
}

const wchar_t* ModeName(MistMsgKind kind) {
  switch (kind) {
    case MistMsgKind::Confirm:
      return L"confirm";
    case MistMsgKind::YesNoCancel:
      return L"ync";
    case MistMsgKind::Alert:
    default:
      return L"alert";
  }
}

MistMsgResult Win32Fallback(HWND owner, const wchar_t* title,
                            const wchar_t* message, MistMsgKind kind,
                            MistMsgTone tone) {
  UINT type = MB_OK;
  switch (kind) {
    case MistMsgKind::Confirm:
      type = MB_YESNO;
      break;
    case MistMsgKind::YesNoCancel:
      type = MB_YESNOCANCEL;
      break;
    case MistMsgKind::Alert:
    default:
      type = MB_OK;
      break;
  }
  switch (tone) {
    case MistMsgTone::Danger:
      type |= MB_ICONERROR;
      break;
    case MistMsgTone::Warn:
      type |= MB_ICONWARNING;
      break;
    case MistMsgTone::Info:
    default:
      type |= MB_ICONINFORMATION;
      break;
  }
  const int r = MessageBoxW(owner, message ? message : L"",
                            title ? title : L"DeskNote", type);
  if (r == IDYES) {
    return MistMsgResult::Yes;
  }
  if (r == IDNO) {
    return MistMsgResult::No;
  }
  if (r == IDOK) {
    return MistMsgResult::Ok;
  }
  return MistMsgResult::Cancel;
}

}  // namespace

MistMsgResult MistMessage(HWND owner, const wchar_t* title,
                          const wchar_t* message, MistMsgKind kind,
                          MistMsgTone tone) {
  const std::wstring html = MistFindAssetHtml(L"dialogs\\confirm.html");
  if (html.empty()) {
    return Win32Fallback(owner, title, message, kind, tone);
  }

  MistMsgResult result = MistMsgResult::Cancel;
  bool answered = false;

  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.frameless = true;
  dlg.user_data_subdir = L"webview2-mist-msg";
  dlg.html_path = html;
  dlg.build_state = [&]() {
    std::wstring json = L"{\"type\":\"state\",\"mode\":\"";
    json += ModeName(kind);
    json += L"\",\"tone\":\"";
    json += ToneClass(tone);
    json += L"\",\"title\":\"";
    json += MistJsonEscape(title ? title : L"提示");
    json += L"\",\"message\":\"";
    json += MistJsonEscape(message ? message : L"");
    json += L"\",\"danger\":";
    json += (tone == MistMsgTone::Danger) ? L"true" : L"false";
    json += L"}";
    return json;
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
    answered = true;
    if (id == L"ok") {
      result = MistMsgResult::Ok;
    } else if (id == L"yes") {
      result = MistMsgResult::Yes;
    } else if (id == L"no") {
      result = MistMsgResult::No;
    } else {
      result = MistMsgResult::Cancel;
    }
    return true;
  };

  MistWebDialogRun(&dlg, title ? title : L"提示", 460, 240);
  if (!answered || !dlg.web_available) {
    return Win32Fallback(owner, title, message, kind, tone);
  }
  return result;
}

std::wstring MistPromptText(HWND owner, const wchar_t* title,
                            const wchar_t* label,
                            const std::wstring& initial) {
  const std::wstring html = MistFindAssetHtml(L"dialogs\\prompt.html");
  if (html.empty()) {
    return PromptTextWin32(owner, title, label, initial);
  }

  std::wstring result;
  bool accepted = false;
  bool answered = false;

  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.frameless = true;
  dlg.user_data_subdir = L"webview2-mist-prompt";
  dlg.html_path = html;
  dlg.build_state = [&]() {
    std::wstring json = L"{\"type\":\"state\",\"title\":\"";
    json += MistJsonEscape(title ? title : L"输入");
    json += L"\",\"label\":\"";
    json += MistJsonEscape(label ? label : L"");
    json += L"\",\"value\":\"";
    json += MistJsonEscape(initial);
    json += L"\"}";
    return json;
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
    answered = true;
    if (id == L"ok") {
      result = MistJsonStringField(json, L"value");
      // Trim ends.
      while (!result.empty() &&
             (result.back() == L' ' || result.back() == L'\t')) {
        result.pop_back();
      }
      size_t i = 0;
      while (i < result.size() &&
             (result[i] == L' ' || result[i] == L'\t')) {
        ++i;
      }
      if (i) {
        result.erase(0, i);
      }
      if (result.empty()) {
        answered = false;
        return false;  // Keep open — empty not allowed.
      }
      accepted = true;
      return true;
    }
    accepted = false;
    result.clear();
    return true;
  };

  MistWebDialogRun(&dlg, title ? title : L"输入", 440, 220);
  if (!answered || !dlg.web_available) {
    return PromptTextWin32(owner, title, label, initial);
  }
  return accepted ? result : std::wstring{};
}
