#include "notes_window.h"

#include "app_prefs.h"
#include "desknote_help.h"
#include "desknote_launch.h"
#include "desktop_shell.h"
#include "display_geom.h"
#include "elevated_save.h"
#include "i18n.h"
#include "log.h"
#include "markdown_preview.h"
#include "mist_msg.h"
#include "mist_webview.h"
#include "notes_library.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "tool_paths.h"
#include "ui_theme.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <Richedit.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <Uxtheme.h>
#include <windowsx.h>
#include <gdiplus.h>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

#pragma comment(lib, "comctl32.lib")

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <string>

#pragma comment(lib, "comdlg32.lib")

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativeNotes";
constexpr int kEditId = 2001;
constexpr int kMenuSave = 2101;
constexpr int kMenuSaveAs = 2102;
constexpr int kMenuBigger = 2103;
constexpr int kMenuSmaller = 2104;
constexpr int kMenuTpl = 2105;
constexpr int kMenuBold = 2106;
constexpr int kMenuItalic = 2107;
constexpr int kMenuUnder = 2108;
constexpr int kMenuLibrary = 2109;
constexpr int kMenuPreview = 2110;
constexpr int kMenuFind = 2111;
constexpr int kMenuReplace = 2112;
constexpr int kMenuSearchLib = 2113;
constexpr int kMenuExportMd = 2114;
constexpr int kMenuPandoc = 2115;
constexpr int kMenuOpenWith = 2116;
constexpr int kMenuPasteHtml = 2117;
constexpr int kMenuToast = 2118;
constexpr int kMenuWysiwyg = 2119;
constexpr int kMenuFindNext = 2120;
constexpr int kMenuFindPrev = 2121;
constexpr int kMenuExportHtml = 2122;
constexpr int kMenuOutline = 2123;
constexpr int kMenuFocus = 2124;
constexpr int kMenuTypewriter = 2125;
constexpr int kMenuNewTab = 2126;
constexpr int kMenuCloseTab = 2127;
constexpr int kMenuOpen = 2128;
constexpr int kMenuHelp = 2129;
constexpr int kMenuOpenFolder = 2130;
constexpr int kMenuSidebar = 2131;
constexpr int kMenuPreviewPane = 2132;
constexpr int kMenuExportPdf = 2133;
constexpr int kMenuWrap = 2134;
constexpr int kMenuEditMode = 2135;
constexpr int kMenuSyncScroll = 2136;
constexpr int kMenuUndo = 2137;
constexpr int kMenuCut = 2138;
constexpr int kMenuCopy = 2139;
constexpr int kMenuPaste = 2140;
constexpr int kMenuSelectAll = 2141;
constexpr int kMenuReload = 2142;
constexpr int kMenuGotoLine = 2143;
constexpr int kMenuCloseWindow = 2144;
constexpr int kMenuRedo = 2145;
constexpr int kMenuNextTab = 2146;
constexpr int kMenuPrevTab = 2147;
constexpr int kMenuAlwaysOnTop = 2148;
constexpr int kMenuInsertLink = 2149;
constexpr int kMenuInsertCode = 2150;
constexpr int kMenuHeading1 = 2151;
constexpr int kMenuHeading2 = 2152;
constexpr int kMenuHeading3 = 2153;
constexpr int kMenuInsertImage = 2154;
constexpr int kMenuEncUtf8 = 2155;
constexpr int kMenuEncUtf8Bom = 2156;
constexpr int kMenuEncAcp = 2157;
constexpr int kMenuCopyPath = 2158;
constexpr int kTabCopyPath = 3105;
constexpr int kTabCloseOthers = 3106;
constexpr int kTabCloseRight = 3107;
constexpr int kSideCopyPath = 3008;
constexpr int kMenuStrike = 2159;
constexpr int kMenuBullet = 2160;
constexpr int kMenuQuote = 2161;
constexpr int kMenuHRule = 2162;
constexpr int kMenuDateTime = 2163;
constexpr int kMenuDupLine = 2164;
constexpr int kMenuMoveLineUp = 2165;
constexpr int kMenuMoveLineDown = 2166;
constexpr int kMenuToggleBookmark = 2167;
constexpr int kMenuNextBookmark = 2168;
constexpr int kMenuPrevBookmark = 2169;
constexpr int kMenuOpenHosts = 2170;
constexpr int kEditCtxUndo = 3201;
constexpr int kEditCtxCut = 3202;
constexpr int kEditCtxCopy = 3203;
constexpr int kEditCtxPaste = 3204;
constexpr int kEditCtxSelectAll = 3205;
constexpr int kEditCtxFind = 3206;
constexpr int kEditCtxGoto = 3207;
constexpr int kOutlineId = 2002;
constexpr int kSidebarRefreshId = 2009;
constexpr int kModeStripId = 2010;
// Compact top-right preview/edit chip (not a full-height strip).
constexpr int kModeBtnW = 64;
constexpr int kModeBtnH = 28;
constexpr int kModeBtnPad = 10;
constexpr int kOutlineStripId = 2011;
constexpr int kGutterId = 2012;
constexpr int kOutlineStripW = 28;
constexpr int kOutlineExpandedW = 150;
constexpr wchar_t kGutterClass[] = L"DeskNoteGutter";
constexpr int kTabsId = 2003;
constexpr int kSidebarId = 2004;
constexpr int kSidebarSearchId = 2005;
constexpr int kSidebarNewId = 2006;
constexpr int kSidebarResultsId = 2007;
constexpr int kStatusId = 2008;
constexpr int kTabRename = 3101;
constexpr int kTabPin = 3102;
constexpr int kTabReveal = 3103;
constexpr int kTabDelete = 3104;
constexpr int kTabClose = 3105;
constexpr UINT_PTR kPreviewTimer = 1;
constexpr UINT_PTR kScrollTimer = 2;
constexpr UINT_PTR kSidebarSearchTimer = 3;
constexpr UINT_PTR kOutlineTimer = 4;
constexpr UINT_PTR kStatusTimer = 5;
constexpr UINT_PTR kPersistTabsTimer = 6;
constexpr UINT_PTR kGutterTimer = 7;
constexpr int kStatusBarH = 26;

// P2-30: follow Windows app light/dark preference for MD preview chrome.
bool SystemAppsUseLightTheme() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          0, KEY_READ, &key) != ERROR_SUCCESS) {
    return true;
  }
  DWORD val = 1;
  DWORD size = sizeof(val);
  const LONG rc =
      RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                       reinterpret_cast<LPBYTE>(&val), &size);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS) {
    return true;
  }
  return val != 0;
}

void CountTextStats(const std::wstring& text, int* chars, int* words) {
  int c = 0;
  int w = 0;
  bool in_word = false;
  for (wchar_t ch : text) {
    if (ch == L'\r') {
      continue;
    }
    ++c;
    const bool space = ch == L' ' || ch == L'\t' || ch == L'\n' ||
                       ch == 0x3000 || ch == L'\u2029';
    if (space) {
      in_word = false;
    } else if (!in_word) {
      in_word = true;
      ++w;
    }
  }
  if (chars) {
    *chars = c;
  }
  if (words) {
    *words = w;
  }
}

bool IsImageDropSuffix(const std::wstring& lower) {
  return lower == L".png" || lower == L".jpg" || lower == L".jpeg" ||
         lower == L".gif" || lower == L".bmp" || lower == L".webp" ||
         lower == L".tif" || lower == L".tiff";
}

// Sidebar context-menu command ids (kept distinct from window menu ids).
constexpr int kSideOpen = 3001;
constexpr int kSideRename = 3002;
constexpr int kSideDelete = 3003;
constexpr int kSideReveal = 3004;
constexpr int kSideRefresh = 3005;
constexpr int kSidePin = 3006;
constexpr int kSideNew = 3007;

// Suffixes the editor can open (mirrors Python notepad.py NOTE_OPEN_SUFFIXES).
bool IsOpenableSuffix(const std::wstring& lower) {
  static const wchar_t* kSfx[] = {
      L".txt",    L".text",  L".md",     L".markdown", L".log",   L".csv",
      L".tsv",    L".json",  L".xml",    L".yml",      L".yaml",  L".toml",
      L".ini",    L".cfg",   L".conf",   L".config",   L".env",   L".properties",
      L".py",     L".pyw",   L".pyi",    L".js",       L".mjs",   L".cjs",
      L".ts",     L".tsx",   L".jsx",    L".css",      L".scss",  L".less",
      L".html",   L".htm",   L".xhtml",  L".vue",      L".java",  L".c",
      L".h",      L".cpp",   L".hpp",    L".cc",       L".cs",    L".go",
      L".rs",     L".rb",    L".php",    L".sql",      L".sh",    L".bash",
      L".zsh",    L".ps1",   L".psm1",   L".bat",      L".cmd",   L".vbs",
      L".gitignore", L".gitattributes", L".editorconfig", L".dockerfile",
      L".makefile", L".cmake", L".gradle", L".r",        L".lua",   L".pl",
      L".swift",  L".kt",    L".kts",    L".dart",     L".jsonc", L".json5",
      L".svg",    L".tex",   L".rst",    L".adoc",     L".nfo",   L".diz",
      L".reg",    L".inf",   L".url",    L".eml",      L".ics",   L".rtf",
  };
  for (const wchar_t* sfx : kSfx) {
    if (lower == sfx) {
      return true;
    }
  }
  return false;
}

std::wstring LowerSuffix(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  const size_t base = (slash == std::wstring::npos) ? 0 : slash + 1;
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos || dot < base) {
    return {};
  }
  std::wstring sfx = path.substr(dot);
  for (auto& c : sfx) {
    if (c >= L'A' && c <= L'Z') {
      c += (L'a' - L'A');
    }
  }
  return sfx;
}

std::wstring FileBaseName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

bool IsHostsPath(const std::wstring& path) {
  return _wcsicmp(FileBaseName(path).c_str(), L"hosts") == 0;
}

bool IsOpenablePath(const std::wstring& path) {
  if (IsHostsPath(path)) {
    return true;
  }
  return IsOpenableSuffix(LowerSuffix(path));
}

std::wstring SystemHostsPath() {
  wchar_t windir[MAX_PATH] = {};
  if (!GetWindowsDirectoryW(windir, MAX_PATH) || !windir[0]) {
    return L"C:\\Windows\\System32\\drivers\\etc\\hosts";
  }
  return JoinPath(JoinPath(windir, L"System32\\drivers\\etc"), L"hosts");
}


struct StreamCookie {
  std::string data;
  size_t pos = 0;
};

DWORD CALLBACK StreamInCb(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
  auto* c = reinterpret_cast<StreamCookie*>(cookie);
  const size_t left = c->data.size() - c->pos;
  const LONG n = static_cast<LONG>((std::min)(left, static_cast<size_t>(cb)));
  if (n > 0) {
    memcpy(buf, c->data.data() + c->pos, static_cast<size_t>(n));
  }
  c->pos += static_cast<size_t>(n);
  *pcb = n;
  return 0;
}

DWORD CALLBACK StreamOutCb(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG* pcb) {
  auto* c = reinterpret_cast<StreamCookie*>(cookie);
  c->data.append(reinterpret_cast<char*>(buf), reinterpret_cast<char*>(buf) + cb);
  *pcb = cb;
  return 0;
}

COLORREF ColorFromCssHex(const std::string& hex) {
  unsigned r = 0, g = 0, b = 0;
  if (hex.size() >= 7 && hex[0] == '#') {
    sscanf_s(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b);
  }
  return RGB(r, g, b);
}

bool RecyclePathQuiet(HWND owner, const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  std::wstring from = path;
  from.push_back(L'\0');
  from.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.hwnd = owner;
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
  return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}


void ApplyCharEffect(HWND edit, DWORD mask, DWORD effect, bool on) {
  CHARFORMAT2W cf{};
  cf.cbSize = sizeof(cf);
  cf.dwMask = mask;
  cf.dwEffects = on ? effect : 0;
  SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&cf));
}

std::string EscapeJson(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20) {
      char buf[8];
      sprintf_s(buf, "\\u%04x", c);
      out += buf;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

int GetEncoderClsid(const wchar_t* format, CLSID* clsid) {
  UINT num = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&num, &size);
  if (!size) {
    return -1;
  }
  auto* info = static_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
  if (!info) {
    return -1;
  }
  Gdiplus::GetImageEncoders(num, size, info);
  int found = -1;
  for (UINT i = 0; i < num; ++i) {
    if (wcscmp(info[i].MimeType, format) == 0) {
      *clsid = info[i].Clsid;
      found = static_cast<int>(i);
      break;
    }
  }
  free(info);
  return found;
}

struct GdiOnce {
  ULONG_PTR token = 0;
  GdiOnce() {
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&token, &in, nullptr);
  }
};
void EnsureGdiplus() {
  static GdiOnce once;
  (void)once;
}

// Save an HBITMAP as PNG via GDI+; returns false on failure.
bool SaveBitmapAsPng(HBITMAP bmp, const std::wstring& path) {
  if (!bmp) {
    return false;
  }
  EnsureGdiplus();
  CLSID clsid{};
  if (GetEncoderClsid(L"image/png", &clsid) < 0) {
    return false;
  }
  Gdiplus::Bitmap bitmap(bmp, nullptr);
  return bitmap.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

// Make a unique assets image path mirroring Python md_assets.py
// (_unique_asset_path): img_YYYYMMDD_HHMMSS.png (img_…_NN.png on collision).
std::wstring MakeAssetImagePath(const std::wstring& assets_dir) {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t stem[32];
  swprintf_s(stem, L"img_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  auto exists = [](const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
  };
  std::wstring path = JoinPath(assets_dir, std::wstring(stem) + L".png");
  for (int n = 1; exists(path) && n < 100; ++n) {
    wchar_t suffix[8];
    swprintf_s(suffix, L"_%02d", n);
    path = JoinPath(assets_dir, std::wstring(stem) + suffix + L".png");
  }
  return path;
}

// Returns the directory of a file path (or "." for a bare name).
std::wstring DirOfPath(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return L".";
  }
  return path.substr(0, slash);
}

// Try to read a bitmap from the clipboard; returns null on failure.
HBITMAP ClipboardBitmap(HWND owner) {
  if (!OpenClipboard(owner)) {
    return nullptr;
  }
  HBITMAP out = nullptr;
  if (IsClipboardFormatAvailable(CF_DIB)) {
    HANDLE h = GetClipboardData(CF_DIB);
    if (h) {
      // CF_DIB gives a packed BITMAPINFO + pixels.
      const BYTE* dib = static_cast<const BYTE*>(GlobalLock(h));
      if (dib) {
        const auto* bih = reinterpret_cast<const BITMAPINFOHEADER*>(dib);
        void* bits = const_cast<BYTE*>(dib + bih->biSize +
                                       bih->biClrUsed * 4);
        BITMAPINFO bmi{};
        memcpy(&bmi.bmiHeader, bih, sizeof(BITMAPINFOHEADER));
        HDC dc = GetDC(nullptr);
        out = CreateDIBitmap(dc, bih, CBM_INIT, bits, &bmi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        GlobalUnlock(h);
      }
    }
  }
  if (!out && IsClipboardFormatAvailable(CF_BITMAP)) {
    HANDLE h = GetClipboardData(CF_BITMAP);
    if (h) {
      HBITMAP src = static_cast<HBITMAP>(h);
      BITMAP bm{};
      if (GetObjectW(src, sizeof(bm), &bm)) {
        HDC dc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(dc);
        HDC mem2 = CreateCompatibleDC(dc);
        HBITMAP copy = CreateCompatibleBitmap(dc, bm.bmWidth, bm.bmHeight);
        HGDIOBJ old = SelectObject(mem, src);
        HGDIOBJ old2 = SelectObject(mem2, copy);
        BitBlt(mem2, 0, 0, bm.bmWidth, bm.bmHeight, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        SelectObject(mem2, old2);
        DeleteDC(mem);
        DeleteDC(mem2);
        ReleaseDC(nullptr, dc);
        out = copy;
      }
    }
  }
  CloseClipboard();
  return out;
}

}  // namespace

bool NotesWindow::Create(HINSTANCE instance) {
  path_ = JoinPath(NotesLibraryDir(true), L"notes.rtf");
  LoadLibraryW(L"Msftedit.dll");
  {
    static ULONG_PTR gdi_token = 0;
    if (!gdi_token) {
      Gdiplus::GdiplusStartupInput in;
      Gdiplus::GdiplusStartup(&gdi_token, &in, nullptr);
    }
  }
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES};
  InitCommonControlsEx(&icc);

  // Clean white stage + cool gray library rail.
  content_bg_ = RGB(255, 255, 255);
  card_bg_ = RGB(255, 255, 255);
  rail_bg_ = RGB(245, 246, 248);
  text_color_ = RGB(32, 36, 42);
  muted_color_ = RGB(120, 128, 140);
  border_color_ = RGB(226, 230, 236);
  bg_brush_ = CreateSolidBrush(content_bg_);
  card_brush_ = CreateSolidBrush(card_bg_);
  rail_brush_ = CreateSolidBrush(rail_bg_);
  toast_ = false;

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = NotesWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = bg_brush_;
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  const RECT work = PrimaryWorkArea();
  const int ww = 960;
  const int wh = 640;
  const int x = work.left + (std::max)(0, static_cast<int>(work.right - work.left - ww) / 2);
  const int y = work.top + (std::max)(0, static_cast<int>(work.bottom - work.top - wh) / 2);
  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClassName, L"DeskNote",
                          WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, ww, wh,
                          nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

  HMENU menu = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, kMenuNewTab, L"新建\tCtrl+N");
  AppendMenuW(file, MF_STRING, kMenuOpen, L"打开…\tCtrl+O");
  AppendMenuW(file, MF_STRING, kMenuOpenHosts, L"打开 hosts 文件");
  AppendMenuW(file, MF_STRING, kMenuCloseTab, L"删除文件并关闭\tCtrl+W");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuSave, L"保存\tCtrl+S");
  AppendMenuW(file, MF_STRING, kMenuSaveAs, L"另存为…\tCtrl+Shift+S");
  AppendMenuW(file, MF_STRING, kMenuReload, L"从磁盘重新加载\tF5");
  AppendMenuW(file, MF_STRING, kMenuCopyPath, L"复制文件路径\tCtrl+Shift+C");
  HMENU enc = CreatePopupMenu();
  AppendMenuW(enc, MF_STRING, kMenuEncUtf8, L"UTF-8");
  AppendMenuW(enc, MF_STRING, kMenuEncUtf8Bom, L"UTF-8 带 BOM");
  AppendMenuW(enc, MF_STRING, kMenuEncAcp, L"系统 ANSI（如 GBK）");
  AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(enc),
              L"编码转换（保存）");
  AppendMenuW(file, MF_STRING, kMenuLibrary, Tr("notes_library").c_str());
  AppendMenuW(file, MF_STRING, kMenuOpenFolder, Tr("notes_open_folder").c_str());
  AppendMenuW(file, MF_STRING, kMenuSearchLib, L"搜索笔记库\tCtrl+Shift+F");
  AppendMenuW(file, MF_STRING, kMenuPreview, L"Markdown 预览窗口…");
  AppendMenuW(file, MF_STRING, kMenuExportMd, Tr("notes_export_md").c_str());
  AppendMenuW(file, MF_STRING, kMenuExportHtml, Tr("notes_export_html").c_str());
  AppendMenuW(file, MF_STRING, kMenuPandoc, Tr("notes_pandoc").c_str());
  AppendMenuW(file, MF_STRING, kMenuOpenWith, Tr("notes_open_with").c_str());
  AppendMenuW(file, MF_STRING, kMenuPasteHtml, Tr("notes_paste_html").c_str());
  AppendMenuW(file, MF_STRING, kMenuToast, Tr("notes_toast").c_str());
  AppendMenuW(file, MF_STRING, kMenuWysiwyg, Tr("notes_wysiwyg").c_str());
  AppendMenuW(file, MF_STRING, kMenuExportPdf, Tr("notes_export_pdf").c_str());
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuTpl, Tr("notes_template").c_str());
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuCloseWindow, L"关闭窗口");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), Tr("file").c_str());
  HMENU editm = CreatePopupMenu();
  AppendMenuW(editm, MF_STRING, kMenuUndo, L"撤销\tCtrl+Z");
  AppendMenuW(editm, MF_STRING, kMenuRedo, L"重做\tCtrl+Y");
  AppendMenuW(editm, MF_STRING, kMenuCut, L"剪切\tCtrl+X");
  AppendMenuW(editm, MF_STRING, kMenuCopy, L"复制\tCtrl+C");
  AppendMenuW(editm, MF_STRING, kMenuPaste, L"粘贴\tCtrl+V");
  AppendMenuW(editm, MF_STRING, kMenuSelectAll, L"全选\tCtrl+A");
  AppendMenuW(editm, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(editm, MF_STRING, kMenuFind, L"查找…\tCtrl+F");
  AppendMenuW(editm, MF_STRING, kMenuFindNext, L"查找下一个\tF3");
  AppendMenuW(editm, MF_STRING, kMenuFindPrev, L"查找上一个\tShift+F3");
  AppendMenuW(editm, MF_STRING, kMenuReplace, L"替换…\tCtrl+H");
  AppendMenuW(editm, MF_STRING, kMenuGotoLine, L"转到行…\tCtrl+G");
  AppendMenuW(editm, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(editm, MF_STRING, kMenuDupLine, L"复制当前行\tCtrl+D");
  AppendMenuW(editm, MF_STRING, kMenuMoveLineUp, L"上移当前行\tCtrl+Shift+↑");
  AppendMenuW(editm, MF_STRING, kMenuMoveLineDown,
              L"下移当前行\tCtrl+Shift+↓");
  AppendMenuW(editm, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(editm, MF_STRING, kMenuToggleBookmark,
              L"切换书签\tCtrl+F2");
  AppendMenuW(editm, MF_STRING, kMenuNextBookmark, L"下一书签\tF2");
  AppendMenuW(editm, MF_STRING, kMenuPrevBookmark, L"上一书签\tShift+F2");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(editm),
              Tr("edit").c_str());
  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, kMenuEditMode, L"编辑\tCtrl+E");
  AppendMenuW(view, MF_STRING, kMenuPreviewPane, L"预览\tCtrl+Shift+V");
  AppendMenuW(view, MF_STRING | MF_CHECKED, kMenuSyncScroll, L"同步滚动");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kMenuBigger, L"放大字体\tCtrl++");
  AppendMenuW(view, MF_STRING, kMenuSmaller, L"缩小字体\tCtrl+-");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kMenuOutline, L"大纲\tCtrl+Shift+O");
  AppendMenuW(view, MF_STRING, kMenuSidebar, L"文件树\tCtrl+Shift+E");
  AppendMenuW(view, MF_STRING, kMenuFocus, L"专注模式\tF11");
  AppendMenuW(view, MF_STRING, kMenuTypewriter, Tr("notes_typewriter").c_str());
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kMenuAlwaysOnTop, L"窗口置顶");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view),
              Tr("view").c_str());
  HMENU fmt = CreatePopupMenu();
  AppendMenuW(fmt, MF_STRING | MF_CHECKED, kMenuWrap, L"自动换行(&W)");
  AppendMenuW(fmt, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(fmt, MF_STRING, kMenuBold, L"粗体\tCtrl+B");
  AppendMenuW(fmt, MF_STRING, kMenuItalic, L"斜体\tCtrl+I");
  AppendMenuW(fmt, MF_STRING, kMenuUnder, L"下划线\tCtrl+U");
  AppendMenuW(fmt, MF_STRING, kMenuStrike, L"删除线");
  AppendMenuW(fmt, MF_STRING, kMenuInsertLink, L"插入链接\tCtrl+K");
  AppendMenuW(fmt, MF_STRING, kMenuInsertCode, L"插入代码块");
  AppendMenuW(fmt, MF_STRING, kMenuInsertImage, L"插入图片…");
  AppendMenuW(fmt, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(fmt, MF_STRING, kMenuHeading1, L"一级标题");
  AppendMenuW(fmt, MF_STRING, kMenuHeading2, L"二级标题");
  AppendMenuW(fmt, MF_STRING, kMenuHeading3, L"三级标题");
  AppendMenuW(fmt, MF_STRING, kMenuBullet, L"无序列表");
  AppendMenuW(fmt, MF_STRING, kMenuQuote, L"引用");
  AppendMenuW(fmt, MF_STRING, kMenuHRule, L"水平分割线");
  AppendMenuW(fmt, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(fmt, MF_STRING, kMenuDateTime, L"插入日期时间\tCtrl+Shift+D");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fmt),
              Tr("format").c_str());
  HMENU help = CreatePopupMenu();
  AppendMenuW(help, MF_STRING, kMenuHelp, L"DeskNote 使用说明\tF1");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), Tr("help").c_str());
  menu_ = menu;
  SetMenu(hwnd_, menu);
  BuildAccelerators();

  {
    static bool tab_class_once = false;
    if (!tab_class_once) {
      WNDCLASSEXW tc{};
      tc.cbSize = sizeof(tc);
      tc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
      tc.lpfnWndProc = NotesWindow::TabsProc;
      tc.hInstance = instance;
      tc.hCursor = LoadCursor(nullptr, IDC_ARROW);
      tc.hbrBackground = nullptr;
      tc.lpszClassName = L"DeskNoteTabBar";
      RegisterClassExW(&tc);
      tab_class_once = true;
    }
  }
  tabs_ = CreateWindowExW(
      0, L"DeskNoteTabBar", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0,
      300, 40, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabsId)),
      instance, nullptr);
  tabs_proc_ = nullptr;

  edit_ = CreateWindowExW(
      0, MSFTEDIT_CLASS, L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
          ES_WANTRETURN | ES_SAVESEL,
      6, 6, 300, 220, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), instance, nullptr);
  if (!edit_) {
    edit_ = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_WANTRETURN,
        6, 6, 300, 220, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), instance,
        nullptr);
  }
  if (!edit_) {
    return false;
  }
  edit_proc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(NotesWindow::EditProc)));
  SendMessageW(edit_, EM_SETBKGNDCOLOR, 0, card_bg_);
  SendMessageW(edit_, EM_SETEVENTMASK, 0,
               ENM_CHANGE | ENM_SELCHANGE | ENM_SCROLL);
  SetFontSize(font_pt_);
  ApplyWordWrap();
  ApplyChrome();
  EnsureGutter();
  outline_list_ = CreateWindowExW(
      0, L"LISTBOX", L"",
      WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 6, 6, 140, 220,
      hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutlineId)), instance,
      nullptr);
  if (outline_list_) {
    SetWindowTheme(outline_list_, L"", L"");
    ShowWindow(outline_list_, SW_HIDE);
  }
  outline_strip_ = CreateWindowExW(
      0, L"BUTTON", L"大\n纲", WS_CHILD | BS_OWNERDRAW, 0, 0, kOutlineStripW,
      120, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutlineStripId)), instance,
      nullptr);
  if (outline_strip_) {
    SetWindowTheme(outline_strip_, L"", L"");
    ShowWindow(outline_strip_, SW_HIDE);
  }
  sidebar_search_ = CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL, 6, 6, 120, 22, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarSearchId)), instance,
      nullptr);
  // Owner-draw quiet chrome (no native 3D / system blue).
  sidebar_new_ = CreateWindowExW(
      0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 130, 6, 28, 28, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarNewId)), instance,
      nullptr);
  sidebar_refresh_ = CreateWindowExW(
      0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 158, 6, 28, 28, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarRefreshId)), instance,
      nullptr);
  sidebar_ = CreateWindowExW(
      0, WC_TREEVIEWW, L"",
      WS_CHILD | TVS_HASBUTTONS | TVS_SHOWSELALWAYS | TVS_EDITLABELS |
          TVS_FULLROWSELECT | TVS_NOHSCROLL | TVS_TRACKSELECT,
      6, 30, 150, 200, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarId)), instance,
      nullptr);
  if (sidebar_) {
    SetWindowTheme(sidebar_, L"", L"");
    TreeView_SetItemHeight(sidebar_, 24);
    TreeView_SetIndent(sidebar_, 16);
    sidebar_proc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        sidebar_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(NotesWindow::SidebarProc)));
  }
  if (sidebar_new_) {
    SetWindowTheme(sidebar_new_, L"", L"");
  }
  if (sidebar_refresh_) {
    SetWindowTheme(sidebar_refresh_, L"", L"");
  }
  mode_strip_ = CreateWindowExW(
      0, L"BUTTON", L"预览", WS_CHILD | BS_OWNERDRAW, 0, 0, kModeBtnW,
      kModeBtnH, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kModeStripId)), instance,
      nullptr);
  if (mode_strip_) {
    SetWindowTheme(mode_strip_, L"", L"");
    ShowWindow(mode_strip_, SW_HIDE);
  }
  // Hover invalidate for owner-draw quiet buttons.
  auto hook_btn = [](HWND btn) {
    if (!btn) {
      return;
    }
    SetWindowSubclass(
        btn,
        [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
            -> LRESULT {
          if (msg == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
          } else if (msg == WM_MOUSELEAVE || msg == WM_LBUTTONDOWN ||
                     msg == WM_LBUTTONUP) {
            InvalidateRect(hwnd, nullptr, FALSE);
          }
          return DefSubclassProc(hwnd, msg, wp, lp);
        },
        1, 0);
  };
  hook_btn(sidebar_new_);
  hook_btn(sidebar_refresh_);
  hook_btn(mode_strip_);
  hook_btn(outline_strip_);
  sidebar_results_ = CreateWindowExW(
      0, L"LISTBOX", L"",
      WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 6, 30, 150, 200,
      hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarResultsId)),
      instance, nullptr);
  if (sidebar_results_) {
    SetWindowTheme(sidebar_results_, L"", L"");
  }
  if (sidebar_) {
    ShowWindow(sidebar_, SW_HIDE);
  }
  if (sidebar_search_) {
    ShowWindow(sidebar_search_, SW_HIDE);
    SetWindowTheme(sidebar_search_, L"", L"");
    SendMessageW(sidebar_search_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"搜索笔记…"));
    SendMessageW(sidebar_search_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(8, 8));
  }
  if (sidebar_new_) {
    ShowWindow(sidebar_new_, SW_HIDE);
  }
  if (sidebar_refresh_) {
    ShowWindow(sidebar_refresh_, SW_HIDE);
  }
  if (sidebar_results_) {
    ShowWindow(sidebar_results_, SW_HIDE);
  }
  status_ = CreateWindowExW(
      0, L"STATIC", L"",
      WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_NOTIFY, 0, 0, 200,
      kStatusBarH, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusId)),
      instance, nullptr);
  if (status_) {
    if (font_) {
      SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
    status_proc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        status_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(NotesWindow::StatusProc)));
  }
  DragAcceptFiles(hwnd_, TRUE);
  web_.Create(hwnd_);
  web_.on_dirty = [this]() {
    // Immediate (no markdown body) — covers the 150ms changed debounce window.
    MarkDirty();
  };
  web_.on_changed = [this]() {
    // Cache already updated in OnWebMessage. Mirror to RichEdit only on
    // Save / SyncFromWysiwyg / tab switch — not every keystroke.
    MarkDirty();
  };
  web_.on_need_content = [this]() {
    // Boot race: prefer RichEdit load buffer, then cache. Do not go through
    // CurrentPlainText() — in WYSIWYG that reads cache and can re-push "".
    std::wstring md;
    if (edit_) {
      const int len = GetWindowTextLengthW(edit_);
      if (len > 0) {
        md.assign(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(edit_, md.data(), len + 1);
        md.resize(static_cast<size_t>(len));
      }
    }
    if (md.empty()) {
      md = web_.CachedMarkdown();
    }
    web_.SetMarkdown(md);
  };
  {
    const std::wstring theme =
        SystemAppsUseLightTheme() ? L"light" : L"dark";
    web_.SetTheme(theme);
    preview_web_.SetTheme(theme);
  }
  LoadTabSession();
  LoadViewSettings();
  ApplyEditorFont();
  ApplyUiFonts();
  if (tab_paths_.empty()) {
    tab_paths_.push_back(path_);
    tab_index_ = 0;
  }
  path_ = tab_paths_[static_cast<size_t>(tab_index_)];
  RebuildTabControl();
  Load();
  ApplyWordWrap();
  ApplyViewMenuChecks();
  if (sidebar_visible_) {
    RebuildSidebar();
  }
  if (outline_) {
    RebuildOutline();
  }
  preview_ = false;
  if (always_on_top_ && !toast_) {
    CheckMenuItem(menu_, kMenuAlwaysOnTop, MF_BYCOMMAND | MF_CHECKED);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  }
  UpdateModeStrip();
  LayoutClient();
  EnsureMarkdownEditor(true);
  RefreshTitle();
  UpdateStatusBar();
  PersistViewSettings();
  // Status chrome; word-count is cached so a modest interval is enough.
  SetTimer(hwnd_, kStatusTimer, 800, nullptr);
  return true;
}

