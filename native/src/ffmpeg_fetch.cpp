#include "ffmpeg_fetch.h"

#include "log.h"
#include "paths_util.h"
#include "tool_paths.h"

#include <ShlObj.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

constexpr wchar_t kDefaultUrl[] =
    L"https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"
    L"ffmpeg-master-latest-win64-gpl-shared.zip";

std::wstring GetEnvW(const wchar_t* name) {
  const wchar_t* v = _wgetenv(name);
  return v ? v : L"";
}

bool FileExistsW(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring FfmpegCacheDir() {
  // P2-2: portable builds keep ffmpeg under NativeDataDir (not %USERPROFILE%).
  if (IsPortableMode()) {
    std::wstring dir = JoinPath(NativeDataDir(), L"ffmpeg");
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
  }
  std::wstring base = GetEnvW(L"USERPROFILE");
  if (base.empty()) {
    base = L"C:\\";
  }
  std::wstring dir = JoinPath(base, L".desktidy");
  CreateDirectoryW(dir.c_str(), nullptr);
  dir = JoinPath(dir, L"ffmpeg");
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

// Run ffmpeg hidden, capture stdout+stderr. Returns process exit presence
// (false when CreateProcess fails); output always appended to `out`.
bool RunCapture(const std::wstring& exe, const std::wstring& args,
                std::string* out) {
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    return false;
  }
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdOutput = wr;
  si.hStdError = wr;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(rd);
    CloseHandle(wr);
    return false;
  }
  CloseHandle(wr);
  char chunk[4096];
  DWORD read = 0;
  for (;;) {
    if (!ReadFile(rd, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
      break;
    }
    out->append(chunk, read);
  }
  WaitForSingleObject(pi.hProcess, 15000);
  CloseHandle(rd);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

}  // namespace

bool ValidateRecordingFfmpeg(const std::wstring& exe, std::wstring* error_out) {
  if (!FileExistsW(exe)) {
    if (error_out) {
      *error_out = L"未找到 ffmpeg 可执行文件";
    }
    return false;
  }
  std::string demuxers;
  std::string encoders;
  if (!RunCapture(exe, L"-hide_banner -demuxers", &demuxers) ||
      !RunCapture(exe, L"-hide_banner -encoders", &encoders)) {
    if (error_out) {
      *error_out = L"无法运行 ffmpeg 探测能力";
    }
    return false;
  }
  const bool has_gdigrab = demuxers.find("gdigrab") != std::string::npos;
  const bool has_lavfi = demuxers.find("lavfi") != std::string::npos;
  const bool has_h264mf = encoders.find("h264_mf") != std::string::npos;
  const bool has_libx264 = encoders.find("libx264") != std::string::npos;
  LogLine(L"[ffmpeg] probe %s: gdigrab=%d lavfi=%d h264_mf=%d libx264=%d",
          exe.c_str(), has_gdigrab ? 1 : 0, has_lavfi ? 1 : 0,
          has_h264mf ? 1 : 0, has_libx264 ? 1 : 0);
  std::wstring missing;
  if (!has_gdigrab || !has_lavfi) {
    missing += L"缺少采集输入（gdigrab/lavfi）";
  }
  if (!has_h264mf && !has_libx264) {
    if (!missing.empty()) {
      missing += L"；";
    }
    missing += L"缺少 H.264 编码器（h264_mf/libx264）";
  }
  if (!missing.empty()) {
    if (error_out) {
      *error_out = missing;
    }
    return false;
  }
  return true;
}

std::wstring PreferredRecordingEncoder(const std::wstring& exe) {
  std::string encoders;
  if (!RunCapture(exe, L"-hide_banner -encoders", &encoders)) {
    return L"libx264";
  }
  if (encoders.find("h264_mf") != std::string::npos) {
    return L"h264_mf";
  }
  return L"libx264";
}

namespace {

// --- Minimal progress window (modeless, pumped during download) ------------

constexpr wchar_t kProgClass[] = L"DeskTidyNativeFfProg";
HWND g_prog_hwnd = nullptr;
std::atomic<ULONGLONG> g_prog_done{0};
std::atomic<ULONGLONG> g_prog_total{0};
bool g_prog_reg = false;

LRESULT CALLBACK ProgProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      HBRUSH br = CreateSolidBrush(RGB(245, 247, 250));
      FillRect(hdc, &rc, br);
      DeleteObject(br);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, RGB(20, 20, 30));
      ULONGLONG done = g_prog_done.load();
      ULONGLONG total = g_prog_total.load();
      wchar_t line[256];
      if (total > 0) {
        swprintf_s(line, L"正在下载 FFmpeg …  %.1f MB / %.1f MB",
                   done / 1048576.0, total / 1048576.0);
      } else {
        swprintf_s(line, L"正在下载 FFmpeg …  %.1f MB", done / 1048576.0);
      }
      DrawTextW(hdc, line, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ShowProgress(HINSTANCE inst) {
  if (!g_prog_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ProgProc;
    wc.hInstance = inst;
    wc.lpszClassName = kProgClass;
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);
    g_prog_reg = true;
  }
  g_prog_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kProgClass,
                                L"FFmpeg", WS_POPUP | WS_CAPTION, 0, 0, 360, 64,
                                nullptr, nullptr, inst, nullptr);
  if (g_prog_hwnd) {
    RECT rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    SetWindowPos(g_prog_hwnd, HWND_TOPMOST, rc.right - 380, rc.bottom - 84, 360,
                 64, SWP_NOACTIVATE);
    ShowWindow(g_prog_hwnd, SW_SHOWNOACTIVATE);
  }
}

