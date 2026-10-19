#include "snapshot.h"

#include "paths_util.h"
#include "shell_icons.h"

#include <Windows.h>

#include <fstream>
#include <sstream>

std::wstring SnapshotDir() {
  const std::wstring dir = JoinPath(NativeDataDir(), L"snapshots");
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

bool SaveSnapshot(const AppState& state, const std::wstring& name) {
  if (name.empty()) {
    return false;
  }
  std::wstring safe = name;
  for (auto& ch : safe) {
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
        ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
      ch = L'_';
    }
  }
  const std::wstring path = JoinPath(SnapshotDir(), safe + L".json");
  // Reuse SaveAppState by writing to a temp then copy — simplest: duplicate JSON write.
  // Call SaveAppState to main file then copy.
  if (!SaveAppState(state)) {
    return false;
  }
  const std::wstring src = LayoutFilePath();
  return CopyFileW(src.c_str(), path.c_str(), FALSE) != 0;
}

std::vector<SnapshotInfo> ListSnapshots() {
  std::vector<SnapshotInfo> out;
  const std::wstring pattern = JoinPath(SnapshotDir(), L"*.json");
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return out;
  }
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      continue;
    }
    SnapshotInfo info;
    info.name = fd.cFileName;
    if (info.name.size() > 5) {
      info.name.resize(info.name.size() - 5);  // strip .json
    }
    info.path = JoinPath(SnapshotDir(), fd.cFileName);
    out.push_back(std::move(info));
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return out;
}

bool LoadSnapshot(const std::wstring& path, AppState* out) {
  if (!out || path.empty()) {
    return false;
  }
  if (!CopyFileW(path.c_str(), LayoutFilePath().c_str(), FALSE)) {
    return false;
  }
  *out = LoadAppState();
  return true;
}

bool SaveSnapshotWithIcons(const AppState& state, const std::wstring& name) {
  if (!SaveSnapshot(state, name)) {
    return false;
  }
  std::wstring safe = name;
  for (auto& ch : safe) {
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
        ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
      ch = L'_';
    }
  }
  const std::wstring path = JoinPath(SnapshotDir(), safe + L".json");
  const auto icons = CaptureShellIconPositions();
  SaveShellIconsSidecar(path, icons);
  return true;
}

bool LoadSnapshotWithIcons(const std::wstring& path, AppState* out) {
  if (!LoadSnapshot(path, out)) {
    return false;
  }
  const auto icons = LoadShellIconsSidecar(path);
  if (!icons.empty()) {
    RestoreShellIconPositions(icons);
  }
  return true;
}