void NotesWindow::SetFontSize(int pt) {
  if (pt < 8) {
    pt = 8;
  }
  if (pt > 48) {
    pt = 48;
  }
  if (font_pt_ == pt) {
    return;
  }
  font_pt_ = pt;
  ApplyEditorFont();
  PersistViewSettings();
}

void NotesWindow::ApplyEditorFont() {
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  HDC hdc = GetDC(nullptr);
  const int height = -MulDiv(font_pt_, GetDeviceCaps(hdc, LOGPIXELSY), 72);
  bool has_consolas = false;
  LOGFONTW probe{};
  probe.lfCharSet = DEFAULT_CHARSET;
  wcsncpy_s(probe.lfFaceName, L"Consolas", _TRUNCATE);
  EnumFontFamiliesExW(
      hdc, &probe,
      [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) -> int {
        *reinterpret_cast<bool*>(lp) = true;
        return 0;
      },
      reinterpret_cast<LPARAM>(&has_consolas), 0);
  ReleaseDC(nullptr, hdc);
  // Writing surface: Consolas 12pt (Cascadia Mono fallback).
  const wchar_t* face = has_consolas ? L"Consolas" : L"Cascadia Mono";
  font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, face);
  if (edit_) {
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  }
}

void NotesWindow::ApplyUiFonts() {
  if (ui_font_) {
    DeleteObject(ui_font_);
    ui_font_ = nullptr;
  }
  if (ui_font_bold_) {
    DeleteObject(ui_font_bold_);
    ui_font_bold_ = nullptr;
  }
  // Match the menubar (工具栏) — NONCLIENTMETRICS lfMenuFont, not a larger
  // display size. Sidebar / tabs previously used 13pt and looked oversized.
  NONCLIENTMETRICS ncm{};
  ncm.cbSize = sizeof(ncm);
  HFONT menu_font = nullptr;
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
    menu_font = CreateFontIndirectW(&ncm.lfMenuFont);
    LOGFONTW bold = ncm.lfMenuFont;
    bold.lfWeight = FW_SEMIBOLD;
    ui_font_bold_ = CreateFontIndirectW(&bold);
  }
  if (!menu_font) {
    HDC hdc = GetDC(nullptr);
    const int height = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);
    menu_font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    ui_font_bold_ = CreateFontW(height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
  }
  ui_font_ = menu_font;
  if (!ui_font_) {
    return;
  }
  // Row height tracks menu font (≈9pt → ~22px @96dpi); floor for touch.
  HDC hdc_m = GetDC(nullptr);
  HFONT old_m = static_cast<HFONT>(SelectObject(hdc_m, ui_font_));
  TEXTMETRICW tm{};
  GetTextMetricsW(hdc_m, &tm);
  SelectObject(hdc_m, old_m);
  ReleaseDC(nullptr, hdc_m);
  const int row_h =
      (std::max)(22, static_cast<int>(tm.tmHeight) + 8);
  const WPARAM font = reinterpret_cast<WPARAM>(ui_font_);
  if (tabs_) {
    SendMessageW(tabs_, WM_SETFONT, font, TRUE);
  }
  if (status_) {
    SendMessageW(status_, WM_SETFONT, font, TRUE);
  }
  if (sidebar_) {
    SendMessageW(sidebar_, WM_SETFONT, font, TRUE);
    TreeView_SetItemHeight(sidebar_, row_h);
    TreeView_SetIndent(sidebar_, 16);
  }
  if (sidebar_search_) {
    SendMessageW(sidebar_search_, WM_SETFONT, font, TRUE);
  }
  if (sidebar_results_) {
    SendMessageW(sidebar_results_, WM_SETFONT, font, TRUE);
  }
  if (outline_list_) {
    SendMessageW(outline_list_, WM_SETFONT, font, TRUE);
  }
  if (outline_strip_) {
    SendMessageW(outline_strip_, WM_SETFONT, font, TRUE);
  }
  if (mode_strip_) {
    SendMessageW(mode_strip_, WM_SETFONT, font, TRUE);
  }
}

void NotesWindow::ApplyChrome() {
  if (!hwnd_) {
    return;
  }
  const COLORREF frame = toast_ ? RGB(255, 248, 220) : content_bg_;
  const COLORREF edit_bg = toast_ ? frame : card_bg_;
  HBRUSH old = bg_brush_;
  bg_brush_ = CreateSolidBrush(frame);
  SetClassLongPtrW(hwnd_, GCLP_HBRBACKGROUND,
                   reinterpret_cast<LONG_PTR>(bg_brush_));
  if (old && old != bg_brush_) {
    DeleteObject(old);
  }
  HBRUSH old_card = card_brush_;
  card_brush_ = CreateSolidBrush(edit_bg);
  if (old_card && old_card != card_brush_) {
    DeleteObject(old_card);
  }
  HBRUSH old_rail = rail_brush_;
  rail_brush_ = CreateSolidBrush(rail_bg_);
  if (old_rail && old_rail != rail_brush_) {
    DeleteObject(old_rail);
  }
  if (edit_) {
    SendMessageW(edit_, EM_SETBKGNDCOLOR, 0, edit_bg);
    // Default ink for new text only — do not rewrite existing RTF runs.
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = text_color_;
    SendMessageW(edit_, EM_SETCHARFORMAT, SCF_DEFAULT,
                 reinterpret_cast<LPARAM>(&cf));
  }
  if (sidebar_) {
    TreeView_SetBkColor(sidebar_, rail_bg_);
    TreeView_SetTextColor(sidebar_, text_color_);
    TreeView_SetLineColor(sidebar_, border_color_);
  }
  if (outline_list_) {
    InvalidateRect(outline_list_, nullptr, TRUE);
  }
  if (sidebar_results_) {
    InvalidateRect(sidebar_results_, nullptr, TRUE);
  }
  if (tabs_) {
    InvalidateRect(tabs_, nullptr, TRUE);
  }
  InvalidateRect(hwnd_, nullptr, TRUE);
  if (edit_) {
    InvalidateRect(edit_, nullptr, TRUE);
  }
  if (status_) {
    InvalidateRect(status_, nullptr, TRUE);
  }
  if (gutter_) {
    InvalidateRect(gutter_, nullptr, TRUE);
  }
  ApplyUiFonts();
}

void NotesWindow::ApplyUiTheme(const std::string& ui_theme) {
  ui_theme_ = NormalizeUiTheme(ui_theme);
  const auto vars = UiThemeCssVars(ui_theme_);
  auto pick = [&](const char* key, COLORREF fallback) -> COLORREF {
    const auto it = vars.find(key);
    if (it == vars.end()) {
      return fallback;
    }
    return ColorFromCssHex(it->second);
  };
  // DeskNote: white writing stage + cool gray file rail.
  content_bg_ = RGB(255, 255, 255);
  card_bg_ = RGB(255, 255, 255);
  rail_bg_ = RGB(245, 246, 248);
  border_color_ = RGB(226, 230, 236);
  text_color_ = pick("text", RGB(32, 36, 42));
  muted_color_ = pick("muted", RGB(120, 128, 140));
  ApplyChrome();
  // P2: preview/WYSIWYG follow the product theme (not only OS light/dark).
  // All shipped UI themes use light content panes → "light" chrome.
  const std::wstring web_theme = L"light";
  web_.SetTheme(web_theme);
  preview_web_.SetTheme(web_theme);
  if (preview_ && PathLooksMarkdown()) {
    SchedulePreviewRefresh();
  }
  if (wysiwyg_) {
    web_.SetMarkdown(CurrentPlainText());
  }
}

void NotesWindow::RefreshTitle() {
  if (!hwnd_) {
    return;
  }
  // Match Python: always "{*}{name} — DeskNote" (mode as suffix only).
  std::wstring title;
  if (dirty_) {
    title += L"*";
  }
  title += TabLabel(path_);
  title += L" — DeskNote";
  if (toast_) {
    title += L" · 便签条";
  } else if (wysiwyg_) {
  }
  SetWindowTextW(hwnd_, title.c_str());
}

void NotesWindow::Destroy() {
  Save(true);
  PersistTabs();
  PersistViewSettings();
  if (hwnd_) {
    KillTimer(hwnd_, kStatusTimer);
    KillTimer(hwnd_, kPersistTabsTimer);
    KillTimer(hwnd_, kGutterTimer);
  }
  web_.Destroy();
  preview_web_.Destroy();
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  if (ui_font_) {
    DeleteObject(ui_font_);
    ui_font_ = nullptr;
  }
  if (ui_font_bold_) {
    DeleteObject(ui_font_bold_);
    ui_font_bold_ = nullptr;
  }
  if (accel_) {
    DestroyAcceleratorTable(accel_);
    accel_ = nullptr;
  }
  if (hwnd_) {
    SetClassLongPtrW(hwnd_, GCLP_HBRBACKGROUND,
                     reinterpret_cast<LONG_PTR>(GetStockObject(WHITE_BRUSH)));
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (bg_brush_) {
    DeleteObject(bg_brush_);
    bg_brush_ = nullptr;
  }
  if (card_brush_) {
    DeleteObject(card_brush_);
    card_brush_ = nullptr;
  }
  if (rail_brush_) {
    DeleteObject(rail_brush_);
    rail_brush_ = nullptr;
  }
}

bool NotesWindow::TranslateAccel(MSG* msg) const {
  return accel_ && hwnd_ && msg &&
         TranslateAcceleratorW(hwnd_, accel_, msg) != 0;
}

void NotesWindow::BuildAccelerators() {
  if (accel_) {
    DestroyAcceleratorTable(accel_);
    accel_ = nullptr;
  }
  // FVIRTKEY | FCONTROL [| FSHIFT] — matches Python DeskNote shortcuts.
  ACCEL table[] = {
      {FVIRTKEY | FCONTROL, 'N', static_cast<WORD>(kMenuNewTab)},
      {FVIRTKEY | FCONTROL, 'O', static_cast<WORD>(kMenuOpen)},
      {FVIRTKEY | FCONTROL, 'S', static_cast<WORD>(kMenuSave)},
      {FVIRTKEY | FCONTROL | FSHIFT, 'S', static_cast<WORD>(kMenuSaveAs)},
      {FVIRTKEY | FCONTROL, 'W', static_cast<WORD>(kMenuCloseTab)},
      {FVIRTKEY | FCONTROL, 'F', static_cast<WORD>(kMenuFind)},
      {FVIRTKEY | FCONTROL, 'H', static_cast<WORD>(kMenuReplace)},
      {FVIRTKEY | FCONTROL, 'E', static_cast<WORD>(kMenuEditMode)},
      {FVIRTKEY | FCONTROL, 'B', static_cast<WORD>(kMenuBold)},
      {FVIRTKEY | FCONTROL, 'I', static_cast<WORD>(kMenuItalic)},
      {FVIRTKEY | FCONTROL, 'U', static_cast<WORD>(kMenuUnder)},
      {FVIRTKEY | FCONTROL | FSHIFT, 'V', static_cast<WORD>(kMenuPreviewPane)},
      {FVIRTKEY | FCONTROL | FSHIFT, 'O', static_cast<WORD>(kMenuOutline)},
      {FVIRTKEY | FCONTROL | FSHIFT, 'E', static_cast<WORD>(kMenuSidebar)},
      {FVIRTKEY | FCONTROL | FSHIFT, 'F', static_cast<WORD>(kMenuSearchLib)},
      {FVIRTKEY | FCONTROL, 'G', static_cast<WORD>(kMenuGotoLine)},
      {FVIRTKEY | FCONTROL, 'Y', static_cast<WORD>(kMenuRedo)},
      {FVIRTKEY | FCONTROL, 'K', static_cast<WORD>(kMenuInsertLink)},
      {FVIRTKEY | FCONTROL | FSHIFT, 'C', static_cast<WORD>(kMenuCopyPath)},
      {FVIRTKEY | FCONTROL | FSHIFT, 'D', static_cast<WORD>(kMenuDateTime)},
      {FVIRTKEY | FCONTROL, VK_TAB, static_cast<WORD>(kMenuNextTab)},
      {FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, static_cast<WORD>(kMenuPrevTab)},
      {FVIRTKEY | FCONTROL, VK_OEM_PLUS, static_cast<WORD>(kMenuBigger)},
      {FVIRTKEY | FCONTROL, VK_ADD, static_cast<WORD>(kMenuBigger)},
      {FVIRTKEY | FCONTROL, VK_OEM_MINUS, static_cast<WORD>(kMenuSmaller)},
      {FVIRTKEY | FCONTROL, VK_SUBTRACT, static_cast<WORD>(kMenuSmaller)},
      {FVIRTKEY, VK_F1, static_cast<WORD>(kMenuHelp)},
      {FVIRTKEY, VK_F3, static_cast<WORD>(kMenuFindNext)},
      {FVIRTKEY | FSHIFT, VK_F3, static_cast<WORD>(kMenuFindPrev)},
      {FVIRTKEY, VK_F5, static_cast<WORD>(kMenuReload)},
      {FVIRTKEY, VK_F11, static_cast<WORD>(kMenuFocus)},
      {FVIRTKEY | FCONTROL, 'D', static_cast<WORD>(kMenuDupLine)},
      {FVIRTKEY | FCONTROL | FSHIFT, VK_UP, static_cast<WORD>(kMenuMoveLineUp)},
      {FVIRTKEY | FCONTROL | FSHIFT, VK_DOWN,
       static_cast<WORD>(kMenuMoveLineDown)},
      {FVIRTKEY | FCONTROL, VK_F2, static_cast<WORD>(kMenuToggleBookmark)},
      {FVIRTKEY, VK_F2, static_cast<WORD>(kMenuNextBookmark)},
      {FVIRTKEY | FSHIFT, VK_F2, static_cast<WORD>(kMenuPrevBookmark)},
  };
  accel_ = CreateAcceleratorTableW(
      table, static_cast<int>(sizeof(table) / sizeof(table[0])));
}

bool NotesWindow::PathLooksMarkdown() const {
  if (path_.size() >= 3 &&
      _wcsicmp(path_.c_str() + path_.size() - 3, L".md") == 0) {
    return true;
  }
  if (path_.size() >= 9 &&
      _wcsicmp(path_.c_str() + path_.size() - 9, L".markdown") == 0) {
    return true;
  }
  return false;
}

bool NotesWindow::PathLooksTextish() const {
  if (path_.empty()) {
    return false;
  }
  if (IsHostsPath(path_)) {
    return true;
  }
  const std::wstring sfx = LowerSuffix(path_);
  if (sfx.empty() || sfx == L".rtf") {
    return false;
  }
  return IsOpenableSuffix(sfx);
}

void NotesWindow::OpenHostsFile() {
  OpenOrActivatePath(SystemHostsPath());
}

void NotesWindow::WrapSelectionMarkdown(const wchar_t* left,
                                        const wchar_t* right) {
  if (!edit_ || !left || !right) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  std::wstring selected;
  if (cr.cpMin != cr.cpMax) {
    const int sel_len = static_cast<int>(cr.cpMax - cr.cpMin);
    selected.assign(static_cast<size_t>(sel_len) + 1, L'\0');
    TEXTRANGEW tr{};
    tr.chrg = cr;
    tr.lpstrText = selected.data();
    SendMessageW(edit_, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&tr));
    selected.resize(static_cast<size_t>(sel_len));
  }
  std::wstring wrapped = left;
  wrapped += selected;
  wrapped += right;
  SendMessageW(edit_, EM_REPLACESEL, TRUE,
               reinterpret_cast<LPARAM>(wrapped.c_str()));
  const LONG left_len = static_cast<LONG>(wcslen(left));
  CHARRANGE next{};
  if (selected.empty()) {
    next.cpMin = cr.cpMin + left_len;
    next.cpMax = next.cpMin;
  } else {
    next.cpMin = cr.cpMin + left_len;
    next.cpMax = next.cpMin + static_cast<LONG>(selected.size());
  }
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&next));
  MarkDirty();
}

void NotesWindow::EnsureSourceEditForFormat() {
  // Format / insert acts on the source editor — leave preview first so the
  // change is visible (and menu actions are not no-ops on a hidden pane).
  if (preview_) {
    ShowEditMode();
  }
}

void NotesWindow::ApplyBold() {
  EnsureSourceEditForFormat();
  if (PathLooksMarkdown()) {
    WrapSelectionMarkdown(L"**", L"**");
  } else {
    ApplyCharEffect(edit_, CFM_BOLD, CFE_BOLD, true);
  }
}

void NotesWindow::ApplyItalic() {
  EnsureSourceEditForFormat();
  if (PathLooksMarkdown()) {
    WrapSelectionMarkdown(L"*", L"*");
  } else {
    ApplyCharEffect(edit_, CFM_ITALIC, CFE_ITALIC, true);
  }
}

void NotesWindow::ApplyUnderline() {
  EnsureSourceEditForFormat();
  if (PathLooksMarkdown()) {
    WrapSelectionMarkdown(L"<u>", L"</u>");
  } else {
    ApplyCharEffect(edit_, CFM_UNDERLINE, CFE_UNDERLINE, true);
  }
}

void NotesWindow::InsertMdLink() {
  EnsureSourceEditForFormat();
  if (!edit_) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  std::wstring selected;
  if (cr.cpMin != cr.cpMax) {
    const int sel_len = static_cast<int>(cr.cpMax - cr.cpMin);
    selected.assign(static_cast<size_t>(sel_len) + 1, L'\0');
    TEXTRANGEW tr{};
    tr.chrg = cr;
    tr.lpstrText = selected.data();
    SendMessageW(edit_, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&tr));
    selected.resize(static_cast<size_t>(sel_len));
  }
  const std::wstring url =
      PromptText(hwnd_, L"插入链接", L"链接地址：", L"https://");
  if (url.empty()) {
    return;
  }
  std::wstring text = selected;
  if (text.empty()) {
    text = PromptText(hwnd_, L"插入链接", L"显示文字：", url);
    if (text.empty()) {
      text = url;
    }
  }
  const std::wstring md = L"[" + text + L"](" + url + L")";
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(md.c_str()));
  MarkDirty();
}

void NotesWindow::InsertMdCodeFence() {
  EnsureSourceEditForFormat();
  if (!edit_) {
    return;
  }
  WrapSelectionMarkdown(L"```\r\n", L"\r\n```");
}

void NotesWindow::InsertMdHeading(int level) {
  EnsureSourceEditForFormat();
  if (!edit_ || level < 1 || level > 6) {
    return;
  }
  std::wstring prefix(static_cast<size_t>(level), L'#');
  prefix += L" ";
  // Prefix the current line.
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  const int line =
      static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, cr.cpMin));
  const LONG start =
      static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, line, 0));
  CHARRANGE at{start, start};
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&at));
  SendMessageW(edit_, EM_REPLACESEL, TRUE,
               reinterpret_cast<LPARAM>(prefix.c_str()));
  MarkDirty();
}

void NotesWindow::RefreshCurrentTabLabel() {
  if (tabs_) {
    tab_chips_dirty_ = true;
    InvalidateRect(tabs_, nullptr, FALSE);
  }
}

bool NotesWindow::InsertImageFile(const std::wstring& src_path) {
  EnsureSourceEditForFormat();
  if (!edit_ || src_path.empty() || !PathLooksMarkdown()) {
    return false;
  }
  const size_t dot = src_path.find_last_of(L'.');
  std::wstring lower =
      dot == std::wstring::npos ? L"" : src_path.substr(dot);
  for (auto& ch : lower) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  if (!IsImageDropSuffix(lower)) {
    return false;
  }
  const std::wstring assets_dir = JoinPath(NoteBaseDir(), L"assets");
  CreateDirectoryW(assets_dir.c_str(), nullptr);
  const size_t slash = src_path.find_last_of(L"\\/");
  std::wstring base =
      slash == std::wstring::npos ? src_path : src_path.substr(slash + 1);
  std::wstring dest = JoinPath(assets_dir, base);
  if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    dest = MakeAssetImagePath(assets_dir);
    const size_t ddot = dest.find_last_of(L'.');
    if (ddot != std::wstring::npos && !lower.empty()) {
      dest = dest.substr(0, ddot) + lower;
    }
  }
  if (!CopyFileW(src_path.c_str(), dest.c_str(), FALSE)) {
    MistAlert(hwnd_, L"插入图片", L"无法导入图片。", MistMsgTone::Warn);
    return false;
  }
  const size_t dslash = dest.find_last_of(L"\\/");
  const std::wstring name =
      dslash == std::wstring::npos ? dest : dest.substr(dslash + 1);
  const std::wstring md = L"![](assets/" + name + L")\n";
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(md.c_str()));
  MarkDirty();
  if (preview_) {
    SchedulePreviewRefresh();
  }
  return true;
}

