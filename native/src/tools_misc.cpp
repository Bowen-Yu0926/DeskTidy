#include "tools_misc.h"

#include "app_prefs.h"
#include "display_geom.h"
#include "help_center.h"
#include "i18n.h"
#include "log.h"
#include "mist_webview.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "screen_record.h"
#include "tool_paths.h"
#include "update_check.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <objidl.h>
#include <gdiplus.h>

#include <fstream>
#include <random>
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

void WriteCfgFull(const std::wstring& folder, const std::wstring& previous,
                  const std::wstring& last_image) {
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
      << esc(previous) << "\",\n  \"last_image\":\"" << esc(last_image)
      << "\"\n}\n";
}

void WriteCfg(const std::wstring& folder, const std::wstring& previous) {
  WriteCfgFull(folder, previous, ReadCfgField("last_image"));
}

void RememberLastImage(const std::wstring& path) {
  WriteCfgFull(ReadCfgField("folder"), ReadCfgField("previous"), path);
}

std::wstring WallpapersCacheDir() {
  const std::wstring dir = JoinPath(NativeDataDir(), L"wallpapers");
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring OriginalWallpaperMarker() {
  return JoinPath(WallpapersCacheDir(), L"previous_wallpaper.txt");
}

bool IsUnderWallpapersCache(const std::wstring& path) {
  const std::wstring cache = WallpapersCacheDir();
  if (path.size() < cache.size()) {
    return false;
  }
  return _wcsnicmp(path.c_str(), cache.c_str(), cache.size()) == 0;
}

void RememberOriginalWallpaperOnce() {
  if (GetFileAttributesW(OriginalWallpaperMarker().c_str()) !=
      INVALID_FILE_ATTRIBUTES) {
    return;
  }
  wchar_t current[MAX_PATH] = {};
  SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, current, 0);
  if (!current[0] || IsUnderWallpapersCache(current)) {
    return;
  }
  std::ofstream out(OriginalWallpaperMarker(), std::ios::binary);
  if (out) {
    out << NarrowUtf8(current);
  }
  // Keep wallpaper.json "previous" in sync for older restore paths.
  WriteCfg(ReadCfgField("folder"), current);
}

