#include "minutes.h"

#include "paths_util.h"
#include "tool_paths.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include "prompt_text.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::wstring CfgPath() { return JoinPath(NativeDataDir(), L"minutes.json"); }

std::wstring ReadFolderCfg() {
  std::ifstream in(CfgPath());
  if (!in) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  const size_t p = json.find("\"folder\"");
  if (p == std::string::npos) {
    return {};
  }
  size_t q = json.find('"', json.find(':', p) + 1);
  if (q == std::string::npos) {
    return {};
  }
  ++q;
  std::string val;
  while (q < json.size() && json[q] != '"') {
    if (json[q] == '\\' && q + 1 < json.size()) {
      ++q;
    }
    val.push_back(json[q++]);
  }
  return WidenUtf8(val);
}

void WriteFolderCfg(const std::wstring& folder) {
  std::ofstream out(CfgPath(), std::ios::binary);
  if (!out) {
    return;
  }
  std::string s = NarrowUtf8(folder);
  std::string esc;
  for (char c : s) {
    if (c == '\\' || c == '"') {
      esc.push_back('\\');
    }
    esc.push_back(c);
  }
  out << "{\n  \"folder\":\"" << esc << "\"\n}\n";
}

int NextSerial(const std::wstring& folder, int year, int month, int day) {
  wchar_t prefix[32];
  swprintf_s(prefix, L"%04d%02d%02d_", year, month, day);
  const size_t plen = wcslen(prefix);
  int max_n = 0;
  const wchar_t* pats[] = {L"*.md", L"*.rtf"};
  for (const wchar_t* pat : pats) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(folder, pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      if (_wcsnicmp(fd.cFileName, prefix, plen) != 0) {
        continue;
      }
      const wchar_t* p = fd.cFileName + plen;
      int n = _wtoi(p);
      if (n > max_n) {
        max_n = n;
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return max_n + 1;
}

}  // namespace

std::wstring MinutesFolder(bool ensure) {
  std::wstring folder = ReadFolderCfg();
  if (folder.empty()) {
    folder = JoinPath(ExeDir(), L"纪要");
  }
  if (ensure) {
    CreateDirectoryW(folder.c_str(), nullptr);
  }
  return folder;
}

void ConfigureMinutesFolder(HWND owner) {
  BROWSEINFOW bi{};
  bi.hwndOwner = owner;
  bi.lpszTitle = L"选择会议纪要保存文件夹";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
  if (!pidl) {
    return;
  }
  wchar_t path[MAX_PATH] = {};
  SHGetPathFromIDListW(pidl, path);
  CoTaskMemFree(pidl);
  if (path[0]) {
    WriteFolderCfg(path);
    MessageBoxW(owner, path, L"纪要文件夹已设置", MB_OK | MB_ICONINFORMATION);
  }
}

std::wstring CreateAndOpenMinutes(HWND owner) {
  const std::wstring folder = MinutesFolder(true);
  SYSTEMTIME st{};
  GetLocalTime(&st);
  const int serial =
      NextSerial(folder, st.wYear, st.wMonth, st.wDay);
  wchar_t name[64];
  swprintf_s(name, L"%04d%02d%02d_%03d.md", st.wYear, st.wMonth, st.wDay,
             serial);
  const std::wstring path = JoinPath(folder, name);
  {
    std::ofstream out(path, std::ios::binary);
    out << "# 会议纪要\n\n"
        << "- 日期：" << st.wYear << "-" << st.wMonth << "-" << st.wDay << "\n"
        << "- 编号：" << serial << "\n"
        << "- 参与人：\n"
        << "- 议程：\n"
        << "- 决议：\n"
        << "- 待办：\n";
  }
  ShellExecuteW(owner, L"open", L"notepad.exe", path.c_str(), nullptr,
                SW_SHOWNORMAL);
  return path;
}

void OpenMinutesFolder(HWND owner) {
  const std::wstring folder = MinutesFolder(true);
  ShellExecuteW(owner, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring CreateAndOpenMinutesRtf(HWND owner) {
  const std::wstring folder = MinutesFolder(true);
  SYSTEMTIME st{};
  GetLocalTime(&st);
  const int serial = NextSerial(folder, st.wYear, st.wMonth, st.wDay);
  wchar_t name[64];
  swprintf_s(name, L"%04d%02d%02d_%03d.rtf", st.wYear, st.wMonth, st.wDay,
             serial);
  const std::wstring path = JoinPath(folder, name);
  {
    char body[512];
    sprintf_s(body,
              "{\\rtf1\\ansi\\ansicpg936\\deff0{\\fonttbl{\\f0\\fnil\\fcharset134 "
              "Microsoft YaHei;}}\\f0\\fs24 "
              "\\b Meeting Minutes\\b0\\par\\par "
              "Date: %04d-%02d-%02d\\par "
              "No: %d\\par\\par "
              "Attendees:\\par Agenda:\\par Decisions:\\par TODOs:\\par}",
              st.wYear, st.wMonth, st.wDay, serial);
    std::ofstream out(path, std::ios::binary);
    out << body;
  }
  ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return path;
}

bool OpenRecentMinutes(HWND owner) {
  const std::wstring folder = MinutesFolder(true);
  std::vector<std::wstring> files;
  const wchar_t* pats[] = {L"*.md", L"*.rtf"};
  for (const wchar_t* pat : pats) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(folder, pat).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        files.push_back(fd.cFileName);
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  std::sort(files.begin(), files.end());
  std::reverse(files.begin(), files.end());
  if (files.empty()) {
    MessageBoxW(owner, L"还没有纪要文件。", L"会议纪要", MB_OK | MB_ICONINFORMATION);
    return false;
  }
  std::wstring msg;
  const size_t n = (std::min)(files.size(), size_t{20});
  for (size_t i = 0; i < n; ++i) {
    msg += std::to_wstring(i + 1) + L". " + files[i] + L"\n";
  }
  const std::wstring ans = PromptText(
      owner, L"最近纪要",
      (msg + L"\n打开编号：").c_str(), L"1");
  const int idx = _wtoi(ans.c_str()) - 1;
  if (idx < 0 || idx >= static_cast<int>(n)) {
    return false;
  }
  const std::wstring path = JoinPath(folder, files[static_cast<size_t>(idx)]);
  ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return true;
}