void NotesWindow::InsertImageFromFile() {
  EnsureSourceEditForFormat();
  if (!edit_) {
    return;
  }
  if (!PathLooksMarkdown()) {
    MistAlert(hwnd_, L"插入图片", L"请先打开或保存一个 Markdown（.md）文档。");
    return;
  }
  wchar_t file[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter =
      L"图片\0*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.svg\0所有文件\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT |
              OFN_EXPLORER;
  if (!GetOpenFileNameW(&ofn)) {
    return;
  }
  std::vector<std::wstring> paths;
  if (ofn.nFileOffset > 0 && file[ofn.nFileOffset - 1] == L'\0') {
    // Multi-select: directory + null-separated names (need larger buffer for
    // many files; single path still works via the else branch for typical use).
    const std::wstring dir = file;
    // With default MAX_PATH buffer, multi-select is best-effort for one file.
    paths.push_back(dir + L"\\" + (file + ofn.nFileOffset));
  } else {
    paths.push_back(file);
  }
  int n = 0;
  for (const auto& p : paths) {
    if (InsertImageFile(p)) {
      ++n;
    }
  }
  if (n == 0 && !paths.empty()) {
    MistAlert(hwnd_, L"插入图片", L"未插入任何图片（格式不受支持或复制失败）。",
              MistMsgTone::Warn);
  }
}

void NotesWindow::ToggleAlwaysOnTop() {
  always_on_top_ = !always_on_top_;
  if (menu_) {
    CheckMenuItem(menu_, kMenuAlwaysOnTop,
                  MF_BYCOMMAND | (always_on_top_ ? MF_CHECKED : MF_UNCHECKED));
  }
  if (hwnd_ && !toast_) {
    SetWindowPos(hwnd_, always_on_top_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0,
                 0, SWP_NOMOVE | SWP_NOSIZE);
  }
  PersistViewSettings();
}

void NotesWindow::ToggleSyncScroll() {
  sync_scroll_ = !sync_scroll_;
  if (menu_) {
    CheckMenuItem(menu_, kMenuSyncScroll,
                  MF_BYCOMMAND | (sync_scroll_ ? MF_CHECKED : MF_UNCHECKED));
  }
  PersistViewSettings();
}

void NotesWindow::ApplyViewMenuChecks() {
  if (!menu_) {
    return;
  }
  CheckMenuItem(menu_, kMenuSyncScroll,
                MF_BYCOMMAND | (sync_scroll_ ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(menu_, kMenuWrap,
                MF_BYCOMMAND | (word_wrap_ ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(menu_, kMenuOutline,
                MF_BYCOMMAND | (outline_ ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(menu_, kMenuSidebar,
                MF_BYCOMMAND | (sidebar_visible_ ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(menu_, kMenuTypewriter,
                MF_BYCOMMAND | (typewriter_ ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(menu_, kMenuPreviewPane,
                MF_BYCOMMAND | (preview_ ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(menu_, kMenuFocus,
                MF_BYCOMMAND | (focus_ ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(menu_, kMenuAlwaysOnTop,
                MF_BYCOMMAND | (always_on_top_ ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(menu_, kMenuEncUtf8,
                MF_BYCOMMAND | (text_enc_ == TextFileEncoding::Utf8
                                    ? MF_CHECKED
                                    : MF_UNCHECKED));
  CheckMenuItem(menu_, kMenuEncUtf8Bom,
                MF_BYCOMMAND | (text_enc_ == TextFileEncoding::Utf8Bom
                                    ? MF_CHECKED
                                    : MF_UNCHECKED));
  CheckMenuItem(menu_, kMenuEncAcp,
                MF_BYCOMMAND | (text_enc_ == TextFileEncoding::Acp
                                    ? MF_CHECKED
                                    : MF_UNCHECKED));
  SyncMdMenuState();
}

void NotesWindow::SyncMdMenuState() {
  if (!menu_) {
    return;
  }
  const bool md = PathLooksMarkdown();
  const UINT flag = MF_BYCOMMAND | (md ? MF_ENABLED : MF_GRAYED);
  EnableMenuItem(menu_, kMenuExportHtml, flag);
  EnableMenuItem(menu_, kMenuExportPdf, flag);
  EnableMenuItem(menu_, kMenuPandoc, flag);
  EnableMenuItem(menu_, kMenuExportMd, flag);
  EnableMenuItem(menu_, kMenuPreview, flag);
  EnableMenuItem(menu_, kMenuPreviewPane, flag);
  EnableMenuItem(menu_, kMenuEditMode, flag);
  EnableMenuItem(menu_, kMenuOutline, flag);
  EnableMenuItem(menu_, kMenuSyncScroll, flag);
  EnableMenuItem(menu_, kMenuWysiwyg, flag);
  EnableMenuItem(menu_, kMenuInsertLink, flag);
  EnableMenuItem(menu_, kMenuInsertCode, flag);
  EnableMenuItem(menu_, kMenuInsertImage, flag);
  EnableMenuItem(menu_, kMenuHeading1, flag);
  EnableMenuItem(menu_, kMenuHeading2, flag);
  EnableMenuItem(menu_, kMenuHeading3, flag);
  EnableMenuItem(menu_, kMenuBold, flag);
  EnableMenuItem(menu_, kMenuItalic, flag);
  EnableMenuItem(menu_, kMenuStrike, flag);
  EnableMenuItem(menu_, kMenuBullet, flag);
  EnableMenuItem(menu_, kMenuQuote, flag);
  EnableMenuItem(menu_, kMenuHRule, flag);
}

void NotesWindow::SetSaveEncoding(TextFileEncoding enc) {
  if (text_enc_ == enc) {
    ApplyViewMenuChecks();
    return;
  }
  text_enc_ = enc;
  MarkDirty();
  ApplyViewMenuChecks();
  UpdateStatusBar();
}

void NotesWindow::CopyPathToClipboard(const std::wstring& path) {
  if (path.empty() || !hwnd_) {
    return;
  }
  if (!OpenClipboard(hwnd_)) {
    return;
  }
  EmptyClipboard();
  const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (mem) {
    void* p = GlobalLock(mem);
    if (p) {
      memcpy(p, path.c_str(), bytes);
      GlobalUnlock(mem);
      SetClipboardData(CF_UNICODETEXT, mem);
    } else {
      GlobalFree(mem);
    }
  }
  CloseClipboard();
}

void NotesWindow::PersistViewSettings() {
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_ui.json");
  std::ofstream out(file, std::ios::binary);
  if (!out) {
    return;
  }
  out << "{\n"
      << "  \"sidebar\": " << (sidebar_visible_ ? "true" : "false") << ",\n"
      << "  \"outline\": " << (outline_ ? "true" : "false") << ",\n"
      << "  \"sync_scroll\": " << (sync_scroll_ ? "true" : "false") << ",\n"
      << "  \"word_wrap\": " << (word_wrap_ ? "true" : "false") << ",\n"
      << "  \"typewriter\": " << (typewriter_ ? "true" : "false") << ",\n"
      << "  \"preview\": " << (preview_ ? "true" : "false") << ",\n"
      << "  \"always_on_top\": " << (always_on_top_ ? "true" : "false") << ",\n"
      << "  \"font_pt\": " << font_pt_ << "\n"
      << "}\n";
}

void NotesWindow::LoadViewSettings() {
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_ui.json");
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  auto flag = [&](const char* key, bool* dest) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t pos = json.find(needle);
    if (pos == std::string::npos) {
      return;
    }
    const size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) {
      return;
    }
    const size_t t = json.find("true", colon);
    const size_t f = json.find("false", colon);
    if (t != std::string::npos && (f == std::string::npos || t < f)) {
      *dest = true;
    } else if (f != std::string::npos) {
      *dest = false;
    }
  };
  flag("sidebar", &sidebar_visible_);
  flag("outline", &outline_);
  flag("sync_scroll", &sync_scroll_);
  flag("word_wrap", &word_wrap_);
  flag("typewriter", &typewriter_);
  flag("preview", &preview_);
  flag("always_on_top", &always_on_top_);
  {
    const std::string needle = "\"font_pt\"";
    const size_t pos = json.find(needle);
    if (pos != std::string::npos) {
      const size_t colon = json.find(':', pos + needle.size());
      if (colon != std::string::npos) {
        const int pt = atoi(json.c_str() + colon + 1);
        if (pt >= 8 && pt <= 48) {
          font_pt_ = pt;
        }
      }
    }
  }
}

bool NotesWindow::SmartPaste() {
  if (PasteImageToMarkdown()) {
    return true;
  }
  if (!PathLooksMarkdown() || !edit_) {
    return false;
  }
  const UINT fmt = RegisterClipboardFormatW(L"HTML Format");
  if (!fmt || !IsClipboardFormatAvailable(fmt) || !OpenClipboard(hwnd_)) {
    return false;
  }
  HANDLE data = GetClipboardData(fmt);
  std::string html;
  if (data) {
    const char* text = static_cast<const char*>(GlobalLock(data));
    if (text) {
      html = text;
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  if (html.empty()) {
    return false;
  }
  // CF_HTML often has a header before the fragment; HtmlToMarkdown tolerates
  // extra chrome, but prefer StartFragment when present.
  const size_t frag = html.find("<!--StartFragment-->");
  if (frag != std::string::npos) {
    const size_t end = html.find("<!--EndFragment-->", frag);
    if (end != std::string::npos) {
      html = html.substr(frag + 20, end - (frag + 20));
    }
  }
  const std::wstring md = HtmlToMarkdown(WidenUtf8(html));
  if (md.empty()) {
    return false;
  }
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(md.c_str()));
  MarkDirty();
  return true;
}

void NotesWindow::Show() {
  if (!hwnd_) {
    return;
  }
  if (toast_) {
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    AttachOverlayToDesktop(hwnd_, true);
    PlaceOverlayInDesktopBand(hwnd_, true);
    RaiseOverlayInDesktopBand(hwnd_);
    return;
  }
  DetachOverlayFromDesktop(hwnd_);
  ShowWindow(hwnd_, SW_SHOW);
}

void NotesWindow::OpenPaths(const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    Present();
    return;
  }
  Present();
  for (const auto& p : paths) {
    OpenOrActivatePath(p);
  }
}

void NotesWindow::OpenOrActivatePath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  const std::wstring norm = NormalizePathKey(path);
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(), norm.c_str()) == 0) {
      SwitchToTab(static_cast<int>(i), true);
      return;
    }
  }
  if (tab_paths_.size() >= 12) {
    MistAlert(hwnd_, L"DeskNote", L"最多 12 个标签。", MistMsgTone::Info);
    return;
  }
  Save();
  tab_paths_.push_back(norm);
  EnsureTabStates();
  RebuildTabControl();
  SwitchToTab(static_cast<int>(tab_paths_.size()) - 1, false);
  if (sidebar_visible_) {
    HighlightSidebarPath(path_);
  }
}

void NotesWindow::Hide() {
  Save();
  PersistTabs();
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

std::wstring NotesWindow::NormalizePathKey(const std::wstring& path) {
  if (path.empty()) {
    return path;
  }
  wchar_t buf[MAX_PATH * 4] = {};
  constexpr DWORD kBuf = static_cast<DWORD>(MAX_PATH * 4);
  const DWORD n = GetFullPathNameW(path.c_str(), kBuf, buf, nullptr);
  if (n == 0 || n >= kBuf) {
    return path;
  }
  return buf;
}

std::wstring NotesWindow::TabLabel(const std::wstring& path) const {
  const size_t slash = path.find_last_of(L"\\/");
  std::wstring name =
      slash == std::wstring::npos ? path : path.substr(slash + 1);
  if (name.empty()) {
    name = L"未命名";
  }
  // Prefer stem for cleaner tabs (notes.md → notes).
  const size_t dot = name.find_last_of(L'.');
  if (dot != std::wstring::npos && dot > 0) {
    name = name.substr(0, dot);
  }
  if (name.size() > 22) {
    name = name.substr(0, 20) + L"…";
  }
  if (dirty_ && !path_.empty() &&
      _wcsicmp(NormalizePathKey(path).c_str(),
               NormalizePathKey(path_).c_str()) == 0) {
    name += L" ·";
  }
  return name;
}

namespace {
void GpAddRoundRect(Gdiplus::GraphicsPath* path, float x, float y, float w,
                    float h, float radius) {
  if (!path || w <= 0.f || h <= 0.f) {
    return;
  }
  const float r = (std::min)(radius, (std::min)(w, h) * 0.5f);
  const float d = r * 2.f;
  path->AddArc(x, y, d, d, 180.f, 90.f);
  path->AddArc(x + w - d, y, d, d, 270.f, 90.f);
  path->AddArc(x + w - d, y + h - d, d, d, 0.f, 90.f);
  path->AddArc(x, y + h - d, d, d, 90.f, 90.f);
  path->CloseFigure();
}
}  // namespace

void NotesWindow::RelayoutTabChips() {
  tab_chip_rects_.clear();
  tab_new_btn_rect_ = {};
  tab_chips_dirty_ = false;
  if (!tabs_) {
    return;
  }
  HDC hdc = GetDC(tabs_);
  HFONT old = nullptr;
  if (ui_font_) {
    old = static_cast<HFONT>(SelectObject(hdc, ui_font_));
  }
  // Compact chips — same size as menubar chrome.
  RECT rc{};
  GetClientRect(tabs_, &rc);
  int x = 12;
  const int h = 26;
  const int y = (std::max)(6, static_cast<int>((rc.bottom - h) / 2));
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    const std::wstring label = TabLabel(tab_paths_[i]);
    const bool pinned = PathIsPinned(tab_paths_[i]);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()),
                          &sz);
    // Left pad + optional pin glyph + label + close hit plate (match PaintTabBar).
    const int lead = pinned ? 28 : 14;
    const int close_w = 28;
    int w = sz.cx + lead + close_w;
    if (w < 100) {
      w = 100;
    }
    if (w > 220) {
      w = 220;
    }
    tab_chip_rects_.push_back(RECT{x, y, x + w, y + h});
    x += w + 4;
  }
  tab_new_btn_rect_ = RECT{x + 4, y + 2, x + 30, y + h - 2};
  if (old) {
    SelectObject(hdc, old);
  }
  ReleaseDC(tabs_, hdc);
}

void NotesWindow::PaintTabBar(HDC hdc) {
  if (!tabs_ || !hdc) {
    return;
  }
  // Measure chips only when labels/count/size change — not every WM_PAINT.
  if (tab_chips_dirty_ || tab_chip_rects_.size() != tab_paths_.size()) {
    RelayoutTabChips();
  }
  RECT rc{};
  GetClientRect(tabs_, &rc);
  const COLORREF desk = toast_ ? RGB(255, 248, 220) : content_bg_;
  HBRUSH desk_br = CreateSolidBrush(desk);
  FillRect(hdc, &rc, desk_br);
  DeleteObject(desk_br);

  // Soft separator — rail gray, not heavy black.
  HPEN pen = CreatePen(PS_SOLID, 1, border_color_);
  HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
  MoveToEx(hdc, 0, rc.bottom - 1, nullptr);
  LineTo(hdc, rc.right, rc.bottom - 1);
  SelectObject(hdc, old_pen);
  DeleteObject(pen);

  SetBkMode(hdc, TRANSPARENT);
  Gdiplus::Graphics g(hdc);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
  g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

  for (size_t i = 0; i < tab_chip_rects_.size(); ++i) {
    const RECT& chip = tab_chip_rects_[i];
    const bool on = static_cast<int>(i) == tab_index_;
    const bool hover = tab_hover_index_ == static_cast<int>(i);
    const std::wstring& path_s = tab_paths_[i];
    const bool pinned = PathIsPinned(path_s);
    const float x = static_cast<float>(chip.left);
    const float y = static_cast<float>(chip.top);
    const float w = static_cast<float>(chip.right - chip.left);
    const float h = static_cast<float>(chip.bottom - chip.top);

    {
      Gdiplus::GraphicsPath path;
      GpAddRoundRect(&path, x + 1.f, y + 1.f, w - 2.f, h - 2.f, 9.f);
      if (on) {
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 236, 238, 242));
        g.FillPath(&fill, &path);
        Gdiplus::Pen ring(Gdiplus::Color(255, 214, 219, 226), 1.f);
        g.DrawPath(&ring, &path);
      } else if (hover) {
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 245, 246, 248));
        g.FillPath(&fill, &path);
      }
    }

    HFONT use = on && ui_font_bold_ ? ui_font_bold_ : ui_font_;
    HFONT old_f =
        use ? static_cast<HFONT>(SelectObject(hdc, use)) : nullptr;
    SetTextColor(hdc, on ? text_color_ : muted_color_);

    int text_left = chip.left + 12;
    if (pinned) {
      // ★ matches sidebar pin mark — clearer than a 6px dot at compact widths.
      RECT pin_rc{chip.left + 8, chip.top, chip.left + 26, chip.bottom};
      SetTextColor(hdc, RGB(100, 116, 139));
      DrawTextW(hdc, L"★", -1, &pin_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      text_left = chip.left + 26;
      SetTextColor(hdc, on ? text_color_ : muted_color_);
    }

    std::wstring label = TabLabel(path_s);
    RECT text_rc{text_left, chip.top, chip.right - 28, chip.bottom};
    DrawTextW(hdc, label.c_str(), -1, &text_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Close always visible (faint); hot plate on hover.
    const bool close_hot = hover && tab_hover_zone_ == 2;
    RECT close_rc{chip.right - 26, chip.top + 6, chip.right - 6,
                  chip.bottom - 6};
    if (close_hot) {
      Gdiplus::GraphicsPath cp;
      GpAddRoundRect(&cp, static_cast<float>(close_rc.left),
                     static_cast<float>(close_rc.top),
                     static_cast<float>(close_rc.right - close_rc.left),
                     static_cast<float>(close_rc.bottom - close_rc.top), 6.f);
      Gdiplus::SolidBrush cbg(Gdiplus::Color(255, 220, 225, 232));
      g.FillPath(&cbg, &cp);
    }
    SetTextColor(hdc, close_hot ? text_color_
                                : (on ? RGB(120, 128, 140) : RGB(168, 174, 184)));
    DrawTextW(hdc, L"×", -1, &close_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (old_f) {
      SelectObject(hdc, old_f);
    }
  }

  if (tab_new_btn_rect_.right > tab_new_btn_rect_.left) {
    const RECT& nb = tab_new_btn_rect_;
    const bool hot = tab_hover_index_ == -2;
    Gdiplus::GraphicsPath np;
    GpAddRoundRect(&np, static_cast<float>(nb.left),
                   static_cast<float>(nb.top),
                   static_cast<float>(nb.right - nb.left),
                   static_cast<float>(nb.bottom - nb.top), 6.f);
    Gdiplus::SolidBrush nfill(Gdiplus::Color(
        255, hot ? 236 : 245, hot ? 238 : 246, hot ? 242 : 248));
    g.FillPath(&nfill, &np);
    Gdiplus::Pen nring(Gdiplus::Color(255, 220, 225, 232), 1.f);
    g.DrawPath(&nring, &np);
    if (ui_font_) {
      SelectObject(hdc, ui_font_);
    }
    SetTextColor(hdc, hot ? text_color_ : muted_color_);
    RECT tr = nb;
    DrawTextW(hdc, L"+", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
}

int NotesWindow::HitTestTabBar(POINT client, int* out_zone) const {
  if (out_zone) {
    *out_zone = 0;
  }
  if (tab_new_btn_rect_.right > tab_new_btn_rect_.left &&
      PtInRect(&tab_new_btn_rect_, client)) {
    if (out_zone) {
      *out_zone = 3;
    }
    return -1;
  }
  for (size_t i = 0; i < tab_chip_rects_.size(); ++i) {
    const RECT& chip = tab_chip_rects_[i];
    if (!PtInRect(&chip, client)) {
      continue;
    }
    if (out_zone) {
      const bool pinned =
          i < tab_paths_.size() && PathIsPinned(tab_paths_[i]);
      if (pinned && client.x <= chip.left + 26) {
        *out_zone = 1;  // pin
      } else if (client.x >= chip.right - 28) {
        *out_zone = 2;  // close
      } else {
        *out_zone = 0;
      }
    }
    return static_cast<int>(i);
  }
  return -1;
}

void NotesWindow::RebuildTabControl() {
  if (tab_paths_.empty()) {
    tab_index_ = 0;
  } else if (tab_index_ < 0 ||
             tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
  }
  if (tabs_) {
    RelayoutTabChips();
    InvalidateRect(tabs_, nullptr, FALSE);
  }
}

void NotesWindow::SchedulePersistTabs() {
  if (!hwnd_) {
    PersistTabs();
    return;
  }
  SetTimer(hwnd_, kPersistTabsTimer, 400, nullptr);
}

void NotesWindow::PersistTabs() {
  if (hwnd_) {
    KillTimer(hwnd_, kPersistTabsTimer);
  }
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_tabs.json");
  std::ofstream out(file, std::ios::binary);
  if (!out) {
    return;
  }
  out << "{\n  \"current\": " << tab_index_ << ",\n  \"tabs\": [\n";
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    out << "    \"" << EscapeJson(NarrowUtf8(tab_paths_[i])) << "\"";
    if (i + 1 < tab_paths_.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n  \"pinned\": [\n";
  for (size_t i = 0; i < pinned_paths_.size(); ++i) {
    out << "    \"" << EscapeJson(NarrowUtf8(pinned_paths_[i])) << "\"";
    if (i + 1 < pinned_paths_.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
}

void NotesWindow::LoadTabSession() {
  tab_paths_.clear();
  pinned_paths_.clear();
  tab_index_ = 0;
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_tabs.json");
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  auto collect_paths = [&](const char* key, std::vector<std::wstring>* out) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(needle);
    if (key_pos == std::string::npos) {
      return;
    }
    const size_t arr = json.find('[', key_pos);
    if (arr == std::string::npos) {
      return;
    }
    size_t pos = arr + 1;
    while (pos < json.size() && json[pos] != ']') {
      const size_t q1 = json.find('"', pos);
      if (q1 == std::string::npos || q1 > json.find(']', pos)) {
        break;
      }
      const size_t q2 = json.find('"', q1 + 1);
      if (q2 == std::string::npos) {
        break;
      }
      const std::string raw = json.substr(q1 + 1, q2 - q1 - 1);
      pos = q2 + 1;
      std::string unescaped;
      for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
          unescaped.push_back(raw[++i]);
        } else {
          unescaped.push_back(raw[i]);
        }
      }
      if (unescaped.size() >= 3 &&
          ((unescaped[1] == ':' &&
            (unescaped[2] == '\\' || unescaped[2] == '/')) ||
           unescaped[0] == '\\')) {
        out->push_back(WidenUtf8(unescaped));
      }
    }
  };
  collect_paths("tabs", &tab_paths_);
  collect_paths("pinned", &pinned_paths_);
  const size_t cur_key = json.find("\"current\"");
  if (cur_key != std::string::npos) {
    const size_t colon = json.find(':', cur_key);
    if (colon != std::string::npos) {
      tab_index_ = atoi(json.c_str() + colon + 1);
    }
  }
  // Drop session paths that no longer exist on disk.
  {
    std::vector<std::wstring> alive;
    alive.reserve(tab_paths_.size());
    for (const auto& p : tab_paths_) {
      if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
        alive.push_back(p);
      }
    }
    tab_paths_ = std::move(alive);
  }
  // Match Python _restore_session: merge recent library notes so files saved
  // outside the last tab list still reopen (capped at NewTab max of 12).
  constexpr int kMaxTabs = 12;
  if (static_cast<int>(tab_paths_.size()) < kMaxTabs) {
    const std::wstring dir = NotesLibraryDir(false);
    struct Recent {
      std::wstring path;
      ULONGLONG mtime = 0;
    };
    std::vector<Recent> recent;
    if (!dir.empty()) {
      WIN32_FIND_DATAW fd{};
      HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
      if (h != INVALID_HANDLE_VALUE) {
        do {
          if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
          }
          const std::wstring name = fd.cFileName;
          if (name.empty() || name[0] == L'.' ||
              name.rfind(L"_sess_", 0) == 0) {
            continue;
          }
          if (!IsOpenablePath(name)) {
            continue;
          }
          Recent r;
          r.path = dir + L"\\" + name;
          ULARGE_INTEGER u{};
          u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
          u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
          r.mtime = u.QuadPart;
          recent.push_back(std::move(r));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
      }
    }
    std::sort(recent.begin(), recent.end(),
              [](const Recent& a, const Recent& b) { return a.mtime > b.mtime; });
    auto already = [&](const std::wstring& p) {
      const std::wstring key = NormalizePathKey(p);
      for (const auto& t : tab_paths_) {
        if (_wcsicmp(NormalizePathKey(t).c_str(), key.c_str()) == 0) {
          return true;
        }
      }
      return false;
    };
    for (const auto& r : recent) {
      if (static_cast<int>(tab_paths_.size()) >= kMaxTabs) {
        break;
      }
      if (!already(r.path)) {
        tab_paths_.push_back(r.path);
      }
    }
  }
  if (tab_paths_.empty()) {
    tab_index_ = 0;
  } else if (tab_index_ < 0 ||
             tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = 0;
  }
}

void NotesWindow::SwitchToTab(int index, bool save_current) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  if (save_current) {
    // Dirty WYSIWYG: authoritative pull flushes JS debounce into cache.
    Save(wysiwyg_ && dirty_);
  }
  SyncFromWysiwyg();
  // Remember the outgoing tab's WYSIWYG / wrap / caret / scroll (M8).
  EnsureTabStates();
  if (tab_index_ >= 0 && tab_index_ < static_cast<int>(tab_states_.size())) {
    tab_states_[static_cast<size_t>(tab_index_)].wysiwyg = wysiwyg_;
    tab_states_[static_cast<size_t>(tab_index_)].wrap = word_wrap_;
    if (edit_) {
      CHARRANGE cr{};
      SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
      tab_states_[static_cast<size_t>(tab_index_)].caret = cr.cpMin;
      tab_states_[static_cast<size_t>(tab_index_)].first_visible_line =
          static_cast<int>(
              SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0));
    }
  }
  tab_index_ = index;
  path_ = tab_paths_[static_cast<size_t>(index)];
  if (tabs_) {
    InvalidateRect(tabs_, nullptr, FALSE);
  }
  Load();
  EnsureTabStates();
  wysiwyg_ = tab_states_[static_cast<size_t>(tab_index_)].wysiwyg;
  word_wrap_ = tab_states_[static_cast<size_t>(tab_index_)].wrap;
  ApplyWordWrap();
  if (menu_) {
    CheckMenuItem(menu_, kMenuWrap,
                  MF_BYCOMMAND | (word_wrap_ ? MF_CHECKED : MF_UNCHECKED));
  }
  ApplyWysiwygVisibility();
  if (wysiwyg_) {
    const std::wstring plain = CurrentPlainText();
    // Skip full Toast re-push when host cache already matches loaded text.
    if (web_.CachedMarkdown() != plain) {
      web_.SetMarkdown(plain);
    }
  } else if (edit_) {
    const int vis =
        tab_states_[static_cast<size_t>(tab_index_)].first_visible_line;
    if (vis > 0) {
      const int cur = static_cast<int>(
          SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0));
      SendMessageW(edit_, EM_LINESCROLL, 0, vis - cur);
    }
    const LONG caret = tab_states_[static_cast<size_t>(tab_index_)].caret;
    if (caret > 0) {
      CHARRANGE cr{caret, caret};
      SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
      SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    }
  }
  // Tab switch / new note: always land in edit (Typora/Obsidian write-first).
  // Preview is opt-in per document via the right strip — do not inherit it.
  if (preview_) {
    preview_ = false;
    if (menu_) {
      CheckMenuItem(menu_, kMenuPreviewPane, MF_BYCOMMAND | MF_UNCHECKED);
    }
    if (preview_web_.Host()) {
      preview_web_.Hide();
    }
    PersistViewSettings();
  }
  if (!wysiwyg_ && edit_) {
    ShowWindow(edit_, SW_SHOW);
  }
  RefreshTitle();
  if (outline_) {
    // Always arm — list may be hidden under WYSIWYG; rebuild before it shows.
    SetTimer(hwnd_, kOutlineTimer, 120, nullptr);
  }
  if (sidebar_visible_) {
    HighlightSidebarPath(path_);
  }
  SchedulePersistTabs();
  UpdateStatusBar();
  UpdateModeStrip();
  SyncMdMenuState();
  ApplyViewMenuChecks();
  LayoutClient();
  if (!wysiwyg_ && edit_ && IsWindowVisible(edit_)) {
    SetFocus(edit_);
  }
}

std::wstring NotesWindow::MakeNewNotePath() {
  // Match Python new_markdown_in_library: 未命名.md / 未命名_01.md …
  const std::wstring dir = NotesLibraryDir(true);
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring path = JoinPath(dir, L"未命名.md");
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return path;
  }
  for (int n = 1; n < 1000; ++n) {
    wchar_t name[64];
    swprintf_s(name, L"未命名_%02d.md", n);
    path = JoinPath(dir, name);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return path;
    }
  }
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t fallback[64];
  swprintf_s(fallback, L"未命名_%04d%02d%02d_%02d%02d%02d.md", st.wYear,
             st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  return JoinPath(dir, fallback);
}

void NotesWindow::NewTab() {
  if (tab_paths_.size() >= 12) {
    MistAlert(hwnd_, L"DeskNote", L"最多 12 个标签。", MistMsgTone::Info);
    return;
  }
  Save();
  const std::wstring path = MakeNewNotePath();
  {
    std::ofstream out(path, std::ios::binary);
    out << u8"# \n\n";
  }
  tab_paths_.push_back(path);
  EnsureTabStates();
  // New md drafts always start in source edit (not preview / not WYSIWYG).
  tab_states_.back() = TabState{};
  tab_states_.back().wrap = word_wrap_;
  RebuildTabControl();
  SwitchToTab(static_cast<int>(tab_paths_.size()) - 1, false);
  ShowEditMode();
  if (sidebar_visible_) {
    RebuildSidebar();
  }
}

void NotesWindow::CloseTabOnly() {
  // Silent autosave (Notepad++ / Fences-class: closing a tab does not discard).
  if (!path_.empty()) {
    Save(true);
  }
  EnsureTabStates();
  if (tab_paths_.size() <= 1) {
    // 关掉最后一个页签时开一份新草稿，避免零页签；原文件已保存仍在库里。
    const std::wstring path = MakeNewNotePath();
    {
      std::ofstream out(path, std::ios::binary);
      out << u8"# \n\n";
    }
    tab_paths_[0] = path;
    if (!tab_states_.empty()) {
      tab_states_[0] = TabState{};
    } else {
      tab_states_.assign(1, TabState{});
    }
    path_ = path;
    dirty_ = false;
    RebuildTabControl();
    SwitchToTab(0, false);
    PersistTabs();
    return;
  }
  tab_paths_.erase(tab_paths_.begin() + tab_index_);
  if (tab_index_ < static_cast<int>(tab_states_.size())) {
    tab_states_.erase(tab_states_.begin() + tab_index_);
  }
  if (tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
  }
  RebuildTabControl();
  SwitchToTab(tab_index_, false);
  PersistTabs();
}

void NotesWindow::CloseTabOnlyAt(int index) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  if (index != tab_index_) {
    SwitchToTab(index, true);
  }
  CloseTabOnly();
}

void NotesWindow::DeleteAndCloseTabAt(int index, bool ask_confirm) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  EnsureTabStates();
  const std::wstring path = tab_paths_[static_cast<size_t>(index)];
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring name =
      slash == std::wstring::npos ? path : path.substr(slash + 1);
  if (ask_confirm) {
    const std::wstring msg =
        L"确定删除「" + name + L"」？\n将移入回收站。";
    if (!MistConfirm(hwnd_, L"删除", msg.c_str(), MistMsgTone::Danger)) {
      return;
    }
  }
  if (!path.empty() &&
      GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    if (!RecyclePathQuiet(hwnd_, path)) {
      MistAlert(hwnd_, L"删除", L"无法删除。", MistMsgTone::Warn);
      return;
    }
  }
  for (size_t i = 0; i < pinned_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(pinned_paths_[i]).c_str(),
                 NormalizePathKey(path).c_str()) == 0) {
      pinned_paths_.erase(pinned_paths_.begin() +
                          static_cast<ptrdiff_t>(i));
      break;
    }
  }
  // 删文件即丢弃未保存改动，不再走保存确认。
  if (index == tab_index_) {
    dirty_ = false;
  }
  tab_paths_.erase(tab_paths_.begin() + index);
  if (index < static_cast<int>(tab_states_.size())) {
    tab_states_.erase(tab_states_.begin() + index);
  }
  if (tab_index_ > index) {
    --tab_index_;
  } else if (tab_index_ == index &&
             tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
  }
  if (tab_paths_.empty()) {
    const std::wstring fresh = MakeNewNotePath();
    {
      std::ofstream out(fresh, std::ios::binary);
      out << u8"# \n\n";
    }
    tab_paths_.push_back(fresh);
    tab_states_.assign(1, TabState{});
    tab_index_ = 0;
    path_ = fresh;
    dirty_ = false;
    RebuildTabControl();
    SwitchToTab(0, false);
  } else {
    if (tab_index_ < 0) {
      tab_index_ = 0;
    }
    RebuildTabControl();
    SwitchToTab(tab_index_, false);
  }
  RebuildSidebar();
  PersistTabs();
}

void NotesWindow::CloseTab() {
  DeleteAndCloseTabAt(tab_index_, true);
}

void NotesWindow::AdoptPath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  const std::wstring norm = NormalizePathKey(path);
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(), norm.c_str()) == 0) {
      SwitchToTab(static_cast<int>(i), true);
      return;
    }
  }
  Save();
  if (tab_paths_.empty()) {
    tab_paths_.push_back(norm);
  } else if (tab_index_ >= 0 &&
             tab_index_ < static_cast<int>(tab_paths_.size())) {
    tab_paths_[static_cast<size_t>(tab_index_)] = norm;
  } else {
    tab_paths_.push_back(norm);
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
  }
  path_ = norm;
  RebuildTabControl();
  Load();
  RefreshTitle();
  PersistTabs();
}

