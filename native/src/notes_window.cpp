#include "notes_window.h"

#include "desknote_help.h"
#include "desktop_shell.h"
#include "i18n.h"
#include "markdown_preview.h"
#include "mist_webview.h"
#include "notes_library.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "tool_paths.h"
#include "tools_misc.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <Richedit.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <Uxtheme.h>
#include <gdiplus.h>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

#pragma comment(lib, "comctl32.lib")

#include <algorithm>
#include <cstring>
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
constexpr int kOutlineId = 2002;
constexpr int kTabsId = 2003;
constexpr int kSidebarId = 2004;
constexpr UINT_PTR kPreviewTimer = 1;
constexpr UINT_PTR kScrollTimer = 2;

// Sidebar context-menu command ids (kept distinct from window menu ids).
constexpr int kSideOpen = 3001;
constexpr int kSideRename = 3002;
constexpr int kSideDelete = 3003;
constexpr int kSideReveal = 3004;
constexpr int kSideRefresh = 3005;

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

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = NotesWindow::WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(255, 248, 220));
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  const int sw = GetSystemMetrics(SM_CXSCREEN);
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClassName, Tr("notes_title").c_str(),
      WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, sw - 360, 100, 320,
      300, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }
  SetLayeredWindowAttributes(hwnd_, 0, 245, LWA_ALPHA);
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

  HMENU menu = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, kMenuNewTab, Tr("notes_new_tab").c_str());
  AppendMenuW(file, MF_STRING, kMenuOpen, Tr("notes_open").c_str());
  AppendMenuW(file, MF_STRING, kMenuCloseTab, Tr("notes_close_tab").c_str());
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuSave, Tr("save").c_str());
  AppendMenuW(file, MF_STRING, kMenuSaveAs, Tr("notes_save_as").c_str());
  AppendMenuW(file, MF_STRING, kMenuLibrary, Tr("notes_library").c_str());
  AppendMenuW(file, MF_STRING, kMenuOpenFolder, Tr("notes_open_folder").c_str());
  AppendMenuW(file, MF_STRING, kMenuSearchLib, Tr("notes_search_lib").c_str());
  AppendMenuW(file, MF_STRING, kMenuPreview, Tr("notes_preview").c_str());
  AppendMenuW(file, MF_STRING, kMenuExportMd, Tr("notes_export_md").c_str());
  AppendMenuW(file, MF_STRING, kMenuExportHtml, Tr("notes_export_html").c_str());
  AppendMenuW(file, MF_STRING, kMenuPandoc, Tr("notes_pandoc").c_str());
  AppendMenuW(file, MF_STRING, kMenuOpenWith, Tr("notes_open_with").c_str());
  AppendMenuW(file, MF_STRING, kMenuPasteHtml, Tr("notes_paste_html").c_str());
  AppendMenuW(file, MF_STRING, kMenuToast, Tr("notes_toast").c_str());
  AppendMenuW(file, MF_STRING, kMenuWysiwyg, Tr("notes_wysiwyg").c_str());
  AppendMenuW(file, MF_STRING, kMenuPreviewPane, Tr("notes_preview_pane").c_str());
  AppendMenuW(file, MF_STRING, kMenuExportPdf, Tr("notes_export_pdf").c_str());
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuTpl, Tr("notes_template").c_str());
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), Tr("file").c_str());
  HMENU editm = CreatePopupMenu();
  AppendMenuW(editm, MF_STRING, kMenuFind, Tr("notes_find").c_str());
  AppendMenuW(editm, MF_STRING, kMenuFindNext, Tr("notes_find_next").c_str());
  AppendMenuW(editm, MF_STRING, kMenuFindPrev, Tr("notes_find_prev").c_str());
  AppendMenuW(editm, MF_STRING, kMenuReplace, Tr("notes_replace").c_str());
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(editm),
              Tr("edit").c_str());
  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, kMenuBigger, Tr("notes_font_bigger").c_str());
  AppendMenuW(view, MF_STRING, kMenuSmaller, Tr("notes_font_smaller").c_str());
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kMenuOutline, Tr("notes_outline").c_str());
  AppendMenuW(view, MF_STRING, kMenuSidebar, Tr("notes_sidebar").c_str());
  AppendMenuW(view, MF_STRING, kMenuFocus, Tr("notes_focus").c_str());
  AppendMenuW(view, MF_STRING, kMenuTypewriter, Tr("notes_typewriter").c_str());
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view),
              Tr("view").c_str());
  HMENU fmt = CreatePopupMenu();
  AppendMenuW(fmt, MF_STRING, kMenuBold, Tr("bold").c_str());
  AppendMenuW(fmt, MF_STRING, kMenuItalic, Tr("italic").c_str());
  AppendMenuW(fmt, MF_STRING, kMenuUnder, Tr("underline").c_str());
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fmt),
              Tr("format").c_str());
  HMENU help = CreatePopupMenu();
  AppendMenuW(help, MF_STRING, kMenuHelp, Tr("notes_help").c_str());
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), Tr("help").c_str());
  menu_ = menu;
  SetMenu(hwnd_, menu);

  tabs_ = CreateWindowExW(
      0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 300,
      28, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabsId)),
      instance, nullptr);
  if (tabs_) {
    SetWindowTheme(tabs_, L"Explorer", nullptr);
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
  SendMessageW(edit_, EM_SETBKGNDCOLOR, 0, RGB(255, 248, 220));
  SetFontSize(font_pt_);
  outline_list_ = CreateWindowExW(
      0, L"LISTBOX", L"",
      WS_CHILD | WS_VSCROLL | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 6, 6,
      140, 220, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kOutlineId)),
      instance, nullptr);
  if (outline_list_) {
    ShowWindow(outline_list_, SW_HIDE);
  }
  sidebar_ = CreateWindowExW(
      0, WC_TREEVIEWW, L"",
      WS_CHILD | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT |
          TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
      6, 6, 150, 220, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarId)), instance,
      nullptr);
  if (sidebar_) {
    ShowWindow(sidebar_, SW_HIDE);
  }
  DragAcceptFiles(hwnd_, TRUE);
  web_.Create(hwnd_);
  LoadTabSession();
  if (tab_paths_.empty()) {
    tab_paths_.push_back(path_);
    tab_index_ = 0;
  }
  path_ = tab_paths_[static_cast<size_t>(tab_index_)];
  RebuildTabControl();
  Load();
  LayoutClient();
  return true;
}

