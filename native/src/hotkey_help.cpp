#include "hotkey_help.h"

#include "hotkey_store.h"
#include "i18n.h"
#include "mist_msg.h"
#include "paths_util.h"

#include <string>

namespace {

std::wstring LabelFor(const HotkeyDef& k) {
  const std::string key = std::string("hk_") + k.id;
  const std::wstring tr = Tr(key.c_str());
  if (!tr.empty() && tr != WidenUtf8(key)) {
    return tr;
  }
  return k.label.empty() ? WidenUtf8(k.id) : k.label;
}

}  // namespace

void ShowHotkeyHelp(HWND owner) {
  const auto keys = LoadHotkeys();
  std::wstring body = Tr("hk_help_intro");
  for (const auto& k : keys) {
    const std::wstring chord = FormatHotkey(k.modifiers, k.vk);
    body += chord.empty() ? Tr("hk_unbound") : chord;
    body += L"　　";
    body += LabelFor(k);
    body += L"\n";
  }
  body += Tr("hk_help_footer");
  MistAlert(owner, Tr("hk_help_title").c_str(), body.c_str(), MistMsgTone::Info);
}