bool NotesWindow::IsOpen() const {
  return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
}

void NotesWindow::ReloadFromDisk() {
  if (path_.empty()) {
    return;
  }
  if (GetFileAttributesW(path_.c_str()) == INVALID_FILE_ATTRIBUTES) {
    MistAlert(hwnd_, L"重新加载", L"文件不存在或无法访问。", MistMsgTone::Warn);
    return;
  }
  if (dirty_) {
    if (!MistConfirm(hwnd_, L"重新加载",
                     L"当前有未保存的修改。重新加载将丢弃这些修改，继续？",
                     MistMsgTone::Warn)) {
      return;
    }
  }
  Load();
  RefreshTitle();
  if (preview_) {
    UpdatePreview();
  }
  if (outline_) {
    RebuildOutline();
  }
  UpdateStatusBar();
}

void NotesWindow::PromptGotoLine() {
  if (!edit_) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  const int cur =
      static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, cr.cpMin)) + 1;
  const int total =
      static_cast<int>(SendMessageW(edit_, EM_GETLINECOUNT, 0, 0));
  wchar_t hint[64];
  swprintf_s(hint, L"行号（1–%d）：", (std::max)(1, total));
  wchar_t initial[16];
  swprintf_s(initial, L"%d", (std::max)(1, cur));
  const std::wstring typed = PromptText(hwnd_, L"转到行", hint, initial);
  if (typed.empty()) {
    return;
  }
  int line = _wtoi(typed.c_str());
  if (line < 1) {
    line = 1;
  }
  if (total > 0 && line > total) {
    line = total;
  }
  GotoLine(line - 1);
}

void NotesWindow::Load() {
  if (!edit_) {
    return;
  }
  bookmarks_.clear();
  loading_ = true;
  dirty_ = false;
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    std::ifstream legacy(JoinPath(NativeDataDir(), L"notes.txt"), std::ios::binary);
    if (!legacy) {
      loading_ = false;
      UpdateStatusBar();
      RefreshGutter();
      return;
    }
    std::ostringstream ss;
    ss << legacy.rdbuf();
    SetWindowTextW(edit_, WidenUtf8(ss.str()).c_str());
    loading_ = false;
    UpdateStatusBar();
    RefreshGutter();
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string data = ss.str();
  if (data.rfind("{\\rtf", 0) == 0) {
    text_enc_ = TextFileEncoding::Utf8;
    StreamCookie cookie;
    cookie.data = data;
    EDITSTREAM es{};
    es.dwCookie = reinterpret_cast<DWORD_PTR>(&cookie);
    es.pfnCallback = StreamInCb;
    SendMessageW(edit_, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));
  } else {
    std::wstring wide;
    TextFileEncoding enc = TextFileEncoding::Utf8;
    if (!DecodeTextFileBytes(data, &wide, &enc)) {
      // Last resort: lossy UTF-8 so the user can still read; do not save
      // until they Save As (refuse overwrite of undecodable originals).
      wide = WidenUtf8(data);
      text_enc_ = TextFileEncoding::Utf8;
      SetWindowTextW(edit_, wide.c_str());
      loading_ = false;
      UpdateStatusBar();
      RefreshGutter();
      return;
    }
    text_enc_ = enc;
    if (wide.empty() && PathLooksMarkdown()) {
      wide = L"# \n\n";
    }
    SetWindowTextW(edit_, wide.c_str());
  }
  loading_ = false;
  dirty_ = false;
  UpdateStatusBar();
  RefreshGutter();
}

bool NotesWindow::OfferElevatedSave(const std::wstring& path,
                                    const std::wstring& text) {
  if (!MistConfirm(
          hwnd_, L"保存",
          (L"「" + path +
           L"」受系统保护，无法直接保存。\n\n"
           L"是否以管理员权限重启 DeskNote 并保存？\n"
           L"（将弹出 UAC 确认框，重启后自动保存该文件）")
              .c_str(),
          MistMsgTone::Warn)) {
    return false;
  }
  if (!WriteElevatedPending(path, text)) {
    MistAlert(hwnd_, L"笔记", L"无法准备提权保存的暂存内容。",
              MistMsgTone::Warn);
    return false;
  }
  ReleaseDeskNoteInstanceLock();
  if (!RelaunchDeskNoteElevated(path)) {
    ClearElevatedPending();
    if (!AcquireDeskNoteInstanceLock()) {
      LogLine(L"[elevated] mutex reacquire failed; exiting");
      dirty_ = false;
      if (quit_on_close_) {
        PostQuitMessage(0);
      } else {
        Destroy();
      }
      return false;
    }
    MistAlert(hwnd_, L"保存",
              L"已取消提权保存（UAC 未确认）。\n"
              L"可尝试以管理员身份运行 DeskNote 后重试。",
              MistMsgTone::Warn);
    return false;
  }
  dirty_ = false;
  if (quit_on_close_) {
    PostQuitMessage(0);
  } else {
    Destroy();
  }
  return true;
}

void NotesWindow::UpdateModeStrip() {
  // Visibility is owned exclusively by LayoutClient (show_mode_chip).
}

void NotesWindow::ShowPreviewMode() {
  if (!PathLooksMarkdown()) {
    return;
  }
  if (wysiwyg_) {
    ToggleWysiwyg();
  }
  if (!preview_web_.Available()) {
    preview_web_.SetKind(NotesWebView::Kind::kViewer);
    preview_web_.SetTheme(SystemAppsUseLightTheme() ? L"light" : L"dark");
    preview_web_.on_scroll = [this](double ratio) { SyncPreviewToEdit(ratio); };
    if (!preview_web_.Create(hwnd_)) {
      MistAlert(hwnd_, L"预览", L"无法启动预览（WebView2 不可用）。",
                MistMsgTone::Warn);
      return;
    }
  }
  const bool was = preview_;
  preview_ = true;
  if (menu_) {
    CheckMenuItem(menu_, kMenuPreviewPane, MF_BYCOMMAND | MF_CHECKED);
  }
  preview_web_.Show();
  UpdatePreview();
  UpdateModeStrip();
  LayoutClient();
  if (!was) {
    PersistViewSettings();
  }
}

void NotesWindow::ToggleMdViewMode() {
  EnsureMarkdownEditor(true);
}

void NotesWindow::ShowEditMode() {
  if (PathLooksMarkdown() && !toast_) {
    EnsureMarkdownEditor(true);
    return;
  }
  if (wysiwyg_) {
    SyncFromWysiwyg();
    wysiwyg_ = false;
    ApplyWysiwygVisibility();
  }
  if (preview_) {
    preview_ = false;
    if (menu_) {
      CheckMenuItem(menu_, kMenuPreviewPane, MF_BYCOMMAND | MF_UNCHECKED);
    }
    if (preview_web_.Host()) {
      preview_web_.Hide();
    }
    PersistViewSettings();
  }
  if (edit_) {
    ShowWindow(edit_, SW_SHOW);
    SetFocus(edit_);
  }
  RefreshTitle();
  UpdateModeStrip();
  LayoutClient();
}

void NotesWindow::Save(bool authoritative_pull) {
  if (wysiwyg_) {
    std::wstring from_toast;
    const bool need_pull =
        web_.Ready() &&
        (authoritative_pull || dirty_ || web_.CachedMarkdown().empty());
    if (need_pull) {
      // Pull reads live Toast markdown (ExecuteScript get) into cache.
      from_toast = web_.PullMarkdown(authoritative_pull ? 1500 : 300);
    }
    if (from_toast.empty()) {
      from_toast = web_.CachedMarkdown();
    }
    if (from_toast.empty()) {
      SyncFromWysiwyg();
    } else if (edit_) {
      SetWindowTextW(edit_, from_toast.c_str());
    }
  } else {
    SyncFromWysiwyg();
  }
  if (!edit_) {
    return;
  }
  if (path_.empty()) {
    SaveAs();
    return;
  }
  const std::wstring plain = CurrentPlainText();
  // Plain text for md/txt/code/hosts；.rtf 仍走 RTF（对标 Notepad++ 文本保存）。
  if (PathLooksTextish()) {
    // Soft saves without a Toast pull must not clobber a non-empty file with
    // empty bytes. Authoritative pulls (Ctrl+S / close) may legitimately clear.
    if (plain.empty() && !authoritative_pull) {
      WIN32_FILE_ATTRIBUTE_DATA fad{};
      if (GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fad)) {
        const ULONGLONG sz =
            (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (sz > 0) {
          return;
        }
      }
      if (dirty_ && wysiwyg_) {
        return;
      }
    }
    std::string bytes;
    if (!EncodeTextFileBytes(plain, text_enc_, &bytes)) {
      MistAlert(hwnd_, L"保存失败",
                L"无法按原编码写回文件（字符无法映射到 ANSI/GBK）。",
                MistMsgTone::Warn);
      return;
    }
    if (!WriteFileBytes(path_, bytes.data(), bytes.size())) {
      if (LastWriteWasAccessDenied() && OfferElevatedSave(path_, plain)) {
        return;
      }
      MistAlert(hwnd_, L"笔记", L"保存失败。", MistMsgTone::Warn);
      return;
    }
    dirty_ = false;
    RefreshTitle();
    RefreshCurrentTabLabel();
    UpdateStatusBar();
    return;
  }
  StreamCookie cookie;
  EDITSTREAM es{};
  es.dwCookie = reinterpret_cast<DWORD_PTR>(&cookie);
  es.pfnCallback = StreamOutCb;
  const LRESULT streamed =
      SendMessageW(edit_, EM_STREAMOUT, SF_RTF, reinterpret_cast<LPARAM>(&es));
  std::string payload;
  if (streamed && !cookie.data.empty()) {
    payload = cookie.data;
  } else {
    payload = NarrowUtf8(plain);
  }
  if (!WriteFileBytes(path_, payload.data(), payload.size())) {
    if (LastWriteWasAccessDenied() && OfferElevatedSave(path_, plain)) {
      return;
    }
    MistAlert(hwnd_, L"笔记", L"保存失败。", MistMsgTone::Warn);
    return;
  }
  dirty_ = false;
  RefreshTitle();
  RefreshCurrentTabLabel();
  UpdateStatusBar();
}

void NotesWindow::MarkDirty() {
  if (loading_) {
    return;
  }
  // Already dirty: skip title/tab/status churn on every Toast change post.
  if (dirty_) {
    return;
  }
  dirty_ = true;
  RefreshTitle();
  RefreshCurrentTabLabel();
  UpdateStatusBar();
}

void NotesWindow::ToggleWordWrap() {
  word_wrap_ = !word_wrap_;
  EnsureTabStates();
  if (tab_index_ >= 0 && tab_index_ < static_cast<int>(tab_states_.size())) {
    tab_states_[static_cast<size_t>(tab_index_)].wrap = word_wrap_;
  }
  ApplyWordWrap();
  if (menu_) {
    CheckMenuItem(menu_, kMenuWrap,
                  MF_BYCOMMAND | (word_wrap_ ? MF_CHECKED : MF_UNCHECKED));
  }
  PersistViewSettings();
}

void NotesWindow::ApplyWordWrap() {
  if (!edit_) {
    return;
  }
  // RichEdit: target width 0 wraps to window; large width disables wrap.
  if (word_wrap_) {
    SendMessageW(edit_, EM_SETTARGETDEVICE, 0, 0);
    LONG style = GetWindowLongW(edit_, GWL_STYLE);
    style &= ~WS_HSCROLL;
    SetWindowLongW(edit_, GWL_STYLE, style);
  } else {
    SendMessageW(edit_, EM_SETTARGETDEVICE, 0, 0x7fffffff);
    LONG style = GetWindowLongW(edit_, GWL_STYLE);
    style |= WS_HSCROLL;
    SetWindowLongW(edit_, GWL_STYLE, style);
  }
  SetWindowPos(edit_, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  InvalidateRect(edit_, nullptr, TRUE);
}

void NotesWindow::UpdateStatusBar() {
  if (!status_) {
    return;
  }
  int line = 1;
  int col = 1;
  int chars = 0;
  int words = 0;
  std::wstring scope = L"全文";
  std::wstring stats_src;
  if (wysiwyg_) {
    // RichEdit is hidden/stale in Toast mode — count from host markdown cache.
    stats_src = web_.CachedMarkdown();
  } else if (edit_) {
    CHARRANGE cr{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    const LONG caret = cr.cpMax;
    line = static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, caret)) + 1;
    const LONG line_start =
        static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, line - 1, 0));
    col = static_cast<int>(caret - line_start) + 1;
    if (cr.cpMin != cr.cpMax) {
      scope = L"选区";
      const int sel_len = static_cast<int>(cr.cpMax - cr.cpMin);
      std::wstring buf(static_cast<size_t>(sel_len) + 1, L'\0');
      TEXTRANGEW tr{};
      tr.chrg = cr;
      tr.lpstrText = buf.data();
      SendMessageW(edit_, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&tr));
      buf.resize(static_cast<size_t>(sel_len));
      stats_src = std::move(buf);
    } else {
      stats_src = CurrentPlainText();
    }
  }
  if (!stats_src.empty() || wysiwyg_ || edit_) {
    const size_t h = std::hash<std::wstring>{}(stats_src);
    if (h == status_stats_hash_ && stats_src.size() == status_stats_len_) {
      chars = status_chars_;
      words = status_words_;
    } else {
      CountTextStats(stats_src, &chars, &words);
      status_stats_hash_ = h;
      status_stats_len_ = stats_src.size();
      status_chars_ = chars;
      status_words_ = words;
    }
  }
  const wchar_t* state = dirty_ ? L"已修改" : L"已保存";
  const wchar_t* enc = L"UTF-8";
  switch (text_enc_) {
    case TextFileEncoding::Utf8Bom:
      enc = L"UTF-8 BOM";
      break;
    case TextFileEncoding::Acp:
      enc = L"ANSI";
      break;
    case TextFileEncoding::Utf16Le:
      enc = L"UTF-16 LE";
      break;
    case TextFileEncoding::Utf16Be:
      enc = L"UTF-16 BE";
      break;
    case TextFileEncoding::Utf8:
    default:
      enc = L"UTF-8";
      break;
  }
  const std::wstring path_part = path_.empty() ? L"未命名" : path_;
  wchar_t line_buf[640]{};
  swprintf_s(line_buf,
             L"%s  ·  %s  ·  %s  |  行 %d, 列 %d  ·  %s %d 字 / %d 词",
             path_part.c_str(), state, enc, line, col, scope.c_str(), chars,
             words);
  wchar_t prev[640] = {};
  GetWindowTextW(status_, prev, 640);
  if (wcscmp(prev, line_buf) != 0) {
    SetWindowTextW(status_, line_buf);
  }
}

int NotesWindow::TabIndexFromPoint(POINT client) const {
  return HitTestTabBar(client, nullptr);
}

void NotesWindow::CloseTabAt(int index) {
  CloseTabOnlyAt(index);
}

void NotesWindow::CloseOtherTabs(int keep_index) {
  if (keep_index < 0 ||
      keep_index >= static_cast<int>(tab_paths_.size()) ||
      tab_paths_.size() <= 1) {
    return;
  }
  // Autosave active tab; other tabs were saved on last switch.
  Save();
  EnsureTabStates();
  const std::wstring keep = tab_paths_[static_cast<size_t>(keep_index)];
  TabState st{};
  if (keep_index < static_cast<int>(tab_states_.size())) {
    st = tab_states_[static_cast<size_t>(keep_index)];
  }
  tab_paths_.assign(1, keep);
  tab_states_.assign(1, st);
  tab_index_ = 0;
  path_ = keep;
  RebuildTabControl();
  SwitchToTab(0, false);
  PersistTabs();
}

void NotesWindow::CloseTabsToTheRight(int index) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size()) - 1) {
    return;
  }
  Save();
  EnsureTabStates();
  tab_paths_.erase(tab_paths_.begin() + index + 1, tab_paths_.end());
  if (static_cast<int>(tab_states_.size()) > index + 1) {
    tab_states_.erase(tab_states_.begin() + index + 1, tab_states_.end());
  }
  if (tab_index_ > index) {
    tab_index_ = index;
    path_ = tab_paths_[static_cast<size_t>(index)];
  }
  RebuildTabControl();
  SwitchToTab(tab_index_, false);
  PersistTabs();
}

void NotesWindow::InsertMdStrike() {
  EnsureSourceEditForFormat();
  if (PathLooksMarkdown()) {
    WrapSelectionMarkdown(L"~~", L"~~");
  }
}

void NotesWindow::InsertMdBullet() {
  EnsureSourceEditForFormat();
  if (!edit_ || !PathLooksMarkdown()) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  const int line =
      static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, cr.cpMin));
  const LONG start =
      static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, line, 0));
  CHARRANGE at{start, start};
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&at));
  SendMessageW(edit_, EM_REPLACESEL, TRUE,
               reinterpret_cast<LPARAM>(L"- "));
  MarkDirty();
}

void NotesWindow::InsertMdQuote() {
  EnsureSourceEditForFormat();
  if (!edit_ || !PathLooksMarkdown()) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  const int line =
      static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, cr.cpMin));
  const LONG start =
      static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, line, 0));
  CHARRANGE at{start, start};
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&at));
  SendMessageW(edit_, EM_REPLACESEL, TRUE,
               reinterpret_cast<LPARAM>(L"> "));
  MarkDirty();
}

void NotesWindow::InsertMdHorizontalRule() {
  EnsureSourceEditForFormat();
  if (!edit_ || !PathLooksMarkdown()) {
    return;
  }
  SendMessageW(edit_, EM_REPLACESEL, TRUE,
               reinterpret_cast<LPARAM>(L"\r\n---\r\n"));
  MarkDirty();
}

void NotesWindow::InsertDateTime() {
  EnsureSourceEditForFormat();
  if (!edit_) {
    return;
  }
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t buf[64];
  swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(buf));
  MarkDirty();
}

bool NotesWindow::TryContinueMarkdownList() {
  if (!edit_ || !PathLooksMarkdown()) {
    return false;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  if (cr.cpMin != cr.cpMax) {
    return false;  // Selection: let default replace with newline.
  }
  const int line =
      static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, cr.cpMin));
  const LONG line_start =
      static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, line, 0));
  const int line_len =
      static_cast<int>(SendMessageW(edit_, EM_LINELENGTH, line_start, 0));
  if (line_len <= 0 || line_len > 512) {
    return false;
  }
  // EM_GETLINE: first WORD of buffer = capacity in TCHARs.
  std::wstring raw(static_cast<size_t>(line_len) + 2, L'\0');
  *reinterpret_cast<WORD*>(raw.data()) = static_cast<WORD>(line_len + 1);
  const int got = static_cast<int>(
      SendMessageW(edit_, EM_GETLINE, line, reinterpret_cast<LPARAM>(raw.data())));
  if (got <= 0) {
    return false;
  }
  raw.resize(static_cast<size_t>(got));
  // Trim CR/LF if present.
  while (!raw.empty() && (raw.back() == L'\r' || raw.back() == L'\n')) {
    raw.pop_back();
  }
  // Match leading indent + list marker: -, *, +, 1. / - [ ] / - [x]
  size_t i = 0;
  while (i < raw.size() && (raw[i] == L' ' || raw[i] == L'\t')) {
    ++i;
  }
  const std::wstring indent = raw.substr(0, i);
  std::wstring marker;
  if (i < raw.size() &&
      (raw[i] == L'-' || raw[i] == L'*' || raw[i] == L'+')) {
    marker.push_back(raw[i++]);
    if (i + 4 <= raw.size() && raw[i] == L' ' && raw[i + 1] == L'[' &&
        (raw[i + 2] == L' ' || raw[i + 2] == L'x' || raw[i + 2] == L'X') &&
        raw[i + 3] == L']') {
      marker += L" [ ]";  // New checkbox always unchecked.
      i += 4;
    }
    if (i < raw.size() && raw[i] == L' ') {
      ++i;
    } else if (marker.size() == 1) {
      return false;
    }
  } else if (i < raw.size() && raw[i] >= L'0' && raw[i] <= L'9') {
    size_t j = i;
    while (j < raw.size() && raw[j] >= L'0' && raw[j] <= L'9') {
      ++j;
    }
    if (j < raw.size() && raw[j] == L'.') {
      // Continue with next number.
      int n = 0;
      for (size_t k = i; k < j; ++k) {
        n = n * 10 + (raw[k] - L'0');
      }
      marker = std::to_wstring(n + 1) + L".";
      i = j + 1;
      if (i < raw.size() && raw[i] == L' ') {
        ++i;
      } else {
        return false;
      }
    } else {
      return false;
    }
  } else {
    return false;
  }
  const std::wstring content = raw.substr(i);
  // Empty item → exit list (delete marker on this line).
  bool only_ws = true;
  for (wchar_t ch : content) {
    if (ch != L' ' && ch != L'\t') {
      only_ws = false;
      break;
    }
  }
  if (only_ws) {
    CHARRANGE whole{line_start, cr.cpMin};
    SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&whole));
    SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    MarkDirty();
    return true;
  }
  std::wstring insert = L"\r\n";
  insert += indent;
  insert += marker;
  insert += L" ";
  SendMessageW(edit_, EM_REPLACESEL, TRUE,
               reinterpret_cast<LPARAM>(insert.c_str()));
  MarkDirty();
  return true;
}

void NotesWindow::HandleEditContextMenu(POINT screen_pt) {
  if (!edit_) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  const bool has_sel = cr.cpMin != cr.cpMax;
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, kEditCtxUndo, L"撤销\tCtrl+Z");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), kEditCtxCut,
              L"剪切\tCtrl+X");
  AppendMenuW(m, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), kEditCtxCopy,
              L"复制\tCtrl+C");
  AppendMenuW(m, MF_STRING, kEditCtxPaste, L"粘贴\tCtrl+V");
  AppendMenuW(m, MF_STRING, kEditCtxSelectAll, L"全选\tCtrl+A");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kEditCtxFind, L"查找…\tCtrl+F");
  AppendMenuW(m, MF_STRING, kEditCtxGoto, L"转到行…\tCtrl+G");
  const int cmd =
      TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, screen_pt.x, screen_pt.y,
                     0, hwnd_, nullptr);
  DestroyMenu(m);
  switch (cmd) {
    case kEditCtxUndo:
      SendMessageW(edit_, EM_UNDO, 0, 0);
      break;
    case kEditCtxCut:
      SendMessageW(edit_, WM_CUT, 0, 0);
      MarkDirty();
      break;
    case kEditCtxCopy:
      SendMessageW(edit_, WM_COPY, 0, 0);
      break;
    case kEditCtxPaste:
      if (!SmartPaste()) {
        SendMessageW(edit_, WM_PASTE, 0, 0);
      }
      break;
    case kEditCtxSelectAll:
      SendMessageW(edit_, EM_SETSEL, 0, -1);
      break;
    case kEditCtxFind:
      ShowFind(false);
      break;
    case kEditCtxGoto:
      PromptGotoLine();
      break;
    default:
      break;
  }
}

void NotesWindow::ReorderTab(int from, int to) {
  if (from < 0 || to < 0 || from == to ||
      from >= static_cast<int>(tab_paths_.size()) ||
      to >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  EnsureTabStates();
  const std::wstring path = tab_paths_[static_cast<size_t>(from)];
  tab_paths_.erase(tab_paths_.begin() + from);
  tab_paths_.insert(tab_paths_.begin() + to, path);
  if (from < static_cast<int>(tab_states_.size()) &&
      to <= static_cast<int>(tab_states_.size())) {
    TabState st = tab_states_[static_cast<size_t>(from)];
    tab_states_.erase(tab_states_.begin() + from);
    tab_states_.insert(tab_states_.begin() + to, st);
  }
  if (tab_index_ == from) {
    tab_index_ = to;
  } else if (from < tab_index_ && to >= tab_index_) {
    --tab_index_;
  } else if (from > tab_index_ && to <= tab_index_) {
    ++tab_index_;
  }
  RebuildTabControl();
  // Keep pinned tabs left after drag (Python _normalize_pin_order).
  ApplyPinnedTabOrder();
  PersistTabs();
}

void NotesWindow::HandleTabContextMenu(int index, POINT screen_pt) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  const std::wstring path = tab_paths_[static_cast<size_t>(index)];
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, kTabClose, L"关闭");
  AppendMenuW(m, MF_STRING, kTabCloseOthers, L"关闭其他页签");
  AppendMenuW(m, MF_STRING, kTabCloseRight, L"关闭右侧页签");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kTabRename, L"重命名");
  AppendMenuW(m, MF_STRING, kTabPin,
              PathIsPinned(path) ? L"取消置顶" : L"置顶");
  AppendMenuW(m, MF_STRING, kTabReveal, L"打开文件所在目录");
  AppendMenuW(m, MF_STRING, kTabCopyPath, L"复制路径");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kTabDelete, L"删除");
  const int cmd =
      TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, screen_pt.x, screen_pt.y,
                     0, hwnd_, nullptr);
  DestroyMenu(m);
  if (cmd == kTabClose) {
    CloseTabOnlyAt(index);
  } else if (cmd == kTabCloseOthers) {
    CloseOtherTabs(index);
  } else if (cmd == kTabCloseRight) {
    CloseTabsToTheRight(index);
  } else if (cmd == kTabPin) {
    TogglePinPath(path);
    RebuildTabControl();
  } else if (cmd == kTabReveal) {
    const std::wstring arg = L"/select,\"" + path + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", arg.c_str(), nullptr,
                  SW_SHOWNORMAL);
  } else if (cmd == kTabCopyPath) {
    CopyPathToClipboard(path);
  } else if (cmd == kTabRename) {
    std::wstring name = path;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      name = path.substr(slash + 1);
    }
    const size_t dot = name.find_last_of(L'.');
    const std::wstring stem =
        dot == std::wstring::npos ? name : name.substr(0, dot);
    const std::wstring new_stem =
        PromptText(hwnd_, L"重命名", L"新名称：", stem);
    if (!new_stem.empty()) {
      ApplySidebarRename(path, new_stem);
    }
  } else if (cmd == kTabDelete) {
    DeleteAndCloseTabAt(index, true);
  }
}

