#include "organize.h"

#include <Windows.h>
#include <ShlObj.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace {

std::wstring DesktopDir() {
  PWSTR p = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) || !p) {
    return {};
  }
  std::wstring out = p;
  CoTaskMemFree(p);
  return out;
}

bool IsUnderDir(const std::wstring& path, const std::wstring& dir) {
  if (path.size() < dir.size()) {
    return false;
  }
  return _wcsnicmp(path.c_str(), dir.c_str(), dir.size()) == 0;
}

}  // namespace

int OrganizeApplyDefaultPins(PinMap* pins, const std::vector<DesktopItem>& items,
                             const std::string& software_id,
                             const std::string& common_id) {
  if (!pins) {
    return 0;
  }
  int n = 0;
  for (const auto& it : items) {
    if (FindPin(*pins, it.path)) {
      continue;
    }
    SetPin(pins, it.path, it.is_link ? software_id : common_id);
    ++n;
  }
  return n;
}

int OrganizeMoveIntoFolders(const std::vector<DesktopItem>& items) {
  const std::wstring desktop = DesktopDir();
  if (desktop.empty()) {
    return 0;
  }
  const fs::path root = fs::path(desktop) / L"DeskTidy";
  const fs::path links = root / L"软件";
  const fs::path files = root / L"常用";
  std::error_code ec;
  fs::create_directories(links, ec);
  fs::create_directories(files, ec);
  int moved = 0;
  for (const auto& it : items) {
    if (!IsUnderDir(it.path, desktop)) {
      continue;
    }
    // Skip items already inside DeskTidy folder.
    if (IsUnderDir(it.path, root.wstring())) {
      continue;
    }
    const fs::path src(it.path);
    const fs::path dest_dir = it.is_link ? links : files;
    fs::path dest = dest_dir / src.filename();
    if (fs::exists(dest, ec)) {
      dest = dest_dir /
             (src.stem().wstring() + L"_" + std::to_wstring(GetTickCount()) +
              src.extension().wstring());
    }
    ec.clear();
    fs::rename(src, dest, ec);
    if (!ec) {
      ++moved;
    }
  }
  return moved;
}
