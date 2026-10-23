#include "tool_paths.h"

#include "paths_util.h"

#include <Windows.h>

#include <string>

std::wstring ExeDir() {
  wchar_t module[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  std::wstring dir = module;
  const size_t slash = dir.find_last_of(L'\\');
  if (slash != std::wstring::npos) {
    dir.resize(slash);
  }
  return dir;
}

namespace {

bool FileExists(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring SearchCandidates(const std::wstring* cands, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (FileExists(cands[i])) {
      return cands[i];
    }
  }
  return {};
}

}  // namespace

std::wstring FindFfmpegExe() {
  const std::wstring dir = ExeDir();
  // Walk up a few levels for repo layout: native\build\Release -> repo\assets
  std::wstring up = dir;
  std::wstring candidates_storage[24];
  size_t n = 0;
  auto add = [&](std::wstring p) {
    if (n < 24) {
      candidates_storage[n++] = std::move(p);
    }
  };
  add(dir + L"\\ffmpeg.exe");
  add(dir + L"\\assets\\ffmpeg\\ffmpeg.exe");
  // On-demand fetch installs under the app data root (portable or LOCALAPPDATA).
  {
    const std::wstring data_dir = NativeDataDir();
    if (!data_dir.empty()) {
      add(data_dir + L"\\ffmpeg\\ffmpeg.exe");
    }
  }
  for (int i = 0; i < 5; ++i) {
    const size_t slash = up.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
      break;
    }
    up.resize(slash);
    add(up + L"\\assets\\ffmpeg\\ffmpeg.exe");
    add(up + L"\\ffmpeg\\ffmpeg.exe");
  }
  // Runtime on-demand fetch location (Python ffmpeg_fetch parity).
  const wchar_t* profile = _wgetenv(L"USERPROFILE");
  if (profile && profile[0]) {
    add((std::wstring(profile) + L"\\.desktidy\\ffmpeg\\ffmpeg.exe"));
  }
  const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
  if (local && local[0]) {
    add((std::wstring(local) + L"\\DeskTidyNative\\ffmpeg\\ffmpeg.exe"));
  }
  const wchar_t* pf = _wgetenv(L"ProgramFiles");
  if (pf && pf[0]) {
    add((std::wstring(pf) + L"\\ffmpeg\\bin\\ffmpeg.exe"));
  }
  const wchar_t* pfx = _wgetenv(L"ProgramFiles(x86)");
  if (pfx && pfx[0]) {
    add((std::wstring(pfx) + L"\\ffmpeg\\bin\\ffmpeg.exe"));
  }
  if (local && local[0]) {
    add((std::wstring(local) +
         L"\\Microsoft\\WinGet\\Links\\ffmpeg.exe"));
  }
  // PATH lookup via SearchPath
  wchar_t found[MAX_PATH] = {};
  if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr)) {
    add(found);
  }
  return SearchCandidates(candidates_storage, n);
}

std::wstring FindFdExe() {
  const std::wstring dir = ExeDir();
  std::wstring up = dir;
  std::wstring candidates_storage[12];
  size_t n = 0;
  auto add = [&](std::wstring p) {
    if (n < 12) {
      candidates_storage[n++] = std::move(p);
    }
  };
  add(dir + L"\\fd.exe");
  add(dir + L"\\assets\\fd\\fd.exe");
  for (int i = 0; i < 5; ++i) {
    const size_t slash = up.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
      break;
    }
    up.resize(slash);
    add(up + L"\\assets\\fd\\fd.exe");
  }
  wchar_t found[MAX_PATH] = {};
  if (SearchPathW(nullptr, L"fd.exe", nullptr, MAX_PATH, found, nullptr)) {
    add(found);
  }
  return SearchCandidates(candidates_storage, n);
}
