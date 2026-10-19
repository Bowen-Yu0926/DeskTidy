#include "rule_store.h"

#include "pin_store.h"

#include <Windows.h>
#include <ShlObj.h>

#include <fstream>
#include <sstream>

namespace {

std::string Narrow(const std::wstring& ws) {
  if (ws.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), n, nullptr,
                        nullptr);
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  }
  return out;
}

std::string EscapeJson(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::wstring ExtOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
    return L"";
  }
  std::wstring ext = path.substr(dot);
  for (auto& ch : ext) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  return ext;
}

std::wstring RuleFilePath() {
  wchar_t* appdata = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata)) ||
      !appdata) {
    return L"rules.json";
  }
  std::wstring dir = appdata;
  CoTaskMemFree(appdata);
  dir += L"\\DeskTidyNative";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\rules.json";
}

RuleMap DefaultRules() {
  return {
      {L".lnk", "system_software"},
      {L".exe", "system_software"},
      {L".url", "system_software"},
      {L".pdf", "system_common"},
      {L".docx", "system_common"},
      {L".doc", "system_common"},
      {L".xlsx", "system_common"},
      {L".pptx", "system_common"},
      {L".txt", "system_common"},
      {L".md", "system_common"},
      {L".png", "system_common"},
      {L".jpg", "system_common"},
      {L".jpeg", "system_common"},
      {L".zip", "system_common"},
      {L"*", "system_common"},
  };
}

RuleMap LoadRules() {
  std::ifstream in(RuleFilePath());
  if (!in) {
    return DefaultRules();
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  RuleMap rules;
  size_t pos = json.find("\"rules\"");
  if (pos == std::string::npos) {
    return DefaultRules();
  }
  pos = json.find('{', pos);
  if (pos == std::string::npos) {
    return DefaultRules();
  }
  ++pos;
  while (pos < json.size()) {
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' ||
            json[pos] == '\t' || json[pos] == ',')) {
      ++pos;
    }
    if (pos >= json.size() || json[pos] == '}') {
      break;
    }
    if (json[pos] != '"') {
      break;
    }
    ++pos;
    std::string key;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        ++pos;
      }
      key.push_back(json[pos++]);
    }
    if (pos >= json.size()) {
      break;
    }
    ++pos;
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
      break;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
      break;
    }
    ++pos;
    std::string val;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        ++pos;
      }
      val.push_back(json[pos++]);
    }
    if (!key.empty() && !val.empty()) {
      rules[Widen(key)] = val;
    }
    if (pos < json.size() && json[pos] == '"') {
      ++pos;
    }
  }
  if (rules.empty()) {
    return DefaultRules();
  }
  return rules;
}

bool SaveRules(const RuleMap& rules) {
  std::ofstream out(RuleFilePath(), std::ios::binary);
  if (!out) {
    return false;
  }
  out << "{\n  \"rules\": {\n";
  size_t i = 0;
  for (const auto& kv : rules) {
    out << "    \"" << EscapeJson(Narrow(kv.first)) << "\":\""
        << EscapeJson(kv.second) << "\"";
    if (++i < rules.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  }\n}\n";
  return true;
}

std::string ResolveFenceId(const std::wstring& path, bool is_link,
                           const std::unordered_map<std::wstring, std::string>& pins,
                           const RuleMap& rules,
                           const std::string& fallback_software,
                           const std::string& fallback_common) {
  const auto pit = pins.find(NormalizePinKey(path));
  if (pit != pins.end()) {
    return pit->second;
  }
  const std::wstring ext = is_link ? L".lnk" : ExtOf(path);
  auto rit = rules.find(ext);
  if (rit != rules.end()) {
    return rit->second;
  }
  rit = rules.find(L"*");
  if (rit != rules.end()) {
    return rit->second;
  }
  return is_link ? fallback_software : fallback_common;
}
