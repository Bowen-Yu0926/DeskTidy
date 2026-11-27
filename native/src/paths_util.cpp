#include "paths_util.h"

#include "log.h"

#include <Windows.h>
#include <ShlObj.h>
#include <Shellapi.h>
#include <aclapi.h>   // SetNamedSecurityInfoW

#include <atomic>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

std::wstring ComputeExeDir() {
  wchar_t mod[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, mod, MAX_PATH);
  std::wstring p(mod, n > 0 ? n : 0);
  const size_t slash = p.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    p.resize(slash);
  }
  return p;
}

const std::wstring& ExeDir() {
  static const std::wstring s_exe_dir = ComputeExeDir();
  return s_exe_dir;
}

bool PortableFileExists() {
  return GetFileAttributesW((ExeDir() + L"\\portable.txt").c_str()) !=
         INVALID_FILE_ATTRIBUTES;
}

bool PortableFlagOnCommandLine() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return false;
  }
  bool found = false;
  for (int i = 0; i < argc; ++i) {
    if (argv[i] && wcscmp(argv[i], L"--portable") == 0) {
      found = true;
      break;
    }
  }
  LocalFree(argv);
  return found;
}

bool FileExistsSimple(const std::wstring& path) {
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES &&
         !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool DetectDevRuntime() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv) {
    int cli_override = -1;
    for (int i = 1; i < argc; ++i) {
      if (!argv[i]) {
        continue;
      }
      if (wcscmp(argv[i], L"--release-mode") == 0) {
        cli_override = 0;
      } else if (wcscmp(argv[i], L"--dev") == 0) {
        cli_override = 1;
      }
    }
    LocalFree(argv);
    if (cli_override >= 0) {
      return cli_override == 1;
    }
  }
  const std::wstring& exe_dir = ExeDir();
  if (exe_dir.empty()) {
    return false;
  }
  // CMake POST_BUILD writes desktidy_dev.marker into the build output dir;
  // Inno Setup never packages it (and deletes any stale copy on install).
  if (FileExistsSimple(exe_dir + L"\\desktidy_dev.marker")) {
    return true;
  }
  // Multi-config (build\Release\*.exe) and single-config (build\*.exe) trees.
  if (FileExistsSimple(exe_dir + L"\\CMakeCache.txt") ||
      FileExistsSimple(exe_dir + L"\\..\\CMakeCache.txt") ||
      FileExistsSimple(exe_dir + L"\\..\\..\\CMakeLists.txt")) {
    return true;
  }
  return false;
}

std::wstring ComputeNativeDataDir() {
  std::wstring dir;
  if (IsPortableMode()) {
    dir = ExeDir() + L"\\data";
  } else {
    wchar_t* appdata = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                    &appdata)) ||
        !appdata) {
      dir = L".";
    } else {
      dir = appdata;
      CoTaskMemFree(appdata);
      dir += L"\\DeskTidyNative";
    }
  }
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

// One-shot log of the resolved data root. Deferred to the second NativeDataDir()
// call: the first call happens inside LogInit() before the logger is ready, so
// LogLine would be a no-op.
std::atomic<int> g_root_call_state{0};

}  // namespace

std::wstring NativeExeDir() { return ExeDir(); }

bool IsPortableMode() {
  static const bool s_portable =
      PortableFlagOnCommandLine() || PortableFileExists();
  return s_portable;
}

bool IsDevRuntime() {
  static const bool s_dev_runtime = DetectDevRuntime();
  return s_dev_runtime;
}

std::wstring NativeDataDir() {
  static const std::wstring s_dir = ComputeNativeDataDir();

  int expected = 0;
  if (!g_root_call_state.compare_exchange_strong(expected, 1)) {
    if (expected == 1 &&
        g_root_call_state.compare_exchange_strong(expected, 2)) {
      LogLine(L"[paths] data root = %s (portable=%s channel=%s)", s_dir.c_str(),
              IsPortableMode() ? L"yes" : L"no",
              IsDevRuntime() ? L"dev" : L"release");
    }
  }
  return s_dir;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) {
    return b;
  }
  if (a.back() == L'\\' || a.back() == L'/') {
    return a + b;
  }
  return a + L"\\" + b;
}

