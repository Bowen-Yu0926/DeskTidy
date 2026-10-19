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

void DesktopWatch::SetExtraFolders(const std::vector<std::wstring>& folders) {
  std::vector<FILETIME> next;
  next.reserve(folders.size());
  for (const auto& folder : folders) {
    FILETIME kept{};
    bool found = false;
    for (size_t i = 0; i < extra_.size(); ++i) {
      if (extra_[i] == folder && i < extra_stamp_.size()) {
        kept = extra_stamp_[i];
        found = true;
        break;
      }
    }
    next.push_back(found ? kept : StampFolder(folder));
  }
  extra_ = folders;
  extra_stamp_ = std::move(next);
}

void DesktopWatch::Reset() {
  user_ = StampFolder(Known(FOLDERID_Desktop));
  public_ = StampFolder(Known(FOLDERID_PublicDesktop));
  extra_stamp_.clear();
  extra_stamp_.reserve(extra_.size());
  for (const auto& folder : extra_) {
    extra_stamp_.push_back(StampFolder(folder));
  }
  primed_ = true;
}

bool DesktopWatch::PollChanged() {
  const FILETIME u = StampFolder(Known(FOLDERID_Desktop));
  const FILETIME p = StampFolder(Known(FOLDERID_PublicDesktop));
  bool extra_changed = false;
  if (extra_stamp_.size() != extra_.size()) {
    extra_stamp_.assign(extra_.size(), FILETIME{});
  }
  for (size_t i = 0; i < extra_.size(); ++i) {
    const FILETIME now = StampFolder(extra_[i]);
    if (primed_ && !FtEq(now, extra_stamp_[i])) {
      extra_changed = true;
    }
    extra_stamp_[i] = now;
  }
  if (!primed_) {
    user_ = u;
    public_ = p;
    primed_ = true;
    return false;
  }
  if (!FtEq(u, user_) || !FtEq(p, public_) || extra_changed) {
    user_ = u;
    public_ = p;
    return true;
  }
  return false;
}