void NotesWindow::SetFontSize(int pt) {
  font_pt_ = pt;
  if (font_) {
    DeleteObject(font_);
  }
  font_ = CreateFontW(font_pt_, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                      L"Segoe UI");
  if (edit_) {
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  }
}

void NotesWindow::Destroy() {
  Save();
  PersistTabs();
  web_.Destroy();
  preview_web_.Destroy();
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void NotesWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

void NotesWindow::OpenPaths(const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    Show();
    return;
  }
  Present();
  AdoptPath(paths[0]);
  for (size_t i = 1; i < paths.size(); ++i) {
    NewTab();
    AdoptPath(paths[i]);
  }
}

void NotesWindow::Hide() {
  Save();
  PersistTabs();
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

std::wstring NotesWindow::TabLabel(const std::wstring& path) const {
  const size_t slash = path.find_last_of(L"\\/");
  std::wstring name =
      slash == std::wstring::npos ? path : path.substr(slash + 1);
  if (name.empty()) {
    name = L"未命名";
  }
  if (name.size() > 18) {
    name = name.substr(0, 16) + L"…";
  }
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
  out << "  ]\n}\n";
}

void NotesWindow::LoadTabSession() {
  tab_paths_.clear();
  tab_index_ = 0;
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_tabs.json");
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  size_t pos = 0;
  while (true) {
    const size_t q1 = json.find('"', pos);
    if (q1 == std::string::npos) {
      break;
    }
    // Prefer paths that look like absolute Windows paths inside the tabs array.
    const size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) {
      break;
    }
    const std::string raw = json.substr(q1 + 1, q2 - q1 - 1);
    pos = q2 + 1;
    if (raw == "tabs" || raw == "current") {
      continue;
    }
    std::string unescaped;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\\' && i + 1 < raw.size()) {
        unescaped.push_back(raw[++i]);
      } else {
        unescaped.push_back(raw[i]);
      }
    }
    if (unescaped.size() >= 3 &&
        ((unescaped[1] == ':' && (unescaped[2] == '\\' || unescaped[2] == '/')) ||
         unescaped[0] == '\\')) {
      tab_paths_.push_back(WidenUtf8(unescaped));
    }
  }
  const size_t cur_key = json.find("\"current\"");
  if (cur_key != std::string::npos) {
    const size_t colon = json.find(':', cur_key);
    if (colon != std::string::npos) {
      tab_index_ = atoi(json.c_str() + colon + 1);
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
  // Remember the outgoing tab's WYSIWYG mode + caret (M8 per-tab state).
  EnsureTabStates();
  if (tab_index_ >= 0 && tab_index_ < static_cast<int>(tab_states_.size())) {
    tab_states_[static_cast<size_t>(tab_index_)].wysiwyg = wysiwyg_;
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
  if (preview_) {
    UpdatePreview();
  }
  SetWindowTextW(hwnd_, (L"便签 · " + TabLabel(path_)).c_str());
  if (outline_) {
    RebuildOutline();
  }
  PersistTabs();
}

std::wstring NotesWindow::MakeNewNotePath() {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf_s(name, L"note_%04d%02d%02d_%02d%02d%02d.md", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  return JoinPath(NotesLibraryDir(true), name);
}

void NotesWindow::NewTab() {
  if (tab_paths_.size() >= 12) {
    MessageBoxW(hwnd_, L"最多 12 个标签。", L"便签", MB_OK | MB_ICONINFORMATION);
    return;
  }
  Save();
  const std::wstring path = MakeNewNotePath();
  {
    std::ofstream out(path, std::ios::binary);
    out << "";
  }
  tab_paths_.push_back(path);
  RebuildTabControl();
  SwitchToTab(static_cast<int>(tab_paths_.size()) - 1, false);
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
      out << "";
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
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (_wcsicmp(tab_paths_[i].c_str(), path.c_str()) == 0) {
      SwitchToTab(static_cast<int>(i), true);
      return;
    }
  }
  Save();
  if (tab_paths_.empty()) {
    tab_paths_.push_back(path);
  } else if (tab_index_ >= 0 &&
             tab_index_ < static_cast<int>(tab_paths_.size())) {
    tab_paths_[static_cast<size_t>(tab_index_)] = path;
  } else {
    tab_paths_.push_back(path);
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
  }
  path_ = path;
  RebuildTabControl();
  Load();
  SetWindowTextW(hwnd_, (L"便签 · " + TabLabel(path_)).c_str());
  PersistTabs();
}

bool NotesWindow::IsOpen() const {
  return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_);
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
      return;
    }
    std::ostringstream ss;
    ss << legacy.rdbuf();
    SetWindowTextW(edit_, WidenUtf8(ss.str()).c_str());
    loading_ = false;
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string data = ss.str();
  if (data.rfind("{\\rtf", 0) == 0) {
    StreamCookie cookie;
    cookie.data = data;
    EDITSTREAM es{};
    es.dwCookie = reinterpret_cast<DWORD_PTR>(&cookie);
    es.pfnCallback = StreamInCb;
    SendMessageW(edit_, EM_STREAMIN, SF_RTF, reinterpret_cast<LPARAM>(&es));
  } else {
    SetWindowTextW(edit_, WidenUtf8(data).c_str());
  }
  loading_ = false;
}

