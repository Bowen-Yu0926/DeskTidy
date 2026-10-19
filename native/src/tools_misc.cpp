#include "tools_misc.h"

#include "display_geom.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "tool_paths.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>

#include <fstream>
#include <sstream>
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

namespace {

std::wstring WallpaperCfgPath() {
  return JoinPath(NativeDataDir(), L"wallpaper.json");
}

std::wstring ReadCfgField(const char* key) {
  std::ifstream in(WallpaperCfgPath());
  if (!in) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return {};
  }
  p = json.find('"', json.find(':', p) + 1);
  if (p == std::string::npos) {
    return {};
  }
  ++p;
  std::string val;
  while (p < json.size() && json[p] != '"') {
    if (json[p] == '\\' && p + 1 < json.size()) {
      ++p;
    }
    val.push_back(json[p++]);
  }
  return WidenUtf8(val);
}

void WriteCfg(const std::wstring& folder, const std::wstring& previous) {
  std::ofstream out(WallpaperCfgPath(), std::ios::binary);
  if (!out) {
    return;
  }
  auto esc = [](const std::wstring& w) {
    std::string s = NarrowUtf8(w);
    std::string o;
    for (char c : s) {
      if (c == '\\' || c == '"') {
        o.push_back('\\');
      }
      o.push_back(c);
    }
    return o;
  };
  out << "{\n  \"folder\":\"" << esc(folder) << "\",\n  \"previous\":\""
      << esc(previous) << "\"\n}\n";
}

bool RememberAndSet(const std::wstring& path) {
  wchar_t current[MAX_PATH] = {};
  SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, current, 0);
  const std::wstring folder = ReadCfgField("folder");
  std::wstring previous = current;
  if (previous.empty()) {
    previous = ReadCfgField("previous");
  }
  WriteCfg(folder, previous);
  std::wstring mutable_path = path;
  return SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, mutable_path.data(),
                               SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE) != 0;
}

std::vector<std::wstring> ListImagesInFolder(const std::wstring& folder) {
  std::vector<std::wstring> out;
  if (folder.empty()) {
    return out;
  }
  const wchar_t* exts[] = {L"*.jpg", L"*.jpeg", L"*.png", L"*.bmp"};
  for (const wchar_t* ext : exts) {
    const std::wstring pattern = JoinPath(folder, ext);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        out.push_back(JoinPath(folder, fd.cFileName));
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  return out;
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
  // Remember folder from picked file.
  std::wstring path = file;
  const size_t slash = path.find_last_of(L"\\/");
  if (slash != std::wstring::npos) {
    WriteCfg(path.substr(0, slash), ReadCfgField("previous"));
  }
  return RememberAndSet(path);
}

bool WallpaperMenu(HWND owner) {
  const int choice = MessageBoxW(
      owner,
      L"壁纸：\n\n是 = 选择图片\n否 = 从库目录随机\n取消 = 恢复上一张",
      L"壁纸", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDYES) {
    return PickAndSetWallpaper(owner);
  }
  if (choice == IDCANCEL) {
    const std::wstring prev = ReadCfgField("previous");
    if (prev.empty()) {
      MessageBoxW(owner, L"没有可恢复的上一张壁纸记录。", L"壁纸",
                  MB_OK | MB_ICONINFORMATION);
      return false;
    }
    std::wstring mutable_prev = prev;
    return SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, mutable_prev.data(),
                                 SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE) != 0;
  }
  // Random from folder
  std::wstring folder = ReadCfgField("folder");
  if (folder.empty()) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择壁纸库文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) {
      return false;
    }
    wchar_t path[MAX_PATH] = {};
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    folder = path;
    WriteCfg(folder, ReadCfgField("previous"));
  }
  auto images = ListImagesInFolder(folder);
  if (images.empty()) {
    MessageBoxW(owner, L"库目录中没有 jpg/png/bmp。", L"壁纸",
                MB_OK | MB_ICONWARNING);
    return false;
  }
  const size_t idx = static_cast<size_t>(GetTickCount()) % images.size();
  return RememberAndSet(images[idx]);
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

  const auto mons = EnumMonitors();
  int mon_index = -1;  // -1 = full virtual desktop
  if (mons.size() > 1) {
    std::wstring msg = L"选择录制目标：\n\n0. 整桌面（所有显示器）\n";
    for (size_t i = 0; i < mons.size(); ++i) {
      const auto& m = mons[i];
      msg += std::to_wstring(i + 1) + L". " +
             std::to_wstring(m.monitor.right - m.monitor.left) + L"x" +
             std::to_wstring(m.monitor.bottom - m.monitor.top) +
             (m.primary ? L" (主屏)" : L"") + L"\n";
    }
    msg += L"\n输入编号（默认 0）：";
    const std::wstring ans = PromptText(owner, L"录屏", msg.c_str(), L"0");
    if (!ans.empty()) {
      if (ans == L"0") {
        mon_index = -1;
      } else {
        mon_index = _wtoi(ans.c_str()) - 1;
      }
    }
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

  std::wstring region;
  if (mon_index >= 0 && mon_index < static_cast<int>(mons.size())) {
    const RECT& r = mons[static_cast<size_t>(mon_index)].monitor;
    wchar_t off[160];
    swprintf_s(off, L" -offset_x %d -offset_y %d -video_size %dx%d", r.left,
               r.top, r.right - r.left, r.bottom - r.top);
    region = off;
  }

  std::wstring cmd = L"\"" + ff + L"\" -y -f gdigrab -framerate 30" + region +
                     L" -i desktop -t 15 \"" + out + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    MessageBoxW(owner,
                (L"正在录制 15 秒…\n完成后文件：\n" + out).c_str(), L"录屏",
                MB_OK | MB_ICONINFORMATION);
    return true;
  }
  MessageBoxW(owner, L"无法启动 ffmpeg。", L"录屏", MB_OK | MB_ICONWARNING);
  return false;
}

void OpenCalculator() {
  if (ShellExecuteW(nullptr, L"open", L"calc.exe", nullptr, nullptr,
                    SW_SHOWNORMAL) <= reinterpret_cast<HINSTANCE>(32)) {
    ShellExecuteW(nullptr, L"open", L"calculator:", nullptr, nullptr,
                  SW_SHOWNORMAL);
  }
}

void ShowAppHelp(HWND owner) {
  MessageBoxW(
      owner,
      L"DeskTidy Native\n\n"
      L"· 分区：拖放钉选、右键样式/换页\n"
      L"· 整理：虚拟钉选 / 物理归档（可白名单）\n"
      L"· 截图：区域钉桌面 / 全屏\n"
      L"· 录屏：可选显示器，15 秒\n"
      L"· 保险库：本地 DPAPI + 可选云同步\n"
      L"· 便签：RichEdit 粗斜体\n"
      L"· 宠物：pets\\*.png 帧动画\n"
      L"· 托盘：语言切换、快捷键改绑、快照\n\n"
      L"数据目录：%LocalAppData%\\DeskTidyNative\\\n"
      L"更新：GitHub Releases",
      L"帮助", MB_OK | MB_ICONINFORMATION);
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
