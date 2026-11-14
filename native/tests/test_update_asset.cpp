// Update-check JSON parsing: the hand-rolled scanner must survive the shapes
// GitHub actually returns. Regression guard for the nested "uploader" object
// inside each release asset, which made a naive brace scan find no asset.
#include <cstdio>
#include <string>

#include "update_json.h"

namespace {

int g_failures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

// Trimmed from a real api.github.com /releases/latest response: assets[0]
// carries a nested uploader object BEFORE browser_download_url.
const char* kRealShape =
    "{\n"
    "  \"url\": \"https://api.github.com/repos/o/r/releases/1\",\n"
    "  \"tag_name\": \"v4.0.45\",\n"
    "  \"assets\": [\n"
    "    {\n"
    "      \"url\": \"https://api.github.com/repos/o/r/releases/assets/1\",\n"
    "      \"id\": 1,\n"
    "      \"name\": \"DeskTidy_Setup_4.0.45.exe\",\n"
    "      \"label\": \"\",\n"
    "      \"uploader\": {\n"
    "        \"login\": \"someone\",\n"
    "        \"id\": 2,\n"
    "        \"avatar_url\": \"https://avatars.githubusercontent.com/u/2?v=4\"\n"
    "      },\n"
    "      \"size\": 30652833,\n"
    "      \"browser_download_url\": \"https://github.com/o/r/releases/download/v4.0.45/DeskTidy_Setup_4.0.45.exe\"\n"
    "    },\n"
    "    {\n"
    "      \"name\": \"DeskTidy_Setup_4.0.45.exe.sha256\",\n"
    "      \"size\": 93,\n"
    "      \"browser_download_url\": \"https://github.com/o/r/releases/download/v4.0.45/DeskTidy_Setup_4.0.45.exe.sha256\"\n"
    "    }\n"
    "  ],\n"
    "  \"body\": \"notes\"\n"
    "}\n";

void TestRealShape() {
  std::string name;
  std::wstring url;
  long long size = 0;
  const bool found = FindExeAsset(kRealShape, &name, &url, &size);
  Check(found, "nested uploader object: installer asset is found");
  Check(name == "DeskTidy_Setup_4.0.45.exe",
        "nested uploader object: picks the Setup exe");
  Check(url ==
            L"https://github.com/o/r/releases/download/v4.0.45/"
            L"DeskTidy_Setup_4.0.45.exe",
        "nested uploader object: download URL is the asset's own");
  Check(size == 30652833, "nested uploader object: size parsed");
}

// A brace inside a string value must not be treated as structure.
void TestBracesInStrings() {
  const char* json =
      "{\"assets\": [{\"name\": \"DeskTidy_Setup_1.2.3.exe\","
      "\"label\": \"}\","
      "\"size\": 10,"
      "\"browser_download_url\": \"https://github.com/o/r/x.exe\"}]}";
  std::string name;
  std::wstring url;
  long long size = 0;
  Check(FindExeAsset(json, &name, &url, &size),
        "brace inside a string: asset still found");
  Check(name == "DeskTidy_Setup_1.2.3.exe",
        "brace inside a string: correct asset name");
}

// Largest matching exe wins; unrelated exes are ignored.
void TestPicksLargest() {
  const char* json =
      "{\"assets\": ["
      "{\"name\": \"DeskTidy_Setup_1.0.0.exe\", \"size\": 100,"
      " \"browser_download_url\": \"https://github.com/o/r/small.exe\"},"
      "{\"name\": \"DeskTidy_Setup_1.0.0.exe\", \"size\": 900,"
      " \"browser_download_url\": \"https://github.com/o/r/big.exe\"},"
      "{\"name\": \"other.exe\", \"size\": 9999,"
      " \"browser_download_url\": \"https://github.com/o/r/other.exe\"}"
      "]}";
  std::string name;
  std::wstring url;
  long long size = 0;
  Check(FindExeAsset(json, &name, &url, &size), "largest: found");
  Check(url == L"https://github.com/o/r/big.exe", "largest: picks biggest setup");
  Check(size == 900, "largest: size matches");
}

// No matching installer → report failure, not a half-filled result.
void TestNoAsset() {
  const char* json =
      "{\"assets\": [{\"name\": \"notes.txt\", \"size\": 3,"
      " \"browser_download_url\": \"https://github.com/o/r/notes.txt\"}]}";
  std::string name = "untouched";
  std::wstring url = L"untouched";
  long long size = 7;
  Check(!FindExeAsset(json, &name, &url, &size), "no asset: returns false");
  Check(name == "untouched" && url == L"untouched" && size == 7,
        "no asset: outputs untouched");
}

}  // namespace

int main() {
  TestRealShape();
  TestBracesInStrings();
  TestPicksLargest();
  TestNoAsset();
  if (g_failures == 0) {
    std::printf("all update asset parse tests passed\n");
    return 0;
  }
  std::printf("%d failure(s)\n", g_failures);
  return 1;
}