void NotesWindow::Save() {
  SyncFromWysiwyg();
  if (!edit_) {
    return;
  }
  if (path_.size() >= 3 &&
      (_wcsicmp(path_.c_str() + path_.size() - 3, L".md") == 0 ||
       (path_.size() >= 4 &&
        _wcsicmp(path_.c_str() + path_.size() - 4, L".txt") == 0))) {
    std::ofstream out(path_, std::ios::binary);
    if (out) {
      out << NarrowUtf8(CurrentPlainText());
    }
    dirty_ = false;
    return;
  }
  StreamCookie cookie;
  EDITSTREAM es{};
  es.dwCookie = reinterpret_cast<DWORD_PTR>(&cookie);
  es.pfnCallback = StreamOutCb;
  const LRESULT streamed =
      SendMessageW(edit_, EM_STREAMOUT, SF_RTF, reinterpret_cast<LPARAM>(&es));
  std::ofstream out(path_, std::ios::binary);
  if (!out) {
    return;
  }
  if (streamed && !cookie.data.empty()) {
    out << cookie.data;
    dirty_ = false;
    return;
  }
  const int len = GetWindowTextLengthW(edit_);
  std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(edit_, wide.data(), len + 1);
  wide.resize(static_cast<size_t>(len));
  out << NarrowUtf8(wide);
  dirty_ = false;
}