std::wstring NotesWindow::EditTextCpAligned() const {
  if (!edit_) {
    return {};
  }
  GETTEXTLENGTHEX tl{};
  tl.flags = GTL_DEFAULT | GTL_NUMCHARS;
  tl.codepage = 1200;
  const LRESULT n =
      SendMessageW(edit_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&tl), 0);
  if (n <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(n) + 1, L'\0');
  GETTEXTEX gt{};
  gt.cb = static_cast<DWORD>((n + 1) * sizeof(wchar_t));
  gt.flags = GT_DEFAULT;
  gt.codepage = 1200;
  SendMessageW(edit_, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&gt),
               reinterpret_cast<LPARAM>(wide.data()));
  wide.resize(static_cast<size_t>(n));
  return wide;
}

std::wstring NotesWindow::CurrentPlainText() const {
  // Toast is the live surface in WYSIWYG — RichEdit is only mirrored on
  // Save/Sync and stays stale across debounced keystrokes.
  if (wysiwyg_ && web_.Ready()) {
    return web_.CachedMarkdown();
  }
  if (!edit_) {
    return {};
  }
  const int len = GetWindowTextLengthW(edit_);
  std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(edit_, wide.data(), len + 1);
  wide.resize(static_cast<size_t>(len));
  return wide;
}

void NotesWindow::PreviewMarkdown() {
  Save();
  const bool is_md =
      path_.size() >= 3 &&
      (_wcsicmp(path_.c_str() + path_.size() - 3, L".md") == 0 ||
       _wcsicmp(path_.c_str() + path_.size() - 4, L".txt") == 0 ||
       (path_.size() >= 4 &&
        _wcsicmp(path_.c_str() + path_.size() - 4, L".markdown") == 0));
  if (is_md && path_.find(L".rtf") == std::wstring::npos) {
    if (PreviewMarkdownFile(hwnd_, path_)) {
      return;
    }
  }
  PreviewMarkdownText(hwnd_, CurrentPlainText(), L"便签预览", NoteBaseDir());
}

void NotesWindow::ExportHtml() {
  wchar_t file[MAX_PATH] = L"note.html";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter = L"HTML\0*.html\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_OVERWRITEPROMPT;
  ofn.lpstrDefExt = L"html";
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  // P1-21: full markdown body + P1-20 relative images via base_dir.
  const std::wstring title =
      path_.empty() ? L"DeskTidy" : TabLabel(path_);
  const std::wstring html =
      MarkdownToHtmlDocument(CurrentPlainText(), title, NoteBaseDir());
  std::ofstream out(file, std::ios::binary);
  if (!out) {
    return;
  }
  out << NarrowUtf8(html);
}

void NotesWindow::ExportMarkdown() {
  wchar_t file[MAX_PATH] = L"note.md";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter = L"Markdown\0*.md\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_OVERWRITEPROMPT;
  ofn.lpstrDefExt = L"md";
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  std::ofstream out(file, std::ios::binary);
  if (out) {
    out << NarrowUtf8(CurrentPlainText());
  }
}

void NotesWindow::ExportWithPandoc() {
  // Choose an output path first (M7: explicit path, verified result).
  wchar_t file[MAX_PATH] = L"note.docx";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter = L"Word 文档\0*.docx\0Markdown\0*.md\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_OVERWRITEPROMPT;
  ofn.lpstrDefExt = L"docx";
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  const std::wstring dst = file;

  wchar_t temp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  const std::wstring src = std::wstring(temp) + L"desktidy_note.md";
  {
    std::ofstream out(src, std::ios::binary);
    if (!out) {
      MistAlert(hwnd_, L"导出", L"无法写入临时文件。", MistMsgTone::Danger);
      return;
    }
    out << NarrowUtf8(CurrentPlainText());
  }

  // P1-20: --resource-path + cwd = note directory so relative images resolve
  // (Python pandoc_export resource_dir).
  const std::wstring resource_dir = NoteBaseDir();
  std::wstring cmd = L"pandoc \"" + src + L"\" -o \"" + dst + L"\"";
  if (!resource_dir.empty()) {
    cmd += L" --resource-path \"" + resource_dir + L"\"";
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const BOOL started = CreateProcessW(
      nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
      resource_dir.empty() ? nullptr : resource_dir.c_str(), &si, &pi);
  if (!started) {
    // pandoc missing → fall back to plain Markdown export (M7).
    std::wstring md_dst = dst;
    const bool had_docx =
        md_dst.size() >= 5 &&
        _wcsicmp(md_dst.c_str() + md_dst.size() - 5, L".docx") == 0;
    if (had_docx) {
      md_dst.resize(md_dst.size() - 5);
    }
    md_dst += L".md";
    if (MistConfirm(
            hwnd_, L"导出",
            (L"未找到 pandoc。是否改为导出纯 Markdown？\n\n目标：\n" + md_dst)
                .c_str(),
            MistMsgTone::Warn)) {
      if (CopyFileW(src.c_str(), md_dst.c_str(), FALSE)) {
        ShellExecuteW(nullptr, L"open", md_dst.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
      }
    }
    return;
  }
  WaitForSingleObject(pi.hProcess, 60000);
  DWORD exit = 1;
  GetExitCodeProcess(pi.hProcess, &exit);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  const DWORD attr = GetFileAttributesW(dst.c_str());
  if (exit != 0 || attr == INVALID_FILE_ATTRIBUTES ||
      (attr & FILE_ATTRIBUTE_DIRECTORY)) {
    MistAlert(hwnd_, L"导出", L"pandoc 导出失败，未打开任何文件。",
              MistMsgTone::Danger);
    return;
  }
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &fad);
  const ULONGLONG sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) |
                       fad.nFileSizeLow;
  if (sz == 0) {
    MistAlert(hwnd_, L"导出", L"pandoc 导出的文件为空，未打开。",
              MistMsgTone::Danger);
    return;
  }
  ShellExecuteW(nullptr, L"open", dst.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void NotesWindow::OpenWithShell() {
  Save();
  if (path_.empty()) {
    return;
  }
  const std::wstring cmd =
      L"rundll32.exe shell32.dll,OpenAs_RunDLL " + path_;
  ShellExecuteW(hwnd_, L"open", L"rundll32.exe",
                (L"shell32.dll,OpenAs_RunDLL \"" + path_ + L"\"").c_str(),
                nullptr, SW_SHOWNORMAL);
  (void)cmd;
}

void NotesWindow::PasteHtmlAsMarkdown() {
  const UINT fmt = RegisterClipboardFormatW(L"HTML Format");
  if (!fmt || !OpenClipboard(hwnd_)) {
    return;
  }
  HANDLE data = GetClipboardData(fmt);
  std::string html;
  if (data) {
    const char* text = static_cast<const char*>(GlobalLock(data));
    if (text) {
      html = text;
      GlobalUnlock(data);
    }
  }
  CloseClipboard();
  if (html.empty()) {
    MistAlert(hwnd_, L"便签", L"剪贴板里没有 HTML。", MistMsgTone::Info);
    return;
  }
  const std::wstring wide = HtmlToMarkdown(WidenUtf8(html));
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(wide.c_str()));
}

void NotesWindow::ToggleToast() {
  toast_ = !toast_;
  LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
  LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
  if (toast_) {
    // Yellow sticky strip (optional mode) — keep desktop-band overlay.
    ex |= WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST;
    ex &= ~WS_EX_APPWINDOW;
    style = (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_CAPTION |
            WS_SYSMENU | WS_THICKFRAME;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
    SetLayeredWindowAttributes(hwnd_, 0, 245, LWA_ALPHA);
    SetWindowPos(hwnd_, HWND_TOPMOST, 40, 40, 280, 140,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    AttachOverlayToDesktop(hwnd_, true);
    PlaceOverlayInDesktopBand(hwnd_, true);
  } else {
    ex &= ~(WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST);
    ex |= WS_EX_APPWINDOW;
    style = (style & ~(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME)) |
            WS_OVERLAPPEDWINDOW;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
    DetachOverlayFromDesktop(hwnd_);
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd_, HWND_NOTOPMOST, (sw - 960) / 2, (sh - 640) / 2, 960,
                 640, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    ShowWindow(hwnd_, SW_MAXIMIZE);
  }
  ApplyChrome();
  RefreshTitle();
}

void NotesWindow::SyncFromWysiwyg() {
  if (!wysiwyg_) {
    return;
  }
  std::wstring md = web_.CachedMarkdown();
  // Prefer cache (updated by debounced changed / PullMarkdown). Only pump+pull
  // when the cache is empty so tab switches stay snappy.
  if (md.empty() && web_.Ready()) {
    MSG msg{};
    for (int i = 0; i < 64; ++i) {
      if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    md = web_.CachedMarkdown();
    if (md.empty()) {
      md = web_.PullMarkdown(250);
    }
  }
  if (!edit_ || md.empty()) {
    return;
  }
  // Compare against RichEdit — CurrentPlainText() returns the Toast cache
  // while wysiwyg_ is on, so md == CurrentPlainText() would always skip.
  const int len = GetWindowTextLengthW(edit_);
  std::wstring edit_text(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(edit_, edit_text.data(), len + 1);
  edit_text.resize(static_cast<size_t>(len));
  if (md != edit_text) {
    SetWindowTextW(edit_, md.c_str());
  }
}


void NotesWindow::EnsureMarkdownEditor(bool force_reload) {
  if (toast_) {
    return;
  }
  if (!PathLooksMarkdown()) {
    if (wysiwyg_) {
      SyncFromWysiwyg();
      wysiwyg_ = false;
      ApplyWysiwygVisibility();
    }
    if (preview_) {
      preview_ = false;
      if (preview_web_.Host()) {
        preview_web_.Hide();
      }
    }
    if (edit_) {
      ShowWindow(edit_, SW_SHOW);
    }
    UpdateModeStrip();
    LayoutClient();
    return;
  }
  if (preview_) {
    preview_ = false;
    if (preview_web_.Host()) {
      preview_web_.Hide();
    }
  }
  if (!web_.Host() && !web_.Available()) {
    MistAlert(hwnd_, L"DeskNote",
              L"无法启动所见即所得编辑器。请安装 Microsoft Edge WebView2 Runtime。",
              MistMsgTone::Warn);
    return;
  }
  const bool entering = !wysiwyg_;
  wysiwyg_ = true;
  // Layout host first, then push markdown (empty notes still boot Toast).
  if (entering) {
    ApplyWysiwygVisibility();
  } else {
    web_.Show();
    web_.Resize();
  }
  LayoutClient();
  std::wstring md = CurrentPlainText();
  if (md.empty()) {
    md = L"# \n\n";
    if (edit_) {
      SetWindowTextW(edit_, md.c_str());
    }
  }
  if (entering || force_reload) {
    // Avoid redundant Toast setMarkdown when content is already on screen.
    if (entering || web_.CachedMarkdown() != md) {
      web_.SetMarkdown(md);
    }
  }
  EnsureTabStates();
  if (tab_index_ >= 0 && tab_index_ < static_cast<int>(tab_states_.size())) {
    tab_states_[static_cast<size_t>(tab_index_)].wysiwyg = true;
  }
  UpdateModeStrip();
  RefreshTitle();
}

void NotesWindow::ToggleWysiwyg() {
  EnsureMarkdownEditor(true);
}

std::wstring NotesWindow::NoteBaseDir() const {
  return DirOfPath(path_);
}

void NotesWindow::EnsureTabStates() {
  const size_t old = tab_states_.size();
  if (tab_states_.size() != tab_paths_.size()) {
    tab_states_.resize(tab_paths_.size());
  }
  // New slots inherit the current global wrap default.
  for (size_t i = old; i < tab_states_.size(); ++i) {
    tab_states_[i].wrap = word_wrap_;
  }
}

void NotesWindow::ApplyWysiwygVisibility() {
  if (!edit_) {
    return;
  }
  if (wysiwyg_) {
    web_.Show();
    web_.Resize();
    ShowWindow(edit_, SW_HIDE);
  } else {
    web_.Hide();
    ShowWindow(edit_, SW_SHOW);
  }
  // Caller owns LayoutClient (SwitchToTab / EnsureMarkdownEditor layout once).
}

double NotesWindow::EditScrollRatio() {
  if (!edit_) {
    return 0.0;
  }
  const LRESULT first = SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0);
  const LRESULT line_count = SendMessageW(edit_, EM_GETLINECOUNT, 0, 0);
  RECT rc{};
  GetClientRect(edit_, &rc);
  TEXTMETRICW tm{};
  HDC hdc = GetDC(edit_);
  HFONT old = nullptr;
  if (font_) {
    old = static_cast<HFONT>(SelectObject(hdc, font_));
  }
  GetTextMetricsW(hdc, &tm);
  if (old) {
    SelectObject(hdc, old);
  }
  ReleaseDC(edit_, hdc);
  const int visible =
      (std::max)(1, (static_cast<int>(rc.bottom - rc.top)) /
                        (std::max)(1, static_cast<int>(tm.tmHeight)));
  const int max_first = (std::max)(0, static_cast<int>(line_count) - visible);
  if (max_first <= 0) {
    return 0.0;
  }
  return static_cast<double>(first) / static_cast<double>(max_first);
}

void NotesWindow::SetEditScrollRatio(double ratio) {
  if (!edit_ || syncing_scroll_) {
    return;
  }
  const LRESULT first = SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0);
  const LRESULT line_count = SendMessageW(edit_, EM_GETLINECOUNT, 0, 0);
  RECT rc{};
  GetClientRect(edit_, &rc);
  TEXTMETRICW tm{};
  HDC hdc = GetDC(edit_);
  HFONT old = nullptr;
  if (font_) {
    old = static_cast<HFONT>(SelectObject(hdc, font_));
  }
  GetTextMetricsW(hdc, &tm);
  if (old) {
    SelectObject(hdc, old);
  }
  ReleaseDC(edit_, hdc);
  const int visible =
      (std::max)(1, (static_cast<int>(rc.bottom - rc.top)) /
                        (std::max)(1, static_cast<int>(tm.tmHeight)));
  const int max_first = (std::max)(0, static_cast<int>(line_count) - visible);
  const int target = static_cast<int>(ratio * max_first);
  const int delta = target - static_cast<int>(first);
  if (delta != 0) {
    syncing_scroll_ = true;
    SendMessageW(edit_, EM_LINESCROLL, 0, delta);
    syncing_scroll_ = false;
  }
}

void NotesWindow::SyncEditToPreview() {
  if (!sync_scroll_ || !preview_ || !preview_web_.Ready()) {
    return;
  }
  if (syncing_scroll_) {
    return;
  }
  syncing_scroll_ = true;
  preview_web_.SetScrollRatio(EditScrollRatio());
  syncing_scroll_ = false;
}

void NotesWindow::SyncPreviewToEdit(double ratio) {
  if (!sync_scroll_ || syncing_scroll_) {
    return;
  }
  syncing_scroll_ = true;
  SetEditScrollRatio(ratio);
  syncing_scroll_ = false;
}

void NotesWindow::UpdatePreview() {
  if (!preview_ || !preview_web_.Ready()) {
    return;
  }
  preview_web_.SetPreview(CurrentPlainText(), NoteBaseDir());
}

void NotesWindow::SchedulePreviewRefresh() {
  if (!hwnd_) {
    return;
  }
  KillTimer(hwnd_, kPreviewTimer);
  SetTimer(hwnd_, kPreviewTimer, 200, nullptr);
}

void NotesWindow::TogglePreview() {
  if (wysiwyg_) {
    ToggleWysiwyg();  // leave WYSIWYG first
  }
  if (!preview_web_.Available()) {
    preview_web_.SetKind(NotesWebView::Kind::kViewer);
    preview_web_.SetTheme(SystemAppsUseLightTheme() ? L"light" : L"dark");
    preview_web_.on_scroll = [this](double ratio) { SyncPreviewToEdit(ratio); };
    if (!preview_web_.Create(hwnd_)) {
      MistAlert(hwnd_, L"预览", L"无法启动预览（WebView2 不可用）。",
                MistMsgTone::Warn);
      return;
    }
  }
  preview_ = !preview_;
  if (menu_) {
    CheckMenuItem(menu_, kMenuPreviewPane,
                  MF_BYCOMMAND | (preview_ ? MF_CHECKED : MF_UNCHECKED));
  }
  if (preview_) {
    preview_web_.Show();
    UpdatePreview();
    SetWindowPos(hwnd_, nullptr, 0, 0, 900, 560,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW);
  } else {
    preview_web_.Hide();
  }
  LayoutClient();
  PersistViewSettings();
}

void NotesWindow::ExportPdf() {
  if (!preview_web_.Available()) {
    preview_web_.SetKind(NotesWebView::Kind::kViewer);
    preview_web_.SetTheme(SystemAppsUseLightTheme() ? L"light" : L"dark");
    if (!preview_web_.Create(hwnd_)) {
      MistAlert(hwnd_, L"导出 PDF", L"无法启动 PDF 导出（WebView2 不可用）。",
                MistMsgTone::Warn);
      return;
    }
  }
  wchar_t file[MAX_PATH] = L"note.pdf";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter = L"PDF\0*.pdf\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_OVERWRITEPROMPT;
  ofn.lpstrDefExt = L"pdf";
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  if (!preview_web_.Ready()) {
    preview_web_.Show();
    const DWORD start = GetTickCount();
    MSG msg{};
    while (!preview_web_.Ready() && GetTickCount() - start < 5000) {
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      Sleep(15);
    }
  }
  preview_web_.SetPreview(CurrentPlainText(), NoteBaseDir());
  std::wstring err;
  if (!preview_web_.PrintToPdf(file, &err)) {
    MistAlert(hwnd_, L"导出 PDF",
              (L"PDF 导出失败：" + (err.empty() ? L"未知错误" : err)).c_str(),
              MistMsgTone::Danger);
    return;
  }
  ShellExecuteW(nullptr, L"open", file, nullptr, nullptr, SW_SHOWNORMAL);
}

bool NotesWindow::PasteImageToMarkdown() {
  HBITMAP bmp = ClipboardBitmap(hwnd_);
  if (!bmp) {
    return false;
  }
  const std::wstring assets_dir = JoinPath(NoteBaseDir(), L"assets");
  CreateDirectoryW(assets_dir.c_str(), nullptr);
  const std::wstring dest = MakeAssetImagePath(assets_dir);
  if (!SaveBitmapAsPng(bmp, dest)) {
    DeleteObject(bmp);
    return false;
  }
  DeleteObject(bmp);
  const size_t slash = dest.find_last_of(L"\\/");
  const std::wstring name =
      slash == std::wstring::npos ? dest : dest.substr(slash + 1);
  const std::wstring md = L"![](assets/" + name + L")";
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(md.c_str()));
  MarkDirty();
  if (preview_) {
    SchedulePreviewRefresh();
  }
  return true;
}

void NotesWindow::GotoLine(int line0) {
  if (!edit_ || line0 < 0) {
    return;
  }
  const LRESULT index =
      SendMessageW(edit_, EM_LINEINDEX, static_cast<WPARAM>(line0), 0);
  if (index < 0) {
    return;
  }
  CHARRANGE cr{};
  cr.cpMin = static_cast<LONG>(index);
  cr.cpMax = static_cast<LONG>(index);
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
  if (typewriter_) {
    CenterCaret();
  }
  SetFocus(edit_);
}

void NotesWindow::SearchLibrary() {
  // Match Python focus_library_search: Ctrl+Shift+F focuses the sidebar
  // search field (live results), not a modal dialog.
  if (!sidebar_visible_) {
    ToggleSidebar();
  }
  if (!sidebar_search_) {
    return;
  }
  SetFocus(sidebar_search_);
  SendMessageW(sidebar_search_, EM_SETSEL, 0, -1);
}

void NotesWindow::OpenLibrary() {
  const std::wstring picked = PickNoteFromLibrary(hwnd_);
  if (picked.empty()) {
    return;
  }
  if (GetFileAttributesW(picked.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::ofstream out(picked, std::ios::binary);
    out << "";
  }
  OpenOrActivatePath(picked);
}

void NotesWindow::SaveAs() {
  wchar_t file[MAX_PATH] = {};
  wcsncpy_s(file, path_.c_str(), _TRUNCATE);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Text\0*.txt;*.md\0All\0*.*\0";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }
  AdoptPath(file);
  Save();
}

void NotesWindow::FindAgain(bool down) {
  if (!edit_ || find_what_[0] == 0) {
    ShowFind(false);
    return;
  }
  std::wstring status;
  if (!RunFindOnce(down, find_wrap_, find_match_case_, find_whole_word_,
                   &status)) {
    MistAlert(hwnd_, L"查找", status.empty() ? L"未找到。" : status.c_str(),
              MistMsgTone::Info);
  }
}

void NotesWindow::LayoutClient() {
  if (!hwnd_ || !edit_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const bool md_preview_flag =
      preview_ && PathLooksMarkdown() && !wysiwyg_ && !focus_;
  const bool looks_md = PathLooksMarkdown();
  wchar_t search_probe[4] = {};
  if (sidebar_search_) {
    GetWindowTextW(sidebar_search_, search_probe, 2);
  }
  const bool searching = search_probe[0] != 0;
  // Mode chip / WW host / search results swap — include in fingerprint.
  if (rc.right == layout_w_ && rc.bottom == layout_h_ &&
      wysiwyg_ == layout_wysiwyg_ && sidebar_visible_ == layout_sidebar_ &&
      outline_ == layout_outline_ && md_preview_flag == layout_preview_ &&
      toast_ == layout_toast_ && focus_ == layout_focus_ &&
      looks_md == layout_looks_md_ && searching == layout_searching_) {
    return;
  }
  layout_w_ = rc.right;
  layout_h_ = rc.bottom;
  layout_wysiwyg_ = wysiwyg_;
  layout_sidebar_ = sidebar_visible_;
  layout_outline_ = outline_;
  layout_preview_ = md_preview_flag;
  layout_toast_ = toast_;
  layout_focus_ = focus_;
  layout_looks_md_ = looks_md;
  layout_searching_ = searching;
  // Tab strip (+ top-right preview/edit chip for Markdown).
  int top = 0;
  const bool show_mode_chip =
      mode_strip_ && PathLooksMarkdown() && !focus_ && !toast_ && !wysiwyg_;
  if (tabs_ && !toast_) {
    const int tabs_w =
        show_mode_chip
            ? (std::max)(120, static_cast<int>(rc.right) -
                                  (kModeBtnW + kModeBtnPad * 2))
            : static_cast<int>(rc.right);
    MoveWindow(tabs_, 0, 0, tabs_w, 48, TRUE);
    ShowWindow(tabs_, SW_SHOW);
    SetWindowPos(tabs_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    top = 48;
  } else if (tabs_) {
    ShowWindow(tabs_, SW_HIDE);
  }
  // Status
  int bottom = rc.bottom;
  if (status_ && !toast_) {
    bottom = rc.bottom - kStatusBarH;
    MoveWindow(status_, 0, bottom, rc.right, kStatusBarH, TRUE);
    ShowWindow(status_, SW_SHOW);
  } else if (status_) {
    ShowWindow(status_, SW_HIDE);
  }
  int left = 0;
  int right = rc.right;
  const int pane_h = (std::max)(40, bottom - top);

  // Left library rail (cool gray) — full height under tabs.
  if (sidebar_visible_ && sidebar_ && !toast_) {
    const int side = 240;
    const int chrome_h = 44;
    const int pad = 12;
    const int btn_w = 30;
    if (sidebar_search_) {
      MoveWindow(sidebar_search_, pad + 4, top + 10,
                 side - (btn_w * 2 + pad * 2 + 12), chrome_h - 18, TRUE);
      ShowWindow(sidebar_search_, SW_SHOW);
    }
    if (sidebar_new_) {
      MoveWindow(sidebar_new_, side - (btn_w * 2 + pad + 2), top + 8, btn_w,
                 btn_w, TRUE);
      ShowWindow(sidebar_new_, SW_SHOW);
    }
    if (sidebar_refresh_) {
      MoveWindow(sidebar_refresh_, side - (btn_w + pad), top + 8, btn_w, btn_w,
                 TRUE);
      ShowWindow(sidebar_refresh_, SW_SHOW);
    }
    const int body_top = top + chrome_h + 4;
    const int body_h = (std::max)(40, bottom - body_top);
    if (searching && sidebar_results_) {
      MoveWindow(sidebar_results_, 0, body_top, side, body_h, TRUE);
      ShowWindow(sidebar_results_, SW_SHOW);
      ShowWindow(sidebar_, SW_HIDE);
    } else {
      MoveWindow(sidebar_, 0, body_top, side, body_h, TRUE);
      ShowWindow(sidebar_, SW_SHOW);
      if (sidebar_results_) {
        ShowWindow(sidebar_results_, SW_HIDE);
      }
    }
    left = side + 1;  // +1 for divider line painted in WM_PAINT
  } else {
    if (sidebar_) {
      ShowWindow(sidebar_, SW_HIDE);
    }
    if (sidebar_search_) {
      ShowWindow(sidebar_search_, SW_HIDE);
    }
    if (sidebar_new_) {
      ShowWindow(sidebar_new_, SW_HIDE);
    }
    if (sidebar_refresh_) {
      ShowWindow(sidebar_refresh_, SW_HIDE);
    }
    if (sidebar_results_) {
      ShowWindow(sidebar_results_, SW_HIDE);
    }
  }

  if (focus_ && !wysiwyg_) {
    const int pad_x =
        (std::max)(48, (static_cast<int>(rc.right) - 720) / 2);
    left = (std::max)(left, pad_x);
    right = static_cast<int>(rc.right) - pad_x;
  }

  // Top-right preview/edit chip (Typora-class); outline stays a right rail.
  if (show_mode_chip) {
    const int bx = static_cast<int>(rc.right) - kModeBtnW - kModeBtnPad;
    const int by = (std::max)(0, (48 - kModeBtnH) / 2);
    MoveWindow(mode_strip_, bx, by, kModeBtnW, kModeBtnH, TRUE);
    SetWindowTextW(mode_strip_, preview_ ? L"编辑" : L"预览");
    SetWindowPos(mode_strip_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    InvalidateRect(mode_strip_, nullptr, FALSE);
  } else if (mode_strip_) {
    ShowWindow(mode_strip_, SW_HIDE);
  }

  const bool show_outline = outline_strip_ && !wysiwyg_ && !focus_ && !toast_ &&
                            !preview_;
  int trail = 0;
  if (show_outline) {
    trail += outline_ ? (kOutlineExpandedW + kOutlineStripW) : kOutlineStripW;
  }
  right = (std::max)(left + 80, right - trail);

  int x = rc.right;
  if (show_outline) {
    if (outline_ && outline_list_) {
      x -= kOutlineStripW;
      MoveWindow(outline_strip_, x, top, kOutlineStripW, pane_h, TRUE);
      SetWindowTextW(outline_strip_, L"‹");
      InvalidateRect(outline_strip_, nullptr, FALSE);
      ShowWindow(outline_strip_, SW_SHOW);
      x -= kOutlineExpandedW;
      MoveWindow(outline_list_, x, top, kOutlineExpandedW, pane_h, TRUE);
      ShowWindow(outline_list_, SW_SHOW);
    } else {
      if (outline_list_) {
        ShowWindow(outline_list_, SW_HIDE);
      }
      x -= kOutlineStripW;
      MoveWindow(outline_strip_, x, top, kOutlineStripW, pane_h, TRUE);
      SetWindowTextW(outline_strip_, L"大\n纲");
      InvalidateRect(outline_strip_, nullptr, FALSE);
      ShowWindow(outline_strip_, SW_SHOW);
    }
  } else {
    if (outline_list_) {
      ShowWindow(outline_list_, SW_HIDE);
    }
    if (outline_strip_) {
      ShowWindow(outline_strip_, SW_HIDE);
    }
  }

  const int stage_l = left + (sidebar_visible_ && !toast_ ? 12 : 8);
  const int stage_r = right - 8;
  const int stage_t = top + 8;
  const int stage_b = bottom - 8;
  const int stage_w = (std::max)(40, stage_r - stage_l);
  const int stage_h = (std::max)(40, stage_b - stage_t);

  const bool md_preview =
      preview_ && PathLooksMarkdown() && !wysiwyg_ && !focus_;
  if (wysiwyg_) {
    if (edit_) {
      ShowWindow(edit_, SW_HIDE);
    }
    if (preview_web_.Host()) {
      preview_web_.Hide();
    }
    if (gutter_) {
      ShowWindow(gutter_, SW_HIDE);
    }
    if (web_.Host()) {
      MoveWindow(web_.Host(), stage_l, stage_t, stage_w, stage_h, TRUE);
      web_.Resize();
    }
  } else if (md_preview) {
    if (edit_) {
      ShowWindow(edit_, SW_HIDE);
    }
    if (gutter_) {
      ShowWindow(gutter_, SW_HIDE);
    }
    if (preview_web_.Host()) {
      MoveWindow(preview_web_.Host(), stage_l, stage_t, stage_w, stage_h, TRUE);
      preview_web_.Show();
      preview_web_.Resize();
    }
  } else {
    if (preview_web_.Host()) {
      preview_web_.Hide();
    }
    if (edit_) {
      EnsureGutter();
      LayoutGutter(stage_l, stage_t, stage_h);
      const int edit_left = stage_l + (gutter_ ? gutter_width_ : 0);
      ShowWindow(edit_, SW_SHOW);
      MoveWindow(edit_, edit_left, stage_t,
                 (std::max)(40, stage_l + stage_w - edit_left), stage_h, TRUE);
      RefreshGutter();
    } else if (gutter_) {
      ShowWindow(gutter_, SW_HIDE);
    }
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void NotesWindow::EnsureGutter() {
  if (gutter_ || !hwnd_) {
    return;
  }
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  if (!GetClassInfoExW(GetModuleHandleW(nullptr), kGutterClass, &wc)) {
    wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = NotesWindow::GutterProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kGutterClass;
    RegisterClassExW(&wc);
  }
  gutter_ = CreateWindowExW(
      0, kGutterClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, gutter_width_, 100,
      hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGutterId)),
      GetModuleHandleW(nullptr), this);
  if (gutter_) {
    SetWindowLongPtrW(gutter_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  }
}

void NotesWindow::LayoutGutter(int left, int top, int height) {
  if (!gutter_) {
    return;
  }
  int lines = 1;
  if (edit_) {
    lines = (std::max)(
        1, static_cast<int>(SendMessageW(edit_, EM_GETLINECOUNT, 0, 0)));
  }
  int digits = 1;
  for (int n = lines; n >= 10; n /= 10) {
    ++digits;
  }
  gutter_width_ = (std::max)(36, 12 + digits * 9 + 10);
  MoveWindow(gutter_, left, top, gutter_width_, height, TRUE);
  ShowWindow(gutter_, SW_SHOW);
}

void NotesWindow::RefreshGutter() {
  if (gutter_ && IsWindowVisible(gutter_)) {
    InvalidateRect(gutter_, nullptr, TRUE);
  }
}

LRESULT CALLBACK NotesWindow::GutterProc(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam) {
  auto* self =
      reinterpret_cast<NotesWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<NotesWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const COLORREF bg = self ? self->content_bg_ : RGB(255, 255, 255);
      const COLORREF fg = self ? self->muted_color_ : RGB(148, 157, 170);
      const COLORREF edge =
          self ? self->border_color_ : RGB(226, 230, 236);
      HBRUSH brush = CreateSolidBrush(bg);
      FillRect(hdc, &rc, brush);
      DeleteObject(brush);
      HPEN pen = CreatePen(PS_SOLID, 1, edge);
      HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
      MoveToEx(hdc, rc.right - 1, 0, nullptr);
      LineTo(hdc, rc.right - 1, rc.bottom);
      SelectObject(hdc, old_pen);
      DeleteObject(pen);
      if (self && self->edit_) {
        const int first = static_cast<int>(
            SendMessageW(self->edit_, EM_GETFIRSTVISIBLELINE, 0, 0));
        const int total = static_cast<int>(
            SendMessageW(self->edit_, EM_GETLINECOUNT, 0, 0));
        HFONT old = nullptr;
        if (self->font_) {
          old = static_cast<HFONT>(SelectObject(hdc, self->font_));
        }
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, fg);
        const LONG first_cp = static_cast<LONG>(
            SendMessageW(self->edit_, EM_LINEINDEX, first, 0));
        POINTL pt0{};
        SendMessageW(self->edit_, EM_POSFROMCHAR,
                     reinterpret_cast<WPARAM>(&pt0), first_cp);
        int line_h = 16;
        if (first + 1 < total) {
          const LONG next_cp = static_cast<LONG>(
              SendMessageW(self->edit_, EM_LINEINDEX, first + 1, 0));
          POINTL pt1{};
          SendMessageW(self->edit_, EM_POSFROMCHAR,
                       reinterpret_cast<WPARAM>(&pt1), next_cp);
          if (pt1.y > pt0.y) {
            line_h = static_cast<int>(pt1.y - pt0.y);
          }
        } else {
          TEXTMETRICW tm{};
          GetTextMetricsW(hdc, &tm);
          line_h = tm.tmHeight + tm.tmExternalLeading;
        }
        const int visible =
            (std::max)(1, static_cast<int>(rc.bottom - rc.top) /
                                  (std::max)(1, line_h) +
                              2);
        for (int i = 0; i < visible; ++i) {
          const int line = first + i;
          if (line >= total) {
            break;
          }
          const int y = static_cast<int>(pt0.y) + i * line_h;
          if (y > rc.bottom) {
            break;
          }
          const bool marked =
              self->bookmarks_.find(line) != self->bookmarks_.end();
          if (marked) {
            Gdiplus::Graphics gg(hdc);
            gg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush mark(Gdiplus::Color(255, 71, 85, 105));
            gg.FillEllipse(&mark, 5.f, static_cast<Gdiplus::REAL>(y) + 5.f, 6.f,
                           6.f);
          }
          wchar_t buf[16];
          swprintf_s(buf, L"%d", line + 1);
          RECT tr{14, y, rc.right - 2, y + line_h};
          DrawTextW(hdc, buf, -1, &tr, DT_RIGHT | DT_TOP | DT_SINGLELINE);
        }
        if (old) {
          SelectObject(hdc, old);
        }
      }
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_LBUTTONDBLCLK:
      if (self) {
        self->ToggleBookmarkAtCaret();
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void NotesWindow::ToggleBookmarkAtCaret() {
  if (!edit_) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  const int line =
      static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, cr.cpMin));
  if (bookmarks_.count(line)) {
    bookmarks_.erase(line);
  } else {
    bookmarks_.insert(line);
  }
  RefreshGutter();
}

void NotesWindow::GotoBookmark(bool next) {
  if (!edit_ || bookmarks_.empty()) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  const int cur =
      static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, cr.cpMin));
  int target = -1;
  if (next) {
    auto it = bookmarks_.upper_bound(cur);
    target = (it != bookmarks_.end()) ? *it : *bookmarks_.begin();
  } else {
    auto it = bookmarks_.lower_bound(cur);
    if (it == bookmarks_.begin()) {
      target = *bookmarks_.rbegin();
    } else {
      --it;
      target = *it;
    }
  }
  if (target >= 0) {
    GotoLine(target);
  }
}

void NotesWindow::DuplicateCurrentLine() {
  if (!edit_) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  const int line =
      static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, cr.cpMin));
  const LONG start =
      static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, line, 0));
  const int total =
      static_cast<int>(SendMessageW(edit_, EM_GETLINECOUNT, 0, 0));
  LONG end = 0;
  if (line + 1 < total) {
    end = static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, line + 1, 0));
  } else {
    GETTEXTLENGTHEX tl{};
    tl.flags = GTL_DEFAULT | GTL_NUMCHARS;
    tl.codepage = 1200;
    end = static_cast<LONG>(
        SendMessageW(edit_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&tl),
                     0));
  }
  if (end < start) {
    return;
  }
  std::wstring chunk(static_cast<size_t>(end - start) + 1, L'\0');
  TEXTRANGEW tr{};
  tr.chrg.cpMin = start;
  tr.chrg.cpMax = end;
  tr.lpstrText = chunk.data();
  SendMessageW(edit_, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&tr));
  chunk.resize(static_cast<size_t>(end - start));
  if (line + 1 >= total &&
      (chunk.empty() || (chunk.back() != L'\r' && chunk.back() != L'\n'))) {
    chunk.push_back(L'\r');
  }
  CHARRANGE ins{end, end};
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&ins));
  SendMessageW(edit_, EM_REPLACESEL, TRUE,
               reinterpret_cast<LPARAM>(chunk.c_str()));
  MarkDirty();
  RefreshGutter();
}

