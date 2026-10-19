#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>
#include <vector>

// Cheap poll of user/public desktop folder mtimes.
class DesktopWatch {
 public:
  void Reset();
  // Portal folders (and any other dirs) polled with the desktop stamps.
  void SetExtraFolders(const std::vector<std::wstring>& folders);
  // Returns true when desktop or extra-folder contents likely changed.
  bool PollChanged();

 private:
  FILETIME StampFolder(const std::wstring& folder) const;
  FILETIME user_{};
  FILETIME public_{};
  std::vector<std::wstring> extra_;
  std::vector<FILETIME> extra_stamp_;
  bool primed_ = false;
};
