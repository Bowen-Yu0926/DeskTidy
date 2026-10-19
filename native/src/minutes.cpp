#include "minutes.h"

#include "paths_util.h"
#include "tool_paths.h"

#include <ShlObj.h>
#include <Shellapi.h>

#include <fstream>
#include <sstream>
#include <string>

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
  const std::wstring pattern = JoinPath(folder, L"*.md");
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return 1;
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