void SetWallpaperStyleFill() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0,
                    KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  const wchar_t style[] = L"10";
  const wchar_t tile[] = L"0";
  RegSetValueExW(key, L"WallpaperStyle", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(style),
                 static_cast<DWORD>((wcslen(style) + 1) * sizeof(wchar_t)));
  RegSetValueExW(key, L"TileWallpaper", 0, REG_SZ,
                 reinterpret_cast<const BYTE*>(tile),
                 static_cast<DWORD>((wcslen(tile) + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
}

std::wstring StageWallpaper(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.');
  std::wstring ext = (dot != std::wstring::npos) ? path.substr(dot) : L".jpg";
  for (auto& c : ext) {
    c = static_cast<wchar_t>(towlower(c));
  }
  if (ext != L".jpg" && ext != L".jpeg" && ext != L".png" && ext != L".bmp") {
    ext = L".jpg";
  }
  const std::wstring dest = JoinPath(WallpapersCacheDir(), L"current" + ext);
  if (_wcsicmp(path.c_str(), dest.c_str()) != 0) {
    if (!CopyFileW(path.c_str(), dest.c_str(), FALSE)) {
      return path;
    }
  }
  return dest;
}

bool ApplyWallpaperPath(const std::wstring& path, bool stage, bool remember) {
  if (path.empty() ||
      GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return false;
  }
  if (remember) {
    RememberOriginalWallpaperOnce();
  }
  std::wstring target = stage ? StageWallpaper(path) : path;
  SetWallpaperStyleFill();
  return SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, target.data(),
                               SPIF_UPDATEINIFILE | SPIF_SENDWININICHANGE) != 0;
}

bool RememberAndSet(const std::wstring& path) {
  return ApplyWallpaperPath(path, true, true);
}

std::wstring ReadOriginalWallpaper() {
  std::ifstream in(OriginalWallpaperMarker());
  if (in) {
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::wstring path = WidenUtf8(ss.str());
    if (!path.empty() &&
        GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return path;
    }
  }
  return ReadCfgField("previous");
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

// True RNG (std::random_device → mt19937), never GetTickCount, so consecutive
// calls don't produce a predictable sequence.
size_t RandomIndex(size_t n) {
  if (n <= 1) {
    return 0;
  }
  thread_local std::mt19937 rng = [] {
    unsigned seed = 0;
    try {
      std::random_device rd;
      seed = rd();
    } catch (...) {
      seed = static_cast<unsigned>(GetTickCount()) ^
             static_cast<unsigned>(reinterpret_cast<uintptr_t>(&seed));
    }
    return std::mt19937(seed);
  }();
  std::uniform_int_distribution<size_t> dist(0, n - 1);
  return dist(rng);
}

// Pick a random image, skipping the previously-applied one when alternatives
// exist (so a library of 2+ images never repeats the same picture twice).
bool PickRandomImage(const std::vector<std::wstring>& images,
                     std::wstring* chosen) {
  if (!chosen || images.empty()) {
    return false;
  }
  if (images.size() == 1) {
    *chosen = images[0];
    return true;
  }
  const std::wstring last = ReadCfgField("last_image");
  std::vector<size_t> cand;
  for (size_t i = 0; i < images.size(); ++i) {
    if (_wcsicmp(images[i].c_str(), last.c_str()) != 0) {
      cand.push_back(i);
    }
  }
  if (cand.empty()) {
    cand.push_back(RandomIndex(images.size()));
  }
  *chosen = images[cand[RandomIndex(cand.size())]];
  return true;
}

}  // namespace

std::wstring BrowseFolderDialog(HWND owner, const wchar_t* title) {
  BROWSEINFOW bi{};
  bi.hwndOwner = owner;
  bi.lpszTitle = title ? title : L"选择文件夹";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
  if (!pidl) {
    return {};
  }
  wchar_t path[MAX_PATH] = {};
  SHGetPathFromIDListW(pidl, path);
  CoTaskMemFree(pidl);
  return path;
}

std::wstring WallpaperLibraryFolder() { return ReadCfgField("folder"); }

void SetWallpaperLibraryFolder(const std::wstring& folder) {
  WriteCfg(folder, ReadCfgField("previous"));
}

std::wstring ResolveRecordOutputDir() {
  AppPrefs prefs = LoadAppPrefs();
  if (!prefs.record_output_dir.empty()) {
    return prefs.record_output_dir;
  }
  const std::wstring dir = JoinPath(ExeDir(), L"录屏");
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

bool BrowseWallpaperLibrary(HWND owner) {
  const std::wstring path =
      BrowseFolderDialog(owner, L"选择壁纸库文件夹");
  if (path.empty()) {
    return false;
  }
  WriteCfg(path, ReadCfgField("previous"));
  return true;
}

bool RandomWallpaperFromLibrary(HWND owner) {
  std::wstring folder = ReadCfgField("folder");
  if (folder.empty()) {
    if (!BrowseWallpaperLibrary(owner)) {
      return false;
    }
    folder = ReadCfgField("folder");
  }
  auto images = ListImagesInFolder(folder);
  if (images.empty()) {
    MessageBoxW(owner, Tr("wallpaper_no_images").c_str(),
                Tr("wallpaper_title").c_str(), MB_OK | MB_ICONWARNING);
    return false;
  }
  std::wstring chosen;
  if (!PickRandomImage(images, &chosen)) {
    return false;
  }
  const std::wstring last = ReadCfgField("last_image");
  LogLine(L"[wallpaper] random pick: %s (last=%s)", chosen.c_str(),
          last.empty() ? L"(none)" : last.c_str());
  const bool ok = RememberAndSet(chosen);
  if (ok) {
    RememberLastImage(chosen);
  } else {
    MessageBoxW(owner, L"设置壁纸失败。", Tr("wallpaper_title").c_str(),
                MB_OK | MB_ICONERROR);
  }
  return ok;
}

bool BrowseRecordOutputDir(HWND owner) {
  AppPrefs prefs = LoadAppPrefs();
  const std::wstring path =
      BrowseFolderDialog(owner, L"选择录屏保存文件夹");
  if (path.empty()) {
    return false;
  }
  prefs.record_output_dir = path;
  SaveAppPrefs(prefs);
  return true;
}

void ResetRecordOutputDir() {
  AppPrefs prefs = LoadAppPrefs();
  prefs.record_output_dir.clear();
  SaveAppPrefs(prefs);
}

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
  const int choice = MessageBoxW(owner, Tr("wallpaper_prompt").c_str(),
                                 Tr("wallpaper_title").c_str(),
                                 MB_YESNOCANCEL | MB_ICONQUESTION);
  if (choice == IDYES) {
    return PickAndSetWallpaper(owner);
  }
  if (choice == IDCANCEL) {
    const std::wstring prev = ReadOriginalWallpaper();
    if (prev.empty()) {
      MessageBoxW(owner, Tr("wallpaper_no_original").c_str(),
                  Tr("wallpaper_title").c_str(), MB_OK | MB_ICONINFORMATION);
      return false;
    }
    return ApplyWallpaperPath(prev, false, false);
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
    MessageBoxW(owner, Tr("wallpaper_no_images").c_str(),
                Tr("wallpaper_title").c_str(), MB_OK | MB_ICONWARNING);
    return false;
  }
  std::wstring chosen;
  if (!PickRandomImage(images, &chosen)) {
    return false;
  }
  const std::wstring last = ReadCfgField("last_image");
  LogLine(L"[wallpaper] random pick: %s (last=%s)", chosen.c_str(),
          last.empty() ? L"(none)" : last.c_str());
  const bool ok = RememberAndSet(chosen);
  if (ok) {
    RememberLastImage(chosen);
  } else {
    MessageBoxW(owner, L"设置壁纸失败。", Tr("wallpaper_title").c_str(),
                MB_OK | MB_ICONERROR);
  }
  return ok;
}

bool CaptureRegionToPng(HWND owner) {
  // Fallback fullscreen capture (region overlay is preferred via App).
  // Capture the whole VIRTUAL desktop so multi-monitor setups (incl. negative
  // coordinates) are covered, not just the primary screen.
  Gdi();
  const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  BitBlt(mem, 0, 0, w, h, screen, left, top, SRCCOPY);

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
  bool ok = false;

  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.user_data_subdir = L"webview2-rec-cfg";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\record_cfg.html");
  if (dlg.html_path.empty()) {
    MessageBoxW(owner, L"找不到 record_cfg.html", Tr("record_title").c_str(),
                MB_OK | MB_ICONWARNING);
    return;
  }
  dlg.build_state = [&]() {
    int q = prefs.record_quality;
    if (q < 0 || q > 2) {
      q = 1;
    }
    int fps = prefs.record_fps;
    if (fps < 15 || fps > 60) {
      fps = 30;
    }
    const std::vector<std::wstring> mics = ListAudioCaptureDevices();
    std::wstring json = L"{\"type\":\"state\",\"seconds\":";
    json += std::to_wstring(prefs.record_seconds);
    json += L",\"fps\":";
    json += std::to_wstring(fps);
    json += L",\"dir\":\"";
    json += MistJsonEscape(prefs.record_output_dir);
    json += L"\",\"quality\":";
    json += std::to_wstring(q);
    json += L",\"mic\":\"";
    json += MistJsonEscape(prefs.record_mic);
    json += L"\",\"mics\":[";
    for (size_t i = 0; i < mics.size(); ++i) {
      if (i) {
        json += L',';
      }
      json += L'"';
      json += MistJsonEscape(mics[i]);
      json += L'"';
    }
    json += L"]}";
    return json;
  };
  dlg.on_message = [&](const std::wstring& json) -> bool {
    const std::wstring type = MistJsonStringField(json, L"type");
    if (type != L"action") {
      return false;
    }
    const std::wstring id = MistJsonStringField(json, L"id");
    HWND parent = dlg.hwnd ? dlg.hwnd : owner;
    if (id == L"ready") {
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"browse") {
      const std::wstring path =
          BrowseFolderDialog(parent, L"选择录屏保存文件夹");
      if (!path.empty()) {
        prefs.record_output_dir = path;
        std::wstring msg = L"{\"type\":\"dir_picked\",\"path\":\"";
        msg += MistJsonEscape(path);
        msg += L"\"}";
        MistWebDialogPostJson(&dlg, msg);
      }
      return false;
    }
    if (id == L"save") {
      prefs.record_seconds = MistJsonIntField(json, L"seconds", 0);
      if (prefs.record_seconds < 0) {
        prefs.record_seconds = 0;
      }
      prefs.record_output_dir = MistJsonStringField(json, L"dir");
      prefs.record_quality = MistJsonIntField(json, L"quality", 1);
      if (prefs.record_quality < 0 || prefs.record_quality > 2) {
        prefs.record_quality = 1;
      }
      prefs.record_fps = MistJsonIntField(json, L"fps", 30);
      if (prefs.record_fps < 15 || prefs.record_fps > 60) {
        prefs.record_fps = 30;
      }
      prefs.record_mic = MistJsonStringField(json, L"mic");
      ok = true;
      return true;
    }
    if (id == L"cancel") {
      return true;
    }
    return false;
  };

  MistWebDialogRun(&dlg, Tr("record_settings").c_str(), 460, 420);
  if (ok) {
    SaveAppPrefs(prefs);
    MessageBoxW(owner, Tr("record_saved").c_str(), Tr("record_title").c_str(),
                MB_OK | MB_ICONINFORMATION);
  }
}

void OpenCalculator() {
  HWND found = nullptr;
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
          return TRUE;
        }
        wchar_t title[256] = {};
        GetWindowTextW(hwnd, title, 256);
        if (!title[0]) {
          return TRUE;
        }
        if (wcsstr(title, L"计算器") || wcsstr(title, L"Calculator")) {
          *reinterpret_cast<HWND*>(lp) = hwnd;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&found));
  if (found) {
    ShowWindow(found, SW_RESTORE);
    SetForegroundWindow(found);
    return;
  }
  if (ShellExecuteW(nullptr, L"open", L"calc.exe", nullptr, nullptr,
                    SW_SHOWNORMAL) <= reinterpret_cast<HINSTANCE>(32)) {
    ShellExecuteW(nullptr, L"open", L"calculator:", nullptr, nullptr,
                  SW_SHOWNORMAL);
  }
}

void ShowAppHelp(HWND owner) { ShowAppHelpCenter(owner); }

void OpenUpdatePage(HWND owner) { CheckForUpdates(owner); }

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