std::string NarrowUtf8(const std::wstring& ws) {
  if (ws.empty()) {
    return {};
  }
  // Without WC_ERR_INVALID_CHARS, lone surrogates and other unencodable code
  // units become U+FFFD instead of failing the conversion. With strict mode
  // the call returns 0 and out comes back EMPTY — saving a note that happened
  // to contain one stray surrogate would truncate the file to nothing.
  const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
  if (n <= 0) {
    return std::string();  // caller keeps the previous file contents
  }
  std::string out(static_cast<size_t>(n) - 1, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), n, nullptr,
                        nullptr);
  }
  return out;
}

std::wstring WidenUtf8(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  // MB_ERR_INVALID_CHARS is deliberately NOT set: without it Windows replaces
  // malformed sequences with U+FFFD. With strict validation the call returns 0
  // and the whole string would vanish — a single corrupt byte in a JSON field
  // (truncated write, mangled sync payload) silently erased the entire value.
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) {
    // Never return an empty result for a non-empty input on conversion failure:
    // keep the bytes readable as Latin-1 so the user still sees something.
    std::wstring fallback;
    fallback.reserve(s.size());
    for (unsigned char c : s) {
      fallback.push_back(static_cast<wchar_t>(c));
    }
    return fallback;
  }
  std::wstring out(static_cast<size_t>(n) - 1, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  }
  return out;
}

namespace {

bool DecodeCodePage(UINT cp, DWORD flags, const char* data, int len,
                    std::wstring* out) {
  if (!out || len < 0) {
    return false;
  }
  if (len == 0) {
    out->clear();
    return true;
  }
  const int n = MultiByteToWideChar(cp, flags, data, len, nullptr, 0);
  if (n <= 0) {
    return false;
  }
  out->assign(static_cast<size_t>(n), L'\0');
  return MultiByteToWideChar(cp, flags, data, len, out->data(), n) > 0;
}

bool EncodeCodePage(UINT cp, const std::wstring& text, std::string* out) {
  if (!out) {
    return false;
  }
  if (text.empty()) {
    out->clear();
    return true;
  }
  const int n = WideCharToMultiByte(cp, 0, text.c_str(),
                                    static_cast<int>(text.size()), nullptr, 0,
                                    nullptr, nullptr);
  if (n <= 0) {
    return false;
  }
  out->assign(static_cast<size_t>(n), '\0');
  return WideCharToMultiByte(cp, 0, text.c_str(), static_cast<int>(text.size()),
                             out->data(), n, nullptr, nullptr) > 0;
}

}  // namespace

bool DecodeTextFileBytes(const std::string& raw, std::wstring* out,
                         TextFileEncoding* detected) {
  if (!out) {
    return false;
  }
  TextFileEncoding enc = TextFileEncoding::Utf8;
  if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
      static_cast<unsigned char>(raw[1]) == 0xBB &&
      static_cast<unsigned char>(raw[2]) == 0xBF) {
    if (DecodeCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data() + 3,
                       static_cast<int>(raw.size() - 3), out)) {
      enc = TextFileEncoding::Utf8Bom;
      if (detected) {
        *detected = enc;
      }
      return true;
    }
  }
  if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFF &&
      static_cast<unsigned char>(raw[1]) == 0xFE) {
    const size_t n = (raw.size() - 2) / 2;
    out->resize(n);
    std::memcpy(out->data(), raw.data() + 2, n * sizeof(wchar_t));
    enc = TextFileEncoding::Utf16Le;
    if (detected) {
      *detected = enc;
    }
    return true;
  }
  if (raw.size() >= 2 && static_cast<unsigned char>(raw[0]) == 0xFE &&
      static_cast<unsigned char>(raw[1]) == 0xFF) {
    const size_t n = (raw.size() - 2) / 2;
    out->resize(n);
    for (size_t i = 0; i < n; ++i) {
      const unsigned char hi =
          static_cast<unsigned char>(raw[2 + i * 2]);
      const unsigned char lo =
          static_cast<unsigned char>(raw[2 + i * 2 + 1]);
      (*out)[i] = static_cast<wchar_t>((hi << 8) | lo);
    }
    enc = TextFileEncoding::Utf16Be;
    if (detected) {
      *detected = enc;
    }
    return true;
  }
  if (DecodeCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(),
                     static_cast<int>(raw.size()), out)) {
    enc = TextFileEncoding::Utf8;
    if (detected) {
      *detected = enc;
    }
    return true;
  }
  if (DecodeCodePage(CP_ACP, 0, raw.data(), static_cast<int>(raw.size()),
                     out)) {
    enc = TextFileEncoding::Acp;
    if (detected) {
      *detected = enc;
    }
    return true;
  }
  return false;
}

