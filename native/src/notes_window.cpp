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
constexpr int kTabCopyPath = 3105;
constexpr int kSideCopyPath = 3008;
constexpr int kOutlineId = 2002;
constexpr int kSidebarRefreshId = 2009;
constexpr int kModeStripId = 2010;
constexpr int kOutlineStripId = 2011;
constexpr int kOutlineStripW = 28;
constexpr int kOutlineExpandedW = 150;
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
constexpr int kStatusBarH = 22;

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
      L".reg",    L".inf",   L".url",    L".eml",      L".ics",
  };
  for (const wchar_t* sfx : kSfx) {
    if (lower == sfx) {
      return true;
    }
  }
  return false;
}

std::wstring LowerSuffix(const std::wstring& path) {
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos) {
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
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES};
  InitCommonControlsEx(&icc);

  // Notepad / DeskNote app window (default). Sticky toast is opt-in via menu.
  content_bg_ = RGB(244, 246, 249);
  card_bg_ = RGB(255, 255, 255);
  text_color_ = RGB(26, 35, 50);
  muted_color_ = RGB(100, 116, 139);
  bg_brush_ = CreateSolidBrush(content_bg_);
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
  AppendMenuW(file, MF_STRING, kMenuCloseTab, L"关闭页签\tCtrl+W");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuSave, L"保存\tCtrl+S");
  AppendMenuW(file, MF_STRING, kMenuSaveAs, L"另存为…\tCtrl+Shift+S");
  AppendMenuW(file, MF_STRING, kMenuReload, L"从磁盘重新加载\tF5");
  HMENU enc = CreatePopupMenu();
  AppendMenuW(enc, MF_STRING, kMenuEncUtf8, L"UTF-8");
  AppendMenuW(enc, MF_STRING, kMenuEncUtf8Bom, L"UTF-8 带 BOM");
  AppendMenuW(enc, MF_STRING, kMenuEncAcp, L"系统 ANSI（如 GBK）");
  AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(enc), L"保存编码");
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
  AppendMenuW(fmt, MF_STRING, kMenuInsertLink, L"插入链接\tCtrl+K");
  AppendMenuW(fmt, MF_STRING, kMenuInsertCode, L"插入代码块");
  AppendMenuW(fmt, MF_STRING, kMenuInsertImage, L"插入图片…");
  AppendMenuW(fmt, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(fmt, MF_STRING, kMenuHeading1, L"一级标题");
  AppendMenuW(fmt, MF_STRING, kMenuHeading2, L"二级标题");
  AppendMenuW(fmt, MF_STRING, kMenuHeading3, L"三级标题");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fmt),
              Tr("format").c_str());
  HMENU help = CreatePopupMenu();
  AppendMenuW(help, MF_STRING, kMenuHelp, L"DeskNote 使用说明\tF1");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), Tr("help").c_str());
  menu_ = menu;
  SetMenu(hwnd_, menu);
  BuildAccelerators();

  tabs_ = CreateWindowExW(
      0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 300,
      28, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabsId)),
      instance, nullptr);
  if (tabs_) {
    SetWindowTheme(tabs_, L"Explorer", nullptr);
    tabs_proc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        tabs_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(NotesWindow::TabsProc)));
  }

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
  outline_list_ = CreateWindowExW(
      0, L"LISTBOX", L"",
      WS_CHILD | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 6, 6,
      140, 220, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutlineId)),
      instance, nullptr);
  if (outline_list_) {
    ShowWindow(outline_list_, SW_HIDE);
  }
  outline_strip_ = CreateWindowExW(
      0, L"BUTTON", L"›\n大\n纲",
      WS_CHILD | BS_PUSHBUTTON | BS_MULTILINE | BS_CENTER | BS_VCENTER, 0, 0,
      kOutlineStripW, 120, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutlineStripId)), instance,
      nullptr);
  if (outline_strip_) {
    ShowWindow(outline_strip_, SW_HIDE);
  }
  sidebar_search_ = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | ES_AUTOHSCROLL,
      6, 6, 120, 22, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarSearchId)), instance,
      nullptr);
  sidebar_new_ = CreateWindowExW(
      0, L"BUTTON", L"+", WS_CHILD | BS_PUSHBUTTON, 130, 6, 26, 22, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarNewId)), instance,
      nullptr);
  sidebar_refresh_ = CreateWindowExW(
      0, L"BUTTON", L"↻", WS_CHILD | BS_PUSHBUTTON, 158, 6, 26, 22, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarRefreshId)), instance,
      nullptr);
  sidebar_ = CreateWindowExW(
      0, WC_TREEVIEWW, L"",
      WS_CHILD | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT |
          TVS_HASBUTTONS | TVS_SHOWSELALWAYS | TVS_EDITLABELS,
      6, 30, 150, 200, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarId)), instance,
      nullptr);
  if (sidebar_) {
    sidebar_proc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        sidebar_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(NotesWindow::SidebarProc)));
  }
  mode_strip_ = CreateWindowExW(
      0, L"BUTTON", L"预\n览",
      WS_CHILD | BS_PUSHBUTTON | BS_MULTILINE | BS_CENTER | BS_VCENTER, 0, 0,
      28, 120, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kModeStripId)),
      instance, nullptr);
  if (mode_strip_) {
    ShowWindow(mode_strip_, SW_HIDE);
  }
  sidebar_results_ = CreateWindowExW(
      0, L"LISTBOX", L"",
      WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
      6, 30, 150, 200, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarResultsId)), instance,
      nullptr);
  if (sidebar_) {
    ShowWindow(sidebar_, SW_HIDE);
  }
  if (sidebar_search_) {
    ShowWindow(sidebar_search_, SW_HIDE);
    SendMessageW(sidebar_search_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"搜索笔记库…"));
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
  {
    const std::wstring theme =
        SystemAppsUseLightTheme() ? L"light" : L"dark";
    web_.SetTheme(theme);
    preview_web_.SetTheme(theme);
  }
  LoadTabSession();
  LoadViewSettings();
  ApplyEditorFont();
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
  if (preview_ && PathLooksMarkdown()) {
    preview_ = false;  // ShowPreviewMode turns it back on + creates WebView.
    ShowPreviewMode();
  }
  if (always_on_top_ && !toast_) {
    CheckMenuItem(menu_, kMenuAlwaysOnTop, MF_BYCOMMAND | MF_CHECKED);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  }
  UpdateModeStrip();
  LayoutClient();
  RefreshTitle();
  UpdateStatusBar();
  PersistViewSettings();
  SetTimer(hwnd_, kStatusTimer, 400, nullptr);
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
  // Match Python DeskNote: Consolas 11pt, fallback Cascadia Mono.
  const wchar_t* face = has_consolas ? L"Consolas" : L"Cascadia Mono";
  font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, face);
  if (edit_) {
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  }
  if (status_ && font_) {
    SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
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
  if (edit_) {
    SendMessageW(edit_, EM_SETBKGNDCOLOR, 0, edit_bg);
  }
  InvalidateRect(hwnd_, nullptr, TRUE);
  if (edit_) {
    InvalidateRect(edit_, nullptr, TRUE);
  }
  if (status_) {
    InvalidateRect(status_, nullptr, TRUE);
  }
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
  content_bg_ = pick("content", RGB(244, 246, 249));
  card_bg_ = pick("card", RGB(255, 255, 255));
  text_color_ = pick("text", RGB(26, 35, 50));
  muted_color_ = pick("muted", RGB(100, 116, 139));
  ApplyChrome();
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
    title += L" · WYSIWYG";
  }
  SetWindowTextW(hwnd_, title.c_str());
}