void NotesWindow::MoveCurrentLine(int delta) {
  if (!edit_ || delta == 0) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  const int line =
      static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, cr.cpMin));
  const int total =
      static_cast<int>(SendMessageW(edit_, EM_GETLINECOUNT, 0, 0));
  const int other = line + delta;
  if (other < 0 || other >= total) {
    return;
  }
  auto line_range = [&](int ln, LONG* a, LONG* b) {
    *a = static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, ln, 0));
    if (ln + 1 < total) {
      *b = static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, ln + 1, 0));
    } else {
      GETTEXTLENGTHEX tl{};
      tl.flags = GTL_DEFAULT | GTL_NUMCHARS;
      tl.codepage = 1200;
      *b = static_cast<LONG>(SendMessageW(
          edit_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&tl), 0));
    }
  };
  LONG a0 = 0, a1 = 0, b0 = 0, b1 = 0;
  if (delta < 0) {
    line_range(other, &a0, &a1);
    line_range(line, &b0, &b1);
  } else {
    line_range(line, &a0, &a1);
    line_range(other, &b0, &b1);
  }
  auto grab = [&](LONG s, LONG e) {
    std::wstring out(static_cast<size_t>(e - s) + 1, L'\0');
    TEXTRANGEW tr{};
    tr.chrg.cpMin = s;
    tr.chrg.cpMax = e;
    tr.lpstrText = out.data();
    SendMessageW(edit_, EM_GETTEXTRANGE, 0, reinterpret_cast<LPARAM>(&tr));
    out.resize(static_cast<size_t>(e - s));
    return out;
  };
  const std::wstring first = grab(a0, a1);
  const std::wstring second = grab(b0, b1);
  std::wstring swapped = second + first;
  CHARRANGE sel{a0, b1};
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&sel));
  SendMessageW(edit_, EM_REPLACESEL, TRUE,
               reinterpret_cast<LPARAM>(swapped.c_str()));
  // Restore caret on the moved line.
  const int new_line = other;
  const LONG new_start =
      static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, new_line, 0));
  CHARRANGE caret{new_start, new_start};
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&caret));
  // Keep bookmarks attached to content lines when swapping neighbors.
  const bool mark_a = bookmarks_.count(line) != 0;
  const bool mark_b = bookmarks_.count(other) != 0;
  if (mark_a) {
    bookmarks_.erase(line);
  }
  if (mark_b) {
    bookmarks_.erase(other);
  }
  if (mark_a) {
    bookmarks_.insert(other);
  }
  if (mark_b) {
    bookmarks_.insert(line);
  }
  MarkDirty();
  RefreshGutter();
}

void NotesWindow::RebuildOutline() {
  if (!outline_list_ || !edit_) {
    return;
  }
  SendMessageW(outline_list_, LB_RESETCONTENT, 0, 0);
  const std::wstring text = CurrentPlainText();
  int line = 0;
  size_t i = 0;
  while (i <= text.size()) {
    size_t end = text.find(L'\n', i);
    if (end == std::wstring::npos) {
      end = text.size();
    }
    std::wstring raw = text.substr(i, end - i);
    while (!raw.empty() && raw.back() == L'\r') {
      raw.pop_back();
    }
    int level = 0;
    while (level < 6 && level < static_cast<int>(raw.size()) &&
           raw[static_cast<size_t>(level)] == L'#') {
      ++level;
    }
    if (level > 0 && static_cast<size_t>(level) < raw.size() &&
        raw[static_cast<size_t>(level)] == L' ') {
      std::wstring title(level, L' ');
      title += raw.substr(static_cast<size_t>(level + 1));
      const int idx = static_cast<int>(SendMessageW(
          outline_list_, LB_ADDSTRING, 0,
          reinterpret_cast<LPARAM>(title.c_str())));
      if (idx >= 0) {
        SendMessageW(outline_list_, LB_SETITEMDATA, idx, line);
      }
    }
    ++line;
    if (end >= text.size()) {
      break;
    }
    i = end + 1;
  }
}

void NotesWindow::JumpToOutline() {
  if (!outline_list_ || !edit_) {
    return;
  }
  const int sel =
      static_cast<int>(SendMessageW(outline_list_, LB_GETCURSEL, 0, 0));
  if (sel < 0) {
    return;
  }
  const int line = static_cast<int>(
      SendMessageW(outline_list_, LB_GETITEMDATA, sel, 0));
  const LRESULT index =
      SendMessageW(edit_, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
  if (index < 0) {
    return;
  }
  CHARRANGE cr{};
  cr.cpMin = static_cast<LONG>(index);
  cr.cpMax = static_cast<LONG>(index);
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
  if (typewriter_) {
    CenterCaret();
  }
  SetFocus(edit_);
}

void NotesWindow::ToggleOutline() {
  outline_ = !outline_;
  if (menu_) {
    CheckMenuItem(menu_, kMenuOutline,
                  MF_BYCOMMAND | (outline_ ? MF_CHECKED : MF_UNCHECKED));
  }
  if (outline_) {
    RebuildOutline();
  }
  LayoutClient();
  PersistViewSettings();
}

void NotesWindow::ToggleFocus() {
  focus_ = !focus_;
  if (menu_) {
    CheckMenuItem(menu_, kMenuFocus,
                  MF_BYCOMMAND | (focus_ ? MF_CHECKED : MF_UNCHECKED));
  }
  LayoutClient();
}

void NotesWindow::ToggleTypewriter() {
  typewriter_ = !typewriter_;
  if (menu_) {
    CheckMenuItem(menu_, kMenuTypewriter,
                  MF_BYCOMMAND | (typewriter_ ? MF_CHECKED : MF_UNCHECKED));
  }
  if (typewriter_) {
    CenterCaret();
  }
  PersistViewSettings();
}

void NotesWindow::CenterCaret() {
  if (!edit_) {
    return;
  }
  CHARRANGE sel{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
  const LRESULT line =
      SendMessageW(edit_, EM_LINEFROMCHAR, static_cast<WPARAM>(sel.cpMin), 0);
  RECT rc{};
  GetClientRect(edit_, &rc);
  TEXTMETRICW tm{};
  HDC hdc = GetDC(edit_);
  HFONT old = nullptr;
  if (font_) {
    old = static_cast<HFONT>(SelectObject(hdc, font_));
  }
  GetTextMetricsW(hdc, &tm);
  if (old) {
    SelectObject(hdc, old);
  }
  ReleaseDC(edit_, hdc);
  const int visible =
      (std::max)(1, (static_cast<int>(rc.bottom - rc.top)) /
                        (std::max)(1, static_cast<int>(tm.tmHeight)));
  const int first = static_cast<int>(
      SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0));
  const int target = static_cast<int>(line) - visible / 2;
  const int delta = target - first;
  if (delta != 0) {
    SendMessageW(edit_, EM_LINESCROLL, 0, delta);
  }
}

bool NotesWindow::RunFindOnce(bool down, bool wrap, bool match_case, bool whole,
                              std::wstring* status_out) {
  if (!edit_ || find_what_[0] == 0) {
    if (status_out) {
      *status_out = L"请输入查找内容";
    }
    return false;
  }

  auto select_range = [&](LONG a, LONG b) {
    CHARRANGE cr{a, b};
    SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
  };

  if (find_regex_) {
    const std::wstring hay = EditTextCpAligned();
    std::wstring pat = find_what_;
    if (whole) {
      pat = L"\\b(?:" + pat + L")\\b";
    }
    std::wregex::flag_type flags = std::regex_constants::ECMAScript;
    if (!match_case) {
      flags |= std::regex_constants::icase;
    }
    std::wregex re;
    try {
      re = std::wregex(pat, flags);
    } catch (const std::regex_error&) {
      if (status_out) {
        *status_out = L"正则表达式无效";
      }
      return false;
    }
    CHARRANGE sel{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
    auto try_forward = [&](size_t from) -> bool {
      std::wsmatch m;
      const std::wstring slice = hay.substr(from);
      if (!std::regex_search(slice, m, re) || m.empty()) {
        return false;
      }
      const LONG start = static_cast<LONG>(from + static_cast<size_t>(m.position(0)));
      const LONG end = start + static_cast<LONG>(m.length(0));
      select_range(start, end);
      return true;
    };
    auto try_backward = [&](size_t until) -> bool {
      bool found = false;
      LONG best_s = 0;
      LONG best_e = 0;
      size_t pos = 0;
      while (pos < until) {
        std::wsmatch m;
        const std::wstring slice = hay.substr(pos);
        if (!std::regex_search(slice, m, re) || m.empty()) {
          break;
        }
        const size_t abs = pos + static_cast<size_t>(m.position(0));
        const size_t abs_end = abs + static_cast<size_t>(m.length(0));
        if (abs_end > until) {
          break;
        }
        found = true;
        best_s = static_cast<LONG>(abs);
        best_e = static_cast<LONG>(abs_end);
        pos = abs + (m.length(0) == 0 ? 1 : static_cast<size_t>(m.length(0)));
        if (pos >= until) {
          break;
        }
      }
      if (found) {
        select_range(best_s, best_e);
      }
      return found;
    };
    bool found = false;
    if (down) {
      const size_t from = static_cast<size_t>((std::max)(0L, sel.cpMax));
      found = try_forward(from);
      if (!found && wrap) {
        found = try_forward(0);
      }
    } else {
      const size_t until = static_cast<size_t>((std::max)(0L, sel.cpMin));
      found = try_backward(until);
      if (!found && wrap) {
        found = try_backward(hay.size());
      }
    }
    if (status_out) {
      *status_out = found ? L"" : L"未找到";
    }
    return found;
  }

  DWORD flags = 0;
  if (match_case) {
    flags |= FR_MATCHCASE;
  }
  if (whole) {
    flags |= FR_WHOLEWORD;
  }
  auto try_find = [&](LONG cp_min, LONG cp_max, bool search_down) -> bool {
    FINDTEXTEXW ft{};
    ft.chrg.cpMin = cp_min;
    ft.chrg.cpMax = cp_max;
    ft.lpstrText = find_what_;
    const LRESULT hit = SendMessageW(
        edit_, EM_FINDTEXTEXW, flags | (search_down ? FR_DOWN : 0),
        reinterpret_cast<LPARAM>(&ft));
    if (hit < 0) {
      return false;
    }
    SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&ft.chrgText));
    SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    return true;
  };

  CHARRANGE sel{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
  bool found = false;
  if (down) {
    found = try_find(sel.cpMax, -1, true);
    if (!found && wrap) {
      found = try_find(0, -1, true);
    }
  } else {
    found = try_find(0, sel.cpMin, false);
    if (!found && wrap) {
      found = try_find(0, -1, false);
    }
  }
  if (status_out) {
    *status_out = found ? L"" : L"未找到";
  }
  return found;
}

bool NotesWindow::RunReplaceOnce(bool match_case, bool whole, bool wrap,
                                 std::wstring* status_out) {
  if (!edit_) {
    return false;
  }
  CHARRANGE sel{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&sel));
  if (sel.cpMax > sel.cpMin) {
    if (find_regex_) {
      const std::wstring hay = EditTextCpAligned();
      if (sel.cpMin >= 0 &&
          static_cast<size_t>(sel.cpMax) <= hay.size()) {
        const std::wstring matched =
            hay.substr(static_cast<size_t>(sel.cpMin),
                       static_cast<size_t>(sel.cpMax - sel.cpMin));
        std::wstring pat = find_what_;
        if (whole) {
          pat = L"\\b(?:" + pat + L")\\b";
        }
        std::wregex::flag_type flags = std::regex_constants::ECMAScript;
        if (!match_case) {
          flags |= std::regex_constants::icase;
        }
        try {
          const std::wregex re(pat, flags);
          const std::wstring repl =
              std::regex_replace(matched, re, std::wstring(replace_with_));
          SendMessageW(edit_, EM_REPLACESEL, TRUE,
                       reinterpret_cast<LPARAM>(repl.c_str()));
        } catch (const std::regex_error&) {
          if (status_out) {
            *status_out = L"正则表达式无效";
          }
          return false;
        }
      }
    } else {
      SendMessageW(edit_, EM_REPLACESEL, TRUE,
                   reinterpret_cast<LPARAM>(replace_with_));
    }
  }
  return RunFindOnce(true, wrap, match_case, whole, status_out);
}

int NotesWindow::RunReplaceAll(bool match_case, bool whole) {
  if (!edit_ || find_what_[0] == 0) {
    return 0;
  }
  if (find_regex_) {
    std::wstring hay = EditTextCpAligned();
    std::wstring pat = find_what_;
    if (whole) {
      pat = L"\\b(?:" + pat + L")\\b";
    }
    std::wregex::flag_type flags = std::regex_constants::ECMAScript;
    if (!match_case) {
      flags |= std::regex_constants::icase;
    }
    try {
      const std::wregex re(pat, flags);
      int n = 0;
      {
        std::wsregex_iterator it(hay.begin(), hay.end(), re);
        std::wsregex_iterator end;
        for (; it != end; ++it) {
          ++n;
          if (n > 100000) {
            break;
          }
        }
      }
      if (n <= 0) {
        return 0;
      }
      const std::wstring out =
          std::regex_replace(hay, re, std::wstring(replace_with_));
      SETTEXTEX st{};
      st.flags = ST_DEFAULT;
      st.codepage = 1200;
      SendMessageW(edit_, EM_SETTEXTEX, reinterpret_cast<WPARAM>(&st),
                   reinterpret_cast<LPARAM>(out.c_str()));
      MarkDirty();
      RefreshGutter();
      return n;
    } catch (const std::regex_error&) {
      return 0;
    }
  }
  int n = 0;
  CHARRANGE top{};
  top.cpMin = 0;
  top.cpMax = 0;
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&top));
  while (RunFindOnce(true, false, match_case, whole, nullptr)) {
    SendMessageW(edit_, EM_REPLACESEL, TRUE,
                 reinterpret_cast<LPARAM>(replace_with_));
    ++n;
    if (n > 100000) {
      break;
    }
  }
  return n;
}

namespace {
bool TextEqualsMatch(const std::wstring& hay, size_t pos, const std::wstring& needle,
                     bool match_case, bool whole) {
  if (pos + needle.size() > hay.size()) {
    return false;
  }
  for (size_t i = 0; i < needle.size(); ++i) {
    wchar_t a = hay[pos + i];
    wchar_t b = needle[i];
    if (!match_case) {
      a = towlower(a);
      b = towlower(b);
    }
    if (a != b) {
      return false;
    }
  }
  if (whole) {
    auto is_word = [](wchar_t c) {
      return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
             (c >= L'a' && c <= L'z') || c == L'_' || c > 127;
    };
    if (pos > 0 && is_word(hay[pos - 1])) {
      return false;
    }
    if (pos + needle.size() < hay.size() &&
        is_word(hay[pos + needle.size()])) {
      return false;
    }
  }
  return true;
}

int ReplaceAllInWideString(std::wstring* text, const std::wstring& find,
                           const std::wstring& repl, bool match_case,
                           bool whole) {
  if (!text || find.empty()) {
    return 0;
  }
  int n = 0;
  size_t pos = 0;
  while (pos < text->size()) {
    if (TextEqualsMatch(*text, pos, find, match_case, whole)) {
      text->replace(pos, find.size(), repl);
      pos += repl.size();
      ++n;
      if (n > 100000) {
        break;
      }
    } else {
      ++pos;
    }
  }
  return n;
}

int ReplaceAllRegexInWideString(std::wstring* text, const std::wstring& find,
                                const std::wstring& repl, bool match_case,
                                bool whole) {
  if (!text || find.empty()) {
    return 0;
  }
  std::wstring pat = find;
  if (whole) {
    pat = L"\\b(?:" + pat + L")\\b";
  }
  std::wregex::flag_type flags = std::regex_constants::ECMAScript;
  if (!match_case) {
    flags |= std::regex_constants::icase;
  }
  try {
    const std::wregex re(pat, flags);
    int n = 0;
    {
      std::wsregex_iterator it(text->begin(), text->end(), re);
      std::wsregex_iterator end;
      for (; it != end; ++it) {
        ++n;
        if (n > 100000) {
          break;
        }
      }
    }
    if (n <= 0) {
      return 0;
    }
    *text = std::regex_replace(*text, re, repl);
    return n;
  } catch (const std::regex_error&) {
    return 0;
  }
}
}  // namespace

int NotesWindow::RunReplaceAllTabs(bool match_case, bool whole) {
  if (find_what_[0] == 0) {
    return 0;
  }
  int total = RunReplaceAll(match_case, whole);
  const std::wstring find = find_what_;
  const std::wstring repl = replace_with_;
  const bool use_regex = find_regex_;
  const int cur = tab_index_;
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (static_cast<int>(i) == cur) {
      continue;
    }
    const std::wstring& path = tab_paths_[i];
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      continue;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string data = ss.str();
    if (data.rfind("{\\rtf", 0) == 0) {
      continue;  // skip RTF tabs for offline replace
    }
    std::wstring wide;
    TextFileEncoding enc = TextFileEncoding::Utf8;
    if (!DecodeTextFileBytes(data, &wide, &enc)) {
      continue;
    }
    const int n =
        use_regex
            ? ReplaceAllRegexInWideString(&wide, find, repl, match_case, whole)
            : ReplaceAllInWideString(&wide, find, repl, match_case, whole);
    if (n <= 0) {
      continue;
    }
    std::string bytes;
    if (!EncodeTextFileBytes(wide, enc, &bytes)) {
      continue;
    }
    std::ofstream out(path, std::ios::binary);
    if (out) {
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      total += n;
    }
  }
  return total;
}

