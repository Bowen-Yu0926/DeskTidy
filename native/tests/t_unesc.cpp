// Verifies UnescapeJsonUtf8 handles the escapes Python's json.dump emits
// (ensure_ascii=True → every non-ASCII character arrives as \uXXXX).
#include "paths_util.h"

#include <cstdio>
#include <string>

int main() {
  struct Case {
    std::string in;
    std::string want;
  };
  const Case cases[] = {
      {R"(\u5DE5\u4F5C)", "\xE5\xB7\xA5\xE4\xBD\x9C"},        // 工作
      {R"(a\"b)", "a\"b"},
      {R"(c\\d)", "c\\d"},
      {R"(line\nnext)", "line\nnext"},
      {R"(C:\\Users\\x)", "C:\\Users\\x"},
      {R"(\uD83D\uDE00)", "\xF0\x9F\x98\x80"},                // emoji pair
      {R"(\u4E0B\u8F7D)", "\xE4\xB8\x8B\xE8\xBD\xBD"},        // 下载
      {"plain", "plain"},
  };
  int bad = 0;
  for (const auto& c : cases) {
    const std::string got = UnescapeJsonUtf8(c.in);
    if (got != c.want) {
      std::printf("FAIL in=%s got=%s want=%s\n", c.in.c_str(), got.c_str(),
                  c.want.c_str());
      ++bad;
    } else {
      std::printf("OK %s\n", c.in.c_str());
    }
  }
  if (bad) {
    std::printf("FAILED %d\n", bad);
    return 1;
  }
  std::printf("all json unescape tests passed\n");
  return 0;
}