void NotesWindow::MarkDirty() {
  if (!loading_) {
    dirty_ = true;
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
  const std::wstring text = CurrentPlainText();
  std::string body;
  std::wstring line;
  auto flush = [&](const std::wstring& raw) {
    std::wstring src = raw;
    while (!src.empty() && (src.back() == L'\r' || src.back() == L'\n')) {
      src.pop_back();
    }
    int level = 0;
    while (level < 3 && level < static_cast<int>(src.size()) &&
           src[static_cast<size_t>(level)] == L'#') {
      ++level;
    }
    std::wstring content = src;
    if (level > 0 && static_cast<size_t>(level) < src.size() &&
        src[static_cast<size_t>(level)] == L' ') {
      content = src.substr(static_cast<size_t>(level + 1));
    } else if (level > 0) {
      level = 0;
    }
    std::string escaped;
    const std::string utf = NarrowUtf8(content);
    for (unsigned char c : utf) {
      if (c == '<') {
        escaped += "&lt;";
      } else if (c == '>') {
        escaped += "&gt;";
      } else if (c == '&') {
        escaped += "&amp;";
      } else {
        escaped.push_back(static_cast<char>(c));
      }
    }
    if (level > 0) {
      body += "<h" + std::to_string(level) + ">" + escaped + "</h" +
              std::to_string(level) + ">\n";
    } else if (escaped.empty()) {
      body += "<br>\n";
    } else {
      body += "<p>" + escaped + "</p>\n";
    }
  };
  for (wchar_t ch : text) {
    if (ch == L'\n') {
      flush(line);
      line.clear();
    } else {
      line.push_back(ch);
    }
  }
  flush(line);
  std::ofstream out(file, std::ios::binary);
  if (!out) {
    return;
  }
  out << "<!DOCTYPE html><meta charset=\"utf-8\"><title>DeskTidy</title>\n"
      << "<body style=\"font-family:sans-serif;max-width:42em;margin:2em auto\">\n"
      << body << "</body>\n";
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

  std::wstring cmd = L"pandoc \"" + src + L"\" -o \"" + dst + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const BOOL started =
      CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
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
  if (toast_) {
    SetWindowPos(hwnd_, HWND_TOPMOST, 40, 40, 280, 140, SWP_SHOWWINDOW);
    SetWindowTextW(hwnd_, L"便签条");
  } else {
    SetWindowPos(hwnd_, HWND_NOTOPMOST, 80, 80, 320, 300, SWP_SHOWWINDOW);
    SetWindowTextW(hwnd_, L"便签");
  }
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
    SetWindowTextW(hwnd_, L"便签 · Toast WYSIWYG");
    SetWindowPos(hwnd_, nullptr, 0, 0, 720, 560,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_SHOWWINDOW);
  } else {
    SyncFromWysiwyg();
    SetWindowTextW(hwnd_, toast_ ? L"便签条" : L"便签");
  }
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
  if (tab_states_.size() != tab_paths_.size()) {
    tab_states_.resize(tab_paths_.size());
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
  if (!preview_ || !preview_web_.Ready()) {
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
  SetEditScrollRatio(ratio);
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
}

void NotesWindow::ExportPdf() {
  if (!preview_web_.Available()) {
    preview_web_.SetKind(NotesWebView::Kind::kViewer);
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

void NotesWindow::SearchLibrary() {
  const std::wstring picked = SearchNotesInLibrary(hwnd_);
  if (picked.empty()) {
    return;
  }
  AdoptPath(picked);
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
  AdoptPath(picked);
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
  if (sidebar_visible_ && sidebar_ && !wysiwyg_) {
    const int side = 170;
    MoveWindow(sidebar_, 6, top, side, bottom - top, TRUE);
    ShowWindow(sidebar_, SW_SHOW);
    left = 6 + side + 6;
  } else if (sidebar_) {
    ShowWindow(sidebar_, SW_HIDE);
  }
  if (outline_ && outline_list_ && !wysiwyg_) {
    const int side = 150;
    MoveWindow(outline_list_, left, top, side, bottom - top, TRUE);
    ShowWindow(outline_list_, SW_SHOW);
    left = left + side + 6;
  } else if (outline_list_) {
    ShowWindow(outline_list_, SW_HIDE);
  }
  if (focus_ && !wysiwyg_) {
    const int pad_x =
        (std::max)(24, (static_cast<int>(rc.right) - 520) / 2);
    left = (std::max)(left, pad_x);
    right = static_cast<int>(rc.right) - pad_x;
    top = (std::max)(top, top + 12);
    bottom = static_cast<int>(rc.bottom) - 28;
  }
  if (wysiwyg_) {
    if (web_.Host()) {
      MoveWindow(web_.Host(), left, top, (std::max)(40, right - left),
                 (std::max)(40, bottom - top), TRUE);
    } else {
      web_.Resize();
    }
  } else if (preview_) {
    const int total = (std::max)(80, right - left);
    const int edit_w = total / 2;
    const int pw = total - edit_w;
    MoveWindow(edit_, left, top, edit_w, (std::max)(40, bottom - top), TRUE);
    if (preview_web_.Host()) {
      MoveWindow(preview_web_.Host(), left + edit_w, top, pw,
                 (std::max)(40, bottom - top), TRUE);
    } else {
      preview_web_.Resize();
    }
  } else {
    MoveWindow(edit_, left, top, (std::max)(40, right - left),
               (std::max)(40, bottom - top), TRUE);
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

void NotesWindow::ShowFind(bool replace) {
  MistWebDialog dlg;
  dlg.owner = hwnd_;
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
      const int n = RunReplaceAll(find_match_case_, find_whole_word_);
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
  const std::wstring dir = NotesLibraryDir(true);
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
  ShowWindow(hwnd_, SW_RESTORE);
  SetForegroundWindow(hwnd_);
  ShowWindow(hwnd_, SW_MAXIMIZE);
  SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
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
  // Only the active tab can carry unsaved edits (switching tabs auto-saves).
  SyncFromWysiwyg();
  return ConfirmSaveCurrent();
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
  AdoptPath(path);
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
        };
        std::vector<Entry> entries;
        do {
          if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) {
              continue;
            }
            entries.push_back({fd.cFileName, dir + L"\\" + fd.cFileName, true});
          } else if (IsOpenableSuffix(LowerSuffix(fd.cFileName))) {
            entries.push_back({fd.cFileName, dir + L"\\" + fd.cFileName, false});
          }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) {
                    if (a.dir != b.dir) {
                      return a.dir;
                    }
                    return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
                  });
        for (const auto& e : entries) {
          HTREEITEM it = insert(parent, e.full, e.dir);
          if (e.dir && depth < 2) {
            walk(e.full, it, depth + 1);
          }
        }
      };

  HTREEITEM root_item = insert(TVI_ROOT, root, true);
  walk(root, root_item, 0);
  TreeView_Expand(sidebar_, root_item, TVE_EXPAND);
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
  AppendMenuW(m, MF_STRING | (is_file ? MF_ENABLED : MF_GRAYED), kSideRename,
              L"重命名…");
  AppendMenuW(m, MF_STRING | (is_file ? MF_ENABLED : MF_GRAYED), kSideDelete,
              L"删除（回收站）");
  AppendMenuW(m, MF_STRING | (is_file ? MF_ENABLED : MF_GRAYED), kSideReveal,
              L"在资源管理器中显示");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kSideRefresh, L"刷新");
  const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                                 hwnd_, nullptr);
  DestroyMenu(m);

  if (cmd == kSideOpen) {
    OpenSelectedSidebarItem();
  } else if (cmd == kSideRename) {
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
    const std::wstring new_stem =
        PromptText(hwnd_, L"重命名", L"新名称：", stem);
    if (new_stem.empty() || new_stem == stem) {
      return;
    }
    const std::wstring new_path = dir + L"\\" + new_stem + suffix;
    if (MoveFileW(path.c_str(), new_path.c_str())) {
      for (auto& p : tab_paths_) {
        if (_wcsicmp(p.c_str(), path.c_str()) == 0) {
          p = new_path;
        }
      }
      if (_wcsicmp(path_.c_str(), path.c_str()) == 0) {
        path_ = new_path;
      }
      RebuildSidebar();
      RebuildTabControl();
      PersistTabs();
    }
  } else if (cmd == kSideDelete) {
    const int r = MessageBoxW(hwnd_, L"删除此文件？将移入回收站。", L"删除",
                              MB_YESNO | MB_ICONQUESTION);
    if (r == IDYES && SendPathsToRecycleBin({path})) {
      for (size_t i = 0; i < tab_paths_.size(); ++i) {
        if (_wcsicmp(tab_paths_[i].c_str(), path.c_str()) == 0) {
          tab_paths_.erase(tab_paths_.begin() + i);
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
      SwitchToTab(tab_index_, false);
      PersistTabs();
    }
  } else if (cmd == kSideReveal) {
    const std::wstring arg = L"/select,\"" + path + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", arg.c_str(), nullptr,
                  SW_SHOWNORMAL);
  } else if (cmd == kSideRefresh) {
    RebuildSidebar();
  }
}