void NotesWindow::ShowFind(bool replace) {
  MistWebDialog dlg;
  dlg.owner = hwnd_;
  dlg.frameless = true;
  dlg.disable_owner = false;  // Modeless-like: keep editor usable (Python).
  dlg.owner_accel = accel_;
  dlg.user_data_subdir = L"webview2-notes-find";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\notes_find.html");
  if (dlg.html_path.empty()) {
    MistAlert(hwnd_, L"查找", L"找不到 notes_find.html", MistMsgTone::Warn);
    return;
  }
  std::wstring status;
  dlg.build_state = [&]() {
    std::wstring json = L"{\"type\":\"state\",\"replace_mode\":";
    json += replace ? L"true" : L"false";
    json += L",\"find\":\"";
    json += MistJsonEscape(find_what_);
    json += L"\",\"replace\":\"";
    json += MistJsonEscape(replace_with_);
    json += L"\",\"match_case\":";
    json += find_match_case_ ? L"true" : L"false";
    json += L",\"whole_word\":";
    json += find_whole_word_ ? L"true" : L"false";
    json += L",\"regex\":";
    json += find_regex_ ? L"true" : L"false";
    json += L",\"wrap\":";
    json += find_wrap_ ? L"true" : L"false";
    json += L",\"all_tabs\":";
    json += find_all_tabs_ ? L"true" : L"false";
    json += L",\"status\":\"";
    json += MistJsonEscape(status);
    json += L"\"}";
    return json;
  };
  auto sync_from_json = [&](const std::wstring& json) {
    wcsncpy_s(find_what_, MistJsonStringField(json, L"find").c_str(),
              _TRUNCATE);
    wcsncpy_s(replace_with_, MistJsonStringField(json, L"replace").c_str(),
              _TRUNCATE);
    find_match_case_ = json.find(L"\"match_case\":true") != std::wstring::npos;
    find_whole_word_ = json.find(L"\"whole_word\":true") != std::wstring::npos;
    find_regex_ = json.find(L"\"regex\":true") != std::wstring::npos;
    find_wrap_ = json.find(L"\"wrap\":false") == std::wstring::npos;
    find_all_tabs_ = json.find(L"\"all_tabs\":true") != std::wstring::npos;
  };
  dlg.on_message = [&](const std::wstring& json) -> bool {
    const std::wstring type = MistJsonStringField(json, L"type");
    if (type != L"action") {
      return false;
    }
    const std::wstring id = MistJsonStringField(json, L"id");
    if (id == L"ready") {
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"close") {
      return true;
    }
    sync_from_json(json);
    if (id == L"find_next") {
      RunFindOnce(true, find_wrap_, find_match_case_, find_whole_word_,
                  &status);
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"find_prev") {
      RunFindOnce(false, find_wrap_, find_match_case_, find_whole_word_,
                  &status);
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"replace") {
      RunReplaceOnce(find_match_case_, find_whole_word_, find_wrap_, &status);
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"replace_all") {
      const int n =
          find_all_tabs_
              ? RunReplaceAllTabs(find_match_case_, find_whole_word_)
              : RunReplaceAll(find_match_case_, find_whole_word_);
      status = L"已替换 " + std::to_wstring(n) + L" 处";
      MistWebDialogPushState(&dlg);
      return false;
    }
    return false;
  };
  MistWebDialogRun(&dlg, replace ? L"替换" : L"查找", 480,
                   replace ? 380 : 320);
}

void NotesWindow::InsertTemplate() {
  if (!edit_) {
    return;
  }
  const wchar_t* tpl =
      L"\r\n---\r\n# 笔记\r\n\r\n- 要点：\r\n- 待办：\r\n- 链接：\r\n";
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(tpl));
}

void NotesWindow::ShowHelp() { ShowDeskNoteHelp(hwnd_); }

void NotesWindow::OpenNotesFolder() {
  // Prefer folder of the active tab (Python resolve_openable_notes_folder).
  std::wstring dir;
  if (!path_.empty()) {
    const DWORD attr = GetFileAttributesW(path_.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
      const size_t slash = path_.find_last_of(L"\\/");
      if (slash != std::wstring::npos) {
        dir = path_.substr(0, slash);
      }
    } else if (attr != INVALID_FILE_ATTRIBUTES) {
      dir = path_;
    }
  }
  if (dir.empty()) {
    dir = NotesLibraryDir(true);
  }
  CreateDirectoryW(dir.c_str(), nullptr);
  ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void NotesWindow::OpenFileDialog() {
  const size_t kCap = 32768;
  std::vector<wchar_t> buf(kCap, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter =
      L"文本与代码\0*.md;*.txt;*.markdown;*.log;*.json;*.xml;*.yml;*.yaml;*.csv;"
      L"*.py;*.js;*.ts;*.tsx;*.css;*.html;*.htm;*.c;*.cpp;*.h;*.hpp;*.java;"
      L"*.cs;*.go;*.rs;*.rb;*.php;*.sql;*.sh;*.bash;*.ps1;*.bat;*.cmd;*.ini;"
      L"*.toml;*.cfg;*.conf;*.env;*.properties\0"
      L"所有文件\0*.*\0";
  ofn.lpstrFile = buf.data();
  ofn.nMaxFile = static_cast<DWORD>(kCap);
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT |
              OFN_EXPLORER;
  if (!GetOpenFileNameW(&ofn)) {
    return;
  }
  std::vector<std::wstring> paths;
  if (ofn.nFileOffset > 0 && buf[ofn.nFileOffset - 1] == L'\0') {
    const std::wstring dir = buf.data();
    const wchar_t* q = buf.data() + ofn.nFileOffset;
    while (*q) {
      const std::wstring name = q;
      paths.push_back(dir + L"\\" + name);
      q += name.size() + 1;
    }
  } else {
    paths.push_back(buf.data());
  }
  if (!paths.empty()) {
    OpenPaths(paths);
  }
}

void NotesWindow::Present() {
  if (!hwnd_) {
    return;
  }
  if (toast_) {
    Show();
    return;
  }
  ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
  SetForegroundWindow(hwnd_);
  LayoutClient();
  EnsureMarkdownEditor(true);
  UpdateWindow(hwnd_);
}

bool NotesWindow::ConfirmSaveCurrent() {
  SyncFromWysiwyg();
  if (!dirty_) {
    return true;
  }
  const std::wstring msg =
      L"是否保存对「" + TabLabel(path_) + L"」的更改？";
  const MistMsgResult r = MistMessage(hwnd_, L"便签", msg.c_str(),
                                      MistMsgKind::YesNoCancel, MistMsgTone::Warn);
  if (r == MistMsgResult::Yes) {
    Save(true);
    return true;
  }
  if (r == MistMsgResult::No) {
    dirty_ = false;
    return true;
  }
  return false;
}

bool NotesWindow::ConfirmCloseAll() {
  // Notepad++ style: close always autosaves (no discard prompt).
  if (!path_.empty()) {
    Save(true);
  } else if (edit_) {
    path_ = MakeNewNotePath();
    if (tab_index_ >= 0 &&
        tab_index_ < static_cast<int>(tab_paths_.size())) {
      tab_paths_[static_cast<size_t>(tab_index_)] = path_;
    } else {
      tab_paths_.push_back(path_);
      tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
    }
    Save(true);
  }
  PersistTabs();
  PersistViewSettings();
  return true;
}

void NotesWindow::ToggleSidebar() {
  sidebar_visible_ = !sidebar_visible_;
  if (menu_) {
    CheckMenuItem(menu_, kMenuSidebar,
                  MF_BYCOMMAND | (sidebar_visible_ ? MF_CHECKED : MF_UNCHECKED));
  }
  if (sidebar_visible_) {
    RebuildSidebar();
  }
  LayoutClient();
  PersistViewSettings();
}

std::wstring NotesWindow::SidebarSelectedPath() {
  if (!sidebar_) {
    return {};
  }
  HTREEITEM sel = TreeView_GetSelection(sidebar_);
  if (!sel) {
    return {};
  }
  TVITEMW it{};
  it.hItem = sel;
  it.mask = TVIF_PARAM;
  if (!TreeView_GetItem(sidebar_, &it) || it.lParam == 0) {
    return {};
  }
  const size_t idx = static_cast<size_t>(it.lParam) - 1;
  if (idx >= sidebar_paths_.size()) {
    return {};
  }
  return sidebar_paths_[idx];
}

void NotesWindow::OpenSelectedSidebarItem() {
  const std::wstring path = SidebarSelectedPath();
  if (path.empty()) {
    return;
  }
  OpenOrActivatePath(path);
}

bool NotesWindow::PathIsPinned(const std::wstring& path) const {
  const std::wstring key = NormalizePathKey(path);
  for (const auto& p : pinned_paths_) {
    if (_wcsicmp(NormalizePathKey(p).c_str(), key.c_str()) == 0) {
      return true;
    }
  }
  return false;
}

void NotesWindow::ApplyPinnedTabOrder() {
  if (tab_paths_.empty()) {
    return;
  }
  EnsureTabStates();
  const std::wstring current = path_;
  std::vector<std::wstring> ordered;
  std::vector<TabState> ordered_states;
  ordered.reserve(tab_paths_.size());
  ordered_states.reserve(tab_paths_.size());
  auto take = [&](size_t i) {
    ordered.push_back(tab_paths_[i]);
    if (i < tab_states_.size()) {
      ordered_states.push_back(tab_states_[i]);
    } else {
      ordered_states.push_back(TabState{});
    }
  };
  for (const auto& pin : pinned_paths_) {
    const std::wstring pin_key = NormalizePathKey(pin);
    for (size_t i = 0; i < tab_paths_.size(); ++i) {
      if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(), pin_key.c_str()) ==
          0) {
        take(i);
        break;
      }
    }
  }
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (!PathIsPinned(tab_paths_[i])) {
      take(i);
    }
  }
  if (ordered.size() != tab_paths_.size()) {
    return;
  }
  tab_paths_ = std::move(ordered);
  tab_states_ = std::move(ordered_states);
  tab_index_ = 0;
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(),
                 NormalizePathKey(current).c_str()) == 0) {
      tab_index_ = static_cast<int>(i);
      break;
    }
  }
  path_ = tab_paths_[static_cast<size_t>(tab_index_)];
  RebuildTabControl();
}

void NotesWindow::TogglePinPath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  const std::wstring norm = NormalizePathKey(path);
  for (size_t i = 0; i < pinned_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(pinned_paths_[i]).c_str(), norm.c_str()) ==
        0) {
      pinned_paths_.erase(pinned_paths_.begin() + static_cast<ptrdiff_t>(i));
      PersistTabs();
      ApplyPinnedTabOrder();
      RebuildSidebar();
      return;
    }
  }
  pinned_paths_.push_back(norm);
  PersistTabs();
  ApplyPinnedTabOrder();
  RebuildSidebar();
}

void NotesWindow::RunSidebarSearch() {
  if (!sidebar_results_ || !sidebar_search_) {
    return;
  }
  wchar_t buf[256] = {};
  GetWindowTextW(sidebar_search_, buf, 256);
  const std::wstring query = buf;
  SendMessageW(sidebar_results_, LB_RESETCONTENT, 0, 0);
  sidebar_hits_.clear();
  if (query.empty()) {
    LayoutClient();
    return;
  }
  const AppPrefs prefs = LoadAppPrefs();
  sidebar_hits_ =
      SearchNotesLibrary(NotesLibraryDir(true), query,
                         prefs.notes_search_case_sensitive,
                         prefs.notes_search_max_hits,
                         prefs.notes_search_max_file_bytes);
  for (const auto& hit : sidebar_hits_) {
    const size_t slash = hit.path.find_last_of(L"\\/");
    std::wstring label = slash == std::wstring::npos
                             ? hit.path
                             : hit.path.substr(slash + 1);
    if (hit.line >= 0) {
      label += L"  · 行 " + std::to_wstring(hit.line + 1);
    }
    SendMessageW(sidebar_results_, LB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label.c_str()));
  }
  LayoutClient();
}

void NotesWindow::OpenSidebarResult(int index) {
  if (index < 0 || index >= static_cast<int>(sidebar_hits_.size())) {
    return;
  }
  const NoteSearchHit& hit = sidebar_hits_[static_cast<size_t>(index)];
  OpenOrActivatePath(hit.path);
  if (hit.line < 0) {
    return;
  }
  // Leave preview/WYSIWYG so the RichEdit caret jump is visible (P1-19).
  if (wysiwyg_) {
    wysiwyg_ = false;
    ApplyWysiwygVisibility();
    LayoutClient();
  }
  if (preview_ && preview_web_.Host()) {
    preview_web_.Hide();
  }
  // SwitchToTab restores a saved caret after Load — jump once more deferred
  // so the search hit wins over the session caret.
  pending_goto_line_ = hit.line;
  PostMessageW(hwnd_, WM_APP + 60, 0, 0);
}

void NotesWindow::RebuildSidebar() {
  if (!sidebar_) {
    return;
  }
  TreeView_DeleteAllItems(sidebar_);
  sidebar_paths_.clear();
  const std::wstring root = NotesLibraryDir(true);
  CreateDirectoryW(root.c_str(), nullptr);

  auto insert = [&](HTREEITEM parent, const std::wstring& full, bool is_dir) {
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    const size_t slash = full.find_last_of(L"\\/");
    std::wstring label =
        slash == std::wstring::npos ? full : full.substr(slash + 1);
    ins.item.pszText = label.data();
    ins.item.lParam = 0;
    if (!is_dir) {
      sidebar_paths_.push_back(full);
      ins.item.lParam = static_cast<LPARAM>(sidebar_paths_.size());
    }
    return TreeView_InsertItem(sidebar_, &ins);
  };

  std::function<void(const std::wstring&, HTREEITEM, int)> walk =
      [&](const std::wstring& dir, HTREEITEM parent, int depth) {
        WIN32_FIND_DATAW fd{};
        const std::wstring pat = dir + L"\\*";
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
          return;
        }
        struct Entry {
          std::wstring name;
          std::wstring full;
          bool dir = false;
          bool pinned = false;
          ULONGLONG ctime = 0;
        };
        std::vector<Entry> entries;
        do {
          if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) {
              continue;
            }
            Entry e{fd.cFileName, dir + L"\\" + fd.cFileName, true, false, 0};
            ULARGE_INTEGER u{};
            u.LowPart = fd.ftCreationTime.dwLowDateTime;
            u.HighPart = fd.ftCreationTime.dwHighDateTime;
            e.ctime = u.QuadPart;
            entries.push_back(std::move(e));
          } else if (IsOpenablePath(fd.cFileName)) {
            Entry e{fd.cFileName, dir + L"\\" + fd.cFileName, false, false, 0};
            e.pinned = PathIsPinned(e.full);
            ULARGE_INTEGER u{};
            u.LowPart = fd.ftCreationTime.dwLowDateTime;
            u.HighPart = fd.ftCreationTime.dwHighDateTime;
            e.ctime = u.QuadPart;
            // Pinned files get a ★ prefix in the label.
            if (e.pinned) {
              e.name = L"★ " + e.name;
            }
            entries.push_back(std::move(e));
          }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        // P1-22: dirs first, then pinned, then newest creation time.
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) {
                    if (a.dir != b.dir) {
                      return a.dir;
                    }
                    if (a.pinned != b.pinned) {
                      return a.pinned && !b.pinned;
                    }
                    if (a.ctime != b.ctime) {
                      return a.ctime > b.ctime;
                    }
                    return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                  });
        for (const auto& e : entries) {
          HTREEITEM it = insert(parent, e.full, e.dir);
          // Re-label pinned files (insert used basename from full path).
          if (!e.dir && e.pinned && it) {
            TVITEMW item{};
            item.mask = TVIF_HANDLE | TVIF_TEXT;
            item.hItem = it;
            std::wstring label = e.name;
            item.pszText = label.data();
            TreeView_SetItem(sidebar_, &item);
          }
          if (e.dir && depth < 2) {
            walk(e.full, it, depth + 1);
          }
        }
      };

  HTREEITEM root_item = insert(TVI_ROOT, root, true);
  if (root_item) {
    // Product label — avoid raw folder name 「笔记」looking like OS Explorer.
    TVITEMW item{};
    item.mask = TVIF_HANDLE | TVIF_TEXT;
    item.hItem = root_item;
    wchar_t label[] = L"全部笔记";
    item.pszText = label;
    TreeView_SetItem(sidebar_, &item);
  }
  walk(root, root_item, 0);
  TreeView_Expand(sidebar_, root_item, TVE_EXPAND);
  // Keep root visible — EnsureVisible on a deep child used to scroll「全部笔记」
  // half off the top of the rail.
  if (root_item) {
    TreeView_EnsureVisible(sidebar_, root_item);
  }
  if (!path_.empty()) {
    HighlightSidebarPath(path_);
  }
}

void NotesWindow::HighlightSidebarPath(const std::wstring& path) {
  if (!sidebar_ || path.empty()) {
    return;
  }
  const std::wstring want = NormalizePathKey(path);
  HTREEITEM found = nullptr;
  std::function<void(HTREEITEM)> walk = [&](HTREEITEM item) {
    while (item && !found) {
      TVITEMW ti{};
      ti.mask = TVIF_HANDLE | TVIF_PARAM | TVIF_CHILDREN;
      ti.hItem = item;
      if (TreeView_GetItem(sidebar_, &ti) && ti.lParam > 0) {
        const size_t idx = static_cast<size_t>(ti.lParam) - 1;
        if (idx < sidebar_paths_.size() &&
            _wcsicmp(NormalizePathKey(sidebar_paths_[idx]).c_str(),
                     want.c_str()) == 0) {
          found = item;
          return;
        }
      }
      HTREEITEM child = TreeView_GetChild(sidebar_, item);
      if (child) {
        walk(child);
      }
      item = TreeView_GetNextSibling(sidebar_, item);
    }
  };
  walk(TreeView_GetRoot(sidebar_));
  if (!found) {
    return;
  }
  // Expand ancestors so the item is visible, then select + ensure visible.
  for (HTREEITEM p = TreeView_GetParent(sidebar_, found); p;
       p = TreeView_GetParent(sidebar_, p)) {
    TreeView_Expand(sidebar_, p, TVE_EXPAND);
  }
  TreeView_SelectItem(sidebar_, found);
  TreeView_EnsureVisible(sidebar_, found);
}

void NotesWindow::RenameSidebarSelected() {
  // Match Python Explorer-style inline F2 (stem only; suffix locked on commit).
  if (!sidebar_) {
    return;
  }
  HTREEITEM sel = TreeView_GetSelection(sidebar_);
  if (!sel || SidebarSelectedPath().empty()) {
    return;
  }
  TreeView_EditLabel(sidebar_, sel);
}

bool NotesWindow::ApplySidebarRename(const std::wstring& path,
                                     const std::wstring& new_stem_raw) {
  if (path.empty()) {
    return false;
  }
  std::wstring new_stem = new_stem_raw;
  // Strip accidental extension / pin star if the user typed them.
  while (!new_stem.empty() &&
         (new_stem.front() == L'*' || new_stem.front() == L'★' ||
          new_stem.front() == L' ')) {
    new_stem.erase(new_stem.begin());
  }
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring dir =
      slash == std::wstring::npos ? L"." : path.substr(0, slash);
  std::wstring name =
      slash == std::wstring::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of(L'.');
  const std::wstring suffix =
      dot == std::wstring::npos ? L"" : name.substr(dot);
  const std::wstring stem =
      dot == std::wstring::npos ? name : name.substr(0, dot);
  // If the edit control still has the extension, drop it so suffix stays locked.
  if (!suffix.empty() && new_stem.size() > suffix.size() &&
      _wcsicmp(new_stem.c_str() + (new_stem.size() - suffix.size()),
               suffix.c_str()) == 0) {
    new_stem.resize(new_stem.size() - suffix.size());
  }
  if (new_stem.empty() || new_stem == stem) {
    return false;
  }
  // Disallow path separators in the stem.
  if (new_stem.find_first_of(L"\\/") != std::wstring::npos) {
    MistAlert(hwnd_, L"重命名", L"名称不能包含路径分隔符。", MistMsgTone::Warn);
    return false;
  }
  const std::wstring new_path = dir + L"\\" + new_stem + suffix;
  if (GetFileAttributesW(new_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    MistAlert(hwnd_, L"重命名", L"该名称已存在。", MistMsgTone::Warn);
    return false;
  }
  if (!MoveFileW(path.c_str(), new_path.c_str())) {
    MistAlert(hwnd_, L"重命名", L"无法重命名。", MistMsgTone::Warn);
    return false;
  }
  for (auto& p : tab_paths_) {
    if (_wcsicmp(NormalizePathKey(p).c_str(),
                 NormalizePathKey(path).c_str()) == 0) {
      p = new_path;
    }
  }
  for (auto& p : pinned_paths_) {
    if (_wcsicmp(NormalizePathKey(p).c_str(),
                 NormalizePathKey(path).c_str()) == 0) {
      p = new_path;
    }
  }
  if (_wcsicmp(NormalizePathKey(path_).c_str(),
               NormalizePathKey(path).c_str()) == 0) {
    path_ = new_path;
  }
  RebuildSidebar();
  RebuildTabControl();
  PersistTabs();
  UpdateStatusBar();
  RefreshTitle();
  return true;
}

void NotesWindow::DeleteSidebarSelected() {
  const std::wstring path = SidebarSelectedPath();
  if (path.empty()) {
    return;
  }
  if (!MistConfirm(hwnd_, L"删除", L"删除此文件？将移入回收站。",
                   MistMsgTone::Danger) ||
      !RecyclePathQuiet(hwnd_, path)) {
    return;
  }
  for (size_t i = 0; i < pinned_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(pinned_paths_[i]).c_str(),
                 NormalizePathKey(path).c_str()) == 0) {
      pinned_paths_.erase(pinned_paths_.begin() + static_cast<ptrdiff_t>(i));
      break;
    }
  }
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(),
                 NormalizePathKey(path).c_str()) == 0) {
      tab_paths_.erase(tab_paths_.begin() + static_cast<ptrdiff_t>(i));
      if (tab_index_ >= static_cast<int>(tab_paths_.size())) {
        tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
      }
      if (tab_paths_.empty()) {
        tab_paths_.push_back(MakeNewNotePath());
        tab_index_ = 0;
      }
      break;
    }
  }
  RebuildSidebar();
  RebuildTabControl();
  SwitchToTab((std::max)(0, tab_index_), false);
  PersistTabs();
}

void NotesWindow::HandleSidebarContextMenu(const POINT& pt) {
  if (!sidebar_) {
    return;
  }
  TVHITTESTINFO ht{};
  ht.pt = pt;
  ScreenToClient(sidebar_, &ht.pt);
  HTREEITEM hit = TreeView_HitTest(sidebar_, &ht);
  if (hit) {
    TreeView_SelectItem(sidebar_, hit);
  }
  const std::wstring path = SidebarSelectedPath();
  const bool is_file = !path.empty();

  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | (is_file ? MF_ENABLED : MF_GRAYED), kSideOpen,
              L"打开");
  AppendMenuW(m, MF_STRING, kSideNew, L"新建 Markdown");
  if (is_file) {
    AppendMenuW(m, MF_STRING, kSidePin,
                PathIsPinned(path) ? L"取消置顶" : L"置顶");
  }
  AppendMenuW(m, MF_STRING | (is_file ? MF_ENABLED : MF_GRAYED), kSideRename,
              L"重命名\tF2");
  AppendMenuW(m, MF_STRING | (is_file ? MF_ENABLED : MF_GRAYED), kSideDelete,
              L"删除\tDelete");
  AppendMenuW(m, MF_STRING | (is_file ? MF_ENABLED : MF_GRAYED), kSideReveal,
              L"在资源管理器中显示");
  AppendMenuW(m, MF_STRING | (is_file ? MF_ENABLED : MF_GRAYED), kSideCopyPath,
              L"复制路径");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kSideRefresh, L"刷新");
  const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                                 hwnd_, nullptr);
  DestroyMenu(m);

  if (cmd == kSideOpen) {
    OpenSelectedSidebarItem();
  } else if (cmd == kSideNew) {
    NewTab();
    RebuildSidebar();
  } else if (cmd == kSidePin) {
    TogglePinPath(path);
  } else if (cmd == kSideRename) {
    RenameSidebarSelected();
  } else if (cmd == kSideDelete) {
    DeleteSidebarSelected();
  } else if (cmd == kSideReveal) {
    const std::wstring arg = L"/select,\"" + path + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", arg.c_str(), nullptr,
                  SW_SHOWNORMAL);
  } else if (cmd == kSideCopyPath && is_file) {
    CopyPathToClipboard(path);
  } else if (cmd == kSideRefresh) {
    RebuildSidebar();
  }
}