void CloseProgress() {
  if (g_prog_hwnd) {
    DestroyWindow(g_prog_hwnd);
    g_prog_hwnd = nullptr;
  }
}

void Pump() {
  MSG m;
  while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&m);
    DispatchMessageW(&m);
  }
  if (g_prog_hwnd) {
    InvalidateRect(g_prog_hwnd, nullptr, FALSE);
    UpdateWindow(g_prog_hwnd);
  }
}

bool DownloadToFile(const std::wstring& url, const std::wstring& dest,
                    std::wstring* error) {
  URL_COMPONENTS uc{};
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  uc.dwStructSize = sizeof(uc);
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
    if (error) {
      *error = L"无法解析下载地址";
    }
    return false;
  }
  HINTERNET session = WinHttpOpen(L"DeskTidyNative-FFmpeg",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                  0);
  if (!session) {
    if (error) {
      *error = L"初始化网络失败";
    }
    return false;
  }
  WinHttpSetTimeouts(session, 8000, 8000, 60000, 60000);
  HINTERNET conn = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET req =
      conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
           : nullptr;
  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  if (req) {
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy,
                     sizeof(policy));
  }
  bool ok = false;
  if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
      WinHttpReceiveResponse(req, nullptr)) {
    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(req,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
      if (error) {
        wchar_t buf[64];
        swprintf_s(buf, L"下载失败（HTTP %lu）", status);
        *error = buf;
      }
    } else {
      ULONGLONG total = 0;
      DWORD len = 0;
      if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH,
                              WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len,
                              WINHTTP_NO_HEADER_INDEX) ==
              ERROR_INSUFFICIENT_BUFFER &&
          len >= sizeof(ULONGLONG)) {
        wchar_t* v = static_cast<wchar_t*>(calloc(len, 1));
        if (v && WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH,
                                     WINHTTP_HEADER_NAME_BY_INDEX, v, &len,
                                     WINHTTP_NO_HEADER_INDEX)) {
          total = _wcstoui64(v, nullptr, 10);
        }
        free(v);
      }
      g_prog_total.store(total);
      g_prog_done.store(0);
      FILE* f = nullptr;
      if (_wfopen_s(&f, dest.c_str(), L"wb") == 0 && f) {
        ULONGLONG done = 0;
        for (;;) {
          DWORD avail = 0;
          if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) {
            ok = done > 0;
            break;
          }
          std::vector<BYTE> chunk(avail);
          DWORD read = 0;
          if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) {
            break;
          }
          fwrite(chunk.data(), 1, read, f);
          done += read;
          g_prog_done.store(done);
          Pump();
        }
        fclose(f);
      }
      if (!ok && error && *error == L"") {
        *error = L"下载失败（连接中断）";
      }
    }
  } else if (error) {
    *error = L"下载失败（无法连接）";
  }
  if (req) {
    WinHttpCloseHandle(req);
  }
  if (conn) {
    WinHttpCloseHandle(conn);
  }
  WinHttpCloseHandle(session);
  return ok;
}

