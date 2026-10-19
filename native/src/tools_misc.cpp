#include "tools_misc.h"

#include "app_prefs.h"
#include "display_geom.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "screen_record.h"
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

bool StartScreenRecord(HWND owner) { return ToggleScreenRecord(owner); }

void ConfigureScreenRecord(HWND owner) {
  AppPrefs prefs = LoadAppPrefs();
  const std::wstring sec = PromptText(
      owner, L"录屏时长",
      L"自动停止秒数（0 = 手动点徽章停止）：",
      std::to_wstring(prefs.record_seconds).c_str());
  if (!sec.empty()) {
    prefs.record_seconds = _wtoi(sec.c_str());
    if (prefs.record_seconds < 0) {
      prefs.record_seconds = 0;
    }
  }
  const std::wstring dir = PromptText(
      owner, L"录屏目录",
      L"输出目录（留空 = Videos\\DeskTidyNative）：",
      prefs.record_output_dir.c_str());
  // Empty answer keeps previous; single space clears to default.
  if (!dir.empty()) {
    std::wstring trimmed = dir;
    while (!trimmed.empty() && trimmed.back() == L' ') {
      trimmed.pop_back();
    }
    prefs.record_output_dir = trimmed;
  }
  SaveAppPrefs(prefs);
  MessageBoxW(owner, L"录屏设置已保存。", L"录屏", MB_OK | MB_ICONINFORMATION);
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
      L"· 录屏：开关切换 + 角标停止，可配时长/目录\n"
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