bool EncodeTextFileBytes(const std::wstring& text, TextFileEncoding enc,
                         std::string* out) {
  if (!out) {
    return false;
  }
  switch (enc) {
    case TextFileEncoding::Utf8Bom: {
      std::string body;
      if (!EncodeCodePage(CP_UTF8, text, &body)) {
        return false;
      }
      out->assign("\xEF\xBB\xBF");
      out->append(body);
      return true;
    }
    case TextFileEncoding::Utf8:
      return EncodeCodePage(CP_UTF8, text, out);
    case TextFileEncoding::Acp:
      return EncodeCodePage(CP_ACP, text, out);
    case TextFileEncoding::Utf16Le: {
      out->assign("\xFF\xFE", 2);
      out->append(reinterpret_cast<const char*>(text.data()),
                  text.size() * sizeof(wchar_t));
      return true;
    }
    case TextFileEncoding::Utf16Be: {
      out->assign("\xFE\xFF", 2);
      for (wchar_t ch : text) {
        out->push_back(static_cast<char>((ch >> 8) & 0xFF));
        out->push_back(static_cast<char>(ch & 0xFF));
      }
      return true;
    }
  }
  return false;
}

// Per-write scratch name. A fixed "<path>.tmp" is shared state: two writers of
// the same file (UI thread saving while a worker saves, or two processes) both
// CREATE_ALWAYS the same handle target, so one truncates the other's bytes
// mid-write and the loser's MoveFileEx publishes a torn file. PID + a process
// wide counter keeps every in-flight write on its own file.
std::wstring AtomicTempPathFor(const std::wstring& path) {
  static std::atomic<unsigned long> s_seq{0};
  wchar_t suffix[48];
  swprintf_s(suffix, L".%lu.%lu.tmp", GetCurrentProcessId(),
             s_seq.fetch_add(1, std::memory_order_relaxed));
  return path + suffix;
}

bool AtomicWriteUtf8File(const std::wstring& path, const std::string& utf8,
                         bool keep_bak) {
  if (path.empty()) {
    return false;
  }
  const std::wstring tmp = AtomicTempPathFor(path);
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok =
      WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
  if (!ok || written != utf8.size()) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  if (keep_bak) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
        !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        (fad.nFileSizeHigh != 0 || fad.nFileSizeLow != 0)) {
      CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
    }
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

std::string EscapeJsonUtf8(const std::string& s) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(kHex[(c >> 4) & 0x0F]);
          out.push_back(kHex[c & 0x0F]);
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

