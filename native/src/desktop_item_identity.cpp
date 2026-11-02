#include "desktop_items.h"

namespace {

std::wstring IdentityPathKey(const std::wstring& path) {
  std::wstring key = path;
  for (auto& ch : key) {
    if (ch == L'/') {
      ch = L'\\';
    } else if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  return key;
}

}  // namespace

bool DesktopItemIdentityEqual(const DesktopItem& a, const DesktopItem& b) {
  return a.is_link == b.is_link && a.name == b.name &&
         IdentityPathKey(a.path) == IdentityPathKey(b.path);
}

bool DesktopItemListsIdentityEqual(const std::vector<DesktopItem>& a,
                                   const std::vector<DesktopItem>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (!DesktopItemIdentityEqual(a[i], b[i])) {
      return false;
    }
  }
  return true;
}
