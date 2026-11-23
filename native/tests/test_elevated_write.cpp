#include "elevated_save.h"
#include "paths_util.h"

#include <Windows.h>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

#define CHECK(expr)                                                          \
  do {                                                                       \
    if (!(expr)) {                                                           \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);   \
      return 1;                                                              \
    }                                                                        \
  } while (0)

std::wstring DeskNoteExePath() {
  return L"";
}

int main() {
  printf("Running elevated write tests...\n");

  wchar_t tmp_dir[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp_dir);
  std::wstring test_file = std::wstring(tmp_dir) + L"desktidy_elev_test_" +
                           std::to_wstring(GetCurrentProcessId()) + L".txt";

  DeleteFileW(test_file.c_str());

  // 1. 普通文件创建与写入
  const std::string text1 = "Hello, world! This is a longer string to test truncation.";
  bool ok = WriteFileBytes(test_file, text1.data(), text1.size());
  CHECK(ok && "WriteFileBytes failed on new file");

  // 2. 较短内容覆盖写入（验证 SetEndOfFile 是否正常截断）
  const std::string text2 = "Short";
  ok = WriteFileBytes(test_file, text2.data(), text2.size());
  CHECK(ok && "WriteFileBytes failed on overwrite");

  // 读取验证大小是否精确为 5 字节
  HANDLE h = CreateFileW(test_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(h != INVALID_HANDLE_VALUE);
  DWORD sz = GetFileSize(h, nullptr);
  char buf[32] = {};
  DWORD read = 0;
  ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
  CloseHandle(h);
  CHECK(sz == 5 && "File was not truncated properly");
  CHECK(std::string(buf, read) == "Short");
  printf("OK: normal overwrite and truncation\n");

  // 3. 只读文件原地覆写
  SetFileAttributesW(test_file.c_str(), FILE_ATTRIBUTE_READONLY);
  DWORD attrs = GetFileAttributesW(test_file.c_str());
  CHECK((attrs & FILE_ATTRIBUTE_READONLY) != 0);

  const std::string text3 = "Readonly in-place overwrite test successful!";
  ok = WriteFileBytes(test_file, text3.data(), text3.size());
  CHECK(ok && "WriteFileBytes failed on readonly file");

  attrs = GetFileAttributesW(test_file.c_str());
  CHECK((attrs & FILE_ATTRIBUTE_READONLY) != 0 && "Readonly attribute should be preserved");

  h = CreateFileW(test_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  CHECK(h != INVALID_HANDLE_VALUE);
  sz = GetFileSize(h, nullptr);
  CloseHandle(h);
  CHECK(sz == text3.size());
  printf("OK: readonly file in-place overwrite and attribute preservation\n");

  // 清理临时文件
  SetFileAttributesW(test_file.c_str(), FILE_ATTRIBUTE_NORMAL);
  DeleteFileW(test_file.c_str());

  // 4a. ConsumeElevatedPending 端到端：临时非系统文本文件走完整的
  //     WriteElevatedPending → ConsumeElevatedPending 链路，验证内容无损。
  const std::wstring payload =
      L"# 主机映射\r\n127.0.0.1 \"local\" \\ path\r\n192.168.1.1 test\r\n";
  const std::wstring pending_target = L"C:\\Windows\\System32\\drivers\\etc\\hosts";
  const std::wstring verify_file = test_file + L".pending_verify";
  {
    std::ofstream out(verify_file, std::ios::binary);
    CHECK(out.good());
    out << "# seed\n";
  }
  CHECK(WriteElevatedPending(verify_file, payload));
  CHECK(ConsumeElevatedPending(verify_file));
  {
    std::ifstream in(verify_file, std::ios::binary);
    CHECK(in.good());
    const std::string written((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    std::wstring decoded;
    CHECK(DecodeTextFileBytes(written, &decoded, nullptr));
    CHECK(decoded == payload);
    CHECK(written.find("\r\n") != std::string::npos);
    CHECK(written.find("\r\r") == std::string::npos);
  }
  DeleteFileW(verify_file.c_str());
  printf("OK: elevated pending consume round-trip\n");

  // 4b. JSON fixture details for the hosts payload.
  CHECK(WriteElevatedPending(pending_target, payload));
  {
    std::ifstream in(ElevatedPendingPath(), std::ios::binary);
    CHECK(in.good());
    const std::string json((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    CHECK(json.find("\\r\\n") != std::string::npos);
    CHECK(json.find("\\\"") != std::string::npos);
  }
  ClearElevatedPending();
  printf("OK: elevated pending JSON round-trip fixture\n");

  // 5. 受保护路径访问测试（当前进程非管理员运行下写入 hosts 应报 ACCESS_DENIED）
  if (!IsCurrentProcessElevated()) {
    wchar_t windir[MAX_PATH] = {};
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::wstring hosts_path = std::wstring(windir) + L"\\System32\\drivers\\etc\\hosts";
    const std::string dummy = "# test\n";
    ok = WriteFileBytes(hosts_path, dummy.data(), dummy.size());
    CHECK(!ok && "Writing to hosts without elevation must fail");
    CHECK(LastWriteWasAccessDenied() && "LastWriteWasAccessDenied must be true");
    printf("OK: non-elevated hosts write safely denied\n");
  } else {
    printf("INFO: running elevated, skipped access denied assertion\n");
  }

  printf("all elevated save tests passed!\n");
  return 0;
}
