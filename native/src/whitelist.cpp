#include "whitelist.h"

#include "paths_util.h"
#include "prompt_text.h"

#include <Windows.h>

#include <fstream>
#include <sstream>

namespace {

std::wstring WhitelistFile() {
  return JoinPath(NativeDataDir(), L"whitelist.json");
}

std::wstring BaseName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

bool MatchOne(const std::wstring& name, const std::wstring& pattern) {
  if (pattern.empty()) {
    return false;
  }
  if (pattern.find(L'*') == std::wstring::npos) {
    return _wcsicmp(name.c_str(), pattern.c_str()) == 0;
  }
  // Simple prefix*suffix glob (one star).
  const size_t star = pattern.find(L'*');
  const std::wstring pre = pattern.substr(0, star);
  const std::wstring suf = pattern.substr(star + 1);
  if (name.size() < pre.size() + suf.size()) {
    return false;
  }
  if (_wcsnicmp(name.c_str(), pre.c_str(), pre.size()) != 0) {
    return false;
  }
  if (suf.empty()) {
    return true;
  }
  return _wcsicmp(name.c_str() + (name.size() - suf.size()), suf.c_str()) == 0;
}

}  // namespace

std::vector<std::wstring> LoadWhitelist() {
  std::vector<std::wstring> out;
  std::ifstream in(WhitelistFile());
  if (!in) {
    return out;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  size_t pos = 0;
  while (true) {
    const size_t q1 = json.find('"', pos);
    if (q1 == std::string::npos) {
      break;
    }
    size_t q2 = q1 + 1;
    std::string val;
    while (q2 < json.size() && json[q2] != '"') {
      if (json[q2] == '\\' && q2 + 1 < json.size()) {
        ++q2;
      }
      val.push_back(json[q2++]);
    }
    if (q2 >= json.size()) {
      break;
    }
    pos = q2 + 1;
    // Skip JSON keys like "names"
    if (val == "names") {
      continue;
    }
    if (!val.empty()) {
      out.push_back(WidenUtf8(val));
    }
  }
  return out;
}

bool SaveWhitelist(const std::vector<std::wstring>& names) {
  std::ofstream out(WhitelistFile(), std::ios::binary);
  if (!out) {
    return false;
  }
  out << "{\n  \"names\": [\n";
  for (size_t i = 0; i < names.size(); ++i) {
    out << "    \"" << NarrowUtf8(names[i]) << "\"";
    if (i + 1 < names.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
  return true;
}

bool IsWhitelisted(const std::wstring& path_or_name,
                   const std::vector<std::wstring>& names) {
  const std::wstring name = BaseName(path_or_name);
  for (const auto& p : names) {
    if (MatchOne(name, p)) {
      return true;
    }
  }
  return false;
}

void EditWhitelistDialog(HWND owner) {
  auto names = LoadWhitelist();
  std::wstring msg = L"整理白名单（文件名，支持 *.lnk）：\n\n";
  if (names.empty()) {
    msg += L"（空）\n";
  } else {
    for (size_t i = 0; i < names.size(); ++i) {
      msg += std::to_wstring(i + 1) + L". " + names[i] + L"\n";
    }
  }
  msg += L"\n是 = 添加　否 = 删除一项　取消 = 关闭";
  const int choice =
      MessageBoxW(owner, msg.c_str(), L"整理白名单", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDCANCEL) {
    return;
  }
  if (choice == IDYES) {
    const std::wstring add =
        PromptText(owner, L"白名单", L"文件名或模式（如 Recycle Bin.lnk / *.tmp）：",
                   L"");
    if (!add.empty()) {
      names.push_back(add);
      SaveWhitelist(names);
    }
    return;
  }
  if (names.empty()) {
    return;
  }
  const std::wstring ans = PromptText(
      owner, L"删除白名单项",
      (L"输入编号 (1-" + std::to_wstring(names.size()) + L")：").c_str(), L"1");
  const int idx = _wtoi(ans.c_str()) - 1;
  if (idx >= 0 && idx < static_cast<int>(names.size())) {
    names.erase(names.begin() + idx);
    SaveWhitelist(names);
  }
}