std::string UnescapeJsonUtf8(const std::string& body) {
  std::string out;
  out.reserve(body.size());
  size_t i = 0;
  while (i < body.size()) {
    const char c = body[i];
    if (c != '\\' || i + 1 >= body.size()) {
      out.push_back(c);
      ++i;
      continue;
    }
    const char esc = body[i + 1];
    i += 2;
    switch (esc) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '/':
        out.push_back('/');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        // \uXXXX, possibly the high half of a surrogate pair.
        auto hex4 = [&](size_t at, unsigned* v) {
          if (at + 4 > body.size()) {
            return false;
          }
          unsigned code = 0;
          for (size_t k = 0; k < 4; ++k) {
            const char h = body[at + k];
            code <<= 4;
            if (h >= '0' && h <= '9') {
              code |= static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              code |= static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              code |= static_cast<unsigned>(h - 'A' + 10);
            } else {
              return false;
            }
          }
          *v = code;
          return true;
        };
        unsigned code = 0;
        if (!hex4(i, &code)) {
          out.push_back('u');  // malformed: keep it literal, never drop text
          break;
        }
        i += 4;
        if (code >= 0xD800 && code <= 0xDBFF && i + 6 <= body.size() &&
            body[i] == '\\' && body[i + 1] == 'u') {
          unsigned low = 0;
          if (hex4(i + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
            i += 6;
          }
        }
        // wchar_t is UTF-16 on Windows, so a code point above the BMP must be
        // written as a surrogate PAIR — casting it to a single wchar_t
        // truncates it (emoji came out as garbage).
        std::wstring wide;
        if (code <= 0xFFFF) {
          wide.push_back(static_cast<wchar_t>(code));
        } else {
          const unsigned v = code - 0x10000;
          wide.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
          wide.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
        }
        out += NarrowUtf8(wide);
        break;
      }
      default:
        out.push_back(esc);
        break;
    }
  }
  return out;
}

bool FileMissingOrTiny(const std::wstring& path, std::uint32_t min_bytes) {
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) ||
      (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return true;
  }
  if (fad.nFileSizeHigh != 0) {
    return false;
  }
  return fad.nFileSizeLow < min_bytes;
}

bool ReadUtf8FileWithBak(const std::wstring& path, std::string* out) {
  if (!out) {
    return false;
  }
  out->clear();
  for (const std::wstring& candidate : {path, path + L".bak"}) {
    HANDLE h = CreateFileW(candidate.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 8 * 1024 * 1024) {
      CloseHandle(h);
      continue;
    }
    std::string buf(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok =
        ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || read == 0) {
      continue;
    }
    buf.resize(read);
    *out = std::move(buf);
    if (candidate != path) {
      LogLine(L"[paths] restored %s from .bak", path.c_str());
    }
    return true;
  }
  return false;
}

