#include "desktop_watch.h"

#include <ShlObj.h>

namespace {

// Portal folders may be huge (user can point one at a tree with tens of
// thousands of files). Desktop/Public are also FindFirstFile-hashed — with
// SHChangeNotify as the primary signal, both use a slower poll cadence so the
// 2s watch timer does not hitch the UI thread on large/OneDrive desktops.
constexpr ULONGLONG kPortalRescanMs = 10000;
constexpr ULONGLONG kDesktopRescanMs = 10000;

std::wstring Known(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p)) || !p) {
    return {};
  }
  std::wstring out = p;
  CoTaskMemFree(p);
  return out;
}

ULONGLONG MixHash(ULONGLONG h, ULONGLONG v) {
  // FNV-ish mix — stable, cheap for poll ticks.
  h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
  return h;
}

ULONGLONG HashName(const wchar_t* name) {
  ULONGLONG h = 14695981039346656037ULL;
  for (const wchar_t* p = name; p && *p; ++p) {
    wchar_t c = *p;
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    h ^= static_cast<ULONGLONG>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

}  // namespace

DesktopWatch::FolderStamp DesktopWatch::StampFolder(
    const std::wstring& folder) const {
  FolderStamp stamp{};
  if (folder.empty()) {
    return stamp;
  }

  // FILE_READ_ATTRIBUTES (not FILE_READ_EA) — EA open often fails on Desktop.
  HANDLE h = CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                         nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    GetFileTime(h, nullptr, nullptr, &stamp.write_time);
    CloseHandle(h);
  }

  // Membership only (name + attrs). Size/mtime churn mid-Explorer-copy must
  // not re-fire polls — Fences-class tools refresh on create/delete/rename,
  // not every byte written while a drop is still in progress.
  const std::wstring pattern = folder + L"\\*";
  WIN32_FIND_DATAW fd{};
  HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) {
    return stamp;
  }
  ULONGLONG hash = 14695981039346656037ULL;
  DWORD count = 0;
  do {
    if (fd.cFileName[0] == L'.' &&
        (fd.cFileName[1] == L'\0' ||
         (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
      continue;
    }
    ++count;
    hash = MixHash(hash, HashName(fd.cFileName));
    hash = MixHash(hash, fd.dwFileAttributes);
  } while (FindNextFileW(find, &fd));
  FindClose(find);
  stamp.entry_count = count;
  stamp.content_hash = hash;
  return stamp;
}

bool DesktopWatch::StampEq(const FolderStamp& a, const FolderStamp& b) {
  // Ignore folder write_time: Desktop mtime ticks continuously during copy.
  return a.entry_count == b.entry_count && a.content_hash == b.content_hash;
}

void DesktopWatch::SetExtraFolders(const std::vector<std::wstring>& folders) {
  // Watch tick rebuilds this every period — skip when portal set unchanged
  // (avoids per-folder StampFolder on new entries / O(n²) stamp remap).
  if (folders == extra_) {
    return;
  }
  std::vector<FolderStamp> next;
  next.reserve(folders.size());
  for (const auto& folder : folders) {
    FolderStamp kept{};
    bool found = false;
    for (size_t i = 0; i < extra_.size(); ++i) {
      if (extra_[i] == folder && i < extra_stamp_.size()) {
        kept = extra_stamp_[i];
        found = true;
        break;
      }
    }
    // Keep prior stamp until PollChanged — avoid false positive on portal list
    // reorder. New folders start from a live stamp.
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
  extra_stamp_ms_ = GetTickCount64();
  desk_stamp_ms_ = extra_stamp_ms_;
  primed_ = true;
}

bool DesktopWatch::PollChanged() {
  bool changed = false;
  bool extra_changed = false;
  // Throttle Desktop/Public + portal full hashes. Shell notifications drive
  // the authoritative refresh; this poll is a OneDrive/quirk backup only.
  const ULONGLONG now_ms = GetTickCount64();
  const bool restamp_desk =
      !primed_ || now_ms - desk_stamp_ms_ >= kDesktopRescanMs;
  if (restamp_desk) {
    desk_stamp_ms_ = now_ms;
    const FolderStamp u = StampFolder(Known(FOLDERID_Desktop));
    const FolderStamp p = StampFolder(Known(FOLDERID_PublicDesktop));
    if (!primed_) {
      user_ = u;
      public_ = p;
      primed_ = true;
    } else if (!StampEq(u, user_) || !StampEq(p, public_)) {
      user_ = u;
      public_ = p;
      changed = true;
    }
  }
  const bool restamp_extras =
      !extra_.empty() && now_ms - extra_stamp_ms_ >= kPortalRescanMs;
  if (restamp_extras) {
    extra_stamp_ms_ = now_ms;
    if (extra_stamp_.size() != extra_.size()) {
      extra_stamp_.assign(extra_.size(), FolderStamp{});
    }
    for (size_t i = 0; i < extra_.size(); ++i) {
      const FolderStamp now = StampFolder(extra_[i]);
      if (primed_ && !StampEq(now, extra_stamp_[i])) {
        extra_changed = true;
      }
      extra_stamp_[i] = now;
    }
  }
  return changed || extra_changed;
}
