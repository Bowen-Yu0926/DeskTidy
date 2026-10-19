#include "tools_misc.h"

#include "paths_util.h"
#include "tool_paths.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>

#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
  UINT num = 0;
  UINT size = 0;
  Gdiplus::GetImageEncodersSize(&num, &size);
  if (size == 0) {
    return -1;
  }
  auto* info = static_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
  if (!info) {
    return -1;
  }
  Gdiplus::GetImageEncoders(num, size, info);
  for (UINT i = 0; i < num; ++i) {
    if (wcscmp(info[i].MimeType, format) == 0) {
      *pClsid = info[i].Clsid;
      free(info);
      return static_cast<int>(i);
    }
  }
  free(info);
  return -1;
}

struct GdiplusOnce {
  ULONG_PTR token = 0;
  GdiplusOnce() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
  }
  ~GdiplusOnce() {
    if (token) {
      Gdiplus::GdiplusShutdown(token);
    }
  }
};

GdiplusOnce& Gdi() {
  static GdiplusOnce g;
  return g;
}

}  // namespace

bool PickAndSetWallpaper(HWND owner) {
  wchar_t file[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Images\0*.bmp;*.jpg;*.jpeg;*.png\0All\0*.*\0";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
  if (!GetOpenFileNameW(&ofn)) {
    return false;
  }
  return SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, file,
                               SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE) != 0;
}

bool CaptureRegionToPng(HWND owner) {
  // Fallback fullscreen capture (region overlay is preferred via App).
  Gdi();
  const int w = GetSystemMetrics(SM_CXSCREEN);
  const int h = GetSystemMetrics(SM_CYSCREEN);
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY);

  PWSTR pics = nullptr;
  std::wstring dir = NativeDataDir();
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics)) &&
      pics) {
    dir = JoinPath(pics, L"DeskTidyNative");
    CoTaskMemFree(pics);
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  wchar_t name[64];
  SYSTEMTIME st{};
  GetLocalTime(&st);
  swprintf_s(name, L"shot_%04d%02d%02d_%02d%02d%02d.png", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  const std::wstring path = JoinPath(dir, name);

  Gdiplus::Bitmap bitmap(bmp, nullptr);
  CLSID clsid{};
  bool ok = false;
  if (GetEncoderClsid(L"image/png", &clsid) >= 0) {
    ok = bitmap.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (ok) {
    MessageBoxW(owner, path.c_str(), L"全屏截图已保存",
                MB_OK | MB_ICONINFORMATION);
  }
  return ok;
}

bool StartScreenRecord(HWND owner) {
  const std::wstring ff = FindFfmpegExe();
  if (ff.empty()) {
    MessageBoxW(owner,
                L"未找到 ffmpeg。\n"
                L"请安装到 PATH，或放到：\n"
                L"  · 程序目录\\ffmpeg.exe\n"
                L"  · 仓库 assets\\ffmpeg\\ffmpeg.exe",
                L"录屏", MB_OK | MB_ICONWARNING);
    return false;
  }

  PWSTR vids = nullptr;
  std::wstring out_dir = NativeDataDir();
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vids)) &&
      vids) {
    out_dir = JoinPath(vids, L"DeskTidyNative");
    CoTaskMemFree(vids);
    CreateDirectoryW(out_dir.c_str(), nullptr);
  }
  wchar_t name[64];
  SYSTEMTIME st{};
  GetLocalTime(&st);
  swprintf_s(name, L"rec_%04d%02d%02d_%02d%02d%02d.mp4", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  const std::wstring out = JoinPath(out_dir, name);

  std::wstring cmd = L"\"" + ff +
                     L"\" -y -f gdigrab -framerate 30 -i desktop -t 15 \"" +
                     out + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    MessageBoxW(owner, (L"已启动 15 秒桌面录制：\n" + out).c_str(), L"录屏",
                MB_OK | MB_ICONINFORMATION);
    return true;
  }
  MessageBoxW(owner, L"无法启动 ffmpeg。", L"录屏", MB_OK | MB_ICONWARNING);
  return false;
}

void OpenUpdatePage() {
  ShellExecuteW(nullptr, L"open",
                L"https://github.com/Bowen-Yu0926/DeskTidy/releases", nullptr,
                nullptr, SW_SHOWNORMAL);
}

bool SendPathsToRecycleBin(const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    return false;
  }
  size_t chars = 1;
  for (const auto& p : paths) {
    chars += p.size() + 1;
  }
  std::vector<wchar_t> buf(chars, L'\0');
  wchar_t* dest = buf.data();
  for (const auto& p : paths) {
    wcscpy_s(dest, p.size() + 1, p.c_str());
    dest += p.size() + 1;
  }
  SHFILEOPSTRUCTW op{};
  op.wFunc = FO_DELETE;
  op.pFrom = buf.data();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}
