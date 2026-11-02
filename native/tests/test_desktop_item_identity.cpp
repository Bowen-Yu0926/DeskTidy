// Identity compare for fence/public SetItems short-circuit.
#include "desktop_items.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_fails = 0;

void Expect(bool ok, const char* msg) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    ++g_fails;
  } else {
    std::printf("OK: %s\n", msg);
  }
}

DesktopItem Make(const wchar_t* path, const wchar_t* name, bool link = true) {
  DesktopItem it;
  it.path = path;
  it.name = name;
  it.is_link = link;
  return it;
}

}  // namespace

int main() {
  Expect(DesktopItemIdentityEqual(Make(L"D:\\desktop\\A.lnk", L"A"),
                                  Make(L"d:/desktop/a.lnk", L"A")),
         "path case/slash normalize");
  Expect(!DesktopItemIdentityEqual(Make(L"D:\\desktop\\A.lnk", L"A"),
                                   Make(L"D:\\desktop\\A.lnk", L"B")),
         "name mismatch");
  Expect(!DesktopItemIdentityEqual(Make(L"D:\\desktop\\A.lnk", L"A", true),
                                   Make(L"D:\\desktop\\A.lnk", L"A", false)),
         "is_link mismatch");

  std::vector<DesktopItem> a{Make(L"D:\\a.lnk", L"a"), Make(L"D:\\b.lnk", L"b")};
  std::vector<DesktopItem> b{Make(L"d:\\a.lnk", L"a"), Make(L"d:\\b.lnk", L"b")};
  std::vector<DesktopItem> c{Make(L"D:\\b.lnk", L"b"), Make(L"D:\\a.lnk", L"a")};
  Expect(DesktopItemListsIdentityEqual(a, b), "list equal ignoring case");
  Expect(!DesktopItemListsIdentityEqual(a, c), "list order matters");
  Expect(!DesktopItemListsIdentityEqual(a, {}), "size mismatch");

  if (g_fails) {
    std::fprintf(stderr, "%d failure(s)\n", g_fails);
    return 1;
  }
  std::printf("all passed\n");
  return 0;
}
