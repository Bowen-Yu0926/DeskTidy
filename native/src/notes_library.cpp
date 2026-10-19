#include "notes_library.h"

#include "paths_util.h"
#include "prompt_text.h"

#include <Shellapi.h>

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
