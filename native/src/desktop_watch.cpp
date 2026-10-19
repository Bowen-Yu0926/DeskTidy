#include "desktop_watch.h"

#include <ShlObj.h>

namespace {

std::wstring Known(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) {
    return {};
  }
  std::wstring out = p;
  CoTaskMemFree(p);
  return out;
}

bool FtEq(const FILETIME& a, const FILETIME& b) {
  return a.dwLowDateTime == b.dwLowDateTime &&
         a.dwHighDateTime == b.dwHighDateTime;
}

}  // namespace

FILETIME DesktopWatch::StampFolder(const std::wstring& folder) const {
  FILETIME ft{};
  if (folder.empty()) {
    return ft;
  }
  HANDLE h = CreateFileW(folder.c_str(), FILE_READ_EA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return ft;
  }
  GetFileTime(h, nullptr, nullptr, &ft);
  CloseHandle(h);
  return ft;
}

void DesktopWatch::Reset() {
  user_ = StampFolder(Known(FOLDERID_Desktop));
  public_ = StampFolder(Known(FOLDERID_PublicDesktop));
  primed_ = true;
}

bool DesktopWatch::PollChanged() {
  const FILETIME u = StampFolder(Known(FOLDERID_Desktop));
  const FILETIME p = StampFolder(Known(FOLDERID_PublicDesktop));
  if (!primed_) {
    user_ = u;
    public_ = p;
    primed_ = true;
    return false;
  }
  if (!FtEq(u, user_) || !FtEq(p, public_)) {
    user_ = u;
    public_ = p;
    return true;
  }
  return false;
}
