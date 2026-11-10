#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Poll user/public desktop (+ portal extras) for membership changes.
// Hash names+attrs only (not size/mtime): mid-copy growth must not look like
// a new desktop change. Folder mtime alone is also unreliable on NTFS/OneDrive.
class DesktopWatch {
 public:
  void Reset();
  // Portal folders (and any other dirs) polled with the desktop stamps.
  void SetExtraFolders(const std::vector<std::wstring>& folders);
  // Returns true when desktop or extra-folder contents likely changed.
  bool PollChanged();

 private:
  struct FolderStamp {
    FILETIME write_time{};
    DWORD entry_count = 0;
    ULONGLONG content_hash = 0;
  };

  FolderStamp StampFolder(const std::wstring& folder) const;
  static bool StampEq(const FolderStamp& a, const FolderStamp& b);

  FolderStamp user_{};
  FolderStamp public_{};
  std::vector<std::wstring> extra_;
  std::vector<FolderStamp> extra_stamp_;
  // Portal/extras can point at huge directories — throttle their full scan so
  // the 2s tick only re-stamps them at a slower cadence (see kPortalRescanMs).
  ULONGLONG extra_stamp_ms_ = 0;
  // Desktop/Public membership stamp cadence (SHChangeNotify is primary).
  ULONGLONG desk_stamp_ms_ = 0;
  bool primed_ = false;
};