void NotesWindow::Destroy() {
  Save();
  PersistTabs();
  PersistViewSettings();
  if (hwnd_) {
    KillTimer(hwnd_, kStatusTimer);
  }
  web_.Destroy();
  preview_web_.Destroy();
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
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

void NotesWindow::ApplyBold() {
  if (PathLooksMarkdown()) {
    WrapSelectionMarkdown(L"**", L"**");
  } else {
    ApplyCharEffect(edit_, CFM_BOLD, CFE_BOLD, true);
  }
}

void NotesWindow::ApplyItalic() {
  if (PathLooksMarkdown()) {
    WrapSelectionMarkdown(L"*", L"*");
  } else {
    ApplyCharEffect(edit_, CFM_ITALIC, CFE_ITALIC, true);
  }
}

void NotesWindow::ApplyUnderline() {
  if (PathLooksMarkdown()) {
    WrapSelectionMarkdown(L"<u>", L"</u>");
  } else {
    ApplyCharEffect(edit_, CFM_UNDERLINE, CFE_UNDERLINE, true);
  }
}

void NotesWindow::InsertMdLink() {
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
  if (!edit_) {
    return;
  }
  WrapSelectionMarkdown(L"```\r\n", L"\r\n```");
}

void NotesWindow::InsertMdHeading(int level) {
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
  if (!tabs_ || tab_index_ < 0 ||
      tab_index_ >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  TCITEMW ti{};
  ti.mask = TCIF_TEXT;
  std::wstring label = TabLabel(tab_paths_[static_cast<size_t>(tab_index_)]);
  ti.pszText = label.data();
  TabCtrl_SetItem(tabs_, tab_index_, &ti);
}

bool NotesWindow::InsertImageFile(const std::wstring& src_path) {
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
    MessageBoxW(hwnd_, L"无法导入图片。", L"插入图片", MB_OK | MB_ICONWARNING);
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
  if (!edit_) {
    return;
  }
  if (!PathLooksMarkdown()) {
    MessageBoxW(hwnd_, L"请先打开或保存一个 Markdown（.md）文档。", L"插入图片",
                MB_OK | MB_ICONINFORMATION);
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
    MessageBoxW(hwnd_, L"未插入任何图片（格式不受支持或复制失败）。", L"插入图片",
                MB_OK | MB_ICONWARNING);
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
    MessageBoxW(hwnd_, L"最多 12 个标签。", L"DeskNote",
                MB_OK | MB_ICONINFORMATION);
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
  if (name.size() > 16) {
    name = name.substr(0, 14) + L"…";
  }
  // Dirty star on the active tab (Python _refresh_tab_title).
  if (dirty_ && !path_.empty() &&
      _wcsicmp(NormalizePathKey(path).c_str(),
               NormalizePathKey(path_).c_str()) == 0) {
    name = L"*" + name;
  }
  // Leading ☆/★ is a pin affordance; TabsProc hit-tests the left edge.
  name = (PathIsPinned(path) ? L"★ " : L"☆ ") + name;
  // Trailing × is a close affordance; TabsProc hit-tests the right edge.
  name += L"  ×";
  return name;
}

void NotesWindow::RebuildTabControl() {
  if (!tabs_) {
    return;
  }
  while (TabCtrl_GetItemCount(tabs_) > 0) {
    TabCtrl_DeleteItem(tabs_, 0);
  }
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    std::wstring label = TabLabel(tab_paths_[i]);
    ti.pszText = label.data();
    TabCtrl_InsertItem(tabs_, static_cast<int>(i), &ti);
  }
  if (tab_index_ < 0 ||
      tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = 0;
  }
  TabCtrl_SetCurSel(tabs_, tab_index_);
}

void NotesWindow::PersistTabs() {
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
          if (!IsOpenableSuffix(LowerSuffix(name))) {
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
    Save();
  }
  SyncFromWysiwyg();
  // Remember the outgoing tab's WYSIWYG / wrap / caret (M8 per-tab state).
  EnsureTabStates();
  if (tab_index_ >= 0 && tab_index_ < static_cast<int>(tab_states_.size())) {
    tab_states_[static_cast<size_t>(tab_index_)].wysiwyg = wysiwyg_;
    tab_states_[static_cast<size_t>(tab_index_)].wrap = word_wrap_;
    if (edit_) {
      CHARRANGE cr{};
      SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
      tab_states_[static_cast<size_t>(tab_index_)].caret = cr.cpMin;
    }
  }
  tab_index_ = index;
  path_ = tab_paths_[static_cast<size_t>(index)];
  if (tabs_) {
    TabCtrl_SetCurSel(tabs_, tab_index_);
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
    web_.SetMarkdown(CurrentPlainText());
  } else {
    const LONG caret = tab_states_[static_cast<size_t>(tab_index_)].caret;
    if (edit_ && caret > 0) {
      CHARRANGE cr{caret, caret};
      SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
      SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    }
  }
  if (preview_ && PathLooksMarkdown()) {
    ShowPreviewMode();
  } else if (preview_ && !PathLooksMarkdown()) {
    // Non-md: keep editor visible even if global preview flag is on.
    if (preview_web_.Host()) {
      preview_web_.Hide();
    }
    if (edit_) {
      ShowWindow(edit_, SW_SHOW);
    }
  }
  RefreshTitle();
  if (outline_) {
    RebuildOutline();
  }
  if (sidebar_visible_) {
    HighlightSidebarPath(path_);
  }
  PersistTabs();
  UpdateStatusBar();
  UpdateModeStrip();
  SyncMdMenuState();
  ApplyViewMenuChecks();
  LayoutClient();
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
    MessageBoxW(hwnd_, L"最多 12 个标签。", L"DeskNote", MB_OK | MB_ICONINFORMATION);
    return;
  }
  Save();
  const std::wstring path = MakeNewNotePath();
  {
    std::ofstream out(path, std::ios::binary);
    out << u8"# \n\n";
  }
  tab_paths_.push_back(path);
  RebuildTabControl();
  SwitchToTab(static_cast<int>(tab_paths_.size()) - 1, false);
  if (sidebar_visible_) {
    RebuildSidebar();
  }
}

void NotesWindow::CloseTab() {
  if (!ConfirmSaveCurrent()) {
    return;
  }
  if (tab_paths_.size() <= 1) {
    // Closing the last tab starts a fresh note instead of closing to zero.
    const std::wstring path = MakeNewNotePath();
    {
      std::ofstream out(path, std::ios::binary);
      out << u8"# \n\n";
    }
    tab_paths_[0] = path;
    path_ = path;
    RebuildTabControl();
    SwitchToTab(0, false);
    return;
  }
  tab_paths_.erase(tab_paths_.begin() + tab_index_);
  if (tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
  }
  RebuildTabControl();
  SwitchToTab(tab_index_, false);
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
    MessageBoxW(hwnd_, L"文件不存在或无法访问。", L"重新加载",
                MB_OK | MB_ICONWARNING);
    return;
  }
  if (dirty_) {
    const int r = MessageBoxW(
        hwnd_, L"当前有未保存的修改。重新加载将丢弃这些修改，继续？",
        L"重新加载", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (r != IDYES) {
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
  loading_ = true;
  dirty_ = false;
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    std::ifstream legacy(JoinPath(NativeDataDir(), L"notes.txt"), std::ios::binary);
    if (!legacy) {
      loading_ = false;
      UpdateStatusBar();
      return;
    }
    std::ostringstream ss;
    ss << legacy.rdbuf();
    SetWindowTextW(edit_, WidenUtf8(ss.str()).c_str());
    loading_ = false;
    UpdateStatusBar();
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
      return;
    }
    text_enc_ = enc;
    SetWindowTextW(edit_, wide.c_str());
  }
  loading_ = false;
  dirty_ = false;
  UpdateStatusBar();
}

bool NotesWindow::OfferElevatedSave(const std::wstring& path,
                                    const std::wstring& text) {
  const int choice = MessageBoxW(
      hwnd_,
      (L"「" + path +
       L"」受系统保护，无法直接保存。\n\n"
       L"是否以管理员权限重启 DeskNote 并保存？\n"
       L"（将弹出 UAC 确认框，重启后自动保存该文件）")
          .c_str(),
      L"保存", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
  if (choice != IDYES) {
    return false;
  }
  if (!WriteElevatedPending(path, text)) {
    MessageBoxW(hwnd_, L"无法准备提权保存的暂存内容。", L"笔记",
                MB_OK | MB_ICONWARNING);
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
    MessageBoxW(hwnd_,
                L"已取消提权保存（UAC 未确认）。\n"
                L"可尝试以管理员身份运行 DeskNote 后重试。",
                L"保存", MB_OK | MB_ICONWARNING);
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
  if (!mode_strip_) {
    return;
  }
  const bool show =
      PathLooksMarkdown() && !focus_ && !toast_ && !wysiwyg_;
  ShowWindow(mode_strip_, show ? SW_SHOW : SW_HIDE);
  if (!show) {
    return;
  }
  // Vertical label: while previewing offer "编辑", else "预览".
  SetWindowTextW(mode_strip_, preview_ ? L"编\n辑" : L"预\n览");
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
      MessageBoxW(hwnd_, L"无法启动预览（WebView2 不可用）。", L"预览",
                  MB_OK | MB_ICONWARNING);
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
  if (!PathLooksMarkdown() || focus_) {
    return;
  }
  if (preview_) {
    ShowEditMode();
  } else {
    ShowPreviewMode();
  }
}

void NotesWindow::ShowEditMode() {
  if (wysiwyg_) {
    ToggleWysiwyg();
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

void NotesWindow::Save() {
  SyncFromWysiwyg();
  if (!edit_) {
    return;
  }
  if (path_.empty()) {
    SaveAs();
    return;
  }
  const std::wstring plain = CurrentPlainText();
  const bool textish =
      path_.size() >= 3 &&
      (_wcsicmp(path_.c_str() + path_.size() - 3, L".md") == 0 ||
       (path_.size() >= 4 &&
        _wcsicmp(path_.c_str() + path_.size() - 4, L".txt") == 0) ||
       (path_.size() >= 9 &&
        _wcsicmp(path_.c_str() + path_.size() - 9, L".markdown") == 0));
  if (textish) {
    std::string bytes;
    if (!EncodeTextFileBytes(plain, text_enc_, &bytes)) {
      MessageBoxW(hwnd_, L"无法按原编码写回文件（字符无法映射到 ANSI/GBK）。",
                  L"保存失败", MB_OK | MB_ICONWARNING);
      return;
    }
    if (!WriteFileBytes(path_, bytes.data(), bytes.size())) {
      if (LastWriteWasAccessDenied() && OfferElevatedSave(path_, plain)) {
        return;
      }
      MessageBoxW(hwnd_, L"保存失败。", L"笔记", MB_OK | MB_ICONWARNING);
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
    MessageBoxW(hwnd_, L"保存失败。", L"笔记", MB_OK | MB_ICONWARNING);
    return;
  }
  dirty_ = false;
  RefreshTitle();
  RefreshCurrentTabLabel();
  UpdateStatusBar();
}

void NotesWindow::MarkDirty() {
  if (!loading_) {
    dirty_ = true;
    RefreshTitle();
    RefreshCurrentTabLabel();
    UpdateStatusBar();
  }
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
  if (edit_) {
    CHARRANGE cr{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    const LONG caret = cr.cpMax;
    line = static_cast<int>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, caret)) + 1;
    const LONG line_start =
        static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, line - 1, 0));
    col = static_cast<int>(caret - line_start) + 1;
    std::wstring stats_src;
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
    CountTextStats(stats_src, &chars, &words);
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
  SetWindowTextW(status_, line_buf);
}

int NotesWindow::TabIndexFromPoint(POINT client) const {
  if (!tabs_) {
    return -1;
  }
  TCHITTESTINFO ht{};
  ht.pt = client;
  ht.flags = 0;
  return TabCtrl_HitTest(tabs_, &ht);
}

void NotesWindow::CloseTabAt(int index) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  if (index != tab_index_) {
    SwitchToTab(index, true);
  }
  CloseTab();
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
  if (tabs_) {
    TabCtrl_SetCurSel(tabs_, tab_index_);
  }
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
    CloseTabAt(index);
  } else if (cmd == kTabPin) {
    TogglePinPath(path);
    RebuildTabControl();
  } else if (cmd == kTabReveal) {
    const std::wstring arg = L"/select,\"" + path + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", arg.c_str(), nullptr,
                  SW_SHOWNORMAL);
  } else if (cmd == kTabCopyPath) {
    if (OpenClipboard(hwnd_)) {
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
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring name =
        slash == std::wstring::npos ? path : path.substr(slash + 1);
    const std::wstring msg =
        L"确定删除「" + name + L"」？\n此操作不可撤销。";
    if (MessageBoxW(hwnd_, msg.c_str(), L"删除",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) {
      return;
    }
    if (!RecyclePathQuiet(hwnd_, path)) {
      MessageBoxW(hwnd_, L"无法删除。", L"删除", MB_OK | MB_ICONWARNING);
      return;
    }
    for (size_t i = 0; i < pinned_paths_.size(); ++i) {
      if (_wcsicmp(pinned_paths_[i].c_str(), path.c_str()) == 0) {
        pinned_paths_.erase(pinned_paths_.begin() +
                            static_cast<ptrdiff_t>(i));
        break;
      }
    }
    if (index != tab_index_) {
      // Close without switching first if not current: remove directly.
      tab_paths_.erase(tab_paths_.begin() + index);
      if (tab_index_ > index) {
        --tab_index_;
      }
      if (tab_paths_.empty()) {
        tab_paths_.push_back(MakeNewNotePath());
        tab_index_ = 0;
        path_ = tab_paths_[0];
        RebuildTabControl();
        SwitchToTab(0, false);
      } else {
        RebuildTabControl();
        SwitchToTab(tab_index_, false);
      }
      PersistTabs();
      return;
    }
    CloseTab();
    PersistTabs();
  }
}

std::wstring NotesWindow::CurrentPlainText() const {
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
  PreviewMarkdownText(hwnd_, CurrentPlainText(), L"便签预览");
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
  // P1-21: reuse MarkdownToHtmlDocument (headings/lists/bold/italic/code).
  const std::wstring title =
      path_.empty() ? L"DeskTidy" : TabLabel(path_);
  const std::wstring html =
      MarkdownToHtmlDocument(CurrentPlainText(), title);
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
      MessageBoxW(hwnd_, L"无法写入临时文件。", L"导出", MB_OK | MB_ICONERROR);
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
    const int r = MessageBoxW(
        hwnd_,
        (L"未找到 pandoc。是否改为导出纯 Markdown？\n\n目标：\n" + md_dst).c_str(),
        L"导出", MB_YESNO | MB_ICONQUESTION);
    if (r == IDYES) {
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
    MessageBoxW(hwnd_, L"pandoc 导出失败，未打开任何文件。", L"导出",
                MB_OK | MB_ICONERROR);
    return;
  }
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &fad);
  const ULONGLONG sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) |
                       fad.nFileSizeLow;
  if (sz == 0) {
    MessageBoxW(hwnd_, L"pandoc 导出的文件为空，未打开。", L"导出",
                MB_OK | MB_ICONERROR);
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
    MessageBoxW(hwnd_, L"剪贴板里没有 HTML。", L"便签", MB_OK);
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
  if (!wysiwyg_ || !web_.Ready()) {
    return;
  }
  const std::wstring md = web_.PullMarkdown();
  if (edit_) {
    SetWindowTextW(edit_, md.c_str());
  }
}

void NotesWindow::ToggleWysiwyg() {
  if (!web_.Available()) {
    const DWORD start = GetTickCount();
    MSG msg{};
    while (!web_.Available() && GetTickCount() - start < 4000) {
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      Sleep(15);
    }
  }
  if (!web_.Available()) {
    MessageBoxW(hwnd_,
                L"无法启动 WebView2。请安装 Microsoft Edge WebView2 Runtime，"
                L"并确认 assets\\md_preview\\wysiwyg_native.html 存在。",
                L"Toast 编辑器", MB_OK | MB_ICONWARNING);
    return;
  }
  wysiwyg_ = !wysiwyg_;
  if (wysiwyg_) {
    if (preview_) {
      preview_ = false;
      if (menu_) {
        CheckMenuItem(menu_, kMenuPreviewPane, MF_BYCOMMAND | MF_UNCHECKED);
      }
    }
    const std::wstring text = CurrentPlainText();
    web_.SetMarkdown(text);
    SetWindowPos(hwnd_, nullptr, 0, 0, 720, 560,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW);
  } else {
    SyncFromWysiwyg();
  }
  RefreshTitle();
  EnsureTabStates();
  if (tab_index_ >= 0 && tab_index_ < static_cast<int>(tab_states_.size())) {
    tab_states_[static_cast<size_t>(tab_index_)].wysiwyg = wysiwyg_;
  }
  ApplyWysiwygVisibility();
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
  LayoutClient();
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
      MessageBoxW(hwnd_, L"无法启动预览（WebView2 不可用）。", L"预览",
                  MB_OK | MB_ICONWARNING);
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
      MessageBoxW(hwnd_, L"无法启动 PDF 导出（WebView2 不可用）。", L"导出 PDF",
                  MB_OK | MB_ICONWARNING);
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
    MessageBoxW(hwnd_,
                (L"PDF 导出失败：" + (err.empty() ? L"未知错误" : err)).c_str(),
                L"导出 PDF", MB_OK | MB_ICONERROR);
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
    MessageBoxW(hwnd_, status.empty() ? L"未找到。" : status.c_str(), L"查找",
                MB_OK | MB_ICONINFORMATION);
  }
}

void NotesWindow::LayoutClient() {
  if (!hwnd_ || !edit_) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  int top = 6;
  if (tabs_ && !toast_) {
    MoveWindow(tabs_, 0, 0, rc.right, 28, TRUE);
    ShowWindow(tabs_, SW_SHOW);
    top = 32;
  } else if (tabs_) {
    ShowWindow(tabs_, SW_HIDE);
  }
  int left = 6;
  int right = rc.right - 6;
  int bottom = rc.bottom - 6;
  if (status_ && !toast_) {
    bottom = rc.bottom - kStatusBarH - 2;
    MoveWindow(status_, 0, rc.bottom - kStatusBarH, rc.right, kStatusBarH,
               TRUE);
    ShowWindow(status_, SW_SHOW);
  } else if (status_) {
    ShowWindow(status_, SW_HIDE);
  }
  if (sidebar_visible_ && sidebar_ && !wysiwyg_) {
    const int side = 170;
    const int chrome_h = 26;
    const int btn_w = 26;
    if (sidebar_search_) {
      MoveWindow(sidebar_search_, 6, top, side - (btn_w * 2 + 6), chrome_h,
                 TRUE);
      ShowWindow(sidebar_search_, SW_SHOW);
    }
    if (sidebar_new_) {
      MoveWindow(sidebar_new_, 6 + side - (btn_w * 2 + 2), top, btn_w, chrome_h,
                 TRUE);
      ShowWindow(sidebar_new_, SW_SHOW);
    }
    if (sidebar_refresh_) {
      MoveWindow(sidebar_refresh_, 6 + side - btn_w, top, btn_w, chrome_h,
                 TRUE);
      ShowWindow(sidebar_refresh_, SW_SHOW);
    }
    const int body_top = top + chrome_h + 4;
    wchar_t search_buf[256] = {};
    if (sidebar_search_) {
      GetWindowTextW(sidebar_search_, search_buf, 256);
    }
    const bool searching = search_buf[0] != 0;
    if (searching && sidebar_results_) {
      MoveWindow(sidebar_results_, 6, body_top, side, bottom - body_top, TRUE);
      ShowWindow(sidebar_results_, SW_SHOW);
      ShowWindow(sidebar_, SW_HIDE);
    } else {
      MoveWindow(sidebar_, 6, body_top, side, bottom - body_top, TRUE);
      ShowWindow(sidebar_, SW_SHOW);
      if (sidebar_results_) {
        ShowWindow(sidebar_results_, SW_HIDE);
      }
    }
    left = 6 + side + 6;
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
  // Outline: expanded = list + ‹ strip; collapsed = ›大纲 strip only (Python).
  // Only in edit/source mode — full preview hides the outline chrome.
  const bool show_outline_chrome = outline_strip_ && !wysiwyg_ && !focus_ &&
                                   !toast_ && !preview_;
  if (show_outline_chrome) {
    const int pane_h = (std::max)(80, bottom - top);
    if (outline_ && outline_list_) {
      MoveWindow(outline_list_, left, top, kOutlineExpandedW, pane_h, TRUE);
      ShowWindow(outline_list_, SW_SHOW);
      left += kOutlineExpandedW;
      MoveWindow(outline_strip_, left, top, kOutlineStripW, pane_h, TRUE);
      SetWindowTextW(outline_strip_, L"‹");
      ShowWindow(outline_strip_, SW_SHOW);
      left += kOutlineStripW + 6;
    } else {
      if (outline_list_) {
        ShowWindow(outline_list_, SW_HIDE);
      }
      MoveWindow(outline_strip_, left, top, kOutlineStripW, pane_h, TRUE);
      SetWindowTextW(outline_strip_, L"›\n大\n纲");
      ShowWindow(outline_strip_, SW_SHOW);
      left += kOutlineStripW + 6;
    }
  } else {
    if (outline_list_) {
      ShowWindow(outline_list_, SW_HIDE);
    }
    if (outline_strip_) {
      ShowWindow(outline_strip_, SW_HIDE);
    }
  }
  if (focus_ && !wysiwyg_) {
    const int pad_x =
        (std::max)(24, (static_cast<int>(rc.right) - 520) / 2);
    left = (std::max)(left, pad_x);
    right = static_cast<int>(rc.right) - pad_x;
    top = (std::max)(top, top + 12);
    bottom = static_cast<int>(rc.bottom) - 28;
  }
  // Right-edge 编辑↔预览 strip (Python notepadMdModeStrip).
  const bool show_strip =
      mode_strip_ && PathLooksMarkdown() && !focus_ && !toast_ && !wysiwyg_;
  if (show_strip) {
    const int strip_w = 28;
    MoveWindow(mode_strip_, rc.right - strip_w, top, strip_w,
               (std::max)(80, bottom - top), TRUE);
    ShowWindow(mode_strip_, SW_SHOW);
    right = rc.right - strip_w - 4;
  } else if (mode_strip_) {
    ShowWindow(mode_strip_, SW_HIDE);
  }
  const bool md_preview =
      preview_ && PathLooksMarkdown() && !wysiwyg_ && !focus_;
  if (wysiwyg_) {
    if (edit_) {
      ShowWindow(edit_, SW_HIDE);
    }
    if (preview_web_.Host()) {
      preview_web_.Hide();
    }
    if (web_.Host()) {
      MoveWindow(web_.Host(), left, top, (std::max)(40, right - left),
                 (std::max)(40, bottom - top), TRUE);
    } else {
      web_.Resize();
    }
  } else if (md_preview) {
    // Full-width preview (Python preview mode), not side-by-side.
    if (edit_) {
      ShowWindow(edit_, SW_HIDE);
    }
    if (preview_web_.Host()) {
      MoveWindow(preview_web_.Host(), left, top, (std::max)(40, right - left),
                 (std::max)(40, bottom - top), TRUE);
      preview_web_.Show();
      preview_web_.Resize();
    }
  } else {
    if (preview_web_.Host() && !md_preview) {
      preview_web_.Hide();
    }
    if (edit_) {
      ShowWindow(edit_, SW_SHOW);
      MoveWindow(edit_, left, top, (std::max)(40, right - left),
                 (std::max)(40, bottom - top), TRUE);
    }
  }
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
    SendMessageW(edit_, EM_REPLACESEL, TRUE,
                 reinterpret_cast<LPARAM>(replace_with_));
  }
  return RunFindOnce(true, wrap, match_case, whole, status_out);
}

int NotesWindow::RunReplaceAll(bool match_case, bool whole) {
  if (!edit_ || find_what_[0] == 0) {
    return 0;
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
}  // namespace

int NotesWindow::RunReplaceAllTabs(bool match_case, bool whole) {
  if (find_what_[0] == 0) {
    return 0;
  }
  int total = RunReplaceAll(match_case, whole);
  const std::wstring find = find_what_;
  const std::wstring repl = replace_with_;
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
    const int n = ReplaceAllInWideString(&wide, find, repl, match_case, whole);
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
  dlg.disable_owner = false;  // Modeless-like: keep editor usable (Python).
  dlg.owner_accel = accel_;
  dlg.user_data_subdir = L"webview2-notes-find";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\notes_find.html");
  if (dlg.html_path.empty()) {
    MessageBoxW(hwnd_, L"找不到 notes_find.html", L"查找",
                MB_OK | MB_ICONWARNING);
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
                   replace ? 360 : 300);
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
  Show();
  if (toast_) {
    return;
  }
  ShowWindow(hwnd_, SW_RESTORE);
  SetForegroundWindow(hwnd_);
  ShowWindow(hwnd_, SW_MAXIMIZE);
  SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  // Match Python show_notepad: Markdown opens in preview by default.
  if (PathLooksMarkdown() && !preview_) {
    ShowPreviewMode();
  }
}

bool NotesWindow::ConfirmSaveCurrent() {
  SyncFromWysiwyg();
  if (!dirty_) {
    return true;
  }
  const std::wstring msg =
      L"是否保存对「" + TabLabel(path_) + L"」的更改？";
  const int r = MessageBoxW(hwnd_, msg.c_str(), L"便签",
                            MB_YESNOCANCEL | MB_ICONQUESTION);
  if (r == IDYES) {
    Save();
    return true;
  }
  if (r == IDNO) {
    dirty_ = false;
    return true;
  }
  return false;
}

bool NotesWindow::ConfirmCloseAll() {
  // Match Python closeEvent: silent autosave, no discard prompt.
  // Tab switches already Save(); only the active tab can be dirty.
  SyncFromWysiwyg();
  if (dirty_) {
    Save();
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
  if (hit.line >= 0) {
    GotoLine(hit.line);
  }
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
          } else if (IsOpenableSuffix(LowerSuffix(fd.cFileName))) {
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
  walk(root, root_item, 0);
  TreeView_Expand(sidebar_, root_item, TVE_EXPAND);
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
    MessageBoxW(hwnd_, L"名称不能包含路径分隔符。", L"重命名",
                MB_OK | MB_ICONWARNING);
    return false;
  }
  const std::wstring new_path = dir + L"\\" + new_stem + suffix;
  if (GetFileAttributesW(new_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    MessageBoxW(hwnd_, L"该名称已存在。", L"重命名", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (!MoveFileW(path.c_str(), new_path.c_str())) {
    MessageBoxW(hwnd_, L"无法重命名。", L"重命名", MB_OK | MB_ICONWARNING);
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
  const int r = MessageBoxW(hwnd_, L"删除此文件？将移入回收站。", L"删除",
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
  if (r != IDYES || !RecyclePathQuiet(hwnd_, path)) {
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
  if (self && msg == WM_LBUTTONDBLCLK) {
    self->PromptGotoLine();
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
  if (self) {
    if (msg == WM_LBUTTONDOWN) {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int hit = self->TabIndexFromPoint(pt);
      if (hit >= 0) {
        RECT item{};
        if (TabCtrl_GetItemRect(hwnd, hit, &item)) {
          if (pt.x >= item.right - 18) {
            // Click on trailing × — close without starting a drag.
            self->CloseTabAt(hit);
            self->tab_drag_from_ = -1;
            self->tab_dragging_ = false;
            return 0;
          }
          if (pt.x <= item.left + 20 &&
              hit < static_cast<int>(self->tab_paths_.size())) {
            // Click on leading ☆/★ — toggle pin (Python _TabPinButton).
            self->TogglePinPath(self->tab_paths_[static_cast<size_t>(hit)]);
            self->tab_drag_from_ = -1;
            self->tab_dragging_ = false;
            return 0;
          }
        }
      }
      self->tab_drag_from_ = hit;
      self->tab_dragging_ = false;
      self->tab_drag_origin_ = pt;
      // Fall through so SysTabControl still selects the tab.
    }
    if (msg == WM_MOUSEMOVE && (wparam & MK_LBUTTON) &&
        self->tab_drag_from_ >= 0) {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
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
        return 0;
      }
    }
    if (msg == WM_LBUTTONUP) {
      self->tab_drag_from_ = -1;
      self->tab_dragging_ = false;
    }
    if (msg == WM_MBUTTONUP) {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int idx = self->TabIndexFromPoint(pt);
      if (idx >= 0) {
        self->CloseTabAt(idx);
      }
      return 0;
    }
    if (msg == WM_LBUTTONDBLCLK) {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (self->TabIndexFromPoint(pt) < 0) {
        self->NewTab();
        return 0;
      }
    }
    if (msg == WM_RBUTTONUP || msg == WM_CONTEXTMENU) {
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
      const int idx = self->TabIndexFromPoint(client);
      if (idx >= 0) {
        self->HandleTabContextMenu(idx, pt);
        return 0;
      }
    }
  }
  if (self && self->tabs_proc_) {
    return CallWindowProcW(self->tabs_proc_, hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK NotesWindow::EditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                        LPARAM lparam) {
  auto* self = reinterpret_cast<NotesWindow*>(
      GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (self && msg == WM_PASTE && self->SmartPaste()) {
    return 0;
  }
  if (self && msg == WM_KEYDOWN) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
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
    case WM_NOTIFY: {
      const auto* hdr = reinterpret_cast<LPNMHDR>(lparam);
      if (hdr && hdr->idFrom == kTabsId && hdr->code == TCN_SELCHANGE) {
        const int sel = TabCtrl_GetCurSel(tabs_);
        if (sel != tab_index_) {
          SwitchToTab(sel, true);
        }
        return 0;
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
        UpdateStatusBar();
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
        case kMenuCloseTab:
          CloseTab();
          return 0;
        case kMenuSave:
          Save();
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
        if (HIWORD(wparam) == EN_SELCHANGE) {
          UpdateStatusBar();
        }
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
    case WM_CTLCOLORSTATIC: {
      HDC hdc = reinterpret_cast<HDC>(wparam);
      SetTextColor(hdc, muted_color_);
      SetBkColor(hdc, toast_ ? RGB(255, 248, 220) : content_bg_);
      return reinterpret_cast<LRESULT>(bg_brush_);
    }
    case WM_SIZE:
      LayoutClient();
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
        return 0;
      }
      return 0;
    case WM_CLOSE:
      SyncFromWysiwyg();
      if (!ConfirmCloseAll()) {
        return 0;
      }
      if (quit_on_close_) {
        DestroyWindow(hwnd_);
      } else {
        Hide();
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