LRESULT CALLBACK NotesWindow::EditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                        LPARAM lparam) {
  auto* self = reinterpret_cast<NotesWindow*>(
      GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (self && msg == WM_PASTE && self->PasteImageToMarkdown()) {
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
    if (wparam == VK_F3) {
      self->FindAgain(!shift);
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
      self->PreviewMarkdown();
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
          TogglePreview();
          return 0;
        case kMenuExportPdf:
          ExportPdf();
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
          SetFontSize(font_pt_ + 2);
          return 0;
        case kMenuSmaller:
          if (font_pt_ > 10) {
            SetFontSize(font_pt_ - 2);
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
        case kMenuTpl:
          InsertTemplate();
          return 0;
        case kMenuBold:
          ApplyCharEffect(edit_, CFM_BOLD, CFE_BOLD, true);
          return 0;
        case kMenuItalic:
          ApplyCharEffect(edit_, CFM_ITALIC, CFE_ITALIC, true);
          return 0;
        case kMenuUnder:
          ApplyCharEffect(edit_, CFM_UNDERLINE, CFE_UNDERLINE, true);
          return 0;
        default:
          break;
      }
      if (LOWORD(wparam) == kOutlineId && HIWORD(wparam) == LBN_SELCHANGE) {
        JumpToOutline();
        return 0;
      }
      if (LOWORD(wparam) == kEditId && HIWORD(wparam) == EN_CHANGE) {
        MarkDirty();
        if (preview_) {
          SchedulePreviewRefresh();
        }
        return 0;
      }
      if (LOWORD(wparam) == kEditId &&
          (HIWORD(wparam) == EN_VSCROLL || HIWORD(wparam) == EN_SELCHANGE)) {
        if (preview_ && !syncing_scroll_) {
          SetTimer(hwnd_, kScrollTimer, 60, nullptr);
        }
        return 0;
      }
      break;
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wparam);
      const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      std::vector<std::wstring> paths;
      for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len + 1, L'\0');
        DragQueryFileW(drop, i, p.data(), len + 1);
        p.resize(len);
        paths.push_back(p);
      }
      DragFinish(drop);
      OpenPaths(paths);
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
      return 0;
    case WM_CLOSE:
      SyncFromWysiwyg();
      if (!ConfirmCloseAll()) {
        return 0;
      }
      Hide();
      return 0;
    case WM_DESTROY:
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}
