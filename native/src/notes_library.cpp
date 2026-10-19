#include "notes_library.h"

#include "paths_util.h"
#include "prompt_text.h"

#include <Shellapi.h>

#include <cwctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

std::wstring NotesLibraryDir(bool ensure) {
  const std::wstring dir = JoinPath(NativeDataDir(), L"notes");
  if (ensure) {
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  return dir;
}

std::wstring PickNoteFromLibrary(HWND owner) {
  const std::wstring dir = NotesLibraryDir(true);
  std::vector<std::wstring> files;
  const wchar_t* patterns[] = {L"*.rtf", L"*.md", L"*.txt"};
  for (const wchar_t* pat : patterns) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, pat).c_str(), &fd);
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

  std::wstring msg = L"笔记库：\n" + dir + L"\n\n";
  if (files.empty()) {
    msg += L"（空）\n";
  } else {
    for (size_t i = 0; i < files.size() && i < 30; ++i) {
      msg += std::to_wstring(i + 1) + L". " + files[i] + L"\n";
    }
  }
  msg += L"\n是 = 打开已有　否 = 新建　取消 = 关闭";
  const int choice =
      MessageBoxW(owner, msg.c_str(), L"笔记库", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDCANCEL) {
    return {};
  }
  if (choice == IDNO) {
    const std::wstring name =
        PromptText(owner, L"新建笔记", L"文件名（不含路径，如 idea.md）：",
                   L"note.md");
    if (name.empty()) {
      return {};
    }
    return JoinPath(dir, name);
  }
  if (files.empty()) {
    return {};
  }
  const std::wstring ans = PromptText(
      owner, L"打开笔记",
      (L"编号 (1-" + std::to_wstring(files.size()) + L")：").c_str(), L"1");
  const int idx = _wtoi(ans.c_str()) - 1;
  if (idx < 0 || idx >= static_cast<int>(files.size())) {
    return {};
  }
  return JoinPath(dir, files[static_cast<size_t>(idx)]);
}

namespace {

std::vector<std::wstring> ListNoteFiles(const std::wstring& dir) {
  std::vector<std::wstring> files;
  const wchar_t* patterns[] = {L"*.rtf", L"*.md", L"*.txt", L"*.markdown"};
  for (const wchar_t* pat : patterns) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, pat).c_str(), &fd);
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
  return files;
}

bool WideContains(std::wstring hay, std::wstring needle) {
  for (wchar_t& c : hay) {
    c = static_cast<wchar_t>(towlower(c));
  }
  for (wchar_t& c : needle) {
    c = static_cast<wchar_t>(towlower(c));
  }
  return hay.find(needle) != std::wstring::npos;
}

}  // namespace

std::wstring SearchNotesInLibrary(HWND owner) {
  const std::wstring query =
      PromptText(owner, L"搜索笔记", L"文件名或正文关键词：", L"");
  if (query.empty()) {
    return {};
  }
  const std::wstring dir = NotesLibraryDir(true);
  const std::string needle = NarrowUtf8(query);
  std::vector<std::wstring> hits;
  for (const auto& name : ListNoteFiles(dir)) {
    bool match = WideContains(name, query);
    if (!match && !needle.empty()) {
      std::ifstream in(JoinPath(dir, name), std::ios::binary);
      if (in) {
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string body = ss.str();
        if (body.size() <= 2 * 1024 * 1024 &&
            body.find(needle) != std::string::npos) {
          match = true;
        }
      }
    }
    if (match) {
      hits.push_back(name);
      if (hits.size() >= 30) {
        break;
      }
    }
  }
  if (hits.empty()) {
    MessageBoxW(owner, L"没有匹配的笔记。", L"搜索笔记", MB_OK | MB_ICONINFORMATION);
    return {};
  }
  std::wstring msg;
  for (size_t i = 0; i < hits.size(); ++i) {
    msg += std::to_wstring(i + 1) + L". " + hits[i] + L"\n";
  }
  const std::wstring ans = PromptText(
      owner, L"搜索笔记",
      (msg + L"\n打开编号 (1-" + std::to_wstring(hits.size()) + L")：").c_str(),
      L"1");
  const int idx = _wtoi(ans.c_str()) - 1;
  if (idx < 0 || idx >= static_cast<int>(hits.size())) {
    return {};
  }
  return JoinPath(dir, hits[static_cast<size_t>(idx)]);
}
