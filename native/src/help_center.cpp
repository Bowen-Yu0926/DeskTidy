#include "help_center.h"

#include "first_run.h"
#include "i18n.h"
#include "mist_msg.h"
#include "mist_webview.h"
#include "paths_util.h"
#include "tool_paths.h"
#include "tools_misc.h"

#include <Shellapi.h>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::wstring FindHelpDocHtml() {
  wchar_t mod[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring dir = mod;
  const size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    dir.resize(slash);
  }
  const std::wstring candidates[] = {
      JoinPath(dir, L"docs\\desktidy.html"),
      JoinPath(dir, L"..\\docs\\desktidy.html"),
      JoinPath(dir, L"..\\..\\docs\\desktidy.html"),
      JoinPath(ExeDir(), L"docs\\desktidy.html"),
  };
  for (const auto& c : candidates) {
    if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return c;
    }
  }
  return {};
}

// Read a whole file (UTF-8). Returns empty on failure.
std::string ReadWholeFile(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Inject the operations handbook chapters into the manual template (Python
// help_content.py parity: sections, anchors and search box). The chapters asset
// is generated at build time and copied next to the exe.
void OpenHelpDocs(HWND owner) {
  const std::wstring path = FindHelpDocHtml();
  if (path.empty()) {
    MistAlert(owner, L"无法打开帮助",
              L"未找到帮助页模板（docs/desktidy.html）。请确认安装完整。",
              MistMsgTone::Warn);
    return;
  }
  std::string html = ReadWholeFile(path);
  const std::wstring chapters_path =
      MistFindAssetHtml(L"dialogs\\help_manual_chapters.html");
  if (!chapters_path.empty()) {
    const std::string chapters = ReadWholeFile(chapters_path);
    const std::string open_marker = "<!-- HELP_MANUAL -->";
    const std::string close_marker = "<!-- /HELP_MANUAL -->";
    const size_t open_pos = html.find(open_marker);
    const size_t close_pos = html.find(close_marker);
    if (!chapters.empty() && open_pos != std::string::npos &&
        close_pos != std::string::npos && close_pos > open_pos) {
      html = html.substr(0, open_pos) + chapters +
             html.substr(close_pos + close_marker.size());
    }
  }
  const std::wstring out = JoinPath(NativeDataDir(), L"DeskTidy_manual.html");
  {
    std::ofstream f(out, std::ios::binary);
    if (f) {
      f << html;
    }
  }
  ShellExecuteW(owner, L"open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ReplayFirstRunGuide(HWND owner) {
  const std::wstring marker = JoinPath(NativeDataDir(), L"firstrun.done");
  DeleteFileW(marker.c_str());
  MaybeShowFirstRun(owner);
}

}  // namespace

void ShowAppHelpCenter(HWND owner) {
  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.user_data_subdir = L"webview2-help";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\help.html");
  if (dlg.html_path.empty()) {
    MistAlert(owner, L"帮助", L"找不到 help.html", MistMsgTone::Warn);
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
    if (id == L"open_docs") {
      OpenHelpDocs(dlg.hwnd ? dlg.hwnd : owner);
      return false;
    }
    if (id == L"guide") {
      MistWebDialogClose(&dlg);
      ReplayFirstRunGuide(owner);
      return true;
    }
    if (id == L"updates") {
      OpenUpdatePage(dlg.hwnd ? dlg.hwnd : owner);
      return false;
    }
    return id == L"close";
  };
  MistWebDialogRun(&dlg, L"帮助", 560, 560);
}