bool RunAndWait(const std::wstring& cmd) {
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return false;
  }
  WaitForSingleObject(pi.hProcess, 120000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

bool ExtractZip(const std::wstring& zip, const std::wstring& dest_dir,
                std::wstring* error) {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  std::wstring tar = JoinPath(sys, L"tar.exe");
  std::wstring cmd;
  if (FileExistsW(tar)) {
    cmd = L"\"" + tar + L"\" -xf \"" + zip + L"\" -C \"" + dest_dir + L"\"";
  } else {
    cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "
          L"\"Expand-Archive -LiteralPath '" +
          zip + L"' -DestinationPath '" + dest_dir + L"' -Force\"";
  }
  if (!RunAndWait(cmd)) {
    if (error) {
      *error = L"解压失败（找不到解压工具）";
    }
    return false;
  }
  return true;
}

// Recursively find a file by name under `root`.
bool FindFileRecursive(const std::wstring& root, const wchar_t* name,
                       std::wstring* out) {
  WIN32_FIND_DATAW fd{};
  HANDLE h =
      FindFirstFileW(JoinPath(root, L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return false;
  }
  do {
    if (fd.cFileName[0] == L'.' && fd.cFileName[1] == 0) {
      continue;
    }
    if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.' &&
        fd.cFileName[2] == 0) {
      continue;
    }
    const std::wstring full = JoinPath(root, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (FindFileRecursive(full, name, out)) {
        FindClose(h);
        return true;
      }
    } else if (_wcsicmp(fd.cFileName, name) == 0) {
      *out = full;
      FindClose(h);
      return true;
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return false;
}

std::wstring DirOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L'\\');
  return (slash != std::wstring::npos) ? path.substr(0, slash) : path;
}

bool FetchAndInstall(std::wstring* error) {
  const std::wstring cache = FfmpegCacheDir();
  const std::wstring staging =
      JoinPath(GetEnvW(L"TEMP").empty() ? L"C:\\Windows\\Temp"
                                        : GetEnvW(L"TEMP"),
               L"desktidy-ffmpeg-" + std::to_wstring(GetTickCount()));
  CreateDirectoryW(staging.c_str(), nullptr);
  const std::wstring zip = JoinPath(staging, L"ffmpeg.zip");

  std::wstring dl_err;
  if (!DownloadToFile(kDefaultUrl, zip, &dl_err)) {
    LogLine(L"[ffmpeg] download failed: %s", dl_err.c_str());
    if (error) {
      *error = dl_err.empty() ? L"下载失败" : dl_err;
    }
    return false;
  }
  LogLine(L"[ffmpeg] downloaded from %s", kDefaultUrl);

  const std::wstring extract = JoinPath(staging, L"x");
  CreateDirectoryW(extract.c_str(), nullptr);
  std::wstring ex_err;
  if (!ExtractZip(zip, extract, &ex_err)) {
    if (error) {
      *error = ex_err;
    }
    return false;
  }
  std::wstring found_exe;
  if (!FindFileRecursive(extract, L"ffmpeg.exe", &found_exe)) {
    if (error) {
      *error = L"压缩包中未找到 bin/ffmpeg.exe";
    }
    return false;
  }
  const std::wstring bin = DirOf(found_exe);
  // Copy ffmpeg.exe + runtime DLLs, skipping ffplay/ffprobe.
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileW(JoinPath(bin, L"*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        continue;
      }
      std::wstring base = fd.cFileName;
      if (_wcsicmp(base.c_str(), L"ffplay.exe") == 0 ||
          _wcsicmp(base.c_str(), L"ffprobe.exe") == 0) {
        continue;
      }
      const std::wstring lower = [&] {
        std::wstring s = base;
        for (auto& c : s) {
          c = static_cast<wchar_t>(towlower(c));
        }
        return s;
      }();
      const bool is_exe = _wcsicmp(base.c_str(), L"ffmpeg.exe") == 0;
      const bool is_dll = lower.size() >= 4 &&
                          lower.compare(lower.size() - 4, 4, L".dll") == 0;
      if (!is_exe && !is_dll) {
        continue;
      }
      CopyFileW(JoinPath(bin, base).c_str(), JoinPath(cache, base).c_str(),
                FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }

  const std::wstring installed = JoinPath(cache, L"ffmpeg.exe");
  std::wstring val_err;
  if (!ValidateRecordingFfmpeg(installed, &val_err)) {
    LogLine(L"[ffmpeg] validation failed: %s", val_err.c_str());
    if (error) {
      *error = L"下载的 FFmpeg 校验失败：" + val_err;
    }
    return false;
  }
  LogLine(L"[ffmpeg] installed + validated: %s", installed.c_str());
  return true;
}

}  // namespace

bool EnsureFfmpegForRecording(HWND owner, std::wstring* out_exe) {
  const std::wstring exe = FindFfmpegExe();
  if (!exe.empty()) {
    std::wstring err;
    if (ValidateRecordingFfmpeg(exe, &err)) {
      if (out_exe) {
        *out_exe = exe;
      }
      return true;
    }
    const int r = MessageBoxW(
        owner,
        (L"检测到 ffmpeg，但能力不满足录屏要求：\n" + err +
         L"\n\n是否重新下载完整版本（约 80 MB）？")
            .c_str(),
        L"录屏", MB_YESNO | MB_ICONWARNING);
    if (r != IDYES) {
      return false;
    }
  } else {
    const int r = MessageBoxW(
        owner, L"录屏需要 FFmpeg。\n未找到 FFmpeg，是否自动下载（约 80 MB）？",
        L"录屏", MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) {
      return false;
    }
  }

  ShowProgress(GetModuleHandleW(nullptr));
  std::wstring err;
  const bool ok = FetchAndInstall(&err);
  CloseProgress();
  if (!ok) {
    LogLine(L"[ffmpeg] fetch failed: %s", err.c_str());
    MessageBoxW(
        owner,
        (L"下载 FFmpeg 失败。\n" + (err.empty() ? L"未知错误" : err) +
         L"\n\n建议手动安装：winget install Gyan.FFmpeg")
            .c_str(),
        L"录屏", MB_OK | MB_ICONERROR);
    return false;
  }
  if (out_exe) {
    *out_exe = FindFfmpegExe();
  }
  return true;
}
