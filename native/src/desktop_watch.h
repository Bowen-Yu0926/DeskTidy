#pragma once

#include <Windows.h>

#include <cstdint>
#include <string>

// Cheap poll of user/public desktop folder mtimes.
class DesktopWatch {
 public:
  void Reset();
  // Returns true when desktop contents likely changed.
  bool PollChanged();

 private:
  FILETIME StampFolder(const std::wstring& folder) const;
  FILETIME user_{};
  FILETIME public_{};
  bool primed_ = false;
};
