#include "organize.h"

#include "whitelist.h"

#include <Windows.h>
#include <ShlObj.h>

#include <filesystem>
#include <string>
#include <vector>

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
  const auto wl = LoadWhitelist();
  int n = 0;
  for (const auto& it : items) {
    if (FindPin(*pins, it.path)) {
      continue;
    }
    if (IsWhitelisted(it.path, wl) || IsWhitelisted(it.name, wl)) {
      continue;
    }
    SetPin(pins, it.path, it.is_link ? software_id : common_id);
    ++n;
  }
  return n;
}

int OrganizeCountPhysicalMoves(const std::vector<DesktopItem>& items) {
  const std::wstring desktop = DesktopDir();
  if (desktop.empty()) {
    return 0;
  }
  const fs::path root = fs::path(desktop) / L"DeskTidy";
  const auto wl = LoadWhitelist();
  int n = 0;
  for (const auto& it : items) {
    if (!IsUnderDir(it.path, desktop)) {
      continue;
    }
    if (IsUnderDir(it.path, root.wstring())) {
      continue;
    }
    if (IsWhitelisted(it.path, wl) || IsWhitelisted(it.name, wl)) {
      continue;
    }
    ++n;
  }
  return n;
}

std::vector<std::wstring> OrganizePreviewPhysicalMoves(
    const std::vector<DesktopItem>& items) {
  std::vector<std::wstring> names;
  const std::wstring desktop = DesktopDir();
  if (desktop.empty()) {
    return names;
  }
  const fs::path root = fs::path(desktop) / L"DeskTidy";
  const auto wl = LoadWhitelist();
  for (const auto& it : items) {
    if (!IsUnderDir(it.path, desktop)) {
      continue;
    }
    if (IsUnderDir(it.path, root.wstring())) {
      continue;
    }
    if (IsWhitelisted(it.path, wl) || IsWhitelisted(it.name, wl)) {
      continue;
    }
    names.push_back(it.name.empty() ? it.path : it.name);
    if (names.size() >= 40) {
      break;
    }
  }
  return names;
}

bool OrganizeConfirmPhysical(HWND owner, const std::vector<DesktopItem>& items) {
  const auto names = OrganizePreviewPhysicalMoves(items);
  const int will = OrganizeCountPhysicalMoves(items);
  std::wstring msg = L"将把约 " + std::to_wstring(will) +
                     L" 个桌面项目移入 Desktop\\DeskTidy（白名单除外）。\n\n";
  if (names.empty()) {
    msg += L"（没有可移动项）\n";
  } else {
    msg += L"预览：\n";
    for (size_t i = 0; i < names.size(); ++i) {
      msg += L" · " + names[i] + L"\n";
    }
    if (will > static_cast<int>(names.size())) {
      msg += L" · …\n";
    }
  }
  msg += L"\n继续？";
  return MessageBoxW(owner, msg.c_str(), L"物理整理",
                     MB_YESNO | MB_ICONWARNING) == IDYES;
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
  const auto wl = LoadWhitelist();
  int moved = 0;
  for (const auto& it : items) {
    if (!IsUnderDir(it.path, desktop)) {
      continue;
    }
    if (IsUnderDir(it.path, root.wstring())) {
      continue;
    }
    if (IsWhitelisted(it.path, wl) || IsWhitelisted(it.name, wl)) {
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