namespace {

bool DirExistsW(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring MirrorDesktidyTree(const std::wstring& exe_dir) {
  // E:\Desktidy\native\build\Release ↔ E:\Desktidy-cpp\native\build\Release
  const std::wstring a = L"\\Desktidy\\";
  const std::wstring b = L"\\Desktidy-cpp\\";
  size_t p = exe_dir.find(a);
  if (p != std::wstring::npos) {
    std::wstring out = exe_dir;
    out.replace(p, a.size(), b);
    return out;
  }
  p = exe_dir.find(b);
  if (p != std::wstring::npos) {
    std::wstring out = exe_dir;
    out.replace(p, b.size(), a);
    return out;
  }
  // Case-insensitive fallback for mixed casing on Windows paths.
  std::wstring lower = exe_dir;
  for (auto& ch : lower) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  const std::wstring la = L"\\desktidy\\";
  const std::wstring lb = L"\\desktidy-cpp\\";
  p = lower.find(la);
  if (p != std::wstring::npos && lower.find(lb) == std::wstring::npos) {
    std::wstring out = exe_dir;
    out.replace(p, la.size(), L"\\Desktidy-cpp\\");
    return out;
  }
  p = lower.find(lb);
  if (p != std::wstring::npos) {
    std::wstring out = exe_dir;
    out.replace(p, lb.size(), L"\\Desktidy\\");
    return out;
  }
  return {};
}

bool CopyIfDestSparse(const std::wstring& src_file, const std::wstring& dst_file,
                      std::uint32_t min_bytes) {
  if (FileMissingOrTiny(src_file, min_bytes)) {
    return false;
  }
  if (!FileMissingOrTiny(dst_file, min_bytes)) {
    return false;
  }
  if (!CopyFileW(src_file.c_str(), dst_file.c_str(), FALSE)) {
    return false;
  }
  LogLine(L"[paths] adopted orphan %s -> %s", src_file.c_str(),
          dst_file.c_str());
  return true;
}

}  // namespace

bool AdoptOrphanPortableSidecarOnce() {
  if (IsPortableMode()) {
    return false;
  }
  const std::wstring dest_root = NativeDataDir();
  const std::wstring marker =
      JoinPath(dest_root, L"adopted_portable_sidecar.done");
  if (!FileMissingOrTiny(marker, 1)) {
    return false;
  }

  std::vector<std::wstring> sources;
  const std::wstring exe_data = JoinPath(NativeExeDir(), L"data");
  if (DirExistsW(exe_data)) {
    sources.push_back(exe_data);
  }
  const std::wstring mirror = MirrorDesktidyTree(NativeExeDir());
  if (!mirror.empty()) {
    const std::wstring mirror_data = JoinPath(mirror, L"data");
    if (DirExistsW(mirror_data) && mirror_data != exe_data &&
        mirror_data != dest_root) {
      sources.push_back(mirror_data);
    }
  }

  // Sparse-only adopt: never clobber a non-empty LocalAppData file.
  static const wchar_t* kSparseFiles[] = {
      L"page_folders.json",
      L"whitelist.json",
      L"dock_items.json",
  };
  bool any = false;
  for (const auto& src_root : sources) {
    for (const wchar_t* name : kSparseFiles) {
      if (CopyIfDestSparse(JoinPath(src_root, name), JoinPath(dest_root, name),
                           24)) {
        any = true;
      }
    }
  }

  HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    const char ok[] = "1\n";
    DWORD w = 0;
    WriteFile(h, ok, 2, &w, nullptr);
    CloseHandle(h);
  }
  return any;
}

// --- file ACL hardening -----------------------------------------------------
// The default ACL on %LOCALAPPDATA%\DeskTidy\*.json inherits whatever the
// parent grants, which on shared machines can include other local users. The
// vault session file holds a bearer token plus the KDF salt and key_check —
// enough to grind the master password offline — so it gets an explicit,
// owner-only DACL.
//
// Built with SetEntriesInAclW (advapi32) rather than an SDDL string: the
// SDDL helper lives in sddl.lib, which is not present on every build image and
// broke CI with LNK1181. This path needs nothing beyond advapi32.
bool RestrictFileToCurrentUser(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  // Current user's SID.
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  std::vector<BYTE> sid_buf;
  DWORD need = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &need);
  if (need == 0) {
    CloseHandle(token);
    return false;
  }
  sid_buf.resize(need);
  const BOOL got = GetTokenInformation(token, TokenUser, sid_buf.data(), need,
                                       &need);
  CloseHandle(token);
  if (!got) {
    return false;
  }
  auto* user = reinterpret_cast<TOKEN_USER*>(sid_buf.data());

  // EXPLICIT_ACCESS: full control for the current user, nothing else. Setting
  // it with SET_ACCESS (not GRANT) plus PROTECTED_DACL below drops the
  // inherited entries, so "everyone else" ends up with no access at all.
  EXPLICIT_ACCESSW ea{};
  ea.grfAccessPermissions = GENERIC_ALL;
  ea.grfAccessMode = SET_ACCESS;
  ea.grfInheritance = NO_INHERITANCE;
  ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
  ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);

  PACL acl = nullptr;
  if (SetEntriesInAclW(1, &ea, nullptr, &acl) != ERROR_SUCCESS || !acl) {
    return false;
  }
  LPWSTR p = const_cast<LPWSTR>(path.c_str());
  // PROTECTED_DACL_SECURITY_INFORMATION stops inheritance from the parent.
  const DWORD rc = SetNamedSecurityInfoW(
      p, SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
      nullptr, acl, nullptr);
  LocalFree(acl);
  return rc == ERROR_SUCCESS;
}