LRESULT CALLBACK NotesWindow::SidebarProc(HWND hwnd, UINT msg, WPARAM wparam,
                                          LPARAM lparam) {
  auto* self = reinterpret_cast<NotesWindow*>(
      GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (self && msg == WM_KEYDOWN) {
    if (wparam == VK_F2) {
      self->RenameSidebarSelected();
      return 0;
    }
    if (wparam == VK_DELETE) {
      self->DeleteSidebarSelected();
      return 0;
    }
  }
  if (self && self->sidebar_proc_) {
    return CallWindowProcW(self->sidebar_proc_, hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK NotesWindow::StatusProc(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam) {
  auto* self = reinterpret_cast<NotesWindow*>(
      GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (msg == WM_ERASEBKGND) {
    return 1;
  }
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const COLORREF bg =
        self ? (self->toast_ ? RGB(255, 248, 220) : self->content_bg_)
             : RGB(255, 255, 255);
    const COLORREF fg = self ? self->muted_color_ : RGB(120, 128, 140);
    const COLORREF edge = self ? self->border_color_ : RGB(226, 230, 236);
    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(hdc, &rc, brush);
    DeleteObject(brush);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
    MoveToEx(hdc, 0, 0, nullptr);
    LineTo(hdc, rc.right, 0);
    SelectObject(hdc, old_pen);
    DeleteObject(pen);
    wchar_t text[512] = {};
    GetWindowTextW(hwnd, text, 512);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    HFONT old = nullptr;
    if (self && self->ui_font_) {
      old = static_cast<HFONT>(SelectObject(hdc, self->ui_font_));
    }
    RECT tr = rc;
    InflateRect(&tr, -12, 0);
    DrawTextW(hdc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                       DT_END_ELLIPSIS | DT_NOPREFIX);
    if (old) {
      SelectObject(hdc, old);
    }
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_SETTEXT) {
    LRESULT r = self && self->status_proc_
                    ? CallWindowProcW(self->status_proc_, hwnd, msg, wparam,
                                      lparam)
                    : DefWindowProcW(hwnd, msg, wparam, lparam);
    InvalidateRect(hwnd, nullptr, FALSE);
    return r;
  }
  if (self && msg == WM_LBUTTONDBLCLK) {
    self->PromptGotoLine();
    return 0;
  }
  if (self && (msg == WM_RBUTTONUP || msg == WM_CONTEXTMENU)) {
    if (!self->path_.empty()) {
      self->CopyPathToClipboard(self->path_);
    }
    return 0;
  }
  if (self && self->status_proc_) {
    return CallWindowProcW(self->status_proc_, hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK NotesWindow::TabsProc(HWND hwnd, UINT msg, WPARAM wparam,
                                       LPARAM lparam) {
  auto* self = reinterpret_cast<NotesWindow*>(
      GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      self->PaintTabBar(hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SIZE:
      self->RelayoutTabChips();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN: {
      self->RelayoutTabChips();
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      int zone = 0;
      const int hit = self->HitTestTabBar(pt, &zone);
      if (zone == 3) {
        self->NewTab();
        return 0;
      }
      if (hit >= 0) {
        if (zone == 2) {
          self->CloseTabAt(hit);
          self->tab_drag_from_ = -1;
          self->tab_dragging_ = false;
          return 0;
        }
        if (zone == 1) {
          self->TogglePinPath(self->tab_paths_[static_cast<size_t>(hit)]);
          self->tab_drag_from_ = -1;
          self->tab_dragging_ = false;
          return 0;
        }
        if (hit != self->tab_index_) {
          self->SwitchToTab(hit, true);
        }
      }
      self->tab_drag_from_ = hit;
      self->tab_dragging_ = false;
      self->tab_drag_origin_ = pt;
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if ((wparam & MK_LBUTTON) && self->tab_drag_from_ >= 0) {
        const int dx = pt.x - self->tab_drag_origin_.x;
        const int dy = pt.y - self->tab_drag_origin_.y;
        if (!self->tab_dragging_ && dx * dx + dy * dy >= 36) {
          self->tab_dragging_ = true;
          SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
        }
        if (self->tab_dragging_) {
          const int to = self->TabIndexFromPoint(pt);
          if (to >= 0 && to != self->tab_drag_from_) {
            self->ReorderTab(self->tab_drag_from_, to);
            self->tab_drag_from_ = to;
            self->tab_drag_origin_ = pt;
          }
        }
        return 0;
      }
      TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&tme);
      int zone = 0;
      const int hit = self->HitTestTabBar(pt, &zone);
      const int hover_idx = (zone == 3) ? -2 : hit;
      if (hover_idx != self->tab_hover_index_ ||
          zone != self->tab_hover_zone_) {
        self->tab_hover_index_ = hover_idx;
        self->tab_hover_zone_ = zone;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (self->tab_hover_index_ != -1 || self->tab_hover_zone_ != 0) {
        self->tab_hover_index_ = -1;
        self->tab_hover_zone_ = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
      self->tab_drag_from_ = -1;
      self->tab_dragging_ = false;
      return 0;
    case WM_MBUTTONUP: {
      self->RelayoutTabChips();
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int idx = self->TabIndexFromPoint(pt);
      if (idx >= 0) {
        self->CloseTabAt(idx);
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      self->RelayoutTabChips();
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      int zone = 0;
      int hit = self->HitTestTabBar(pt, &zone);
      if (zone == 3) {
        self->NewTab();
        return 0;
      }
      // 空白双击新建；页签双击关闭（不删文件，内容已静默保存）。
      if (hit < 0) {
        self->NewTab();
      } else {
        self->CloseTabAt(hit);
      }
      return 0;
    }
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU: {
      POINT pt{};
      if (msg == WM_CONTEXTMENU) {
        pt.x = GET_X_LPARAM(lparam);
        pt.y = GET_Y_LPARAM(lparam);
        if (pt.x == -1 && pt.y == -1) {
          GetCursorPos(&pt);
        }
      } else {
        pt.x = GET_X_LPARAM(lparam);
        pt.y = GET_Y_LPARAM(lparam);
        ClientToScreen(hwnd, &pt);
      }
      POINT client = pt;
      ScreenToClient(hwnd, &client);
      self->RelayoutTabChips();
      const int idx = self->TabIndexFromPoint(client);
      if (idx >= 0) {
        self->HandleTabContextMenu(idx, pt);
      }
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK NotesWindow::EditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                        LPARAM lparam) {
  auto* self = reinterpret_cast<NotesWindow*>(
      GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (self && msg == WM_MOUSEWHEEL &&
      (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
    const short delta = GET_WHEEL_DELTA_WPARAM(wparam);
    if (delta > 0) {
      self->SetFontSize(self->font_pt_ + 1);
    } else if (delta < 0 && self->font_pt_ > 8) {
      self->SetFontSize(self->font_pt_ - 1);
    }
    return 0;
  }
  if (self && msg == WM_PASTE && self->SmartPaste()) {
    return 0;
  }
  if (self && (msg == WM_RBUTTONUP || msg == WM_CONTEXTMENU)) {
    POINT pt{};
    if (msg == WM_CONTEXTMENU) {
      pt.x = GET_X_LPARAM(lparam);
      pt.y = GET_Y_LPARAM(lparam);
      if (pt.x == -1 && pt.y == -1) {
        GetCursorPos(&pt);
      }
    } else {
      pt.x = GET_X_LPARAM(lparam);
      pt.y = GET_Y_LPARAM(lparam);
      ClientToScreen(hwnd, &pt);
    }
    self->HandleEditContextMenu(pt);
    return 0;
  }
  if (self && msg == WM_KEYDOWN) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (wparam == VK_RETURN && !ctrl && !shift &&
        self->TryContinueMarkdownList()) {
      return 0;
    }
    if (wparam == VK_F1) {
      self->ShowHelp();
      return 0;
    }
    if (wparam == VK_F11) {
      self->ToggleFocus();
      return 0;
    }
    if (wparam == VK_F5) {
      self->ReloadFromDisk();
      return 0;
    }
    if (wparam == VK_F3) {
      self->FindAgain(!shift);
      return 0;
    }
    if (wparam == VK_F2) {
      if (ctrl) {
        self->ToggleBookmarkAtCaret();
      } else {
        self->GotoBookmark(!shift);
      }
      return 0;
    }
    if (ctrl && !shift && (wparam == 'D' || wparam == 'd')) {
      self->DuplicateCurrentLine();
      return 0;
    }
    if (ctrl && shift && wparam == VK_UP) {
      self->MoveCurrentLine(-1);
      return 0;
    }
    if (ctrl && shift && wparam == VK_DOWN) {
      self->MoveCurrentLine(1);
      return 0;
    }
    if (ctrl && !shift && (wparam == 'G' || wparam == 'g')) {
      self->PromptGotoLine();
      return 0;
    }
    if (ctrl && !shift &&
        (wparam == VK_OEM_PLUS || wparam == VK_ADD || wparam == '=')) {
      self->SetFontSize(self->font_pt_ + 1);
      return 0;
    }
    if (ctrl && !shift &&
        (wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT)) {
      self->SetFontSize(self->font_pt_ - 1);
      return 0;
    }
    if (ctrl && !shift && (wparam == 'F' || wparam == 'f')) {
      self->ShowFind(false);
      return 0;
    }
    if (ctrl && !shift && (wparam == 'H' || wparam == 'h')) {
      self->ShowFind(true);
      return 0;
    }
    if (ctrl && (wparam == 'T' || wparam == 't')) {
      self->NewTab();
      return 0;
    }
    if (ctrl && (wparam == 'W' || wparam == 'w')) {
      self->CloseTab();
      return 0;
    }
    if (ctrl && shift && (wparam == 'V' || wparam == 'v')) {
      self->ShowPreviewMode();
      return 0;
    }
    if (ctrl && !shift && (wparam == 'B' || wparam == 'b')) {
      self->ApplyBold();
      return 0;
    }
    if (ctrl && !shift && (wparam == 'I' || wparam == 'i')) {
      self->ApplyItalic();
      return 0;
    }
    if (ctrl && !shift && (wparam == 'U' || wparam == 'u')) {
      self->ApplyUnderline();
      return 0;
    }
    if (ctrl && !shift && (wparam == 'K' || wparam == 'k')) {
      self->InsertMdLink();
      return 0;
    }
    if (ctrl && shift && (wparam == 'O' || wparam == 'o')) {
      self->ToggleOutline();
      return 0;
    }
    if (ctrl && shift && (wparam == 'E' || wparam == 'e')) {
      self->ToggleSidebar();
      return 0;
    }
    if (ctrl && !shift && (wparam == 'E' || wparam == 'e')) {
      self->ShowEditMode();
      return 0;
    }
    if (ctrl && shift && (wparam == 'F' || wparam == 'f')) {
      self->SearchLibrary();
      return 0;
    }
    if (ctrl && (wparam == 'N' || wparam == 'n')) {
      self->NewTab();
      return 0;
    }
    if (ctrl && (wparam == 'O' || wparam == 'o')) {
      self->OpenFileDialog();
      return 0;
    }
    if (ctrl && (wparam == 'S' || wparam == 's')) {
      if (shift) {
        self->SaveAs();
      } else {
        self->Save();
      }
      return 0;
    }
  }
  LRESULT result = 0;
  if (self && self->edit_proc_) {
    result = CallWindowProcW(self->edit_proc_, hwnd, msg, wparam, lparam);
  } else {
    result = DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (self && self->typewriter_ &&
      (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_KEYUP)) {
    self->CenterCaret();
  }
  return result;
}

LRESULT CALLBACK NotesWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
  NotesWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<NotesWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self =
        reinterpret_cast<NotesWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT NotesWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_DRAWITEM: {
      const auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (!dis || dis->CtlType != ODT_BUTTON) {
        break;
      }
      const UINT id = dis->CtlID;
      const bool is_new = id == static_cast<UINT>(kSidebarNewId);
      const bool is_ref = id == static_cast<UINT>(kSidebarRefreshId);
      const bool is_mode = id == static_cast<UINT>(kModeStripId);
      const bool is_outline = id == static_cast<UINT>(kOutlineStripId);
      if (!is_new && !is_ref && !is_mode && !is_outline) {
        break;
      }
      HDC hdc = dis->hDC;
      RECT rc = dis->rcItem;
      const bool press = (dis->itemState & ODS_SELECTED) != 0;
      POINT cur{};
      GetCursorPos(&cur);
      RECT wr{};
      GetWindowRect(dis->hwndItem, &wr);
      const bool hot = !press && PtInRect(&wr, cur);
      const bool strip = is_outline;
      const bool chip = is_mode || is_new || is_ref;
      // Outline rail on white stage; mode chip on tab chrome; sidebar on rail.
      const COLORREF plate =
          strip ? content_bg_ : (is_mode ? content_bg_ : rail_bg_);
      HBRUSH back = CreateSolidBrush(plate);
      FillRect(hdc, &rc, back);
      DeleteObject(back);
      Gdiplus::Graphics g(hdc);
      g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
      const float pad = strip ? 3.f : 1.f;
      const float x = static_cast<float>(rc.left) + pad;
      const float y = static_cast<float>(rc.top) + pad;
      const float w = static_cast<float>(rc.right - rc.left) - pad * 2.f;
      const float h = static_cast<float>(rc.bottom - rc.top) - pad * 2.f;
      Gdiplus::GraphicsPath path;
      GpAddRoundRect(&path, x, y, w, h, strip ? 8.f : 6.f);
      BYTE br = 255, bg_g = 255, bb = 255;
      if (press) {
        br = 228;
        bg_g = 232;
        bb = 238;
      } else if (hot) {
        br = 241;
        bg_g = 243;
        bb = 246;
      } else if (strip || is_mode) {
        br = 248;
        bg_g = 249;
        bb = 251;
      }
      Gdiplus::SolidBrush bg(Gdiplus::Color(255, br, bg_g, bb));
      g.FillPath(&bg, &path);
      Gdiplus::Pen ring(Gdiplus::Color(255, 220, 225, 232), 1.f);
      g.DrawPath(&ring, &path);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, press || hot ? text_color_ : muted_color_);
      if (ui_font_) {
        SelectObject(hdc, ui_font_);
      }
      wchar_t label[32] = {};
      if (is_new) {
        wcscpy_s(label, L"+");
      } else if (is_ref) {
        wcscpy_s(label, L"↻");
      } else {
        GetWindowTextW(dis->hwndItem, label, 32);
      }
      const UINT dt = chip ? (DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX)
                           : (DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
      DrawTextW(hdc, label, -1, &rc, dt);
      return TRUE;
    }
    case WM_NOTIFY: {
      const auto* hdr = reinterpret_cast<LPNMHDR>(lparam);
      if (hdr && hdr->idFrom == kSidebarId && hdr->code == NM_CUSTOMDRAW) {
        // Soft gray selection — avoid system blue highlight.
        auto* cd = reinterpret_cast<NMTVCUSTOMDRAW*>(lparam);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) {
          return CDRF_NOTIFYITEMDRAW;
        }
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
          const UINT state = TreeView_GetItemState(
              sidebar_, reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec),
              TVIS_SELECTED);
          if (state & TVIS_SELECTED) {
            cd->clrText = text_color_;
            cd->clrTextBk = RGB(226, 230, 236);
          } else if (cd->nmcd.uItemState & CDIS_HOT) {
            cd->clrText = text_color_;
            cd->clrTextBk = RGB(236, 238, 242);
          } else {
            cd->clrText = text_color_;
            cd->clrTextBk = rail_bg_;
          }
          // Prefer UI chrome font over Tiny TreeView default.
          if (ui_font_) {
            SelectObject(cd->nmcd.hdc, ui_font_);
          }
          return CDRF_NEWFONT;
        }
        return CDRF_DODEFAULT;
      }
      if (hdr && hdr->idFrom == kSidebarId && hdr->code == NM_DBLCLK) {
        OpenSelectedSidebarItem();
        return 0;
      }
      if (hdr && hdr->idFrom == kSidebarId &&
          hdr->code == TVN_BEGINLABELEDITW) {
        // Prefill stem only (no ★ / extension) — Python keep_filename_suffix.
        const auto* disp =
            reinterpret_cast<NMTVDISPINFOW*>(lparam);
        if (!disp || !disp->item.hItem) {
          return TRUE;
        }
        TVITEMW ti{};
        ti.mask = TVIF_HANDLE | TVIF_PARAM;
        ti.hItem = disp->item.hItem;
        if (!TreeView_GetItem(sidebar_, &ti) || ti.lParam <= 0) {
          return TRUE;  // Cancel edit on folders / invalid.
        }
        const size_t idx = static_cast<size_t>(ti.lParam) - 1;
        if (idx >= sidebar_paths_.size()) {
          return TRUE;
        }
        const std::wstring& full = sidebar_paths_[idx];
        const size_t slash = full.find_last_of(L"\\/");
        std::wstring name =
            slash == std::wstring::npos ? full : full.substr(slash + 1);
        const size_t dot = name.find_last_of(L'.');
        const std::wstring stem =
            dot == std::wstring::npos ? name : name.substr(0, dot);
        if (HWND edit = TreeView_GetEditControl(sidebar_)) {
          SetWindowTextW(edit, stem.c_str());
        }
        return FALSE;  // Allow edit.
      }
      if (hdr && hdr->idFrom == kSidebarId &&
          hdr->code == TVN_ENDLABELEDITW) {
        const auto* disp =
            reinterpret_cast<NMTVDISPINFOW*>(lparam);
        if (!disp || !disp->item.pszText) {
          return FALSE;  // Cancel / Escape — keep old label.
        }
        TVITEMW ti{};
        ti.mask = TVIF_HANDLE | TVIF_PARAM;
        ti.hItem = disp->item.hItem;
        if (!TreeView_GetItem(sidebar_, &ti) || ti.lParam <= 0) {
          return FALSE;
        }
        const size_t idx = static_cast<size_t>(ti.lParam) - 1;
        if (idx >= sidebar_paths_.size()) {
          return FALSE;
        }
        const std::wstring old_path = sidebar_paths_[idx];
        ApplySidebarRename(old_path, disp->item.pszText);
        return FALSE;  // We rebuild the tree ourselves.
      }
      if (hdr && hdr->hwndFrom == edit_ && hdr->code == EN_SELCHANGE) {
        // Word-count is timer/cached; avoid O(n) on every caret move.
        if (!wysiwyg_) {
          SetTimer(hwnd_, kStatusTimer, 120, nullptr);
        }
        return 0;
      }
      break;
    }
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kMenuNewTab:
          NewTab();
          return 0;
        case kMenuOpen:
          OpenFileDialog();
          return 0;
        case kMenuOpenHosts:
          OpenHostsFile();
          return 0;
        case kMenuCloseTab:
          CloseTab();
          return 0;
        case kMenuSave:
          Save(true);
          return 0;
        case kMenuSaveAs:
          SaveAs();
          return 0;
        case kMenuReload:
          ReloadFromDisk();
          return 0;
        case kMenuGotoLine:
          PromptGotoLine();
          return 0;
        case kMenuDupLine:
          DuplicateCurrentLine();
          return 0;
        case kMenuMoveLineUp:
          MoveCurrentLine(-1);
          return 0;
        case kMenuMoveLineDown:
          MoveCurrentLine(1);
          return 0;
        case kMenuToggleBookmark:
          ToggleBookmarkAtCaret();
          return 0;
        case kMenuNextBookmark:
          GotoBookmark(true);
          return 0;
        case kMenuPrevBookmark:
          GotoBookmark(false);
          return 0;
        case kMenuCloseWindow:
          PostMessageW(hwnd_, WM_CLOSE, 0, 0);
          return 0;
        case kMenuOpenFolder:
          OpenNotesFolder();
          return 0;
        case kMenuHelp:
          ShowHelp();
          return 0;
        case kMenuSidebar:
          ToggleSidebar();
          return 0;
        case kMenuLibrary:
          OpenLibrary();
          return 0;
        case kMenuSearchLib:
          SearchLibrary();
          return 0;
        case kMenuEditMode:
          ShowEditMode();
          return 0;
        case kMenuPreview:
          PreviewMarkdown();
          return 0;
        case kMenuExportMd:
          ExportMarkdown();
          return 0;
        case kMenuExportHtml:
          ExportHtml();
          return 0;
        case kMenuPandoc:
          ExportWithPandoc();
          return 0;
        case kMenuOpenWith:
          OpenWithShell();
          return 0;
        case kMenuPasteHtml:
          PasteHtmlAsMarkdown();
          return 0;
        case kMenuToast:
          ToggleToast();
          return 0;
        case kMenuWysiwyg:
          ToggleWysiwyg();
          return 0;
        case kMenuPreviewPane:
          ToggleMdViewMode();
          return 0;
        case kModeStripId:
          if (HIWORD(wparam) == BN_CLICKED) {
            ToggleMdViewMode();
          }
          return 0;
        case kOutlineStripId:
          if (HIWORD(wparam) == BN_CLICKED) {
            ToggleOutline();
          }
          return 0;
        case kMenuExportPdf:
          ExportPdf();
          return 0;
        case kMenuUndo:
          if (edit_) {
            SendMessageW(edit_, EM_UNDO, 0, 0);
          }
          return 0;
        case kMenuRedo:
          if (edit_) {
            SendMessageW(edit_, EM_REDO, 0, 0);
          }
          return 0;
        case kMenuNextTab:
          if (!tab_paths_.empty()) {
            const int n = static_cast<int>(tab_paths_.size());
            SwitchToTab((tab_index_ + 1) % n, true);
          }
          return 0;
        case kMenuPrevTab:
          if (!tab_paths_.empty()) {
            const int n = static_cast<int>(tab_paths_.size());
            SwitchToTab((tab_index_ - 1 + n) % n, true);
          }
          return 0;
        case kMenuCut:
          if (edit_) {
            SendMessageW(edit_, WM_CUT, 0, 0);
          }
          return 0;
        case kMenuCopy:
          if (edit_) {
            SendMessageW(edit_, WM_COPY, 0, 0);
          }
          return 0;
        case kMenuPaste:
          if (edit_) {
            if (!SmartPaste()) {
              SendMessageW(edit_, WM_PASTE, 0, 0);
            }
          }
          return 0;
        case kMenuSelectAll:
          if (edit_) {
            SendMessageW(edit_, EM_SETSEL, 0, -1);
          }
          return 0;
        case kMenuFind:
          ShowFind(false);
          return 0;
        case kMenuFindNext:
          FindAgain(true);
          return 0;
        case kMenuFindPrev:
          FindAgain(false);
          return 0;
        case kMenuReplace:
          ShowFind(true);
          return 0;
        case kMenuBigger:
          SetFontSize(font_pt_ + 1);
          return 0;
        case kMenuSmaller:
          if (font_pt_ > 8) {
            SetFontSize(font_pt_ - 1);
          }
          return 0;
        case kMenuOutline:
          ToggleOutline();
          return 0;
        case kMenuFocus:
          ToggleFocus();
          return 0;
        case kMenuTypewriter:
          ToggleTypewriter();
          return 0;
        case kMenuWrap:
          ToggleWordWrap();
          return 0;
        case kMenuTpl:
          InsertTemplate();
          return 0;
        case kMenuBold:
          ApplyBold();
          return 0;
        case kMenuItalic:
          ApplyItalic();
          return 0;
        case kMenuUnder:
          ApplyUnderline();
          return 0;
        case kMenuInsertLink:
          InsertMdLink();
          return 0;
        case kMenuInsertCode:
          InsertMdCodeFence();
          return 0;
        case kMenuInsertImage:
          InsertImageFromFile();
          return 0;
        case kMenuStrike:
          InsertMdStrike();
          return 0;
        case kMenuBullet:
          InsertMdBullet();
          return 0;
        case kMenuQuote:
          InsertMdQuote();
          return 0;
        case kMenuHRule:
          InsertMdHorizontalRule();
          return 0;
        case kMenuDateTime:
          InsertDateTime();
          return 0;
        case kMenuHeading1:
          InsertMdHeading(1);
          return 0;
        case kMenuHeading2:
          InsertMdHeading(2);
          return 0;
        case kMenuHeading3:
          InsertMdHeading(3);
          return 0;
        case kMenuAlwaysOnTop:
          ToggleAlwaysOnTop();
          return 0;
        case kMenuSyncScroll:
          ToggleSyncScroll();
          return 0;
        case kMenuEncUtf8:
          SetSaveEncoding(TextFileEncoding::Utf8);
          return 0;
        case kMenuEncUtf8Bom:
          SetSaveEncoding(TextFileEncoding::Utf8Bom);
          return 0;
        case kMenuEncAcp:
          SetSaveEncoding(TextFileEncoding::Acp);
          return 0;
        case kMenuCopyPath:
          CopyPathToClipboard(path_);
          return 0;
        default:
          break;
      }
      if (LOWORD(wparam) == kOutlineId && HIWORD(wparam) == LBN_SELCHANGE) {
        JumpToOutline();
        return 0;
      }
      if (LOWORD(wparam) == kSidebarNewId && HIWORD(wparam) == BN_CLICKED) {
        NewTab();
        RebuildSidebar();
        return 0;
      }
      if (LOWORD(wparam) == kSidebarRefreshId &&
          HIWORD(wparam) == BN_CLICKED) {
        RebuildSidebar();
        return 0;
      }
      if (LOWORD(wparam) == kSidebarSearchId && HIWORD(wparam) == EN_CHANGE) {
        SetTimer(hwnd_, kSidebarSearchTimer, 280, nullptr);
        return 0;
      }
      if (LOWORD(wparam) == kSidebarResultsId && HIWORD(wparam) == LBN_DBLCLK) {
        const int sel = static_cast<int>(
            SendMessageW(sidebar_results_, LB_GETCURSEL, 0, 0));
        OpenSidebarResult(sel);
        return 0;
      }
      if (LOWORD(wparam) == kEditId && HIWORD(wparam) == EN_CHANGE) {
        MarkDirty();
        SetTimer(hwnd_, kGutterTimer, 60, nullptr);
        if (preview_) {
          SchedulePreviewRefresh();
        }
        // P2-33: keep outline in sync while editing (debounced).
        if (outline_) {
          SetTimer(hwnd_, kOutlineTimer, 400, nullptr);
        }
        return 0;
      }
      if (LOWORD(wparam) == kEditId &&
          (HIWORD(wparam) == EN_VSCROLL || HIWORD(wparam) == EN_SELCHANGE)) {
        if (HIWORD(wparam) == EN_SELCHANGE && !wysiwyg_) {
          SetTimer(hwnd_, kStatusTimer, 120, nullptr);
        }
        SetTimer(hwnd_, kGutterTimer, 60, nullptr);
        if (preview_ && !syncing_scroll_) {
          SetTimer(hwnd_, kScrollTimer, 60, nullptr);
        }
        return 0;
      }
      break;
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wparam);
      const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      std::vector<std::wstring> notes;
      for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len + 1, L'\0');
        DragQueryFileW(drop, i, p.data(), len + 1);
        p.resize(len);
        const size_t dot = p.find_last_of(L'.');
        std::wstring lower =
            dot == std::wstring::npos ? L"" : p.substr(dot);
        for (auto& ch : lower) {
          if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
          }
        }
        // Image drops: copy into assets/ and insert relative markdown (Python).
        if (IsImageDropSuffix(lower)) {
          InsertImageFile(p);
        } else {
          notes.push_back(p);
        }
      }
      DragFinish(drop);
      if (!notes.empty()) {
        OpenPaths(notes);
      }
      return 0;
    }
    case WM_CONTEXTMENU: {
      if (reinterpret_cast<HWND>(wparam) == sidebar_) {
        POINT pt{};
        GetCursorPos(&pt);
        HandleSidebarContextMenu(pt);
        return 0;
      }
      break;
    }
    case WM_ERASEBKGND: {
      HDC hdc = reinterpret_cast<HDC>(wparam);
      RECT rc{};
      GetClientRect(hwnd_, &rc);
      FillRect(hdc, &rc, bg_brush_ ? bg_brush_
                                   : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
      if (sidebar_visible_ && !toast_ && rail_brush_) {
        const int tab_h = (tabs_ && IsWindowVisible(tabs_)) ? 48 : 0;
        const int status_h =
            (status_ && IsWindowVisible(status_)) ? kStatusBarH : 0;
        RECT rail{0, tab_h, 240, rc.bottom - status_h};
        FillRect(hdc, &rail, rail_brush_);
        HPEN pen = CreatePen(PS_SOLID, 1, border_color_);
        HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));
        MoveToEx(hdc, 240, rail.top, nullptr);
        LineTo(hdc, 240, rail.bottom);
        SelectObject(hdc, old);
        DeleteObject(pen);
        // Soft search field plate behind the edit control (keep inside chrome).
        if (sidebar_search_ && IsWindowVisible(sidebar_search_)) {
          RECT sr{};
          GetWindowRect(sidebar_search_, &sr);
          MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&sr), 2);
          InflateRect(&sr, 10, 4);
          if (sr.bottom > tab_h + 44) {
            sr.bottom = tab_h + 44;
          }
          Gdiplus::Graphics gg(hdc);
          gg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
          gg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
          Gdiplus::GraphicsPath path;
          GpAddRoundRect(&path, static_cast<float>(sr.left),
                         static_cast<float>(sr.top),
                         static_cast<float>(sr.right - sr.left),
                         static_cast<float>(sr.bottom - sr.top), 8.f);
          Gdiplus::SolidBrush plate(Gdiplus::Color(255, 255, 255, 255));
          gg.FillPath(&plate, &path);
          Gdiplus::Pen edge(Gdiplus::Color(255, 220, 225, 232), 1.f);
          gg.DrawPath(&edge, &path);
        }
      }
      return 1;
    }
    case WM_CTLCOLORSTATIC: {
      HDC hdc = reinterpret_cast<HDC>(wparam);
      SetTextColor(hdc, muted_color_);
      SetBkColor(hdc, toast_ ? RGB(255, 248, 220) : content_bg_);
      return reinterpret_cast<LRESULT>(bg_brush_);
    }
    case WM_CTLCOLORLISTBOX: {
      HDC hdc = reinterpret_cast<HDC>(wparam);
      SetTextColor(hdc, text_color_);
      SetBkColor(hdc, rail_bg_);
      return reinterpret_cast<LRESULT>(rail_brush_ ? rail_brush_ : bg_brush_);
    }
    case WM_CTLCOLOREDIT: {
      HDC hdc = reinterpret_cast<HDC>(wparam);
      // Sidebar search sits on the rail; editor uses white card.
      HWND ctl = reinterpret_cast<HWND>(lparam);
      if (ctl == sidebar_search_) {
        SetTextColor(hdc, text_color_);
        SetBkColor(hdc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(card_brush_ ? card_brush_ : bg_brush_);
      }
      SetTextColor(hdc, text_color_);
      SetBkColor(hdc, card_bg_);
      return reinterpret_cast<LRESULT>(card_brush_ ? card_brush_ : bg_brush_);
    }
    case WM_CTLCOLORBTN: {
      HDC hdc = reinterpret_cast<HDC>(wparam);
      SetTextColor(hdc, text_color_);
      SetBkColor(hdc, rail_bg_);
      return reinterpret_cast<LRESULT>(rail_brush_ ? rail_brush_ : bg_brush_);
    }
    case WM_SIZE:
      LayoutClient();
      return 0;
    case WM_APP + 60:
      if (pending_goto_line_ >= 0) {
        const int line = pending_goto_line_;
        pending_goto_line_ = -1;
        GotoLine(line);
        if (edit_) {
          const LRESULT start =
              SendMessageW(edit_, EM_LINEINDEX, static_cast<WPARAM>(line), 0);
          if (start >= 0) {
            const LRESULT len = SendMessageW(
                edit_, EM_LINELENGTH, static_cast<WPARAM>(start), 0);
            CHARRANGE cr{};
            cr.cpMin = static_cast<LONG>(start);
            cr.cpMax = static_cast<LONG>(start + (std::max)(LRESULT{0}, len));
            SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
            SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
          }
        }
      }
      return 0;
    case WM_TIMER:
      if (wparam == kPreviewTimer) {
        KillTimer(hwnd_, kPreviewTimer);
        UpdatePreview();
        return 0;
      }
      if (wparam == kScrollTimer) {
        KillTimer(hwnd_, kScrollTimer);
        SyncEditToPreview();
        return 0;
      }
      if (wparam == kSidebarSearchTimer) {
        KillTimer(hwnd_, kSidebarSearchTimer);
        RunSidebarSearch();
        return 0;
      }
      if (wparam == kOutlineTimer) {
        KillTimer(hwnd_, kOutlineTimer);
        RebuildOutline();
        return 0;
      }
      if (wparam == kStatusTimer) {
        UpdateStatusBar();
        // Keep periodic refresh at the steadier cadence.
        SetTimer(hwnd_, kStatusTimer, 800, nullptr);
        return 0;
      }
      if (wparam == kPersistTabsTimer) {
        PersistTabs();
        return 0;
      }
      if (wparam == kGutterTimer) {
        KillTimer(hwnd_, kGutterTimer);
        RefreshGutter();
        return 0;
      }
      return 0;
    case WM_CLOSE:
      ConfirmCloseAll();
      if (hwnd_ && IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
      }
      if (quit_on_close_) {
        DestroyWindow(hwnd_);
      }
      return 0;
    case WM_DESTROY:
      hwnd_ = nullptr;
      if (quit_on_close_) {
        PostQuitMessage(0);
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}
