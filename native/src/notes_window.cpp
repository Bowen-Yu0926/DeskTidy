#include "notes_window.h"

#include "app_prefs.h"
#include "desknote_help.h"
#include "desknote_launch.h"
#include "desktop_shell.h"
#include "display_geom.h"
#include "docx_export.h"
#include "elevated_save.h"
#include "i18n.h"
#include "log.h"
#include "markdown_preview.h"
#include "mist_msg.h"
#include "mist_webview.h"
#include "notes_cloud_sync.h"
#include "notes_library.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "tool_paths.h"
#include "toast.h"
#include "ui_theme.h"
#include "vault_http.h"

#include <CommCtrl.h>
#include <commdlg.h>
#include <Richedit.h>
#include <Shellapi.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Uxtheme.h>
#include <windowsx.h>
#include <gdiplus.h>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")

#pragma comment(lib, "comctl32.lib")

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <atomic>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#pragma comment(lib, "comdlg32.lib")

namespace {
// The DeskTidy account vault (账号库) was removed, so no note path is a vault
// mirror any more. Kept as a named predicate rather than deleting all ~20
// call sites: they encode "is this the special managed note", and the answer
// is now always no.
bool IsVaultAccountsNotePath(const std::wstring&) { return false; }

constexpr wchar_t kClassName[] = L"DeskTidyNativeNotes";
// 提权写盘完成后，由原普通权限窗口恢复并重新加载目标文件。
constexpr UINT kMsgElevatedSaveDone = WM_APP + 90;
struct ElevatedSaveDone {
  std::wstring target;
  unsigned request_id = 0;
  bool ok = false;
  int target_tab = -1;
  LONG caret = 0;
  int first_visible_line = 0;
  bool typed_text_mode = false;
};
constexpr int kEditId = 2001;
constexpr int kTitleEditId = 2012;
constexpr int kFontFaceId = 2020;
constexpr int kFontSizeId = 2021;
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
constexpr int kMenuExportWord = 2174;
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
constexpr int kMenuOnlineNotes = 2171;
constexpr int kMenuToOnline = 2172;
constexpr int kMenuNewOnlineNote = 2173;

bool PathLooksCloudNotes(const std::wstring& path) {
  // Local files landed by「转本地」live under …\云笔记\…
  std::wstring lower = path;
  for (auto& c : lower) {
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    if (c == L'/') {
      c = L'\\';
    }
  }
  return lower.find(L"\\云笔记\\") != std::wstring::npos;
}

bool IsOnlineNotePath(const std::wstring& path) {
  return path.rfind(L"online://", 0) == 0;
}

int OnlineNoteIdFromPath(const std::wstring& path) {
  if (!IsOnlineNotePath(path)) {
    return 0;
  }
  return _wtoi(path.c_str() + 9);
}

std::wstring OnlineNotePathFromId(int id) {
  return L"online://" + std::to_wstring(id);
}

std::wstring JsonEscLib(const std::wstring& s) {
  std::wstring o;
  o.reserve(s.size() + 8);
  for (wchar_t c : s) {
    if (c == L'\\' || c == L'"') {
      o.push_back(L'\\');
      o.push_back(c);
    } else if (c == L'\n') {
      o += L"\\n";
    } else if (c == L'\r') {
      o += L"\\r";
    } else if (c == L'\t') {
      o += L"\\t";
    } else if (c < 0x20 || c == 0x2028 || c == 0x2029) {
      wchar_t buf[8];
      swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
      o += buf;
    } else {
      o.push_back(c);
    }
  }
  return o;
}

// Minimal JSON string field extract (library rail actions).
std::wstring LibJsonString(const std::wstring& json, const wchar_t* key) {
  const std::wstring needle = std::wstring(L"\"") + key + L"\"";
  size_t p = json.find(needle);
  if (p == std::wstring::npos) {
    return {};
  }
  p = json.find(L':', p + needle.size());
  if (p == std::wstring::npos) {
    return {};
  }
  ++p;
  while (p < json.size() &&
         (json[p] == L' ' || json[p] == L'\t' || json[p] == L'\r' ||
          json[p] == L'\n')) {
    ++p;
  }
  if (p >= json.size() || json[p] != L'"') {
    return {};
  }
  ++p;
  std::wstring v;
  while (p < json.size() && json[p] != L'"') {
    if (json[p] == L'\\' && p + 1 < json.size()) {
      ++p;
      const wchar_t e = json[p++];
      if (e == L'n') {
        v.push_back(L'\n');
      } else if (e == L'r') {
        v.push_back(L'\r');
      } else if (e == L't') {
        v.push_back(L'\t');
      } else if (e == L'b') {
        v.push_back(L'\b');
      } else if (e == L'f') {
        v.push_back(L'\f');
      } else if (e == L'u' && p + 4 <= json.size()) {
        unsigned code = 0;
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
          const wchar_t hc = json[p + k];
          code <<= 4;
          if (hc >= L'0' && hc <= L'9') {
            code |= static_cast<unsigned>(hc - L'0');
          } else if (hc >= L'a' && hc <= L'f') {
            code |= static_cast<unsigned>(hc - L'a' + 10);
          } else if (hc >= L'A' && hc <= L'F') {
            code |= static_cast<unsigned>(hc - L'A' + 10);
          } else {
            ok = false;
            break;
          }
        }
        if (ok) {
          p += 4;
          v.push_back(static_cast<wchar_t>(code));
        } else {
          v.push_back(L'u');
        }
      } else {
        v.push_back(e);
      }
      continue;
    }
    v.push_back(json[p++]);
  }
  return v;
}

int LibJsonInt(const std::wstring& json, const wchar_t* key) {
  const std::wstring needle = std::wstring(L"\"") + key + L"\"";
  size_t p = json.find(needle);
  if (p == std::wstring::npos) {
    return 0;
  }
  p = json.find(L':', p + needle.size());
  if (p == std::wstring::npos) {
    return 0;
  }
  ++p;
  while (p < json.size() &&
         (json[p] == L' ' || json[p] == L'\t' || json[p] == L'\r' ||
          json[p] == L'\n')) {
    ++p;
  }
  return _wtoi(json.c_str() + p);
}
constexpr int kEditCtxUndo = 3201;
constexpr int kEditCtxRedo = 3202;
constexpr int kEditCtxCut = 3203;
constexpr int kEditCtxCopy = 3204;
constexpr int kEditCtxPaste = 3205;
constexpr int kEditCtxSelectAll = 3206;
constexpr int kEditCtxFind = 3207;
constexpr int kEditCtxGoto = 3208;
constexpr int kOutlineId = 2002;
constexpr int kSidebarRefreshId = 2009;
constexpr int kModeStripId = 2010;
// Compact top-right preview/edit chip (not a full-height strip).
constexpr int kModeBtnW = 64;
constexpr int kModeBtnH = 28;
constexpr int kModeBtnPad = 10;
constexpr int kOutlineStripId = 2011;
constexpr int kGutterId = 2014;
constexpr int kOutlineStripW = 28;
constexpr int kOutlineExpandedW = 150;
constexpr wchar_t kGutterClass[] = L"DeskNoteGutter";
constexpr int kTabsId = 2003;
constexpr int kSidebarId = 2004;
constexpr int kSidebarSearchId = 2005;
constexpr int kSidebarNewId = 2006;
// Must not collide with kTitleEditId (2012) / kGutterId — shared IDs made
// BN_CLICKED / DRAWITEM hit the wrong control and could tear down the app.
constexpr int kSidebarAccountId = 2015;  // bottom login/username strip
constexpr int kSidebarResultsId = 2007;
constexpr int kStatusId = 2008;
constexpr int kTabRename = 3101;
constexpr int kTabPin = 3102;
constexpr int kTabReveal = 3103;
constexpr int kTabDelete = 3104;
constexpr int kTabClose = 3105;
constexpr int kTabCloseOthers = 3106;
constexpr int kTabCloseRight = 3107;
constexpr int kTabLock = 3108;
constexpr int kTabCopyPath = 3109;
constexpr int kTabExport = 3110;
constexpr int kTabExportWord = 3111;
constexpr int kTabExportPdf = 3112;
constexpr int kTabExportHtml = 3113;
constexpr int kTabExportMd = 3114;
constexpr UINT_PTR kPreviewTimer = 1;
constexpr UINT_PTR kScrollTimer = 2;
constexpr UINT_PTR kSidebarSearchTimer = 3;
constexpr UINT_PTR kOutlineTimer = 4;
constexpr UINT_PTR kStatusTimer = 5;
constexpr UINT_PTR kPersistTabsTimer = 6;
constexpr UINT_PTR kGutterTimer = 7;
constexpr UINT_PTR kSyncFilenameTimer = 8;
constexpr UINT_PTR kTitleSyncTimer = 9;
// View settings (font size/face, toggles) are persisted on a short debounce:
// Ctrl+wheel zoom used to do a blocking write+flush+rename per wheel notch.
constexpr UINT_PTR kPersistViewTimer = 10;
constexpr UINT kPersistViewDebounceMs = 400;
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

// "# \n\n" / whitespace only — WW boot seed, not real note content.
bool LooksLikeEmptyMarkdownSeed(const std::wstring& md) {
  size_t i = 0;
  while (i < md.size() &&
         (md[i] == L' ' || md[i] == L'\t' || md[i] == L'\r' || md[i] == L'\n' ||
          md[i] == L'\xFEFF')) {
    ++i;
  }
  if (i < md.size() && md[i] == L'#') {
    ++i;
    while (i < md.size() && (md[i] == L' ' || md[i] == L'\t')) {
      ++i;
    }
  }
  while (i < md.size() &&
         (md[i] == L' ' || md[i] == L'\t' || md[i] == L'\r' || md[i] == L'\n')) {
    ++i;
  }
  return i >= md.size();
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
constexpr int kSideRename = 3002;
constexpr int kSideDelete = 3003;
constexpr int kSideReveal = 3004;
constexpr int kSidePin = 3006;
constexpr int kSideExport = 3007;
constexpr int kSideExportWord = 3008;
constexpr int kSideExportPdf = 3009;
constexpr int kSideExportHtml = 3010;
constexpr int kSideExportMd = 3011;

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

// RichEdit 会把 CRLF 规范成 LF（有时只留 CR）；比较内容时统一当作 \n，
// 真正写回时仍用 CRLF，保持系统 hosts 的单字节/双字节文本语义。
std::wstring NormalizeNewlinesForCompare(const std::wstring& s) {
  std::wstring out;
  out.reserve(s.size());
  for (wchar_t c : s) {
    if (c != L'\r') {
      out.push_back(c);
    }
  }
  return out;
}

std::wstring HostsTextForWrite(const std::wstring& text) {
  std::wstring out;
  out.reserve(text.size() + 16);
  for (wchar_t c : text) {
    if (c == L'\r') {
      continue;
    }
    if (c == L'\n') {
      out.push_back(L'\r');
    }
    out.push_back(c);
  }
  return out;
}

// Typora-class chrome (title strip / WW / title→filename) — .md only.
bool PathIsMarkdownFile(const std::wstring& path) {
  if (IsOnlineNotePath(path)) {
    return true;
  }
  if (path.size() >= 3 &&
      _wcsicmp(path.c_str() + path.size() - 3, L".md") == 0) {
    return true;
  }
  if (path.size() >= 9 &&
      _wcsicmp(path.c_str() + path.size() - 9, L".markdown") == 0) {
    return true;
  }
  return false;
}

bool IsOpenablePath(const std::wstring& path) {
  if (IsOnlineNotePath(path)) {
    return true;
  }
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

bool RecyclePathsViaShellOp(HWND owner, const std::vector<std::wstring>& paths) {
  std::wstring from;
  for (const auto& p : paths) {
    if (p.empty()) {
      continue;
    }
    from.append(p);
    from.push_back(L'\0');
  }
  if (from.empty()) {
    return false;
  }
  from.push_back(L'\0');
  SHFILEOPSTRUCTW op{};
  op.hwnd = owner;
  op.wFunc = FO_DELETE;
  op.pFrom = from.c_str();
  op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
  return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

bool RecyclePathsQuiet(HWND owner, const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    return false;
  }
  // IFileOperation is Explorer-class and much snappier than SHFileOperation
  // for recycle-bin deletes (no progress UI / shell copy engine stall).
  IFileOperation* op = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&op));
  if (SUCCEEDED(hr) && op) {
    if (owner && IsWindow(owner)) {
      op->SetOwnerWindow(owner);
    }
    op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NO_UI | FOF_NOCONFIRMMKDIR);
    int queued = 0;
    for (const auto& p : paths) {
      if (p.empty()) {
        continue;
      }
      IShellItem* item = nullptr;
      if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr,
                                                IID_PPV_ARGS(&item))) &&
          item) {
        if (SUCCEEDED(op->DeleteItem(item, nullptr))) {
          ++queued;
        }
        item->Release();
      }
    }
    if (queued > 0) {
      hr = op->PerformOperations();
      BOOL aborted = FALSE;
      op->GetAnyOperationsAborted(&aborted);
      op->Release();
      if (SUCCEEDED(hr) && !aborted) {
        InvalidateNotesFileCache();
        return true;
      }
    } else {
      op->Release();
    }
  }
  const bool ok = RecyclePathsViaShellOp(owner, paths);
  if (ok) {
    InvalidateNotesFileCache();
  }
  return ok;
}

bool RecyclePathQuiet(HWND owner, const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  return RecyclePathsQuiet(owner, std::vector<std::wstring>{path});
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
std::wstring MakeAssetImagePath(const std::wstring& assets_dir,
                                const std::wstring& ext = L".png") {
  const std::wstring suffix_ext = ext.empty() ? L".png" : ext;
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t stem[32];
  swprintf_s(stem, L"img_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  auto exists = [](const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
  };
  std::wstring path = JoinPath(assets_dir, std::wstring(stem) + suffix_ext);
  for (int n = 1; exists(path) && n < 100; ++n) {
    wchar_t suffix[8];
    swprintf_s(suffix, L"_%02d", n);
    path = JoinPath(assets_dir, std::wstring(stem) + suffix + suffix_ext);
  }
  return path;
}

bool DecodeBase64Bytes(const std::wstring& b64, std::vector<uint8_t>* out) {
  if (!out) {
    return false;
  }
  out->clear();
  out->reserve((b64.size() * 3) / 4);
  auto val_of = [](wchar_t c) -> int {
    if (c >= L'A' && c <= L'Z') return c - L'A';
    if (c >= L'a' && c <= L'z') return c - L'a' + 26;
    if (c >= L'0' && c <= L'9') return c - L'0' + 52;
    if (c == L'+' || c == L'-') return 62;
    if (c == L'/' || c == L'_') return 63;
    return -1;
  };
  uint32_t acc = 0;
  int bits = 0;
  for (wchar_t c : b64) {
    if (c == L'=') {
      break;
    }
    const int v = val_of(c);
    if (v < 0) {
      continue;
    }
    acc = (acc << 6) | static_cast<uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<uint8_t>((acc >> bits) & 0xFFu));
    }
  }
  return !out->empty();
}

std::wstring ImageExtFromMimeOrName(const std::wstring& mime,
                                    const std::wstring& name) {
  std::wstring m = mime;
  for (auto& ch : m) {
    if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
  }
  if (m.find(L"jpeg") != std::wstring::npos ||
      m.find(L"jpg") != std::wstring::npos) {
    return L".jpg";
  }
  if (m.find(L"gif") != std::wstring::npos) return L".gif";
  if (m.find(L"webp") != std::wstring::npos) return L".webp";
  if (m.find(L"bmp") != std::wstring::npos) return L".bmp";
  if (m.find(L"svg") != std::wstring::npos) return L".svg";
  if (m.find(L"png") != std::wstring::npos) return L".png";
  const size_t dot = name.find_last_of(L'.');
  if (dot != std::wstring::npos) {
    std::wstring ext = name.substr(dot);
    for (auto& ch : ext) {
      if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif" ||
        ext == L".webp" || ext == L".bmp" || ext == L".svg") {
      return ext;
    }
  }
  return L".png";
}

std::wstring SaveDataUrlToAssets(const std::wstring& assets_dir,
                                 const std::wstring& name,
                                 const std::wstring& mime,
                                 const std::wstring& data_url) {
  const size_t comma = data_url.find(L',');
  const std::wstring b64 =
      comma == std::wstring::npos ? data_url : data_url.substr(comma + 1);
  std::vector<uint8_t> bytes;
  if (!DecodeBase64Bytes(b64, &bytes)) {
    return {};
  }
  // Guard the same limit the editor enforces: an oversized paste would create
  // a multi-megabyte .md that is slow to save and cannot be synced sanely.
  constexpr size_t kMaxImageBytes = 8u * 1024u * 1024u;
  if (bytes.size() > kMaxImageBytes) {
    LogLineA("[notes] image rejected: %d bytes > cap",
             static_cast<int>(bytes.size()));
    return {};
  }
  const std::wstring ext = ImageExtFromMimeOrName(mime, name);
  const std::wstring dest = MakeAssetImagePath(assets_dir, ext);
  std::ofstream out(dest, std::ios::binary);
  if (!out) {
    return {};
  }
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) {
    return {};
  }
  const size_t slash = dest.find_last_of(L"\\/");
  const std::wstring leaf =
      slash == std::wstring::npos ? dest : dest.substr(slash + 1);
  return L"assets/" + leaf;
}

void ReplaceAllW(std::wstring& str, const std::wstring& from, const std::wstring& to) {
  if (from.empty()) return;
  size_t start_pos = 0;
  while ((start_pos = str.find(from, start_pos)) != std::wstring::npos) {
    str.replace(start_pos, from.length(), to);
    start_pos += to.length();
  }
}

void ReplaceAllA(std::string& str, const std::string& from, const std::string& to) {
  if (from.empty()) return;
  size_t start_pos = 0;
  while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
    str.replace(start_pos, from.length(), to);
    start_pos += to.length();
  }
}

bool ReadBinaryFile(const std::wstring& path, std::string* out) {
  if (!out) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff len = in.tellg();
  if (len < 0) return false;
  in.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(len));
  if (len > 0) in.read(out->data(), len);
  return true;
}

std::wstring EncodeBase64(const std::string& bytes) {
  static const char b64_table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string res;
  res.reserve(((bytes.size() + 2) / 3) * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    uint32_t b = (static_cast<uint8_t>(bytes[i]) << 16);
    if (i + 1 < bytes.size()) b |= (static_cast<uint8_t>(bytes[i + 1]) << 8);
    if (i + 2 < bytes.size()) b |= static_cast<uint8_t>(bytes[i + 2]);
    res.push_back(b64_table[(b >> 18) & 0x3F]);
    res.push_back(b64_table[(b >> 12) & 0x3F]);
    res.push_back((i + 1 < bytes.size()) ? b64_table[(b >> 6) & 0x3F] : '=');
    res.push_back((i + 2 < bytes.size()) ? b64_table[b & 0x3F] : '=');
  }
  return WidenUtf8(res);
}

std::wstring GetImageMimeType(const std::wstring& filename) {
  size_t dot = filename.find_last_of(L'.');
  if (dot == std::wstring::npos) return L"image/png";
  std::wstring ext = filename.substr(dot);
  for (auto& c : ext) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  if (ext == L".jpg" || ext == L".jpeg") return L"image/jpeg";
  if (ext == L".gif") return L"image/gif";
  if (ext == L".webp") return L"image/webp";
  if (ext == L".svg") return L"image/svg+xml";
  if (ext == L".bmp") return L"image/bmp";
  return L"image/png";
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
        DWORD extra = 0;
        if (bih->biSize == sizeof(BITMAPINFOHEADER)) {
          if (bih->biCompression == BI_BITFIELDS) {
            extra += 12;
          } else if (bih->biCompression == 6 /* BI_ALPHABITFIELDS */) {
            extra += 16;
          }
        }
        if (bih->biClrUsed > 0) {
          extra += bih->biClrUsed * 4;
        } else if (bih->biBitCount > 0 && bih->biBitCount <= 8) {
          extra += (1u << bih->biBitCount) * 4;
        }
        void* bits = const_cast<BYTE*>(dib + bih->biSize + extra);
        HDC dc = GetDC(nullptr);
        out = CreateDIBitmap(dc, bih, CBM_INIT, bits,
                             reinterpret_cast<const BITMAPINFO*>(dib),
                             DIB_RGB_COLORS);
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
  if (bg_brush_) {
    DeleteObject(bg_brush_);
  }
  if (card_brush_) {
    DeleteObject(card_brush_);
  }
  if (rail_brush_) {
    DeleteObject(rail_brush_);
  }
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
  POINT mid{(work.left + work.right) / 2, (work.top + work.bottom) / 2};
  const int dpi = DpiAtPoint(mid);
  const int max_w = (std::max)(480, static_cast<int>(work.right - work.left) * 92 / 100);
  const int max_h = (std::max)(360, static_cast<int>(work.bottom - work.top) * 92 / 100);
  int ww = (std::min)(max_w, ScalePxForDpi(960, dpi));
  int wh = (std::min)(max_h, ScalePxForDpi(640, dpi));
  ww = (std::max)((std::min)(640, max_w), ww);
  wh = (std::max)((std::min)(420, max_h), wh);
  const int x = work.left + (std::max)(0, static_cast<int>(work.right - work.left - ww) / 2);
  const int y = work.top + (std::max)(0, static_cast<int>(work.bottom - work.top - wh) / 2);
  hwnd_ = CreateWindowExW(
      WS_EX_APPWINDOW, kClassName,
      IsDevRuntime() ? L"DeskNote [dev]" : L"DeskNote",
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
  AppendMenuW(file, MF_STRING, kMenuCloseTab, L"删除文件并关闭\tCtrl+W");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuSave, L"保存\tCtrl+S");
  AppendMenuW(file, MF_STRING, kMenuSaveAs, L"另存为…\tCtrl+Shift+S");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuExportWord, L"导出为 Word (.docx)…");
  AppendMenuW(file, MF_STRING, kMenuExportPdf, L"导出为 PDF (.pdf)…");
  AppendMenuW(file, MF_STRING, kMenuExportHtml, L"导出为 HTML 网页…");
  AppendMenuW(file, MF_STRING, kMenuExportMd, L"导出为 Markdown (含图片)…");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kMenuPasteHtml, Tr("notes_paste_html").c_str());
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
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, kMenuBigger, L"放大字体\tCtrl++");
  AppendMenuW(view, MF_STRING, kMenuSmaller, L"缩小字体\tCtrl+-");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING | MF_CHECKED, kMenuWrap, L"自动换行(&W)");
  AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view),
              Tr("view").c_str());
  // Format menu removed — toolbar + Ctrl+B/I/U/K (and Shift+Enter soft break).
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
  EnsureTitleEdit();
  EnsureGutter();
  // Right outline strip/list removed.
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
      0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 186, 6, 28, 28, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarRefreshId)), instance,
      nullptr);
  sidebar_account_ = CreateWindowExW(
      0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 0, 0, 240, 36, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSidebarAccountId)), instance,
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
    // Restore the user's drag order before the first tree build.
    LoadSidebarOrder();
  }
  if (sidebar_new_) {
    SetWindowTheme(sidebar_new_, L"", L"");
  }
  if (sidebar_refresh_) {
    SetWindowTheme(sidebar_refresh_, L"", L"");
  }
  // Top-right preview/edit chip removed.
  // Hover invalidate for owner-draw quiet buttons.
  auto hook_btn = [](HWND btn) {
    if (!btn) {
      return;
    }
    struct BtnSubclass {
      static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR id, DWORD_PTR) {
        if (msg == WM_NCDESTROY) {
          RemoveWindowSubclass(hwnd, &BtnSubclass::Proc, id);
        } else if (msg == WM_MOUSEMOVE) {
          TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
          TrackMouseEvent(&tme);
          InvalidateRect(hwnd, nullptr, FALSE);
        } else if (msg == WM_MOUSELEAVE || msg == WM_LBUTTONDOWN ||
                   msg == WM_LBUTTONUP) {
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
      }
    };
    SetWindowSubclass(btn, &BtnSubclass::Proc, 1, 0);
  };
  hook_btn(sidebar_new_);
  hook_btn(sidebar_refresh_);
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
  NotesWarmWebViewEnv();
  web_.Create(hwnd_);
  MistWarmMessageEnv();  // Prefetch mist confirm card (delete / alerts).
  EnsureDropTarget();
  RegisterDropOnHwnd(edit_);
  RegisterDropOnHwnd(tabs_);
  RegisterDropOnHwnd(sidebar_);
  // WebView surface: AllowExternalDrop + NavigationStarting; host margins OLE.
  RegisterDropOnSubtree(web_.Host());
  auto on_drop = [this](const std::vector<std::wstring>& paths) {
    HandleDroppedPaths(paths);
  };
  web_.on_external_drop = on_drop;
  preview_web_.on_external_drop = on_drop;
  web_.on_upload_image = [this](const std::wstring& name,
                                const std::wstring& mime,
                                const std::wstring& data_url) -> std::wstring {
    const std::wstring base_dir = NoteBaseDir();
    const std::wstring assets_dir = JoinPath(base_dir, L"assets");
    CreateDirectoryW(assets_dir.c_str(), nullptr);
    web_.SetAssetBaseDir(base_dir);
    const std::wstring rel =
        SaveDataUrlToAssets(assets_dir, name, mime, data_url);
    if (rel.empty()) {
      return rel;
    }
    MarkDirty();
    // Attach the binary to the cloud note as well; otherwise the Markdown
    // reference is dead on any other device. Determine the note id for both
    // online tabs and locally-synced files.
    int asset_note_id = 0;
    if (IsOnlineNotePath(path_)) {
      asset_note_id = OnlineNoteIdFromPath(path_);
    } else if (!path_.empty()) {
      if (const auto* link = FindNoteCloudLink(cloud_sync_, path_)) {
        asset_note_id = link->note_id;
      }
    }
    if (asset_note_id > 0 && VaultHttpLoggedIn()) {
      const size_t slash = rel.find_last_of(L'/');
      const std::wstring leaf =
          slash == std::wstring::npos ? rel : rel.substr(slash + 1);
      const size_t comma = data_url.find(L',');
      const std::wstring b64 =
          comma == std::wstring::npos ? data_url : data_url.substr(comma + 1);
      std::thread([asset_note_id, leaf, mime, b64]() {
        VaultHttpPutNoteAsset(asset_note_id, leaf, mime, NarrowUtf8(b64));
      }).detach();
    }
    return rel;
  };
  web_.on_dirty = [this]() {
    // Immediate (no markdown body) — covers the 150ms changed debounce window.
    MarkDirty();
  };
  web_.on_changed = [this]() {
    // Cache already updated in OnWebMessage. Mirror to RichEdit only on
    // Save / SyncFromWysiwyg / tab switch — not every keystroke.
    MarkDirty();
    SyncCurrentTabTitleHint();
  };
  web_.on_save = [this]() { Save(true); };
  web_.on_context_menu = [this](POINT pt, bool has_sel) {
    HandleWysiwygContextMenu(pt, has_sel);
  };
  web_.on_font_prefs = [this](const std::wstring& face, int pt) {
    // Toolbar owns the live style; only persist host prefs (avoid re-Eval loop).
    std::wstring next = face;
    if (_wcsicmp(next.c_str(), L"SimSun") == 0 || next == L"宋体") {
      next = L"SimSun";
    } else {
      next = L"Microsoft YaHei";
    }
    bool changed = false;
    if (_wcsicmp(font_face_.c_str(), next.c_str()) != 0) {
      font_face_ = std::move(next);
      changed = true;
    }
    if (pt >= 8 && pt <= 48 && font_pt_ != pt) {
      font_pt_ = pt;
      changed = true;
    }
    if (changed) {
      if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
      }
      HDC hdc = GetDC(nullptr);
      const int height = -MulDiv(font_pt_, GetDeviceCaps(hdc, LOGPIXELSY), 72);
      ReleaseDC(nullptr, hdc);
      font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          VARIABLE_PITCH | FF_SWISS, font_face_.c_str());
      if (edit_) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
      }
      SyncFontControls();
      PersistViewSettings();
    }
  };
  web_.on_content_ready = [this]() {
    // WebView2 + Toast cold-start can take seconds — keep RichEdit staging
    // until the first painted set, then reveal WYSIWYG (Typora-class open).
    if (!wysiwyg_ || toast_) {
      return;
    }
    layout_w_ = -1;  // bust LayoutClient early-out (wysiwyg_ already true)
    ApplyWysiwygVisibility();
    LayoutClient();
    PushEditorFontToWeb();
    FocusActiveEditor(false);
  };
  web_.on_find = [this](bool replace) { ShowFind(replace); };
  web_.on_find_again = [this](bool down) { FindAgain(down); };
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
    // Body-only staging → rejoin native title before pushing Toast.
    if (!md.empty()) {
      md = ComposeMarkdownWithNativeTitle(md);
    }
    if (!LooksLikeEmptyMarkdownSeed(md) || web_.CachedMarkdown().empty()) {
      web_.SetMarkdown(md, NoteBaseDir());
    }
  };
  {
    // DeskNote 保持经典清晰素雅界面，不与系统/全局深色主题联动
    const std::wstring theme = L"light";
    web_.SetTheme(theme);
    preview_web_.SetTheme(theme);
  }
  LoadTabSession();
  // Same for the pin/lock lists: entries for files that are gone only ever add
  // noise (they are persisted back out every save) and would silently apply
  // again if a file with that exact path ever reappeared.
  auto drop_missing = [](std::vector<std::wstring>* list) {
    if (!list) {
      return;
    }
    list->erase(std::remove_if(list->begin(), list->end(),
                               [](const std::wstring& p) {
                                 if (p.empty() || IsOnlineNotePath(p)) {
                                   return false;
                                 }
                                 return GetFileAttributesW(p.c_str()) ==
                                        INVALID_FILE_ATTRIBUTES;
                               }),
                list->end());
  };
  drop_missing(&pinned_paths_);
  drop_missing(&locked_paths_);
  // Drop tabs whose file no longer exists (deleted between runs, moved outside
  // DeskNote, or removed by another tab action). Restoring them produced a tab
  // that looks fine but cannot be saved, and the editor then wrote a brand-new
  // file at a path the user thought was gone. online:// entries are virtual and
  // always kept.
  if (!tab_paths_.empty()) {
    std::vector<std::wstring> kept;
    std::vector<TabState> kept_states;
    int new_index = 0;
    const int old_index =
        (tab_index_ >= 0 && tab_index_ < static_cast<int>(tab_paths_.size()))
            ? tab_index_
            : 0;
    for (size_t i = 0; i < tab_paths_.size(); ++i) {
      const std::wstring& tp = tab_paths_[i];
      const bool ok = tp.empty() || IsOnlineNotePath(tp) ||
                      GetFileAttributesW(tp.c_str()) != INVALID_FILE_ATTRIBUTES;
      if (!ok) {
        continue;
      }
      if (static_cast<int>(i) == old_index) {
        new_index = static_cast<int>(kept.size());
      }
      kept.push_back(tp);
      kept_states.push_back(i < tab_states_.size() ? tab_states_[i]
                                                   : TabState{});
    }
    if (kept.size() != tab_paths_.size()) {
      tab_paths_ = std::move(kept);
      tab_states_ = std::move(kept_states);
      tab_index_ = new_index;
    }
  }
  LoadAndSweepCloudSync();
  LoadViewSettings();
  EnsureFontControls();
  ApplyEditorFont();
  SyncFontControls();
  ApplyUiFonts();
  if (tab_paths_.empty()) {
    tab_paths_.push_back(path_);
    tab_index_ = 0;
  }
  if (tab_index_ < 0 || tab_index_ >= static_cast<int>(tab_paths_.size())) {
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
  outline_ = false;
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
    SyncFontControls();
    return;
  }
  font_pt_ = pt;
  ApplyEditorFont();
  SyncFontControls();
  SchedulePersistViewSettings();
}

void NotesWindow::SetFontFace(const std::wstring& face) {
  std::wstring next = face;
  if (_wcsicmp(next.c_str(), L"SimSun") == 0 ||
      next == L"宋体") {
    next = L"SimSun";
  } else {
    next = L"Microsoft YaHei";
  }
  if (_wcsicmp(font_face_.c_str(), next.c_str()) == 0) {
    SyncFontControls();
    return;
  }
  font_face_ = std::move(next);
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  ApplyEditorFont();
  SyncFontControls();
  PersistViewSettings();
}

void NotesWindow::EnsureFontControls() {
  if (!hwnd_ || (font_face_combo_ && font_size_combo_ && font_smaller_btn_ &&
                 font_bigger_btn_)) {
    return;
  }
  HINSTANCE inst = GetModuleHandleW(nullptr);
  if (!font_face_combo_) {
    font_face_combo_ = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | CBS_DROPDOWNLIST |
            CBS_HASSTRINGS | WS_VSCROLL,
        0, 0, 120, 200, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFontFaceId)), inst,
        nullptr);
    if (font_face_combo_) {
      SendMessageW(font_face_combo_, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"微软雅黑"));
      SendMessageW(font_face_combo_, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(L"宋体"));
      // Default face: 微软雅黑 (index 0).
      SendMessageW(font_face_combo_, CB_SETCURSEL, 0, 0);
    }
  }
  if (!font_size_combo_) {
    font_size_combo_ = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | CBS_DROPDOWNLIST |
            CBS_HASSTRINGS | WS_VSCROLL,
        0, 0, 64, 280, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFontSizeId)), inst,
        nullptr);
    if (font_size_combo_) {
      static const int kPts[] = {10, 11, 12, 14, 16, 18, 20, 22, 24, 28, 32};
      for (int pt : kPts) {
        wchar_t buf[8];
        swprintf_s(buf, L"%d", pt);
        SendMessageW(font_size_combo_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(buf));
      }
    }
  }
  // Same command IDs as View · 放大/缩小 — BN_CLICKED hits kMenuBigger/Smaller.
  if (!font_smaller_btn_) {
    font_smaller_btn_ = CreateWindowExW(
        0, L"BUTTON", L"A-",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_PUSHBUTTON | BS_CENTER,
        0, 0, 28, 24, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMenuSmaller)), inst,
        nullptr);
  }
  if (!font_bigger_btn_) {
    font_bigger_btn_ = CreateWindowExW(
        0, L"BUTTON", L"A+",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_PUSHBUTTON | BS_CENTER,
        0, 0, 28, 24, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMenuBigger)), inst,
        nullptr);
  }
  if (ui_font_) {
    const WPARAM font = reinterpret_cast<WPARAM>(ui_font_);
    if (font_face_combo_) {
      SendMessageW(font_face_combo_, WM_SETFONT, font, TRUE);
    }
    if (font_size_combo_) {
      SendMessageW(font_size_combo_, WM_SETFONT, font, TRUE);
    }
    if (font_smaller_btn_) {
      SendMessageW(font_smaller_btn_, WM_SETFONT, font, TRUE);
    }
    if (font_bigger_btn_) {
      SendMessageW(font_bigger_btn_, WM_SETFONT, font, TRUE);
    }
  }
  SyncFontControls();
}

void NotesWindow::SyncFontControls() {
  if (font_face_combo_) {
    const int want =
        (_wcsicmp(font_face_.c_str(), L"SimSun") == 0) ? 1 : 0;
    if (SendMessageW(font_face_combo_, CB_GETCURSEL, 0, 0) != want) {
      SendMessageW(font_face_combo_, CB_SETCURSEL, want, 0);
    }
  }
  if (font_size_combo_) {
    wchar_t buf[8];
    swprintf_s(buf, L"%d", font_pt_);
    int idx = static_cast<int>(
        SendMessageW(font_size_combo_, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                     reinterpret_cast<LPARAM>(buf)));
    if (idx == CB_ERR) {
      // CBS_DROPDOWNLIST cannot display values absent from the list (A± / Ctrl+/-).
      idx = static_cast<int>(SendMessageW(font_size_combo_, CB_ADDSTRING, 0,
                                          reinterpret_cast<LPARAM>(buf)));
    }
    if (idx != CB_ERR &&
        SendMessageW(font_size_combo_, CB_GETCURSEL, 0, 0) != idx) {
      SendMessageW(font_size_combo_, CB_SETCURSEL, idx, 0);
    }
  }
}

void NotesWindow::PushEditorFontToWeb() {
  if (!web_.Ready()) {
    return;
  }
  const wchar_t* face = font_face_.c_str();
  if (!face || !*face) {
    face = L"Microsoft YaHei";
  }
  wchar_t script[384];
  swprintf_s(script,
             L"try{if(window.__desknoteSetEditorFont)"
             L"__desknoteSetEditorFont(\"%s\",%d);}catch(e){}",
             face, font_pt_);
  web_.EvalScript(script);
}

void NotesWindow::ApplyEditorFont() {
  if (font_) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  HDC hdc = GetDC(nullptr);
  const int height = -MulDiv(font_pt_, GetDeviceCaps(hdc, LOGPIXELSY), 72);
  ReleaseDC(nullptr, hdc);
  // Chinese writing surface: 微软雅黑 / 宋体 (Typora-class body fonts).
  const wchar_t* face = font_face_.c_str();
  if (!face || !*face) {
    face = L"Microsoft YaHei";
  }
  font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, face);
  // RichEdit: Word-class — restyle selection only; bare caret updates typing
  // defaults. Never WM_SETFONT here (that rewrote the whole buffer).
  if (edit_ && !wysiwyg_) {
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET;
    cf.yHeight = font_pt_ * 20;  // twips
    cf.bCharSet = DEFAULT_CHARSET;
    wcsncpy_s(cf.szFaceName, face, _TRUNCATE);
    CHARRANGE cr{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    const WPARAM how = (cr.cpMin != cr.cpMax)
                           ? SCF_SELECTION
                           : (SCF_SELECTION | SCF_DEFAULT);
    SendMessageW(edit_, EM_SETCHARFORMAT, how,
                 reinterpret_cast<LPARAM>(&cf));
  } else if (edit_ && font_ && !IsWindowVisible(edit_)) {
    // Staging surface before WW paints — seed control default once.
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  }
  // WW: host only restores the soft document default; toolbar applies marks
  // to the current selection inside the page.
  PushEditorFontToWeb();
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
    if (ui_font_bold_) {
      DeleteObject(ui_font_bold_);
      ui_font_bold_ = nullptr;
    }
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
  if (font_face_combo_) {
    SendMessageW(font_face_combo_, WM_SETFONT, font, TRUE);
  }
  if (font_size_combo_) {
    SendMessageW(font_size_combo_, WM_SETFONT, font, TRUE);
  }
  if (font_smaller_btn_) {
    SendMessageW(font_smaller_btn_, WM_SETFONT, font, TRUE);
  }
  if (font_bigger_btn_) {
    SendMessageW(font_bigger_btn_, WM_SETFONT, font, TRUE);
  }
  ApplyTitleFont();
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
  // DeskNote 保证在白底上始终为深黑清晰字色
  text_color_ = RGB(32, 36, 42);
  muted_color_ = RGB(120, 128, 140);
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
    web_.SetMarkdown(CurrentPlainText(), NoteBaseDir());
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
  if (IsCurrentProcessElevated()) {
    title += L" [管理员]";
  }
  if (IsDevRuntime()) {
    title += L" [dev]";
  }
  if (toast_) {
    title += L" · 便签条";
  } else if (wysiwyg_) {
  }
  SetWindowTextW(hwnd_, title.c_str());
}

NotesWindow::~NotesWindow() {
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
  if (title_font_) {
    DeleteObject(title_font_);
    title_font_ = nullptr;
  }
  if (accel_) {
    DestroyAcceleratorTable(accel_);
    accel_ = nullptr;
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

void NotesWindow::Destroy() {
  if (hwnd_) {
    if (!in_elevated_save_) {
      Save(true);
    }
    PersistTabs();
    PersistViewSettings();
    KillTimer(hwnd_, kStatusTimer);
    KillTimer(hwnd_, kPersistTabsTimer);
    KillTimer(hwnd_, kGutterTimer);
  }
  online_notes_.Destroy();
  if (web_.Host()) {
    RevokeDropOnSubtree(web_.Host());
  }
  if (preview_web_.Host()) {
    RevokeDropOnSubtree(preview_web_.Host());
  }
  if (edit_) {
    RevokePathDropTarget(edit_);
  }
  if (tabs_) {
    RevokePathDropTarget(tabs_);
  }
  if (sidebar_) {
    RevokePathDropTarget(sidebar_);
  }
  if (hwnd_) {
    RevokePathDropTarget(hwnd_);
  }
  if (drop_target_) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
  web_.Destroy();
  preview_web_.Destroy();
  library_web_.Destroy();
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
  if (title_font_) {
    DeleteObject(title_font_);
    title_font_ = nullptr;
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
  // Clear AFTER the final Save() above: saving an online/synced note enqueues a
  // cloud push, and that path re-registers hwnd_. Clearing earlier would be
  // undone. The (process-scoped, detached) push worker must not post into a
  // window that no longer exists — worse, the HWND may be recycled for an
  // unrelated window, where IsWindow() still passes.
  g_cloud_push_notify_hwnd.store(nullptr, std::memory_order_release);
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
  return PathIsMarkdownFile(path_);
}

bool NotesWindow::PathLooksTextish() const {
  if (path_.empty()) {
    return false;
  }
  if (IsOnlineNotePath(path_)) {
    return true;
  }
  if (IsHostsPath(path_)) {
    return true;
  }
  const std::wstring sfx = LowerSuffix(path_);
  // Extensionless (hosts / LICENSE / Dockerfile): always plain text — never
  // EM_STREAMOUT RTF. Saving hosts as RTF after a mistaken rename left the
  // system file unusable.
  if (sfx.empty()) {
    return true;
  }
  if (sfx == L".rtf") {
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
  // Same cap as the paste/upload path (SaveDataUrlToAssets). Dragging a file
  // in bypassed the editor's limit entirely, so a large photo could still end
  // up in the library and make the note body impractical to sync.
  {
    constexpr ULONGLONG kMaxImageBytes = 8ull * 1024ull * 1024ull;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(src_path.c_str(), GetFileExInfoStandard, &fad)) {
      const ULONGLONG sz =
          (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
      if (sz > kMaxImageBytes) {
        MistAlert(hwnd_, L"插入图片",
                  L"图片过大（超过 8 MB），请压缩后再插入。",
                  MistMsgTone::Warn);
        return false;
      }
    }
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
  const std::wstring rel = L"assets/" + name;
  // WYSIWYG stage is the visible editor — RichEdit is only the staging buffer,
  // so the node has to be inserted in Toast (the markdown stays relative).
  if (wysiwyg_ && web_.Ready()) {
    web_.SetAssetBaseDir(NoteBaseDir());
    const size_t ndot = name.find_last_of(L'.');
    web_.InsertImage(rel, ndot == std::wstring::npos ? name
                                                     : name.substr(0, ndot));
    MarkDirty();
    return true;
  }
  const std::wstring md = L"![](" + rel + L")\n";
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
  std::vector<wchar_t> file(32768, L'\0');
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter =
      L"图片\0*.png;*.jpg;*.jpeg;*.gif;*.webp;*.bmp;*.svg\0所有文件\0*.*\0";
  ofn.lpstrFile = file.data();
  ofn.nMaxFile = static_cast<DWORD>(file.size());
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_ALLOWMULTISELECT |
              OFN_EXPLORER | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) {
    return;
  }
  std::vector<std::wstring> paths;
  const wchar_t* p = file.data();
  const std::wstring first(p);
  p += first.size() + 1;
  if (*p == L'\0') {
    paths.push_back(first);
  } else {
    while (*p != L'\0') {
      std::wstring name(p);
      p += name.size() + 1;
      paths.push_back(first + L"\\" + name);
    }
  }
  int n = 0;
  for (const auto& img_path : paths) {
    if (InsertImageFile(img_path)) {
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
  EnableMenuItem(menu_, kMenuExportWord, flag);
  EnableMenuItem(menu_, kMenuExportPdf, flag);
  EnableMenuItem(menu_, kMenuExportHtml, flag);
  EnableMenuItem(menu_, kMenuExportMd, flag);
  EnableMenuItem(menu_, kMenuPandoc, flag);
  EnableMenuItem(menu_, kMenuEditMode, flag);
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

void NotesWindow::SchedulePersistViewSettings() {
  if (hwnd_) {
    SetTimer(hwnd_, kPersistViewTimer, kPersistViewDebounceMs, nullptr);
  } else {
    PersistViewSettings();
  }
}

void NotesWindow::PersistViewSettings() {
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_ui.json");
  const char* face_json = "Microsoft YaHei";
  if (_wcsicmp(font_face_.c_str(), L"SimSun") == 0) {
    face_json = "SimSun";
  }
  std::ostringstream out;
  out << "{\n"
      << "  \"sidebar\": " << (sidebar_visible_ ? "true" : "false") << ",\n"
      << "  \"outline\": " << (outline_ ? "true" : "false") << ",\n"
      << "  \"sync_scroll\": " << (sync_scroll_ ? "true" : "false") << ",\n"
      << "  \"word_wrap\": " << (word_wrap_ ? "true" : "false") << ",\n"
      << "  \"typewriter\": " << (typewriter_ ? "true" : "false") << ",\n"
      << "  \"preview\": " << (preview_ ? "true" : "false") << ",\n"
      << "  \"always_on_top\": " << (always_on_top_ ? "true" : "false") << ",\n"
      << "  \"font_pt\": " << font_pt_ << ",\n"
      << "  \"font_face\": \"" << face_json << "\"\n"
      << "}\n";
  AtomicWriteUtf8File(file, out.str());
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
  // Right-side preview / outline chrome removed — never restore those modes.
  outline_ = false;
  preview_ = false;
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
  {
    const std::string needle = "\"font_face\"";
    const size_t pos = json.find(needle);
    if (pos != std::string::npos) {
      const size_t colon = json.find(':', pos + needle.size());
      const size_t q1 = colon == std::string::npos
                            ? std::string::npos
                            : json.find('"', colon + 1);
      const size_t q2 =
          q1 == std::string::npos ? std::string::npos : json.find('"', q1 + 1);
      if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
        const std::string face = json.substr(q1 + 1, q2 - q1 - 1);
        if (face == "SimSun") {
          font_face_ = L"SimSun";
        } else {
          font_face_ = L"Microsoft YaHei";
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
  // Loading a file re-lays out the editor and can leave the window behind the
  // foreground app again; re-raise after the document is in place.
  Present();
}

void NotesWindow::HandleDroppedPaths(const std::vector<std::wstring>& paths) {
  std::vector<std::wstring> notes;
  for (const auto& p : paths) {
    if (p.empty()) {
      continue;
    }
    const DWORD attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
      continue;
    }
    const std::wstring lower = LowerSuffix(p);
    if (IsImageDropSuffix(lower)) {
      InsertImageFile(p);
    } else if (IsOpenablePath(p)) {
      notes.push_back(p);
    }
  }
  if (!notes.empty()) {
    OpenPaths(notes);
  }
}

void NotesWindow::RegisterDropOnHwnd(HWND hwnd) {
  if (!hwnd || !drop_target_) {
    return;
  }
  // Same IDropTarget on chrome + WebView host (SetWindow only for highlight).
  const HRESULT hr = RegisterDragDrop(hwnd, drop_target_);
  if (FAILED(hr) && hr != DRAGDROP_E_ALREADYREGISTERED) {
    return;
  }
  DragAcceptFiles(hwnd, TRUE);
}

void NotesWindow::RegisterDropOnSubtree(HWND root) {
  if (!root || !drop_target_) {
    return;
  }
  RegisterDropOnHwnd(root);
  EnumChildWindows(
      root,
      [](HWND child, LPARAM lp) -> BOOL {
        reinterpret_cast<NotesWindow*>(lp)->RegisterDropOnHwnd(child);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(this));
}

void NotesWindow::RevokeDropOnSubtree(HWND root) {
  if (!root) {
    return;
  }
  EnumChildWindows(
      root,
      [](HWND child, LPARAM) -> BOOL {
        RevokePathDropTarget(child);
        return TRUE;
      },
      0);
  RevokePathDropTarget(root);
}

void NotesWindow::EnsureDropTarget() {
  if (!hwnd_ || drop_target_) {
    return;
  }
  drop_target_ =
      new PathDropTarget([this](const std::vector<std::wstring>& paths,
                                DWORD proposed) -> DWORD {
        HandleDroppedPaths(paths);
        return proposed;
      });
  // AIGC START
  // Opening/attaching must not MOVE the source out of Explorer.
  drop_target_->SetFileDropSemantics(FileDropSemantics::VirtualPin);
  // AIGC END
  if (!RegisterPathDropTarget(hwnd_, drop_target_)) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
}

void NotesWindow::OpenOrActivatePath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  // 账号库 is DeskTidy-only; refuse opening the mirror note in DeskNote.
  if (IsVaultAccountsNotePath(path)) {
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
  // Opened .md defaults to editable preview (matches Typora / DeskNote product).
  tab_states_.back().wysiwyg = PathIsMarkdownFile(norm);
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
  if (path.empty() || IsOnlineNotePath(path)) {
    return path;
  }
  // GetFullPathNameW is a syscall, and this runs inside per-item loops (library
  // rail rebuild compares every file against every open tab, and per keystroke).
  // Paths repeat heavily within a session, so memoise the mapping. Keys are
  // immutable per path, so a cache entry never goes stale.
  static std::mutex s_mu;
  static std::unordered_map<std::wstring, std::wstring> s_cache;
  {
    std::lock_guard<std::mutex> lk(s_mu);
    const auto it = s_cache.find(path);
    if (it != s_cache.end()) {
      return it->second;
    }
  }
  std::wstring key = path;
  wchar_t buf[MAX_PATH * 4] = {};
  constexpr DWORD kBuf = static_cast<DWORD>(MAX_PATH * 4);
  const DWORD n = GetFullPathNameW(path.c_str(), kBuf, buf, nullptr);
  if (n != 0 && n < kBuf) {
    key = buf;
  }
  {
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_cache.size() >= 8192) {
      s_cache.clear();
    }
    s_cache.emplace(path, key);
  }
  return key;
}

namespace {
std::wstring TrimWsLocal(std::wstring s) {
  while (!s.empty() && iswspace(s.front())) {
    s.erase(s.begin());
  }
  while (!s.empty() && iswspace(s.back())) {
    s.pop_back();
  }
  return s;
}

// English drafts (untitled.md / Untitled-1.md) → 未命名… (Typora CN / product).
std::wstring LocalizeUntitledStem(const std::wstring& stem) {
  if (stem.size() < 8 || _wcsnicmp(stem.c_str(), L"untitled", 8) != 0) {
    return stem;
  }
  const wchar_t* rest = stem.c_str() + 8;
  if (*rest == L'\0') {
    return L"未命名";
  }
  if (*rest == L'_' || *rest == L'-' || *rest == L' ') {
    return std::wstring(L"未命名") + rest;
  }
  return stem;
}

bool PathIsEnglishUntitledNote(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  std::wstring base =
      slash == std::wstring::npos ? path : path.substr(slash + 1);
  const size_t dot = base.find_last_of(L'.');
  if (dot != std::wstring::npos && dot > 0) {
    base = base.substr(0, dot);
  }
  return LocalizeUntitledStem(base) != base;
}

// Split leading ATX heading ↔ title / body (mirrors wysiwyg_native.html).
struct MdTitleBody {
  std::wstring title;
  std::wstring body;
  bool emit_title_line = false;
};

MdTitleBody SplitMarkdownTitleBody(const std::wstring& md_in) {
  MdTitleBody out;
  std::wstring raw = md_in;
  if (!raw.empty() && raw[0] == L'\xFEFF') {
    raw.erase(raw.begin());
  }
  std::wstring prefix;
  if (raw.size() >= 4 && raw.compare(0, 4, L"---\n") == 0) {
    const size_t fm_end = raw.find(L"\n---", 4);
    if (fm_end != std::wstring::npos) {
      const size_t after = raw.find(L'\n', fm_end + 4);
      if (after != std::wstring::npos) {
        prefix = raw.substr(0, after + 1);
        raw = raw.substr(after + 1);
        while (!raw.empty() && (raw[0] == L'\n' || raw[0] == L'\r')) {
          raw.erase(raw.begin());
        }
      }
    }
  } else if (raw.size() >= 5 && raw.compare(0, 5, L"---\r\n") == 0) {
    const size_t fm_end = raw.find(L"\n---", 5);
    if (fm_end != std::wstring::npos) {
      const size_t after = raw.find(L'\n', fm_end + 4);
      if (after != std::wstring::npos) {
        prefix = raw.substr(0, after + 1);
        raw = raw.substr(after + 1);
        while (!raw.empty() && (raw[0] == L'\n' || raw[0] == L'\r')) {
          raw.erase(raw.begin());
        }
      }
    }
  }
  // # title\nbody… (RichEdit often uses \r or \r\n — accept both.)
  size_t i = 0;
  while (i < raw.size() && raw[i] == L'#') {
    ++i;
  }
  if (i >= 1 && i <= 6 && i < raw.size() &&
      (raw[i] == L' ' || raw[i] == L'\t')) {
    size_t line_end = raw.find_first_of(L"\r\n", i);
    std::wstring title_line;
    std::wstring rest;
    if (line_end == std::wstring::npos) {
      title_line = raw.substr(i + 1);
      rest.clear();
    } else {
      title_line = raw.substr(i + 1, line_end - (i + 1));
      rest = raw.substr(line_end);
      while (!rest.empty() && (rest[0] == L'\n' || rest[0] == L'\r')) {
        rest.erase(rest.begin());
      }
    }
    while (!title_line.empty() &&
           (title_line.back() == L' ' || title_line.back() == L'\t' ||
            title_line.back() == L'#')) {
      if (title_line.back() == L'#') {
        // trim trailing " ###"
        title_line.pop_back();
        while (!title_line.empty() &&
               (title_line.back() == L' ' || title_line.back() == L'\t')) {
          title_line.pop_back();
        }
        break;
      }
      title_line.pop_back();
    }
    out.emit_title_line = true;
    out.title = TrimWsLocal(title_line);
    out.body = prefix + rest;
    return out;
  }
  out.emit_title_line = false;
  out.title.clear();
  out.body = prefix + raw;
  return out;
}

std::wstring JoinMarkdownTitleBody(const std::wstring& title,
                                   const std::wstring& body,
                                   bool emit_title_line) {
  std::wstring t = TrimWsLocal(title);
  for (auto& ch : t) {
    if (ch == L'\r' || ch == L'\n') {
      ch = L' ';
    }
  }
  std::wstring b = body;
  while (!b.empty() && (b[0] == L'\n' || b[0] == L'\r')) {
    b.erase(b.begin());
  }
  if (t.empty() && !emit_title_line) {
    return b;
  }
  return L"# " + t + L"\n\n" + b;
}

// First ATX heading (# … ######); empty heading text → empty (use filename).
std::wstring ExtractMarkdownTitle(const std::wstring& md) {
  size_t pos = 0;
  const size_t n = md.size();
  if (n > 0 && md[0] == L'\xFEFF') {
    pos = 1;
  }
  auto read_line = [&](std::wstring* out) -> bool {
    if (pos >= n) {
      return false;
    }
    const size_t start = pos;
    while (pos < n && md[pos] != L'\n') {
      ++pos;
    }
    *out = md.substr(start, pos - start);
    if (!out->empty() && out->back() == L'\r') {
      out->pop_back();
    }
    if (pos < n) {
      ++pos;
    }
    return true;
  };

  std::wstring line;
  const size_t save = pos;
  if (!read_line(&line)) {
    return L"";
  }
  if (TrimWsLocal(line) == L"---") {
    while (read_line(&line)) {
      if (TrimWsLocal(line) == L"---") {
        break;
      }
    }
  } else {
    pos = save;
  }

  while (read_line(&line)) {
    const std::wstring t = TrimWsLocal(line);
    if (t.empty()) {
      continue;
    }
    size_t hashes = 0;
    while (hashes < t.size() && t[hashes] == L'#') {
      ++hashes;
    }
    if (hashes >= 1 && hashes <= 6 && hashes < t.size() &&
        (t[hashes] == L' ' || t[hashes] == L'\t')) {
      std::wstring title = TrimWsLocal(t.substr(hashes + 1));
      while (!title.empty() && title.back() == L'#') {
        title.pop_back();
      }
      return TrimWsLocal(title);
    }
    break;
  }
  return L"";
}

// Library tree: cheap first-heading peek so closed notes match tab titles.
// Disk peek is called once per file on every sidebar rebuild, so it must not
// hit the disk repeatedly. Cached by (path → mtime,size) — same scheme as the
// note-text cache in notes_library.cpp, so edits still show up immediately.
std::wstring PeekMarkdownTitleFromDisk(const std::wstring& path) {
  struct PeekEntry {
    FILETIME mtime{};
    ULONGLONG size = 0;
    std::wstring title;
  };
  static std::mutex s_mu;
  static std::unordered_map<std::wstring, PeekEntry> s_cache;
  static unsigned long long s_generation = 0;
  // A flush elsewhere (delete/rename/external invalidation) must drop our copy
  // too — mtime+size cannot see a same-length rewrite within one second.
  {
    const unsigned long long gen = NotesFileCacheGeneration();
    std::lock_guard<std::mutex> lk(s_mu);
    if (gen != s_generation) {
      s_cache.clear();
      s_generation = gen;
    }
  }
  FILETIME mt{};
  ULONGLONG sz = 0;
  {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
      mt = fad.ftLastWriteTime;
      sz = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }
    std::lock_guard<std::mutex> lk(s_mu);
    const auto it = s_cache.find(path);
    if (it != s_cache.end() && CompareFileTime(&it->second.mtime, &mt) == 0 &&
        it->second.size == sz) {
      return it->second.title;
    }
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::string raw(4096, '\0');
  in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
  const std::streamsize got = in.gcount();
  if (got <= 0) {
    return {};
  }
  raw.resize(static_cast<size_t>(got));
  std::wstring wide;
  TextFileEncoding enc = TextFileEncoding::Utf8;
  if (!DecodeTextFileBytes(raw, &wide, &enc)) {
    wide = WidenUtf8(raw);
  }
  std::wstring title = ExtractMarkdownTitle(wide);
  {
    std::lock_guard<std::mutex> lk(s_mu);
    if (s_cache.size() >= 4096) {
      s_cache.clear();
    }
    PeekEntry e;
    e.mtime = mt;
    e.size = sz;
    e.title = title;
    s_cache[path] = std::move(e);
  }
  return title;
}
}  // namespace

std::wstring NotesWindow::NoteDisplayName(const std::wstring& path,
                                          bool peek_disk) const {
  std::wstring name;
  std::wstring ext;
  if (IsOnlineNotePath(path)) {
    // Prefer live title_hint; else short cloud label.
    const std::wstring key = NormalizePathKey(path);
    for (size_t i = 0; i < tab_paths_.size(); ++i) {
      if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(), key.c_str()) != 0) {
        continue;
      }
      if (i < tab_states_.size() && !tab_states_[i].title_hint.empty()) {
        std::wstring h = tab_states_[i].title_hint;
        if (h.rfind(L"☁ ", 0) == 0) h = h.substr(2);
        return h;
      }
      break;
    }
    const int id = OnlineNoteIdFromPath(path);
    for (const auto& it : library_online_cache_) {
      if (it.id == id && !it.title.empty()) {
        std::wstring t = it.title;
        if (t.rfind(L"☁ ", 0) == 0) t = t.substr(2);
        return t;
      }
    }
    return L"在线笔记";
  }
  {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring leaf =
        slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = leaf.find_last_of(L'.');
    // VS Code / Explorer: keep type suffix on tabs and the library tree.
    if (dot != std::wstring::npos && dot > 0 &&
        dot + 1 < leaf.size()) {
      ext = leaf.substr(dot);  // includes '.'
    }
  }
  // Managed vault mirror — never inherit a stale title_hint from another tab.
  if (IsVaultAccountsNotePath(path)) {
    name = L"账号库";
  }
  const bool md = PathIsMarkdownFile(path);
  const std::wstring key = NormalizePathKey(path);
  // Title-as-name is Markdown-only (Typora). Plain files keep Explorer names.
  if (name.empty() && md) {
    for (size_t i = 0; i < tab_paths_.size(); ++i) {
      if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(), key.c_str()) !=
          0) {
        continue;
      }
      // Only trust the synced title_hint — never Toast cache here. After
      // delete→seed 未命名, cache still holds the recycled note's H1 and would
      // paint「测试」on the new draft in the file tree (tab stem stayed 未命名).
      if (i < tab_states_.size() && !tab_states_[i].title_hint.empty()) {
        name = tab_states_[i].title_hint;
      }
      break;
    }
    if (name.empty() && peek_disk && !path.empty()) {
      name = PeekMarkdownTitleFromDisk(path);
    }
  }
  if (name.empty()) {
    const size_t slash = path.find_last_of(L"\\/");
    name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (name.empty()) {
      name = L"未命名";
    }
    // Prefer stem (notes.md → notes) then re-attach ext below.
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
      name = name.substr(0, dot);
    }
    name = LocalizeUntitledStem(name);
  }
  // Leave room for ".md" / ".txt" after ellipsis.
  const size_t max_base = ext.empty() ? 22u : 18u;
  if (name.size() > max_base) {
    name = name.substr(0, max_base > 2 ? max_base - 2 : max_base) + L"…";
  }
  if (!ext.empty()) {
    name += ext;
  }
  return name;
}

std::wstring NotesWindow::TabLabel(const std::wstring& path) const {
  // Dirty/saved glyph is painted beside the close plate (VS Code / Chrome
  // tab disk) — keep the text label clean of " ·".
  return NoteDisplayName(path, false);
}

void NotesWindow::SyncCurrentTabTitleHint() {
  if (loading_) {
    return;
  }
  EnsureTabStates();
  if (tab_index_ < 0 ||
      tab_index_ >= static_cast<int>(tab_states_.size())) {
    return;
  }
  auto& st = tab_states_[static_cast<size_t>(tab_index_)];
  // Non-Markdown: Explorer/Notepad — tab/tree show real filename, never H1.
  if (!PathLooksMarkdown()) {
    if (st.title_hint.empty()) {
      return;
    }
    st.title_hint.clear();
    RefreshCurrentTabLabel();
    RefreshTitle();
    RefreshSidebarLabelForPath(path_);
    SchedulePersistTabs();
    return;
  }
  std::wstring hint;
  if (wysiwyg_ && title_edit_) {
    // Native title is authoritative in WW — parsing body-only RichEdit after
    // H1 strip was clearing title_hint and left the title field empty on open.
    hint = NativeTitleText();
  } else {
    // Dirty WYSIWYG: Toast cache. Otherwise RichEdit (post-Load).
    std::wstring md;
    if (wysiwyg_ && web_.Ready() && dirty_) {
      md = web_.CachedMarkdown();
    } else if (edit_) {
      const int len = GetWindowTextLengthW(edit_);
      md.assign(static_cast<size_t>(len) + 1, L'\0');
      GetWindowTextW(edit_, md.data(), len + 1);
      md.resize(static_cast<size_t>(len));
    } else if (wysiwyg_) {
      md = web_.CachedMarkdown();
    }
    hint = ExtractMarkdownTitle(md);
  }
  if (st.title_hint == hint) {
    return;
  }
  st.title_hint = std::move(hint);
  RefreshCurrentTabLabel();
  RefreshTitle();
  RefreshSidebarLabelForPath(path_);
  // WebView library rail (TreeView is hidden) — keep titles in sync with H1.
  if (library_web_.Host() && IsWindowVisible(library_web_.Host())) {
    PushLibraryRailState(false);
  }
  SchedulePersistTabs();
  // Debounced — syncing on every keystroke blocked the UI and made the
  // title field feel late (MoveFile + SHChangeNotify on the UI thread).
  ScheduleSyncFilenameToTitle();
}

void NotesWindow::RefreshSidebarLabelForPath(const std::wstring& path) {
  if (!sidebar_ || !sidebar_visible_ || path.empty()) {
    return;
  }
  const std::wstring key = NormalizePathKey(path);
  std::function<bool(HTREEITEM)> walk = [&](HTREEITEM item) -> bool {
    while (item) {
      TVITEMW ti{};
      ti.mask = TVIF_HANDLE | TVIF_PARAM;
      ti.hItem = item;
      if (TreeView_GetItem(sidebar_, &ti) && ti.lParam > 0) {
        const size_t idx = static_cast<size_t>(ti.lParam) - 1;
        if (idx < sidebar_paths_.size() &&
            _wcsicmp(NormalizePathKey(sidebar_paths_[idx]).c_str(),
                     key.c_str()) == 0) {
          std::wstring label = NoteDisplayName(path, false);
          if (PathIsPinned(path)) {
            label = L"   " + label;
          }
          TVITEMW set{};
          set.mask = TVIF_HANDLE | TVIF_TEXT;
          set.hItem = item;
          set.pszText = label.data();
          TreeView_SetItem(sidebar_, &set);
          return true;
        }
      }
      if (HTREEITEM child = TreeView_GetChild(sidebar_, item)) {
        if (walk(child)) {
          return true;
        }
      }
      item = TreeView_GetNextSibling(sidebar_, item);
    }
    return false;
  };
  walk(TreeView_GetRoot(sidebar_));
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

// Small pushpin (no ★): round head + short neck + needle tip.
void DrawTabPushPin(Gdiplus::Graphics& g, float cx, float cy, bool pinned,
                    bool hot) {
  const BYTE a = 255;
  Gdiplus::Color ink;
  if (pinned) {
    ink = hot ? Gdiplus::Color(a, 51, 65, 85) : Gdiplus::Color(a, 71, 85, 105);
  } else {
    ink = hot ? Gdiplus::Color(a, 100, 116, 139)
              : Gdiplus::Color(a, 168, 174, 184);
  }
  Gdiplus::SolidBrush br(ink);
  Gdiplus::Pen pen(ink, 1.25f);
  pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                 Gdiplus::DashCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);

  // Tilt ~18° so it reads as a thumbtack, not a bullet.
  const float ang = -0.32f;
  const float ca = std::cos(ang);
  const float sa = std::sin(ang);
  auto xf = [&](float x, float y) {
    return Gdiplus::PointF(cx + x * ca - y * sa, cy + x * sa + y * ca);
  };

  // Head.
  const float hr = pinned ? 3.6f : 3.3f;
  const Gdiplus::PointF hc = xf(0.f, -3.2f);
  if (pinned) {
    g.FillEllipse(&br, hc.X - hr, hc.Y - hr, hr * 2.f, hr * 2.f);
  } else {
    g.DrawEllipse(&pen, hc.X - hr, hc.Y - hr, hr * 2.f, hr * 2.f);
  }
  // Collar under the head.
  const Gdiplus::PointF c0 = xf(-2.4f, -0.2f);
  const Gdiplus::PointF c1 = xf(2.4f, -0.2f);
  g.DrawLine(&pen, c0, c1);
  // Needle.
  const Gdiplus::PointF n0 = xf(0.f, -0.2f);
  const Gdiplus::PointF n1 = xf(0.f, 6.2f);
  g.DrawLine(&pen, n0, n1);
  if (pinned) {
    Gdiplus::PointF tip[3] = {xf(-1.8f, 4.2f), xf(1.8f, 4.2f), xf(0.f, 6.4f)};
    g.FillPolygon(&br, tip, 3);
  }
}

// 优化后的现代高质感云朵矢量图标 (天蓝色调，饱满圆润)
void DrawTabCloudGlyph(Gdiplus::Graphics& g, float cx, float cy, bool on) {
  Gdiplus::Color pen_color = on ? Gdiplus::Color(255, 37, 99, 235)
                                : Gdiplus::Color(255, 96, 165, 250);
  Gdiplus::Color fill_color = on ? Gdiplus::Color(200, 219, 234, 254)
                                 : Gdiplus::Color(140, 239, 246, 255);
  Gdiplus::Pen pen(pen_color, 1.25f);
  pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  Gdiplus::SolidBrush fill(fill_color);

  Gdiplus::GraphicsPath path;
  path.AddArc(cx + 0.9f, cy - 1.5f, 4.6f, 4.6f, 270.f, 180.f);
  path.AddArc(cx - 3.6f, cy - 4.6f, 6.8f, 6.8f, 200.f, 150.f);
  path.AddArc(cx - 5.7f, cy - 2.0f, 5.0f, 5.0f, 90.f, 180.f);
  path.CloseFigure();

  g.FillPath(&fill, &path);
  g.DrawPath(&pen, &path);
}

// 本地文档/便签折角小纸张矢量图标 (极简现代，中性灰/石板蓝)
void DrawTabDocumentGlyph(Gdiplus::Graphics& g, float cx, float cy, bool on) {
  Gdiplus::Color pen_color = on ? Gdiplus::Color(255, 100, 116, 139)
                                : Gdiplus::Color(255, 148, 163, 184);
  Gdiplus::Color fill_color = on ? Gdiplus::Color(255, 255, 255, 255)
                                 : Gdiplus::Color(220, 241, 245, 249);
  Gdiplus::Pen pen(pen_color, 1.15f);
  pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  Gdiplus::SolidBrush fill(fill_color);

  const float l = cx - 4.3f;
  const float r = cx + 4.3f;
  const float t = cy - 5.5f;
  const float b = cy + 5.5f;
  const float fold = 2.8f;

  Gdiplus::GraphicsPath doc;
  Gdiplus::PointF pts[5] = {
      {l, t},
      {r - fold, t},
      {r, t + fold},
      {r, b},
      {l, b}
  };
  doc.AddPolygon(pts, 5);
  g.FillPath(&fill, &doc);
  g.DrawPath(&pen, &doc);

  Gdiplus::PointF fold_pts[3] = {
      {r - fold, t},
      {r - fold, t + fold},
      {r, t + fold}
  };
  g.DrawLines(&pen, fold_pts, 3);

  Gdiplus::Pen line_pen(pen_color, 0.85f);
  g.DrawLine(&line_pen, l + 2.0f, cy + 0.6f, r - 2.0f, cy + 0.6f);
  g.DrawLine(&line_pen, l + 2.0f, cy + 3.0f, r - 3.0f, cy + 3.0f);
}

// System sticky tab (账号库): padlock where × would be.
void DrawTabLockGlyph(Gdiplus::Graphics& g, float cx, float cy) {
  Gdiplus::Pen pen(Gdiplus::Color(255, 100, 116, 139), 1.5f);
  pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                 Gdiplus::DashCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  // Shackle
  g.DrawArc(&pen, cx - 3.4f, cy - 6.2f, 6.8f, 6.8f, 200.f, 140.f);
  // Body
  Gdiplus::SolidBrush body(Gdiplus::Color(255, 100, 116, 139));
  Gdiplus::GraphicsPath box;
  GpAddRoundRect(&box, cx - 4.2f, cy - 1.2f, 8.4f, 6.6f, 1.6f);
  g.FillPath(&body, &box);
  // Keyhole
  Gdiplus::SolidBrush hole(Gdiplus::Color(255, 248, 250, 252));
  g.FillEllipse(&hole, cx - 1.1f, cy + 0.2f, 2.2f, 2.2f);
  g.FillRectangle(&hole, cx - 0.55f, cy + 1.6f, 1.1f, 2.2f);
}

// VS Code / Chrome-class tab dirty: filled disc = unsaved; check = saved.
void DrawTabSaveGlyph(Gdiplus::Graphics& g, float cx, float cy, bool dirty) {
  if (dirty) {
    Gdiplus::SolidBrush disc(Gdiplus::Color(255, 71, 85, 105));
    const float r = 3.4f;
    g.FillEllipse(&disc, cx - r, cy - r, r * 2.f, r * 2.f);
    return;
  }
  // Saved: small check in muted ink (always visible so both states read).
  Gdiplus::Pen pen(Gdiplus::Color(255, 34, 160, 107), 1.6f);
  pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound,
                 Gdiplus::DashCapRound);
  pen.SetLineJoin(Gdiplus::LineJoinRound);
  g.DrawLine(&pen, cx - 3.2f, cy + 0.4f, cx - 0.6f, cy + 3.0f);
  g.DrawLine(&pen, cx - 0.6f, cy + 3.0f, cx + 3.6f, cy - 2.8f);
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
    SIZE sz{};
    GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()),
                          &sz);
    // pin | text | save glyph | close
    const int lead = 30;
    const int type_w = 16;
    const int save_w = 16;
    const int close_w = 26;
    const int pad = 8;
    int w = sz.cx + lead + type_w + save_w + close_w + pad;
    if (w < 148) {
      w = 148;
    }
    if (w > 280) {
      w = 280;
    }
    tab_chip_rects_.push_back(RECT{x, y, x + w, y + h});
    x += w + 8;  // clear gap so idle chips read as separate units
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
  const int rw = rc.right - rc.left;
  const int rh = rc.bottom - rc.top;
  if (rw <= 0 || rh <= 0) {
    return;
  }
  // Double-buffer: GDI FillRect + Gdiplus on the same screen DC flickers on
  // every hover invalidate.
  HDC mem = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, rw, rh);
  HGDIOBJ old_bmp = SelectObject(mem, bmp);
  // Cool rail under chips so white idle/active pills separate (Chrome-like).
  const COLORREF desk = toast_ ? RGB(255, 248, 220) : rail_bg_;
  HBRUSH desk_br = CreateSolidBrush(desk);
  FillRect(mem, &rc, desk_br);
  DeleteObject(desk_br);
  HDC screen = hdc;
  hdc = mem;

  // Soft separator — rail gray, not heavy black.
  HPEN pen = CreatePen(PS_SOLID, 1, border_color_);
  HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
  MoveToEx(hdc, 0, rc.bottom - 1, nullptr);
  LineTo(hdc, rc.right, rc.bottom - 1);
  SelectObject(hdc, old_pen);
  DeleteObject(pen);

  SetBkMode(hdc, TRANSPARENT);
  {
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
      // Cloud-import tabs (…\云笔记\…) use a blue shell to contrast local tabs.
      const bool cloud = PathLooksCloudNotes(path_s);
      if (cloud) {
        Gdiplus::SolidBrush fill(on ? Gdiplus::Color(255, 219, 234, 254)
                                    : hover ? Gdiplus::Color(255, 239, 246, 255)
                                            : Gdiplus::Color(255, 248, 250, 252));
        g.FillPath(&fill, &path);
        Gdiplus::Pen ring(Gdiplus::Color(255, 96, 165, 250), on ? 1.6f : 1.2f);
        g.DrawPath(&ring, &path);
        Gdiplus::SolidBrush bar(Gdiplus::Color(255, 47, 111, 237));
        g.FillRectangle(&bar, x + 2.f, y + 4.f, 3.f, h - 8.f);
      } else if (on) {
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 255, 255, 255));
        g.FillPath(&fill, &path);
        Gdiplus::Pen ring(Gdiplus::Color(255, 180, 188, 200), 1.25f);
        g.DrawPath(&ring, &path);
      } else if (hover) {
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 255, 255, 255));
        g.FillPath(&fill, &path);
        Gdiplus::Pen ring(Gdiplus::Color(255, 198, 204, 214), 1.f);
        g.DrawPath(&ring, &path);
      } else {
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, 252, 253, 254));
        g.FillPath(&fill, &path);
        Gdiplus::Pen ring(Gdiplus::Color(255, 214, 219, 226), 1.f);
        g.DrawPath(&ring, &path);
      }
    }

    HFONT use = on && ui_font_bold_ ? ui_font_bold_ : ui_font_;
    HFONT old_f =
        use ? static_cast<HFONT>(SelectObject(hdc, use)) : nullptr;
    // Idle text a touch stronger so labels don't wash into the bar.
    SetTextColor(hdc, on ? text_color_ : RGB(72, 80, 92));

    // Thumbtack control (not ★): click = pin / unpin.
    const bool pin_hot = hover && tab_hover_zone_ == 1;
    RECT pin_hit{chip.left + 4, chip.top + 3, chip.left + 24, chip.bottom - 3};
    if (pin_hot) {
      Gdiplus::GraphicsPath pp;
      GpAddRoundRect(&pp, static_cast<float>(pin_hit.left),
                     static_cast<float>(pin_hit.top),
                     static_cast<float>(pin_hit.right - pin_hit.left),
                     static_cast<float>(pin_hit.bottom - pin_hit.top), 5.f);
      Gdiplus::SolidBrush pbg(Gdiplus::Color(255, 220, 225, 232));
      g.FillPath(&pbg, &pp);
    }
    DrawTabPushPin(g,
                   (static_cast<float>(pin_hit.left + pin_hit.right)) * 0.5f,
                   (static_cast<float>(pin_hit.top + pin_hit.bottom)) * 0.5f +
                       0.5f,
                   pinned, pin_hot);
    SetTextColor(hdc, on ? text_color_ : RGB(72, 80, 92));
    const int text_left = chip.left + 26;
    // Locked tabs (账号库 always; user lock): padlock instead of ×.
    const bool locked = PathIsLocked(path_s);
    const bool vault_locked = IsVaultAccountsNotePath(path_s);
    // Layout on right side (from right to left):
    // [close/lock ×: right-24..right-6] [save glyph: right-34] [cloud/doc glyph: right-50]
    const int save_cx = chip.right - 34;
    const int type_cx = chip.right - 50;
    const bool tab_dirty = on && dirty_;

    std::wstring label = TabLabel(path_s);
    if (label.rfind(L"☁ ", 0) == 0) {
      label = label.substr(2);
    }
    RECT text_rc{text_left, chip.top, chip.right - 60, chip.bottom};
    DrawTextW(hdc, label.c_str(), -1, &text_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    const float cy_mid = (static_cast<float>(chip.top + chip.bottom)) * 0.5f;
    const bool is_cloud = IsOnlineNotePath(path_s) || PathLooksCloudNotes(path_s) || PathIsCloudSynced(path_s);
    if (is_cloud) {
      DrawTabCloudGlyph(g, static_cast<float>(type_cx), cy_mid, on);
    } else {
      DrawTabDocumentGlyph(g, static_cast<float>(type_cx), cy_mid, on);
    }

    DrawTabSaveGlyph(
        g, static_cast<float>(save_cx), cy_mid, tab_dirty);

    if (locked) {
      // Padlock: vault cannot unlock; user-locked → click unlocks.
      const bool lock_hot = hover && tab_hover_zone_ == 2 && !vault_locked;
      RECT lock_rc{chip.right - 26, chip.top + 6, chip.right - 6,
                   chip.bottom - 6};
      if (lock_hot) {
        Gdiplus::GraphicsPath lp;
        GpAddRoundRect(&lp, static_cast<float>(lock_rc.left),
                       static_cast<float>(lock_rc.top),
                       static_cast<float>(lock_rc.right - lock_rc.left),
                       static_cast<float>(lock_rc.bottom - lock_rc.top), 6.f);
        Gdiplus::SolidBrush lbg(Gdiplus::Color(255, 220, 225, 232));
        g.FillPath(&lbg, &lp);
      }
      DrawTabLockGlyph(
          g, static_cast<float>(chip.right - 14),
          (static_cast<float>(chip.top + chip.bottom)) * 0.5f);
    } else {
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
      SetTextColor(hdc,
                   close_hot ? text_color_
                             : (on ? RGB(120, 128, 140) : RGB(168, 174, 184)));
      DrawTextW(hdc, L"×", -1, &close_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
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
  }  // Gdiplus::Graphics scope — flush before BitBlt

  BitBlt(screen, 0, 0, rw, rh, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old_bmp);
  DeleteObject(bmp);
  DeleteDC(mem);
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
      // Hysteresis on pin/close edges so cursor hotspot changes don't oscillate
      // zone → Invalidate → flicker while the mouse is still.
      constexpr int kPinIn = 24;
      constexpr int kPinOut = 28;
      constexpr int kCloseIn = 26;
      constexpr int kCloseOut = 30;
      int zone = 0;
      if (tab_hover_index_ == static_cast<int>(i) && tab_hover_zone_ == 1) {
        zone = (client.x <= chip.left + kPinOut) ? 1 : 0;
      } else if (tab_hover_index_ == static_cast<int>(i) &&
                 tab_hover_zone_ == 2) {
        zone = (client.x >= chip.right - kCloseOut) ? 2 : 0;
      } else if (client.x <= chip.left + kPinIn) {
        zone = 1;
      } else if (client.x >= chip.right - kCloseIn) {
        zone = 2;
      }
      // Vault lock: no click target. User-locked: zone 2 = unlock (not close).
      if (zone == 2 && i < tab_paths_.size() &&
          IsVaultAccountsNotePath(tab_paths_[i])) {
        zone = 0;
      }
      *out_zone = zone;
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
  EnsureTabStates();
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_tabs.json");
  // Objects carry optional title so cold-start tab chips match last-known heading
  // (Typora / VS Code: reopen session shows document titles, not only filenames).
  std::ostringstream out;
  out << "{\n  \"current\": " << tab_index_ << ",\n  \"tabs\": [\n";
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    out << "    {\"path\": \"" << EscapeJson(NarrowUtf8(tab_paths_[i]))
        << "\"";
    if (i < tab_states_.size()) {
      // Always emit title (even "") so clearing a heading drops the stale hint.
      out << ", \"title\": \""
          << EscapeJson(NarrowUtf8(tab_states_[i].title_hint)) << "\"";
    }
    out << "}";
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
  out << "  ],\n  \"locked\": [\n";
  for (size_t i = 0; i < locked_paths_.size(); ++i) {
    out << "    \"" << EscapeJson(NarrowUtf8(locked_paths_[i])) << "\"";
    if (i + 1 < locked_paths_.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
  // Atomic replace: the old in-place ofstream truncated notes_tabs.json first,
  // so a crash mid-write (timer fires every 400ms of typing) corrupted the
  // whole session file.
  AtomicWriteUtf8File(file, out.str(), false);
}

void NotesWindow::LoadTabSession() {
  tab_paths_.clear();
  pinned_paths_.clear();
  locked_paths_.clear();
  tab_states_.clear();
  tab_index_ = 0;
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_tabs.json");
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  auto append_utf8 = [](std::string* out, uint32_t cp) {
    if (cp <= 0x7F) {
      out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };
  auto parse_hex4 = [](const std::string& s, size_t idx, uint32_t* out_val) {
    if (idx + 4 > s.size()) {
      return false;
    }
    uint32_t v = 0;
    for (size_t k = 0; k < 4; ++k) {
      const char c = s[idx + k];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return false;
      }
    }
    *out_val = v;
    return true;
  };
  auto unescape_json = [&](const std::string& raw) {
    std::string unescaped;
    unescaped.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\\' && i + 1 < raw.size()) {
        const char e = raw[++i];
        switch (e) {
          case 'n':
            unescaped.push_back('\n');
            break;
          case 'r':
            unescaped.push_back('\r');
            break;
          case 't':
            unescaped.push_back('\t');
            break;
          case 'b':
            unescaped.push_back('\b');
            break;
          case 'f':
            unescaped.push_back('\f');
            break;
          case '"':
          case '\\':
          case '/':
            unescaped.push_back(e);
            break;
          case 'u': {
            uint32_t cp = 0;
            if (parse_hex4(raw, i + 1, &cp)) {
              i += 4;
              if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < raw.size() &&
                  raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                uint32_t low = 0;
                if (parse_hex4(raw, i + 3, &low) && low >= 0xDC00 &&
                    low <= 0xDFFF) {
                  cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                  i += 6;
                }
              }
              append_utf8(&unescaped, cp);
            } else {
              unescaped.push_back('u');
            }
            break;
          }
          default:
            unescaped.push_back(e);
            break;
        }
      } else {
        unescaped.push_back(raw[i]);
      }
    }
    return unescaped;
  };
  auto find_closing_quote = [](const std::string& s, size_t start_after_open) {
    size_t i = start_after_open;
    while (i < s.size()) {
      if (s[i] == '\\') {
        i += 2;
        continue;
      }
      if (s[i] == '"') {
        return i;
      }
      ++i;
    }
    return std::string::npos;
  };
  auto find_closing_brace = [&](const std::string& s, size_t open_brace) {
    size_t i = open_brace + 1;
    while (i < s.size()) {
      if (s[i] == '"') {
        const size_t q2 = find_closing_quote(s, i + 1);
        if (q2 == std::string::npos) {
          return std::string::npos;
        }
        i = q2 + 1;
        continue;
      }
      if (s[i] == '}') {
        return i;
      }
      ++i;
    }
    return std::string::npos;
  };
  auto looks_like_path = [](const std::string& unescaped) {
    if (unescaped.rfind("online://", 0) == 0) {
      return true;
    }
    return unescaped.size() >= 3 &&
           ((unescaped[1] == ':' &&
             (unescaped[2] == '\\' || unescaped[2] == '/')) ||
            unescaped[0] == '\\');
  };
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
      while (pos < json.size() &&
             (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' ||
              json[pos] == '\t' || json[pos] == ',')) {
        ++pos;
      }
      if (pos >= json.size() || json[pos] == ']') {
        break;
      }
      if (json[pos] != '"') {
        break;
      }
      const size_t q1 = pos;
      const size_t q2 = find_closing_quote(json, q1 + 1);
      if (q2 == std::string::npos) {
        break;
      }
      const std::string unescaped =
          unescape_json(json.substr(q1 + 1, q2 - q1 - 1));
      pos = q2 + 1;
      if (looks_like_path(unescaped)) {
        out->push_back(WidenUtf8(unescaped));
      }
    }
  };
  // Tabs: accept legacy string entries or {"path","title"} objects.
  std::vector<std::wstring> session_titles;
  {
    const size_t key_pos = json.find("\"tabs\"");
    if (key_pos != std::string::npos) {
      const size_t arr = json.find('[', key_pos);
      if (arr != std::string::npos) {
        size_t pos = arr + 1;
        while (pos < json.size() && json[pos] != ']') {
          while (pos < json.size() &&
                 (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' ||
                  json[pos] == '\t' || json[pos] == ',')) {
            ++pos;
          }
          if (pos >= json.size() || json[pos] == ']') {
            break;
          }
          if (json[pos] == '{') {
            const size_t end = find_closing_brace(json, pos);
            if (end == std::string::npos) {
              break;
            }
            const std::string obj = json.substr(pos, end - pos + 1);
            pos = end + 1;
            std::string path_u8;
            std::string title_u8;
            auto field = [&](const char* name, std::string* dest) {
              const std::string needle = std::string("\"") + name + "\"";
              const size_t kp = obj.find(needle);
              if (kp == std::string::npos) {
                return;
              }
              const size_t colon = obj.find(':', kp + needle.size());
              if (colon == std::string::npos) {
                return;
              }
              const size_t q1 = obj.find('"', colon + 1);
              if (q1 == std::string::npos) {
                return;
              }
              const size_t q2 = find_closing_quote(obj, q1 + 1);
              if (q2 == std::string::npos) {
                return;
              }
              *dest = unescape_json(obj.substr(q1 + 1, q2 - q1 - 1));
            };
            field("path", &path_u8);
            field("title", &title_u8);
            if (looks_like_path(path_u8)) {
              tab_paths_.push_back(WidenUtf8(path_u8));
              session_titles.push_back(WidenUtf8(title_u8));
            }
          } else if (json[pos] == '"') {
            const size_t q1 = pos;
            const size_t q2 = find_closing_quote(json, q1 + 1);
            if (q2 == std::string::npos) {
              break;
            }
            const std::string unescaped =
                unescape_json(json.substr(q1 + 1, q2 - q1 - 1));
            pos = q2 + 1;
            if (looks_like_path(unescaped)) {
              tab_paths_.push_back(WidenUtf8(unescaped));
              session_titles.push_back(L"");
            }
          } else {
            break;
          }
        }
      }
    }
  }
  collect_paths("pinned", &pinned_paths_);
  collect_paths("locked", &locked_paths_);
  const size_t cur_key = json.find("\"current\"");
  if (cur_key != std::string::npos) {
    const size_t colon = json.find(':', cur_key);
    if (colon != std::string::npos) {
      tab_index_ = atoi(json.c_str() + colon + 1);
    }
  }
  // Drop session paths that no longer exist on disk (keep title aligned).
  {
    std::vector<std::wstring> alive;
    std::vector<std::wstring> alive_titles;
    alive.reserve(tab_paths_.size());
    alive_titles.reserve(tab_paths_.size());
    for (size_t i = 0; i < tab_paths_.size(); ++i) {
      if (GetFileAttributesW(tab_paths_[i].c_str()) !=
          INVALID_FILE_ATTRIBUTES) {
        alive.push_back(tab_paths_[i]);
        alive_titles.push_back(i < session_titles.size() ? session_titles[i]
                                                         : L"");
      }
    }
    tab_paths_ = std::move(alive);
    session_titles = std::move(alive_titles);
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
        session_titles.push_back(L"");
      }
    }
  }
  if (tab_paths_.empty()) {
    tab_index_ = 0;
  } else if (tab_index_ < 0 ||
             tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = 0;
  }
  // Migrate leftover English drafts (untitled.md) → 未命名.md on disk + tabs.
  HealUntitledDraftsOnDisk();
  bool healed = false;
  for (auto& p : tab_paths_) {
    if (HealEnglishUntitledPath(&p)) {
      healed = true;
    }
  }
  for (auto& p : pinned_paths_) {
    if (HealEnglishUntitledPath(&p)) {
      healed = true;
    }
  }
  for (auto& p : locked_paths_) {
    if (HealEnglishUntitledPath(&p)) {
      healed = true;
    }
  }
  EnsureTabStates();
  for (size_t i = 0; i < tab_states_.size(); ++i) {
    if (i < session_titles.size() && !session_titles[i].empty()) {
      tab_states_[i].title_hint = session_titles[i];
    }
  }
  if (healed) {
    SchedulePersistTabs();
  }
}

void NotesWindow::SwitchToTab(int index, bool save_current) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  // Drop any pending debounced title sync: the outgoing tab is flushed by the
  // immediate SyncCurrentTabTitleHint() below, and a stray timer firing after
  // tab_index_/edit_ repoint would read the wrong tab's body.
  if (hwnd_) {
    KillTimer(hwnd_, kTitleSyncTimer);
  }
  const std::wstring incoming = tab_paths_[static_cast<size_t>(index)];
  const bool same_path =
      !path_.empty() &&
      _wcsicmp(NormalizePathKey(path_).c_str(),
               NormalizePathKey(incoming).c_str()) == 0;
  if (save_current) {
    // Only flush when dirty — Save() on a clean tab still rewrote the file
    // (and could push cloud), which made tab switches feel stuck.
    if (dirty_) {
      // Local write must not block the switch (no network), but the cloud push
      // still has to happen: skipping it here meant an edit made just before
      // switching tabs never reached the server once the user stopped editing
      // that note. Enqueue is async, so this stays off the UI thread's back.
      Save(wysiwyg_, /*push_cloud=*/true);
    }
    SyncCurrentTabTitleHint();
  } else if (!same_path) {
    // delete→seed / open another file: do NOT SyncFromWysiwyg — that copies
    // the previous Toast body into RichEdit and lets title sync rename the
    // new 未命名 draft back to the deleted note's name.
  } else {
    SyncFromWysiwyg();
    SyncCurrentTabTitleHint();
  }
  // Remember the outgoing tab's WYSIWYG / wrap / caret / scroll (M8).
  EnsureTabStates();
  if (save_current && tab_index_ >= 0 &&
      tab_index_ < static_cast<int>(tab_states_.size()) &&
      tab_index_ < static_cast<int>(tab_paths_.size())) {
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
  path_ = incoming;
  DismissLibraryMenu();
  HealRetargetPath(&path_);
  if (tabs_) {
    InvalidateRect(tabs_, nullptr, FALSE);
  }
  Load();
  EnsureTabStates();
  word_wrap_ = tab_states_[static_cast<size_t>(tab_index_)].wrap;
  ApplyWordWrap();
  if (menu_) {
    CheckMenuItem(menu_, kMenuWrap,
                  MF_BYCOMMAND | (word_wrap_ ? MF_CHECKED : MF_UNCHECKED));
  }
  // Read-only preview pane is opt-in; never inherit across tabs.
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
  // Markdown → Typora-class editable preview (Toast WYSIWYG). Do not restore
  // tab_states_.wysiwyg=false from OpenPaths / session defaults — that left a
  // blank RichEdit stage after 「用 DeskNote 打开」 / 拖入 .md.
  if (PathLooksMarkdown() && !toast_) {
    EnsureMarkdownEditor(true);
    SyncCurrentTabTitleHint();
  } else {
    wysiwyg_ = false;
    tab_states_[static_cast<size_t>(tab_index_)].wysiwyg = false;
    ApplyWysiwygVisibility();
    if (edit_) {
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
      ShowWindow(edit_, SW_SHOW);
      SetFocus(edit_);
    }
  }
  RefreshTitle();
  if (outline_) {
    // Always arm — list may be hidden under WYSIWYG; rebuild before it shows.
    SetTimer(hwnd_, kOutlineTimer, 120, nullptr);
  }
  if (sidebar_visible_) {
    // Unified WebView rail owns selection highlight — skip TreeView walk.
    if (library_web_.Host() && IsWindowVisible(library_web_.Host())) {
      UpdateLibraryRailActiveOnly();
    } else {
      HighlightSidebarPath(path_);
    }
  }
  SchedulePersistTabs();
  UpdateStatusBar();
  UpdateModeStrip();
  // EnsureMarkdownEditor already LayoutClient'd for markdown; still needed
  // for non-md so chrome sizes catch up.
  if (!PathLooksMarkdown()) {
    LayoutClient();
  }
  FocusActiveEditor(true);
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

bool NotesWindow::SeedBlankDraftTab() {
  // Reuse an existing empty 未命名 draft before creating another one. Closing
  // the last tab (or deleting the last note) always seeds a blank draft, so
  // without this every such gesture left another empty 未命名_N.md behind and
  // the library filled up with them.
  const std::wstring dir = NotesLibraryDir(true);
  std::wstring path;
  {
    for (int i = 0; i < 1000; ++i) {
      wchar_t name[64];
      if (i == 0) {
        swprintf_s(name, L"未命名.md");
      } else {
        swprintf_s(name, L"未命名_%02d.md", i);
      }
      const std::wstring cand = JoinPath(dir, name);
      if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) {
        continue;
      }
      // Only reuse a placeholder that carries no content the user could lose.
      std::ifstream in(cand, std::ios::binary);
      if (!in) {
        continue;
      }
      std::string raw((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
      const std::wstring txt = WidenUtf8(raw);
      std::wstring trimmed = txt;
      while (!trimmed.empty() && iswspace(trimmed.back())) {
        trimmed.pop_back();
      }
      while (!trimmed.empty() && iswspace(trimmed.front())) {
        trimmed.erase(trimmed.begin());
      }
      if (trimmed == L"#" || trimmed.empty()) {
        path = cand;
        break;
      }
    }
  }
  if (path.empty()) {
    path = MakeNewNotePath();
  }
  bool wrote = true;
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::ofstream out(path, std::ios::binary);
    out << u8"# \n\n";
    wrote = static_cast<bool>(out);
  }
  if (!wrote) {
    // Do not point the editor at a file that does not exist: the tab would look
    // fine but every later Save/rename would fail against a phantom path.
    MistAlert(hwnd_, L"新建笔记",
              L"无法在笔记库创建文件，请检查目录权限或磁盘空间。",
              MistMsgTone::Danger);
    return false;
  }
  InvalidateNotesFileCache();
  tab_paths_.assign(1, path);
  tab_states_.assign(1, TabState{});
  tab_index_ = 0;
  path_ = path;
  // New .md draft → editable preview (same as NewTab).
  tab_states_[0].wysiwyg = PathLooksMarkdown();
  dirty_ = false;
  // Drop the recycled note's Toast body immediately so tree/tab/title sync
  // cannot re-read「测试」and rename this 未命名 draft.
  if (web_.Available()) {
    web_.SetMarkdown(L"# \n\n", NoteBaseDir());
  }
  if (edit_) {
    SetWindowTextW(edit_, L"# \n\n");
  }
  SetNativeTitleText(L"");
  return true;
}

void NotesWindow::ActivateSeededBlankDraft(const std::wstring& removed_path) {
  dirty_ = false;
  // SeedBlank already wrote disk + cleared Toast/RichEdit — skip Load and a
  // second force SetMarkdown (those made delete-last feel multi-second).
  if (PathLooksMarkdown() && !toast_) {
    EnsureMarkdownEditor(false);
  } else {
    wysiwyg_ = false;
    ApplyWysiwygVisibility();
  }
  tab_chips_dirty_ = true;
  RebuildTabControl();
  RefreshTitle();
  UpdateStatusBar();
  UpdateModeStrip();
  SyncMdMenuState();
  ApplyViewMenuChecks();
  LayoutClient();
  if (sidebar_visible_ && sidebar_) {
    if (!removed_path.empty()) {
      SidebarRemovePaths({removed_path});
    }
    SidebarUpsertFile(path_);
  }
  SchedulePersistTabs();
  if (outline_) {
    SetTimer(hwnd_, kOutlineTimer, 120, nullptr);
  }
}

void NotesWindow::SyncTabsAndSidebar(bool reload_editor, bool rebuild_tree) {
  tab_chips_dirty_ = true;
  if (reload_editor) {
    if (tab_index_ < 0 ||
        tab_index_ >= static_cast<int>(tab_paths_.size())) {
      tab_index_ = tab_paths_.empty() ? 0
                                      : static_cast<int>(tab_paths_.size()) - 1;
    }
    if (!tab_paths_.empty()) {
      SwitchToTab(tab_index_, false);
    }
  }
  RebuildTabControl();
  if (rebuild_tree && sidebar_visible_) {
    RebuildSidebar();
  }
  SchedulePersistTabs();
}

bool NotesWindow::HealRetargetPath(std::wstring* path_inout) {
  if (!path_inout || path_inout->empty()) {
    return false;
  }
  if (IsOnlineNotePath(*path_inout)) {
    return false;
  }
  const std::wstring old = *path_inout;
  if (!HealEnglishUntitledPath(path_inout)) {
    return false;
  }
  RetargetNotePath(old, *path_inout);
  RefreshSidebarLabelForPath(*path_inout);
  RefreshCurrentTabLabel();
  RefreshTitle();
  if (tabs_) {
    InvalidateRect(tabs_, nullptr, FALSE);
  }
  return true;
}

namespace {
std::wstring SanitizeNoteFileStem(const std::wstring& title) {
  std::wstring s;
  s.reserve(title.size());
  for (wchar_t c : title) {
    if (c < 32) {
      continue;
    }
    if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
        c == L'"' || c == L'<' || c == L'>' || c == L'|') {
      c = L'_';
    }
    s.push_back(c);
  }
  while (!s.empty() && iswspace(s.front())) {
    s.erase(s.begin());
  }
  while (!s.empty() && iswspace(s.back())) {
    s.pop_back();
  }
  // Trim trailing dots/spaces (Windows forbids).
  while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) {
    s.pop_back();
  }
  if (s.size() > 60) {
    s.resize(60);
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) {
      s.pop_back();
    }
  }
  return s;
}

}  // namespace

void NotesWindow::ScheduleSyncFilenameToTitle() {
  if (!hwnd_) {
    SyncFilenameToTitle();
    return;
  }
  SetTimer(hwnd_, kSyncFilenameTimer, 450, nullptr);
}

bool NotesWindow::SyncFilenameToTitle() {
  if (hwnd_) {
    KillTimer(hwnd_, kSyncFilenameTimer);
  }
  if (loading_ || path_.empty()) {
    return false;
  }
  // Cloud notes have no local filename to rename.
  if (IsOnlineNotePath(path_)) {
    return false;
  }
  // Title ↔ filename is Markdown / Typora behavior only — never for hosts,
  // .txt, or other Explorer-opened plain files.
  if (!PathLooksMarkdown()) {
    return false;
  }
  // Typora only auto-renames notes you own — never system/external files.
  if (IsHostsPath(path_)) {
    return false;
  }
  {
    const std::wstring key = NormalizePathKey(path_);
    auto under = [&](const std::wstring& root) {
      if (root.empty()) {
        return false;
      }
      const std::wstring r = NormalizePathKey(root);
      if (key.size() < r.size() ||
          _wcsnicmp(key.c_str(), r.c_str(), r.size()) != 0) {
        return false;
      }
      return key.size() == r.size() || key[r.size()] == L'\\';
    };
    if (!under(NotesLibraryDir(false)) &&
        !under(DocumentsDeskTidyNotesDir())) {
      return false;
    }
  }
  EnsureTabStates();
  if (tab_index_ < 0 ||
      tab_index_ >= static_cast<int>(tab_states_.size())) {
    return false;
  }
  const std::wstring& hint = tab_states_[static_cast<size_t>(tab_index_)].title_hint;
  if (hint.empty()) {
    return false;
  }
  const std::wstring want = SanitizeNoteFileStem(hint);
  if (want.empty()) {
    return false;
  }
  const size_t slash = path_.find_last_of(L"\\/");
  std::wstring name =
      slash == std::wstring::npos ? path_ : path_.substr(slash + 1);
  const size_t dot = name.find_last_of(L'.');
  const std::wstring stem =
      (dot == std::wstring::npos || dot == 0) ? name : name.substr(0, dot);
  if (_wcsicmp(stem.c_str(), want.c_str()) == 0) {
    return false;
  }
  // Typora-class: filename keeps following the title on every edit — not only
  // the first 未命名→标题 rename (Explorer stayed stale after that).
  return ApplySidebarRename(path_, want, true);
}

void NotesWindow::HealUntitledDraftsOnDisk() {
  auto scan = [this](const std::wstring& dir) {
    if (dir.empty()) {
      return;
    }
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      return;
    }
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        continue;
      }
      std::wstring full = dir + L"\\" + fd.cFileName;
      const std::wstring before = full;
      if (HealEnglishUntitledPath(&full)) {
        RetargetNotePath(before, full);
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  };
  scan(NotesLibraryDir(true));
  const std::wstring docs = DocumentsDeskTidyNotesDir();
  if (!docs.empty() &&
      _wcsicmp(NormalizePathKey(docs).c_str(),
               NormalizePathKey(NotesLibraryDir(false)).c_str()) != 0) {
    scan(docs);
  }
}

namespace {
// The vault API syncs note bodies only — image binaries are never uploaded.
// An online note's `assets/foo.png` therefore resolves against 云笔记/assets,
// but the file usually still lives under whichever local note inserted it.
// Gather referenced files into the shared folder so online notes (and their
// preview tab) can display images that already exist somewhere in the library.
// Directory list under the notes library, cached briefly. MaterializeOnline-
// NoteAssets runs on the UI thread every time an online note is opened and only
// needs this list to locate assets/; a full walk is hundreds of FindFirstFile
// calls. The set changes at most when the user creates a note folder, so a
// few seconds of staleness is invisible (a miss simply re-walks next time).
std::vector<std::wstring> CachedNoteLibraryDirs() {
  static std::mutex mu;
  static std::vector<std::wstring> cache;
  static std::wstring cache_root;
  static ULONGLONG cache_tick = 0;
  constexpr ULONGLONG kTtlMs = 5000;

  const std::wstring root = NotesLibraryDir(false);
  if (root.empty()) {
    return {};
  }
  const ULONGLONG now = GetTickCount64();
  {
    std::lock_guard<std::mutex> lk(mu);
    if (!cache.empty() && cache_root == root && now - cache_tick < kTtlMs) {
      return cache;
    }
  }
  std::vector<std::wstring> dirs{root};
  for (size_t i = 0; i < dirs.size() && dirs.size() < 512; ++i) {
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((dirs[i] + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      const std::wstring name = fd.cFileName;
      if (name == L"." || name == L"..") {
        continue;
      }
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        dirs.push_back(JoinPath(dirs[i], name));
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  {
    std::lock_guard<std::mutex> lk(mu);
    cache = dirs;
    cache_root = root;
    cache_tick = now;
  }
  return dirs;
}

void MaterializeOnlineNoteAssets(HWND hwnd, int note_id,
                                 const std::wstring& body,
                                 const std::wstring& dest_assets) {
  if (body.empty()) {
    return;
  }
  // Collect referenced asset filenames (assets/<name>, case-insensitive).
  std::vector<std::wstring> wanted;
  size_t pos = 0;
  const std::wstring needle = L"assets/";
  while ((pos = body.find(needle, pos)) != std::string::npos) {
    size_t begin = pos + needle.size();
    size_t end = begin;
    while (end < body.size() && body[end] != L'"' && body[end] != L'\'' &&
           body[end] != L')' && body[end] != L' ' && body[end] != L'\n' &&
           body[end] != L'\r' && body[end] != L'>') {
      ++end;
    }
    std::wstring leaf = body.substr(begin, end - begin);
    if (!leaf.empty() && leaf.find(L'/') == std::wstring::npos &&
        leaf.find(L'\\') == std::wstring::npos) {
      wanted.push_back(leaf);
    }
    pos = end;
  }
  if (wanted.empty()) {
    return;
  }
  CreateDirectoryW(dest_assets.c_str(), nullptr);

  const std::wstring root = NotesLibraryDir(false);
  if (root.empty()) {
    return;
  }
  // Bounded, cached walk (max depth mirrors notes_library.cpp's WalkOpenable).
  const std::vector<std::wstring> dirs = CachedNoteLibraryDirs();

  for (const std::wstring& leaf : wanted) {
    const std::wstring dest = JoinPath(dest_assets, leaf);
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
      continue;  // already present
    }
    bool found_local = false;
    for (const std::wstring& dir : dirs) {
      if (_wcsicmp(dir.c_str(), dest_assets.c_str()) == 0) {
        continue;
      }
      // Images are stored under each note's own <folder>/assets/<name>,
      // not at the folder root.
      const std::wstring candidate =
          JoinPath(JoinPath(dir, L"assets"), leaf);
      if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
        continue;
      }
      CopyFileW(candidate.c_str(), dest.c_str(), FALSE);
      found_local = true;
      break;
    }
    // Not on this machine — fetch the binary from the cloud in the background
    // (never block the UI on WinHTTP retries) and refresh the preview when done.
    if (!found_local && note_id > 0 && hwnd && VaultHttpLoggedIn()) {
      std::thread([hwnd, note_id, leaf, dest]() {
        std::vector<BYTE> bytes;
        if (!VaultHttpGetNoteAsset(note_id, leaf, &bytes) || bytes.empty()) {
          return;
        }
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        const bool wrote = static_cast<bool>(out);
        if (wrote) {
          out.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
          out.close();
        }
        if (wrote && IsWindow(hwnd)) {
          PostMessageW(hwnd, WM_APP + 95, 0, 0);  // repaint image preview
        }
      }).detach();
    }
  }
}
bool TryRenameNoteFile(const std::wstring& src, const std::wstring& dest) {
  if (src.empty() || dest.empty() ||
      _wcsicmp(src.c_str(), dest.c_str()) == 0) {
    return false;
  }
  SetFileAttributesW(src.c_str(), FILE_ATTRIBUTE_NORMAL);
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (attempt > 0) {
      Sleep(50u * static_cast<DWORD>(attempt));
    }
    if (MoveFileW(src.c_str(), dest.c_str())) {
      return true;
    }
    if (MoveFileExW(src.c_str(), dest.c_str(),
                    MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING |
                        MOVEFILE_WRITE_THROUGH)) {
      return true;
    }
  }
  // Shell rename (OneDrive / Explorer locks often honor this when MoveFile fails).
  {
    std::vector<wchar_t> from(src.begin(), src.end());
    from.push_back(L'\0');
    from.push_back(L'\0');
    std::vector<wchar_t> to(dest.begin(), dest.end());
    to.push_back(L'\0');
    to.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_RENAME;
    op.pFrom = from.data();
    op.pTo = to.data();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_NORECURSION;
    if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted &&
        GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return true;
    }
  }
  // Byte-copy then delete source — last resort so Explorer never keeps untitled.md
  // while the UI already says 未命名.
  {
    std::ifstream in(src, std::ios::binary);
    if (in) {
      std::string data;
      in.seekg(0, std::ios::end);
      const std::streamoff len = in.tellg();
      in.seekg(0, std::ios::beg);
      if (len > 0) {
        data.resize(static_cast<size_t>(len));
        in.read(data.data(), len);
      }
      in.close();
      std::ofstream out(dest, std::ios::binary | std::ios::trunc);
      if (out) {
        if (!data.empty()) {
          out.write(data.data(), static_cast<std::streamsize>(data.size()));
        }
        out.close();
        if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
          SetFileAttributesW(src.c_str(), FILE_ATTRIBUTE_NORMAL);
          if (DeleteFileW(src.c_str())) {
            return true;
          }
          SHFILEOPSTRUCTW del{};
          std::vector<wchar_t> doomed(src.begin(), src.end());
          doomed.push_back(L'\0');
          doomed.push_back(L'\0');
          del.wFunc = FO_DELETE;
          del.pFrom = doomed.data();
          del.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT |
                       FOF_ALLOWUNDO | FOF_NORECURSION;
          if (SHFileOperationW(&del) == 0 && !del.fAnyOperationsAborted) {
            return true;
          }
          LogLine(L"[notes] renamed via copy but source linger %lu: %s",
                  GetLastError(), src.c_str());
          return true;
        }
      }
    }
  }
  LogLine(L"[notes] rename failed %lu: %s -> %s", GetLastError(), src.c_str(),
          dest.c_str());
  return false;
}

// Next free 未命名[_NN].ext beside `dir` (same folder as the English draft).
std::wstring NextLocalizedUntitledPath(const std::wstring& dir,
                                       const std::wstring& sfx) {
  std::wstring path = JoinPath(dir, std::wstring(L"未命名") + sfx);
  if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return path;
  }
  for (int n = 1; n < 1000; ++n) {
    wchar_t name[80];
    swprintf_s(name, L"未命名_%02d", n);
    path = JoinPath(dir, std::wstring(name) + sfx);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      return path;
    }
  }
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t fallback[96];
  swprintf_s(fallback, L"未命名_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  return JoinPath(dir, std::wstring(fallback) + sfx);
}

void NotifyShellFileRenamed(const std::wstring& old_path,
                            const std::wstring& new_path) {
  if (old_path.empty() || new_path.empty()) {
    return;
  }
  // Flush asynchronously so an already-open Explorer window drops the stale
  // "untitled.md" row without blocking the UI thread.
  SHChangeNotify(SHCNE_RENAMEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                 old_path.c_str(), new_path.c_str());
  const size_t slash = new_path.find_last_of(L"\\/");
  if (slash != std::wstring::npos && slash > 0) {
    const std::wstring dir = new_path.substr(0, slash);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                   dir.c_str(), nullptr);
  }
}

bool SelectPathInExplorer(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  PIDLIST_ABSOLUTE pidl = nullptr;
  if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) ||
      !pidl) {
    return false;
  }
  const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
  CoTaskMemFree(pidl);
  return SUCCEEDED(hr);
}
}  // namespace

bool NotesWindow::HealEnglishUntitledPath(std::wstring* path_inout) {
  if (!path_inout || path_inout->empty() ||
      !PathIsEnglishUntitledNote(*path_inout)) {
    return false;
  }
  const std::wstring old_path = *path_inout;
  const std::wstring old_key = NormalizePathKey(old_path);
  const size_t slash = old_path.find_last_of(L"\\/");
  const std::wstring base =
      slash == std::wstring::npos ? old_path : old_path.substr(slash + 1);
  const size_t dot = base.find_last_of(L'.');
  const std::wstring stem =
      (dot == std::wstring::npos || dot == 0) ? base : base.substr(0, dot);
  const std::wstring sfx =
      (dot == std::wstring::npos || dot == 0) ? L".md" : base.substr(dot);
  const std::wstring new_stem = LocalizeUntitledStem(stem);
  if (new_stem == stem) {
    return false;
  }
  // Same folder as the draft (Documents\DeskTidy\notes, exe\笔记, …) —
  // do not require NotesLibraryDir: empty notepad_folder still left legacy
  // untitled.md under 文档\DeskTidy\notes while UI already said 未命名.
  std::wstring dir =
      slash == std::wstring::npos ? NotesLibraryDir(true) : old_path.substr(0, slash);
  std::wstring dest = JoinPath(dir, new_stem + sfx);
  if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
    dest = NextLocalizedUntitledPath(dir, sfx);
  }
  if (_wcsicmp(NormalizePathKey(dest).c_str(), old_key.c_str()) == 0) {
    return false;
  }
  if (GetFileAttributesW(old_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    // Only retarget when dest already exists (prior rename). Never invent a
    // path and leave untitled.md orphaned on disk.
    if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
      *path_inout = dest;
      return true;
    }
    return false;
  }
  if (!TryRenameNoteFile(old_path, dest)) {
    return false;
  }
  InvalidateNotesFileCache();
  NotifyShellFileRenamed(old_path, dest);
  *path_inout = dest;
  return true;
}

void NotesWindow::RetargetNotePath(const std::wstring& old_path,
                                   const std::wstring& new_path) {
  if (old_path.empty() || new_path.empty()) {
    return;
  }
  const std::wstring old_key = NormalizePathKey(old_path);
  const std::wstring new_key = NormalizePathKey(new_path);
  if (_wcsicmp(old_key.c_str(), new_key.c_str()) == 0) {
    return;
  }
  for (auto& t : tab_paths_) {
    if (_wcsicmp(NormalizePathKey(t).c_str(), old_key.c_str()) == 0) {
      t = new_path;
    }
  }
  for (auto& p : pinned_paths_) {
    if (_wcsicmp(NormalizePathKey(p).c_str(), old_key.c_str()) == 0) {
      p = new_path;
    }
  }
  for (auto& p : locked_paths_) {
    if (_wcsicmp(NormalizePathKey(p).c_str(), old_key.c_str()) == 0) {
      p = new_path;
    }
  }
  for (auto& s : sidebar_paths_) {
    if (!s.empty() &&
        _wcsicmp(NormalizePathKey(s).c_str(), old_key.c_str()) == 0) {
      s = new_path;
    }
  }
  if (_wcsicmp(NormalizePathKey(path_).c_str(), old_key.c_str()) == 0) {
    path_ = new_path;
  }
  // Cloud link must follow the file, or a renamed synced note silently stops
  // syncing and the cloud copy becomes an orphan.
  if (NoteCloudRetargetLink(&cloud_sync_, old_path, new_path)) {
    SaveNoteCloudSync(cloud_sync_);
  }
  SchedulePersistTabs();
}

void NotesWindow::RevealPathInExplorer(std::wstring path) {
  if (path.empty()) {
    return;
  }
  HealUntitledDraftsOnDisk();
  HealRetargetPath(&path);
  // Flush title→filename before selecting so Explorer never stays on 未命名.md.
  if (!path_.empty() &&
      _wcsicmp(NormalizePathKey(path).c_str(),
               NormalizePathKey(path_).c_str()) == 0) {
    SyncCurrentTabTitleHint();
    SyncFilenameToTitle();
    path = path_;
  }
  // If still English on disk, force one more heal after flush.
  if (PathIsEnglishUntitledNote(path)) {
    Save(true);
    HealRetargetPath(&path);
  }
  // Stale sidebar/tab path may still say untitled while path_ is already 未命名.
  if (PathIsEnglishUntitledNote(path) && !path_.empty() &&
      !PathIsEnglishUntitledNote(path_)) {
    path = path_;
  }
  const DWORD attr = GetFileAttributesW(path.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) {
    OpenNotesFolder();
    return;
  }
  // Unicode-safe select; explorer.exe /select often fails on 未命名.md and
  // leaves a stale untitled.md Explorer window open.
  if (!SelectPathInExplorer(path)) {
    const std::wstring arg = L"/select,\"" + path + L"\"";
    ShellExecuteW(hwnd_, L"open", L"explorer.exe", arg.c_str(), nullptr,
                  SW_SHOWNORMAL);
  }
  if (status_) {
    SetWindowTextW(status_, L"已在资源管理器中选中");
  }
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
  InvalidateNotesFileCache();
  tab_paths_.push_back(path);
  EnsureTabStates();
  // Fresh draft: blank title seed, no inherited title_hint / caret.
  tab_states_.back() = TabState{};
  tab_states_.back().wrap = word_wrap_;
  // New .md draft → editable preview (SwitchToTab → EnsureMarkdownEditor).
  tab_states_.back().wysiwyg = true;
  RebuildTabControl();
  SwitchToTab(static_cast<int>(tab_paths_.size()) - 1, false);
  SyncCurrentTabTitleHint();
  if (sidebar_visible_) {
    RebuildSidebar();
  }
}

bool NotesWindow::IsLockedAccountsTab(int index) const {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size())) {
    return false;
  }
  return IsVaultAccountsNotePath(tab_paths_[static_cast<size_t>(index)]);
}

void NotesWindow::NotifyAccountsTabLocked() {
  if (status_) {
    SetWindowTextW(status_, L"账号库为固定页签，不可关闭、删除或解锁");
  }
}

void NotesWindow::NotifyTabDeleteLocked() {
  if (status_) {
    SetWindowTextW(status_, L"页签已锁定，请先解锁再删除");
  }
}

void NotesWindow::CloseTabOnly() {
  if (IsLockedAccountsTab(tab_index_)) {
    NotifyAccountsTabLocked();
    return;
  }
  // Silent autosave (Notepad++ / Fences-class: closing a tab does not discard).
  if (!path_.empty()) {
    Save(true);
  }
  EnsureTabStates();
  if (tab_paths_.size() <= 1) {
    // 关掉最后一个页签：新草稿替换，原文件仍在库里（Notepad++ new 1）。
    if (SeedBlankDraftTab()) {
      ActivateSeededBlankDraft(L"");  // keep old file row; only add 未命名
    }
    return;
  }
  tab_paths_.erase(tab_paths_.begin() + tab_index_);
  if (tab_index_ < static_cast<int>(tab_states_.size())) {
    tab_states_.erase(tab_states_.begin() + tab_index_);
  }
  if (tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
  }
  // File stays on disk — only reload editor + chips (no full tree rescan).
  SyncTabsAndSidebar(true, false);
}

void NotesWindow::CloseTabOnlyAt(int index) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  if (IsLockedAccountsTab(index)) {
    NotifyAccountsTabLocked();
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
  if (IsLockedAccountsTab(index)) {
    NotifyAccountsTabLocked();
    return;
  }
  EnsureTabStates();
  const bool deleting_current = (index == tab_index_);
  const std::wstring path = tab_paths_[static_cast<size_t>(index)];
  if (IsOnlineNotePath(path)) {
    DeleteOnlineNote(OnlineNoteIdFromPath(path), NoteDisplayName(path, false));
    return;
  }
  if (PathIsLocked(path)) {
    NotifyTabDeleteLocked();
    return;
  }
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring name =
      slash == std::wstring::npos ? path : path.substr(slash + 1);
  if (ask_confirm) {
    std::wstring msg = L"确定删除「" + name + L"」？\n将移入回收站。";
    // This note is bound to a cloud copy that survives the local delete (the
    // rail keeps showing it as an online row). Say so, or the user reasonably
    // expects a cloud delete as well.
    if (PathIsCloudSynced(path)) {
      msg += L"\n\n注意：该笔记已云同步，云端副本会保留（可在「在线笔记」中删除）。";
    }
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
  for (size_t i = 0; i < locked_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(locked_paths_[i]).c_str(),
                 NormalizePathKey(path).c_str()) == 0) {
      locked_paths_.erase(locked_paths_.begin() +
                          static_cast<ptrdiff_t>(i));
      break;
    }
  }
  // Deleting the local file must not orphan its cloud copy. The link is left
  // exactly as-is on purpose: the rail treats a missing bound file as a pure
  // online row (see linked_ids / path_exists in PushLibraryRailState), so the
  // note stays reachable and can still be「转本地」again. Clearing the path
  // here would instead hide it, because empty paths are filtered out.
  // 删文件即丢弃未保存改动，不再走保存确认。
  if (deleting_current) {
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
    // Last tab deleted with the file — seed blank draft; swap one tree row
    // without Load + HealUntitledDraftsOnDisk full walk.
    if (SeedBlankDraftTab()) {
      ActivateSeededBlankDraft(path);
    }
    return;
  }
  if (deleting_current) {
    if (tab_index_ < 0) {
      tab_index_ = 0;
    }
    SidebarRemovePaths({path});
    SyncTabsAndSidebar(true, false);
    return;
  }
    // Other tab deleted — keep the live editor; drop one tree row + chips.
  if (tab_index_ < 0) {
    tab_index_ = 0;
  }
  SidebarRemovePaths({path});
  tab_chips_dirty_ = true;
  RebuildTabControl();
  SchedulePersistTabs();
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
  // 全文统计缓存按文档失效：否则切到长度/哈希相同的另一篇（尤其是两个
  // 空草稿）后会一直显示上一篇的字数。
  status_full_text_dirty_ = true;
  status_stats_hash_ = 0;
  status_stats_len_ = 0;
  // 提权实例启动时临时持有 hosts 文件；其它标签的标题提示位不代表它们
  // 有未保存内容，不要因为这种残留状态把非 Markdown 文件误判为 dirty。
  if (IsHostsPath(path_) && !IsCurrentProcessElevated()) {
    for (auto& st : tab_states_) {
      st.title_hint.clear();
    }
    SchedulePersistTabs();
  }
  if (IsOnlineNotePath(path_)) {
    const int id = OnlineNoteIdFromPath(path_);
    std::wstring wide = L"# \n\n";
    std::wstring title;
    auto it = online_body_cache_.find(id);
    if (id > 0 && it != online_body_cache_.end()) {
      // Tab switch: use memory cache — do not re-fetch over the network.
      title = it->second.title;
      wide = it->second.body.empty()
                 ? (L"# " + (title.empty() ? std::wstring(L"") : title) + L"\n\n")
                 : it->second.body;
    } else if (id > 0 && VaultHttpLoggedIn()) {
      // Reopening retries the fetch, so drop the stale failure guard. Also
      // drop any stale "still loading" mark: if the previous worker's
      // PostMessageW failed (window mid-teardown), the id would otherwise stay
      // pending forever and this note could never be saved in this session.
      online_load_failed_ids_.erase(id);
      pending_online_load_ids_.erase(id);
      // Cache miss: fetch on a worker. VaultHttpGetNote retries with blocking
      // sleeps (up to ~45 s with a dead server); running it here froze the whole
      // window — no paint, no input — on the first open of an online note.
      // Render the placeholder now and fill the body in when WM_APP+89 lands.
      pending_online_load_ids_.insert(id);
      std::thread([hwnd = hwnd_, id]() {
        auto* n = new VaultHttpNote;
        const bool ok = VaultHttpGetNote(id, n);
        if (!PostMessageW(hwnd, WM_APP + 89, ok ? 1 : 0,
                          reinterpret_cast<LPARAM>(n))) {
          delete n;
        }
      }).detach();
    }
    if (!title.empty() && wide.rfind(L"# ", 0) != 0) {
      wide = L"# " + title + L"\n\n" + wide;
    }
    // Make referenced images resolvable in the preview/WYSIWYG surface: the
    // vault never uploaded image binaries, so gather them from local notes.
    const int online_id = OnlineNoteIdFromPath(path_);
    MaterializeOnlineNoteAssets(
        hwnd_, online_id, wide,
        JoinPath(JoinPath(NotesLibraryDir(true), L"云笔记"), L"assets"));
    text_enc_ = TextFileEncoding::Utf8;
    SetWindowTextW(edit_, wide.c_str());
    ApplyNativeTitleFromMarkdown(wide);
    loading_ = false;
    dirty_ = false;
    SyncCurrentTabTitleHint();
    UpdateStatusBar();
    RefreshGutter();
    return;
  }
  std::ifstream in(path_, std::ios::binary);
  if (!in) {
    std::ifstream legacy(JoinPath(NativeDataDir(), L"notes.txt"), std::ios::binary);
    if (!legacy) {
      // The tab points at a file that is gone (deleted/renamed in Explorer).
      // Leaving the previous tab's body in the editor would let the next
      // keystroke mark it dirty and Save() would write that body into this
      // path — silently copying one note over another. Clear it instead.
      SetWindowTextW(edit_, L"");
      ApplyNativeTitleFromMarkdown(L"");
      loading_ = false;
      dirty_ = false;
      SyncCurrentTabTitleHint();
      UpdateStatusBar();
      RefreshGutter();
      return;
    }
    std::ostringstream ss;
    ss << legacy.rdbuf();
    const std::wstring legacy_wide = WidenUtf8(ss.str());
    SetWindowTextW(edit_, legacy_wide.c_str());
    loading_ = false;
    ApplyNativeTitleFromMarkdown(legacy_wide);
    SyncCurrentTabTitleHint();
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
      SyncCurrentTabTitleHint();
      UpdateStatusBar();
      RefreshGutter();
      return;
    }
    text_enc_ = enc;
    if (wide.empty() && PathLooksMarkdown()) {
      wide = L"# \n\n";
    }
    SetWindowTextW(edit_, wide.c_str());
    ApplyNativeTitleFromMarkdown(wide);
  }
  loading_ = false;
  dirty_ = false;
  // 打开 hosts 不再弹提示：保存时的提权确认已说明后续行为，避免同一流程弹两次。
  SyncCurrentTabTitleHint();
  UpdateStatusBar();
  RefreshGutter();
}

bool NotesWindow::OfferElevatedSave(const std::wstring& path,
                                    const std::wstring& text) {
  if (in_elevated_save_) {
    return true;
  }
  static std::atomic<unsigned> s_request_id{0};
  const unsigned request_id = ++s_request_id;
  if (IsCurrentProcessElevated()) {
    MistAlert(hwnd_, L"保存",
              (L"当前已处于管理员权限，但仍无法写入文件：\n" + path +
               L"\n\n请检查文件是否被其它程序独占或安全软件拦截。")
                  .c_str(),
              MistMsgTone::Warn);
    return true;
  }
  in_elevated_save_ = true;
  // UAC 授权框本身就是明确确认，不再额外弹应用内确认框，避免保存时出现两次提示。
  // 用户在 UAC 里点“否”时，RelaunchDeskNoteElevated 会返回 false 并走取消分支。
  if (!WriteElevatedPending(path, text)) {
    in_elevated_save_ = false;
    MistAlert(hwnd_, L"笔记", L"无法准备提权保存的暂存内容。",
              MistMsgTone::Warn);
    return true;
  }
  // 保存本次会话，UAC 确认成功后再收敛标签；取消时原样恢复。
  const std::vector<std::wstring> saved_tab_paths = tab_paths_;
  const std::vector<TabState> saved_tab_states = tab_states_;
  const int saved_tab_index = tab_index_;
  const std::wstring saved_path = path_;
  PersistTabs();
  PersistViewSettings();
  // 写 pending 后启动提权助手；原普通权限窗口保持存活，由下面的监听线程
  // 等待结果文件出现后再自行恢复。不让提权进程启动新的 DeskNote，
  // 避免子进程继承管理员令牌并抢占单实例。
  if (!RelaunchDeskNoteElevated(path, request_id, /*reopen=*/false)) {
    ClearElevatedPending();
    ClearElevatedResult();
    tab_paths_ = saved_tab_paths;
    tab_states_ = saved_tab_states;
    tab_index_ = saved_tab_index;
    path_ = saved_path;
    RebuildTabControl();
    PersistTabs();
    in_elevated_save_ = false;
    MistAlert(hwnd_, L"保存",
              L"已取消提权保存（UAC 未确认）。\n"
              L"文件未被修改，可重试或手动以管理员身份运行 DeskNote。",
              MistMsgTone::Warn);
    return true;
  }
  // 提权实例只写盘；若保留其它标签，其自动保存可能把旧内容覆盖回刚写好的
  // 系统文件。这里把会话收敛到目标文件，其它标签仍留在磁盘上，用户可再次打开。
  tab_paths_.assign(1, NormalizePathKey(path));
  tab_index_ = 0;
  path_ = tab_paths_.front();
  EnsureTabStates();
  RebuildTabControl();
  PersistTabs();
  const HWND notify_hwnd = hwnd_;
  const std::wstring notify_target = NormalizePathKey(path);
  const int notify_tab = tab_index_;
  // 先记下当前插入点和首可见行；提权助手写完后只恢复视图，不做整文件重载，
  // 否则光标会跳回文档开头。
  LONG caret = 0;
  int first_visible_line = 0;
  if (edit_) {
    CHARRANGE cr{};
    SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
    caret = cr.cpMin;
    first_visible_line = static_cast<int>(
        SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0));
  }
  const bool typed_text_mode = !wysiwyg_;
  std::thread([notify_hwnd, notify_target, request_id, notify_tab, caret,
               first_visible_line, typed_text_mode]() {
    const ULONGLONG deadline = GetTickCount64() + 30000;
    bool ok = false;
    while (GetTickCount64() < deadline) {
      if (ReadElevatedResult(notify_target, request_id, &ok)) {
        break;
      }
      Sleep(100);
    }
    auto* payload = new ElevatedSaveDone{notify_target, request_id, ok,
                                         notify_tab, caret,
                                         first_visible_line, typed_text_mode};
    if (!notify_hwnd || !IsWindow(notify_hwnd) ||
        !PostMessageW(notify_hwnd, kMsgElevatedSaveDone, 0,
                      reinterpret_cast<LPARAM>(payload))) {
      delete payload;
    }
    // 只清理与本次请求完全匹配的结果文件：重试同一目标时不能误删新结果。
    bool current_ok = false;
    if (ReadElevatedResult(notify_target, request_id, &current_ok)) {
      ClearElevatedResult();
    }
  }).detach();
  dirty_ = false;
  return true;
}

void NotesWindow::UpdateModeStrip() {
  // Visibility is owned exclusively by LayoutClient (show_mode_chip).
}

void NotesWindow::ShowPreviewMode() {
  // In-window / right-rail preview removed — stay in the source editor.
  // Optional separate window: File · Markdown 预览窗口…
  preview_ = false;
  ShowEditMode();
}

void NotesWindow::ToggleMdViewMode() {
  EnsureMarkdownEditor(true);
}

void NotesWindow::ShowEditMode() {
  if (PathLooksMarkdown() && !toast_) {
    // Already editing this document: keep the live editor (and its undo
    // history / caret) instead of re-pushing the body.
    EnsureMarkdownEditor(false);
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

void NotesWindow::Save(bool authoritative_pull, bool push_cloud) {
  if (in_elevated_save_) {
    return;
  }
  // Arm before Pull/SetWindowText — EN_CHANGE + Toast posts during Save must
  // not keep the tab disc after a successful write (Typora / VS Code).
  ignore_dirty_until_ = GetTickCount64() + 600;
  if (wysiwyg_) {
    std::wstring from_toast;
    const bool cache_warm = !web_.CachedMarkdown().empty();
    const bool need_pull =
        web_.Ready() &&
        (authoritative_pull || dirty_ || !cache_warm);
    if (need_pull) {
      // Short pull: cache is kept warm by Toast `changed`. Long 1500ms waits
      // made Ctrl+S feel stuck before the tab could flip to ✓.
      // Pass the host cache as the expected body: if the WebView is still
      // rendering a different (previous) note, the pull is rejected instead
      // of cross-writing that note's body under this tab's cloud id.
      from_toast = web_.PullMarkdown(cache_warm ? 250 : 500,
                                     web_.CachedMarkdown());
    }
    if (from_toast.empty()) {
      from_toast = web_.CachedMarkdown();
    }
    if (from_toast.empty()) {
      SyncFromWysiwyg();
    } else if (edit_) {
      const std::wstring full = ComposeMarkdownWithNativeTitle(from_toast);
      web_.SetCachedMarkdown(full);
      SetWindowTextW(edit_, full.c_str());
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
  HealRetargetPath(&path_);
  const bool writing_hosts = IsHostsPath(path_) && !IsOnlineNotePath(path_);
  // hosts 不是 Markdown：RichEdit 中保存的是正文，不能再拼上标题栏文本。
  const std::wstring plain =
      writing_hosts && edit_
          ? [&] {
              const int len = GetWindowTextLengthW(edit_);
              std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
              GetWindowTextW(edit_, wide.data(), len + 1);
              wide.resize(static_cast<size_t>(len));
              return wide;
            }()
          : CurrentPlainText();
  if (IsOnlineNotePath(path_)) {
    const int id = OnlineNoteIdFromPath(path_);
    if (id <= 0) {
      MistAlert(hwnd_, L"保存", L"无效的在线笔记。", MistMsgTone::Warn);
      return;
    }
    // 正文还在后台拉取时：用户还没看到真实内容，不能把占位符 "# \n\n"
    // 当成编辑上传，也不能把占位符写进缓存挡住随后到达的真实正文。
    if (pending_online_load_ids_.count(id) != 0) {
      MistAlert(hwnd_, L"保存", L"云端正文仍在加载，请稍候再保存。",
                MistMsgTone::Info);
      return;
    }
    if (online_load_failed_ids_.count(id) != 0) {
      // Refuse: the editor only has the placeholder. Uploading now would
      // replace the real note with '# '. The guard lifts after a reload
      // succeeds (reopen the note once the network is back).
      MistAlert(hwnd_, L"保存",
                L"云端正文未能加载，已阻止保存以免覆盖云端内容。\n"
                L"请检查网络或登录状态后重新打开该笔记。",
                MistMsgTone::Danger);
      return;
    }
    std::wstring title = NativeTitleText();
    if (title.empty()) {
      title = TabLabel(path_);
      if (title.rfind(L"☁ ", 0) == 0) {
        title = title.substr(2);
      }
    }
    if (title.empty()) {
      title = L"未命名";
    }
    RememberOnlineBody(id, title, plain);
    // Explicit Save: enqueue cloud push — never block the UI thread on WinHTTP.
    if (push_cloud) {
      // Never open a modal login from Save: Save also runs from Hide()/close,
      // where a modal would block teardown behind a network dialog. The push
      // worker keeps unauthenticated batches queued and retries, so the edit
      // still reaches the cloud once the session is restored.
      if (VaultHttpLoggedIn()) {
        for (auto& row : library_online_cache_) {
          if (row.id == id) {
            row.title = title;
            break;
          }
        }
        QueueCloudNotePush(id, title, plain);
      } else if (status_) {
        SetWindowTextW(status_, L"已保存到本地缓存；未登录，稍后自动上传");
      }
    }
    dirty_ = false;
    ignore_dirty_until_ = GetTickCount64() + 600;
    if (wysiwyg_) {
      web_.AckClean();
    }
    EnsureTabStates();
    if (tab_index_ >= 0 &&
        tab_index_ < static_cast<int>(tab_states_.size())) {
      tab_states_[static_cast<size_t>(tab_index_)].title_hint = title;
    }
    RefreshTitle();
    RefreshCurrentTabLabel();
    UpdateStatusBar();
    return;
  }
  // Plain text for md/txt/code/hosts；.rtf 仍走 RTF（对标 Notepad++ 文本保存）。
  if (PathLooksTextish()) {
    // Never clobber a real note with the empty WW boot seed ("# \n\n") or "".
    // (A soft-save race used to wipe 12131.md / similar files this way.)
    if (LooksLikeEmptyMarkdownSeed(plain)) {
      WIN32_FILE_ATTRIBUTE_DATA fad{};
      if (GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &fad)) {
        const ULONGLONG sz =
            (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        if (sz > static_cast<ULONGLONG>(plain.size() * 3 + 4)) {
          return;
        }
      }
      if (!authoritative_pull) {
        return;
      }
      if (dirty_ && wysiwyg_ && !web_.ContentReady()) {
        return;
      }
    }
    std::string bytes;
    // hosts 等非笔记文本文件按原字节回写：RichEdit 会把 CRLF 规范成 LF，
    // 把解码后的文本再编码一次会悄悄改变系统文件的换行和 BOM。
    // 先比较规范化内容，只有确有修改时才写入。
    bool content_changed = true;
    std::string original_bytes;
    if (writing_hosts && ReadBinaryFile(path_, &original_bytes)) {
      // hosts 固定无 BOM UTF-8 + CRLF（与提权写回路径一致，BOM 会破坏解析）。
      text_enc_ = TextFileEncoding::Utf8;
      const std::wstring desired = HostsTextForWrite(plain);
      std::wstring original_text;
      if (DecodeTextFileBytes(original_bytes, &original_text, nullptr) &&
          NormalizeNewlinesForCompare(original_text) ==
              NormalizeNewlinesForCompare(desired)) {
        content_changed = false;  // 仅换行差异：RichEdit 规范化，不是用户修改。
      } else {
        // 确有修改时按无 BOM UTF-8 写回。
        if (!EncodeTextFileBytes(desired, TextFileEncoding::Utf8, &bytes)) {
          MistAlert(hwnd_, L"保存失败", L"无法按 UTF-8 编码写回 hosts 文件。",
                    MistMsgTone::Warn);
          return;
        }
      }
    }
    if (content_changed && bytes.empty() &&
        !EncodeTextFileBytes(plain, text_enc_, &bytes)) {
      MistAlert(hwnd_, L"保存失败",
                L"无法按原编码写回文件（字符无法映射到 ANSI/GBK）。",
                MistMsgTone::Warn);
      return;
    }
    if (content_changed) {
      if (!WriteFileBytes(path_, bytes.data(), bytes.size())) {
        if (LastWriteWasAccessDenied() && OfferElevatedSave(path_, plain)) {
          return;
        }
        MistAlert(hwnd_, L"笔记", L"保存失败。", MistMsgTone::Warn);
        return;
      }
    }
    // Synced attribute: local write done; cloud push is async (queue only).
    if (push_cloud && PathIsCloudSynced(path_) && !IsOnlineNotePath(path_) &&
        VaultHttpLoggedIn()) {
      if (const auto* L = FindNoteCloudLink(cloud_sync_, path_)) {
        std::wstring title = NativeTitleText();
        if (title.empty()) {
          title = NoteDisplayName(path_, false);
          if (title.size() > 3 &&
              _wcsicmp(title.c_str() + title.size() - 3, L".md") == 0) {
            title.resize(title.size() - 3);
          }
        }
        QueueCloudNotePush(L->note_id, title, plain);
      }
    }
    dirty_ = false;
    ignore_dirty_until_ = GetTickCount64() + 600;
    if (wysiwyg_) {
      web_.AckClean();
    }
    // Flip disc→✓ before title→filename rename (shell notify can hitch ~1s).
    RefreshTitle();
    RefreshCurrentTabLabel();
    UpdateStatusBar();
    if (tabs_) {
      UpdateWindow(tabs_);
    }
    SyncCurrentTabTitleHint();
    ScheduleSyncFilenameToTitle();
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
    // NarrowUtf8 can only come back empty for a non-empty body if the UTF-16
    // conversion failed outright (lone surrogates). Writing the zero-length
    // buffer would truncate the note to nothing — refuse and keep the file.
    if (payload.empty() && !plain.empty()) {
      MistAlert(hwnd_, L"笔记",
                L"内容包含无法编码的字符，已取消保存（原文件未改动）。",
                MistMsgTone::Danger);
      return;
    }
  }
  if (!WriteFileBytes(path_, payload.data(), payload.size())) {
    if (LastWriteWasAccessDenied() && OfferElevatedSave(path_, plain)) {
      return;
    }
    MistAlert(hwnd_, L"笔记", L"保存失败。", MistMsgTone::Warn);
    return;
  }
  dirty_ = false;
  ignore_dirty_until_ = GetTickCount64() + 600;
  if (wysiwyg_) {
    web_.AckClean();
  }
  RefreshTitle();
  RefreshCurrentTabLabel();
  UpdateStatusBar();
  if (tabs_) {
    UpdateWindow(tabs_);
  }
  SyncCurrentTabTitleHint();
  ScheduleSyncFilenameToTitle();
}

void NotesWindow::MarkDirty() {
  if (loading_) {
    return;
  }
  if (ignore_dirty_until_ != 0 && GetTickCount64() < ignore_dirty_until_) {
    return;
  }
  // Already dirty: skip title/status churn on every Toast change post, but
  // still poke the tab bar so the save glyph cannot stick on「✓».
  if (dirty_) {
    if (tabs_) {
      InvalidateRect(tabs_, nullptr, FALSE);
    }
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
  bool stats_done = false;
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
      // Full-document stats: cache separately from selection stats and re-read
      // the whole text only when EN_CHANGE marked it dirty. Caret moves alone
      // (the common 800ms no-op tick) reuse cached counts without GetWindowTextW.
      if (status_full_text_dirty_) {
        CountTextStats(CurrentPlainText(), &status_full_chars_,
                       &status_full_words_);
        status_full_text_dirty_ = false;
      }
      chars = status_full_chars_;
      words = status_full_words_;
      stats_done = true;
    }
  }
  if (!stats_done && (!stats_src.empty() || wysiwyg_ || edit_)) {
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
  // Keep the requested tab + locked tabs (账号库 / user lock).
  std::vector<std::wstring> next_paths;
  std::vector<TabState> next_states;
  next_paths.reserve(tab_paths_.size());
  next_states.reserve(tab_paths_.size());
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (static_cast<int>(i) != keep_index && !PathIsLocked(tab_paths_[i])) {
      continue;
    }
    next_paths.push_back(tab_paths_[i]);
    if (i < tab_states_.size()) {
      next_states.push_back(tab_states_[i]);
    } else {
      next_states.push_back({});
    }
  }
  if (next_paths.empty()) {
    return;
  }
  tab_paths_ = std::move(next_paths);
  tab_states_ = std::move(next_states);
  tab_index_ = 0;
  const std::wstring keep_key = NormalizePathKey(keep);
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(), keep_key.c_str()) ==
        0) {
      tab_index_ = static_cast<int>(i);
      break;
    }
  }
  path_ = tab_paths_[static_cast<size_t>(tab_index_)];
  RebuildTabControl();
  SwitchToTab(tab_index_, false);
  PersistTabs();
}

void NotesWindow::CloseTabsToTheRight(int index) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size()) - 1) {
    return;
  }
  Save();
  EnsureTabStates();
  std::vector<std::wstring> next_paths;
  std::vector<TabState> next_states;
  next_paths.reserve(tab_paths_.size());
  next_states.reserve(tab_paths_.size());
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (static_cast<int>(i) > index && !PathIsLocked(tab_paths_[i])) {
      continue;
    }
    next_paths.push_back(tab_paths_[i]);
    if (i < tab_states_.size()) {
      next_states.push_back(tab_states_[i]);
    } else {
      next_states.push_back({});
    }
  }
  tab_paths_ = std::move(next_paths);
  tab_states_ = std::move(next_states);
  if (tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
  }
  if (tab_index_ < 0) {
    tab_index_ = 0;
  }
  if (!tab_paths_.empty()) {
    path_ = tab_paths_[static_cast<size_t>(tab_index_)];
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

void NotesWindow::InsertSoftLineBreak() {
  // WYSIWYG: Toast maps Shift-Enter→Enter; host steals key and runs JS.
  if (wysiwyg_ && web_.Available()) {
    web_.EvalScript(
        L"try{window.__desknoteSoftBreak&&window.__desknoteSoftBreak()}"
        L"catch(e){}");
    MarkDirty();
    return;
  }
  if (!edit_) {
    return;
  }
  CHARRANGE cr{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&cr));
  // Notepad-class: leave text after the caret on this line. Open a blank line
  // below and park the caret at the same *visual* X (YaHei / CJK — char
  // count spaces do not line up).
  const LONG line =
      static_cast<LONG>(SendMessageW(edit_, EM_EXLINEFROMCHAR, 0, cr.cpMin));
  const LONG line_start =
      static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, line, 0));
  const LRESULT caret_lr =
      SendMessageW(edit_, EM_POSFROMCHAR, static_cast<WPARAM>(cr.cpMin), 0);
  const int target_x = static_cast<short>(LOWORD(caret_lr));
  LONG line_len = static_cast<LONG>(
      SendMessageW(edit_, EM_LINELENGTH, static_cast<WPARAM>(line_start), 0));
  if (line_len < 0) {
    line_len = 0;
  }
  LONG insert_at = line_start + line_len;
  const LONG next_line =
      static_cast<LONG>(SendMessageW(edit_, EM_LINEINDEX, line + 1, 0));
  if (next_line > line_start && insert_at > next_line) {
    insert_at = next_line;
  }
  if (next_line < 0) {
    GETTEXTLENGTHEX gtl{};
    gtl.flags = GTL_DEFAULT | GTL_PRECISE | GTL_NUMCHARS;
    gtl.codepage = 1200;
    const LONG total = static_cast<LONG>(SendMessageW(
        edit_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&gtl), 0));
    if (total > insert_at) {
      insert_at = total;
    }
  }
  CHARRANGE at{insert_at, insert_at};
  SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&at));
  SendMessageW(edit_, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"\r\n"));
  CHARRANGE now{};
  SendMessageW(edit_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&now));
  const LRESULT line_lr =
      SendMessageW(edit_, EM_POSFROMCHAR, static_cast<WPARAM>(now.cpMin), 0);
  const int start_x = static_cast<short>(LOWORD(line_lr));
  int space_cx = 8;
  if (HDC hdc = GetDC(edit_)) {
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit_, WM_GETFONT, 0, 0));
    HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
    SIZE sz{};
    if (GetTextExtentPoint32W(hdc, L" ", 1, &sz) && sz.cx > 0) {
      space_cx = sz.cx;
    }
    if (old) {
      SelectObject(hdc, old);
    }
    ReleaseDC(edit_, hdc);
  }
  int n = 0;
  if (space_cx > 0 && target_x > start_x) {
    n = (target_x - start_x + space_cx / 2) / space_cx;
  }
  if (n > 200) {
    n = 200;
  }
  if (n > 0) {
    const std::wstring pad(static_cast<size_t>(n), L' ');
    SendMessageW(edit_, EM_REPLACESEL, TRUE,
                 reinterpret_cast<LPARAM>(pad.c_str()));
  }
  SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
  SetFocus(edit_);
  MarkDirty();
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

  HMENU exp = CreatePopupMenu();
  AppendMenuW(exp, MF_STRING, kTabExportWord, L"Word 文档 (*.docx)…");
  AppendMenuW(exp, MF_STRING, kTabExportPdf, L"PDF 文档 (*.pdf)…");
  AppendMenuW(exp, MF_STRING, kTabExportHtml, L"HTML 网页 (*.html)…");
  AppendMenuW(exp, MF_STRING, kTabExportMd, L"Markdown (含图片) (*.md)…");
  AppendMenuW(exp, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(exp, MF_STRING, kTabExport, L"更多导出选项…");
  AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(exp), L"导出");

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
    case kTabExportWord:
      ExportWord(path_);
      break;
    case kTabExportPdf:
      ExportPdf(path_);
      break;
    case kTabExportHtml:
      ExportHtml(path_);
      break;
    case kTabExportMd:
      ExportMarkdown(path_);
      break;
    case kTabExport:
      ExportNoteWithAssets(path_);
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

void NotesWindow::HandleWysiwygContextMenu(POINT screen_pt, bool has_sel) {
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING, kEditCtxUndo, L"撤销\tCtrl+Z");
  AppendMenuW(m, MF_STRING, kEditCtxRedo, L"重做\tCtrl+Y");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), kEditCtxCut,
              L"剪切\tCtrl+X");
  AppendMenuW(m, MF_STRING | (has_sel ? MF_ENABLED : MF_GRAYED), kEditCtxCopy,
              L"复制\tCtrl+C");
  AppendMenuW(m, MF_STRING, kEditCtxPaste, L"粘贴\tCtrl+V");
  AppendMenuW(m, MF_STRING, kEditCtxSelectAll, L"全选\tCtrl+A");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

  HMENU exp = CreatePopupMenu();
  AppendMenuW(exp, MF_STRING, kTabExportWord, L"Word 文档 (*.docx)…");
  AppendMenuW(exp, MF_STRING, kTabExportPdf, L"PDF 文档 (*.pdf)…");
  AppendMenuW(exp, MF_STRING, kTabExportHtml, L"HTML 网页 (*.html)…");
  AppendMenuW(exp, MF_STRING, kTabExportMd, L"Markdown (含图片) (*.md)…");
  AppendMenuW(exp, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(exp, MF_STRING, kTabExport, L"更多导出选项…");
  AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(exp), L"导出");

  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, kEditCtxFind, L"查找与替换…\tCtrl+F");

  const int cmd =
      TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, screen_pt.x, screen_pt.y,
                     0, hwnd_, nullptr);
  DestroyMenu(m);

  switch (cmd) {
    case kEditCtxUndo:
      web_.ExecCommand(L"undo");
      break;
    case kEditCtxRedo:
      web_.ExecCommand(L"redo");
      break;
    case kEditCtxCut:
      web_.ExecCommand(L"cut");
      break;
    case kEditCtxCopy:
      web_.ExecCommand(L"copy");
      break;
    case kEditCtxPaste:
      web_.ExecCommand(L"paste");
      break;
    case kEditCtxSelectAll:
      web_.ExecCommand(L"selectAll");
      break;
    case kTabExportWord:
      ExportWord(path_);
      break;
    case kTabExportPdf:
      ExportPdf(path_);
      break;
    case kTabExportHtml:
      ExportHtml(path_);
      break;
    case kTabExportMd:
      ExportMarkdown(path_);
      break;
    case kTabExport:
      ExportNoteWithAssets(path_);
      break;
    case kEditCtxFind:
      ShowFind(false);
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
  const bool vault = IsVaultAccountsNotePath(path);
  const bool locked = PathIsLocked(path);
  HMENU m = CreatePopupMenu();
  if (!vault) {
    AppendMenuW(m, MF_STRING, kTabClose, L"关闭");
  }
  AppendMenuW(m, MF_STRING, kTabCloseOthers, L"关闭其他页签");
  AppendMenuW(m, MF_STRING, kTabCloseRight, L"关闭右侧页签");
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  if (!vault) {
    AppendMenuW(m, MF_STRING, kTabRename, L"重命名");
    HMENU exp = CreatePopupMenu();
    AppendMenuW(exp, MF_STRING, kTabExportWord, L"Word 文档 (*.docx)…");
    AppendMenuW(exp, MF_STRING, kTabExportPdf, L"PDF 文档 (*.pdf)…");
    AppendMenuW(exp, MF_STRING, kTabExportHtml, L"HTML 网页 (*.html)…");
    AppendMenuW(exp, MF_STRING, kTabExportMd, L"Markdown (含图片) (*.md)…");
    AppendMenuW(exp, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(exp, MF_STRING, kTabExport, L"更多导出选项…");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(exp), L"导出");

    AppendMenuW(m, MF_STRING, kTabPin,
                PathIsPinned(path) ? L"取消置顶" : L"置顶");
    AppendMenuW(m, MF_STRING, kTabLock, locked ? L"解锁" : L"锁定");
  }
  AppendMenuW(m, MF_STRING, kTabReveal, L"打开文件所在目录");
  AppendMenuW(m, MF_STRING, kTabCopyPath, L"复制路径");
  // Locked (含账号库): 不显示「删除」.
  if (!locked) {
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kTabDelete, L"删除");
  }
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
    TogglePinPath(path);  // RebuildTabControl inside TogglePinPath
  } else if (cmd == kTabLock) {
    ToggleLockPath(path);
  } else if (cmd == kTabReveal) {
    RevealPathInExplorer(path);
  } else if (cmd == kTabCopyPath) {
    std::wstring copy = path;
    HealRetargetPath(&copy);
    CopyPathToClipboard(copy);
  } else if (cmd == kTabRename) {
    RenameTabAt(index);
  } else if (cmd == kTabExportWord) {
    ExportWord(path);
  } else if (cmd == kTabExportPdf) {
    ExportPdf(path);
  } else if (cmd == kTabExportHtml) {
    ExportHtml(path);
  } else if (cmd == kTabExportMd) {
    ExportMarkdown(path);
  } else if (cmd == kTabExport) {
    ExportNoteWithAssets(path);
  } else if (cmd == kTabDelete) {
    DeleteAndCloseTabAt(index, true);
  }
}

void NotesWindow::RenameTabAt(int index) {
  if (index < 0 || index >= static_cast<int>(tab_paths_.size())) {
    return;
  }
  if (IsLockedAccountsTab(index)) {
    NotifyAccountsTabLocked();
    return;
  }
  const std::wstring path = tab_paths_[static_cast<size_t>(index)];
  if (path.empty()) {
    return;
  }
  if (IsOnlineNotePath(path)) {
    RenameOnlineNote(OnlineNoteIdFromPath(path), NoteDisplayName(path, false));
    return;
  }
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
  // Toast body + native title compose the on-disk markdown in WYSIWYG.
  if (wysiwyg_ && web_.Ready()) {
    std::wstring composed =
        ComposeMarkdownWithNativeTitle(web_.CachedMarkdown());
    // Prefer RichEdit when Toast cache is still the empty boot seed.
    if (LooksLikeEmptyMarkdownSeed(composed) && edit_) {
      const int len = GetWindowTextLengthW(edit_);
      if (len > 0) {
        std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(edit_, wide.data(), len + 1);
        wide.resize(static_cast<size_t>(len));
        if (!LooksLikeEmptyMarkdownSeed(wide)) {
          return ComposeMarkdownWithNativeTitle(wide);
        }
      }
    }
    return composed;
  }
  if (!edit_) {
    return {};
  }
  const int len = GetWindowTextLengthW(edit_);
  std::wstring wide(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(edit_, wide.data(), len + 1);
  wide.resize(static_cast<size_t>(len));
  if (wysiwyg_) {
    return ComposeMarkdownWithNativeTitle(wide);
  }
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

bool NotesWindow::ExportNoteWithAssets(const std::wstring& note_path,
                                      const std::wstring& default_ext) {
  if (note_path.empty()) {
    return false;
  }
  std::wstring title = NoteDisplayName(note_path, false);
  if (title.empty()) {
    title = L"未命名笔记";
  }
  std::wstring base_dir = DirOfPath(note_path);
  if (base_dir.empty() || base_dir == L".") {
    base_dir = NoteBaseDir();
  }
  if (base_dir.empty()) {
    base_dir = NotesLibraryDir(true);
  }

  std::wstring md_text;
  const bool is_current = (!path_.empty() && _wcsicmp(NormalizePathKey(path_).c_str(),
                                                      NormalizePathKey(note_path).c_str()) == 0);
  if (is_current) {
    if (wysiwyg_ && web_.Ready()) {
      SyncFromWysiwyg();
    }
    md_text = CurrentPlainText();
  } else {
    int tidx = -1;
    for (size_t i = 0; i < tab_paths_.size(); ++i) {
      if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(),
                   NormalizePathKey(note_path).c_str()) == 0) {
        tidx = static_cast<int>(i);
        break;
      }
    }
    if (IsOnlineNotePath(note_path)) {
      const int id = OnlineNoteIdFromPath(note_path);
      auto it = online_body_cache_.find(id);
      if (it != online_body_cache_.end()) {
        md_text = it->second.body;
      } else {
        // Do not block the UI thread on a network fetch here: VaultHttpGetNote
        // retries with blocking sleeps (worst case tens of seconds) and export
        // runs synchronously on the UI thread. If the body is not cached yet,
        // ask the user to open the note first (opening fills the cache in the
        // background). Exporting an empty body would silently produce a blank
        // document.
        MistAlert(hwnd_, L"导出", L"云端笔记正文尚未加载。请先打开该笔记，稍候再导出。",
                  MistMsgTone::Info);
        return false;
      }
    } else if (GetFileAttributesW(note_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      std::string bytes;
      if (ReadBinaryFile(note_path, &bytes)) {
        TextFileEncoding enc = TextFileEncoding::Utf8;
        DecodeTextFileBytes(bytes, &md_text, &enc);
      }
    }

  }

  std::wstring safe_name = title;
  for (auto& ch : safe_name) {
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
        ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
      ch = L'_';
    }
  }
  if (safe_name.size() >= 3 && _wcsicmp(safe_name.c_str() + safe_name.size() - 3, L".md") == 0) {
    safe_name.resize(safe_name.size() - 3);
  } else if (safe_name.size() >= 9 && _wcsicmp(safe_name.c_str() + safe_name.size() - 9, L".markdown") == 0) {
    safe_name.resize(safe_name.size() - 9);
  } else if (safe_name.size() >= 4 && _wcsicmp(safe_name.c_str() + safe_name.size() - 4, L".txt") == 0) {
    safe_name.resize(safe_name.size() - 4);
  }
  if (safe_name.empty()) {
    safe_name = L"未命名笔记";
  }

  std::wstring def_ext = default_ext.empty() ? L"docx" : default_ext;
  if (!def_ext.empty() && def_ext[0] == L'.') {
    def_ext = def_ext.substr(1);
  }

  DWORD filter_index = 1;
  if (_wcsicmp(def_ext.c_str(), L"pdf") == 0) {
    filter_index = 2;
  } else if (_wcsicmp(def_ext.c_str(), L"md") == 0) {
    filter_index = 3;
  } else if (_wcsicmp(def_ext.c_str(), L"html") == 0 || _wcsicmp(def_ext.c_str(), L"htm") == 0) {
    filter_index = 4;
  } else if (_wcsicmp(def_ext.c_str(), L"txt") == 0) {
    filter_index = 5;
  }

  wchar_t file_buf[MAX_PATH * 2] = {};
  wcsncpy_s(file_buf, (safe_name + L"." + def_ext).c_str(), _TRUNCATE);

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter =
      L"Word 文档 (*.docx)\0*.docx\0"
      L"PDF 文档 (*.pdf)\0*.pdf\0"
      L"Markdown 文档 (包含图片资源) (*.md)\0*.md\0"
      L"单文件网页 (含内嵌图片) (*.html)\0*.html\0"
      L"纯文本 (*.txt)\0*.txt\0"
      L"所有文件 (*.*)\0*.*\0";
  ofn.lpstrFile = file_buf;
  ofn.nMaxFile = static_cast<DWORD>(sizeof(file_buf) / sizeof(file_buf[0]));
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
  ofn.nFilterIndex = filter_index;
  ofn.lpstrDefExt = def_ext.c_str();
  if (!GetSaveFileNameW(&ofn)) {
    return false;
  }

  const std::wstring dst_file = file_buf;
  const std::wstring dst_dir = DirOfPath(dst_file);

  struct ImgEntry {
    std::wstring original_src;
    std::wstring disk_path;
    std::wstring leaf_name;
    bool exists = false;
  };
  std::vector<ImgEntry> images;

  auto probe_disk_path = [&](const std::wstring& src) -> std::wstring {
    if (src.empty() || src.rfind(L"http://", 0) == 0 || src.rfind(L"https://", 0) == 0 ||
        src.rfind(L"data:", 0) == 0 || src[0] == L'#') {
      return L"";
    }
    std::wstring s = src;
    if (s.rfind(L"file:///", 0) == 0) {
      s = s.substr(8);
      for (auto& c : s) if (c == L'/') c = L'\\';
    }
    std::wstring norm = s;
    for (auto& c : norm) if (c == L'/') c = L'\\';

    if (norm.size() >= 2 && norm[1] == L':') {
      if (GetFileAttributesW(norm.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return norm;
      }
    }
    std::wstring p1 = JoinPath(base_dir, norm);
    if (GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return p1;
    }
    size_t sl = norm.find_last_of(L"\\/");
    std::wstring leaf = (sl == std::wstring::npos) ? norm : norm.substr(sl + 1);
    std::wstring p2 = JoinPath(JoinPath(base_dir, L"assets"), leaf);
    if (GetFileAttributesW(p2.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return p2;
    }
    std::wstring p3 = JoinPath(JoinPath(NotesLibraryDir(true), L"assets"), leaf);
    if (GetFileAttributesW(p3.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return p3;
    }
    return L"";
  };

  size_t pos = 0;
  while ((pos = md_text.find(L"![", pos)) != std::wstring::npos) {
    size_t alt_end = md_text.find(L"]", pos + 2);
    if (alt_end != std::wstring::npos && alt_end + 1 < md_text.size() && md_text[alt_end + 1] == L'(') {
      size_t src_end = md_text.find(L")", alt_end + 2);
      if (src_end != std::wstring::npos) {
        std::wstring raw_src = md_text.substr(alt_end + 2, src_end - (alt_end + 2));
        size_t sp = raw_src.find(L' ');
        if (sp != std::wstring::npos) {
          raw_src = raw_src.substr(0, sp);
        }
        while (!raw_src.empty() && (raw_src.front() == L' ' || raw_src.front() == L'<' || raw_src.front() == L'"')) {
          raw_src.erase(raw_src.begin());
        }
        while (!raw_src.empty() && (raw_src.back() == L' ' || raw_src.back() == L'>' || raw_src.back() == L'"')) {
          raw_src.pop_back();
        }
        if (!raw_src.empty()) {
          bool already = false;
          for (const auto& im : images) {
            if (im.original_src == raw_src) { already = true; break; }
          }
          if (!already) {
            ImgEntry e;
            e.original_src = raw_src;
            e.disk_path = probe_disk_path(raw_src);
            if (!e.disk_path.empty()) {
              e.exists = true;
              size_t sl = e.disk_path.find_last_of(L"\\/");
              e.leaf_name = (sl == std::wstring::npos) ? e.disk_path : e.disk_path.substr(sl + 1);
            }
            images.push_back(e);
          }
        }
        pos = src_end + 1;
        continue;
      }
    }
    pos += 2;
  }

  pos = 0;
  while ((pos = md_text.find(L"<img", pos)) != std::wstring::npos) {
    size_t tag_end = md_text.find(L">", pos + 4);
    if (tag_end == std::wstring::npos) break;
    size_t src_pos = md_text.find(L"src=", pos);
    if (src_pos != std::wstring::npos && src_pos < tag_end) {
      wchar_t q = md_text[src_pos + 4];
      if (q == L'"' || q == L'\'') {
        size_t val_end = md_text.find(q, src_pos + 5);
        if (val_end != std::wstring::npos && val_end < tag_end) {
          std::wstring raw_src = md_text.substr(src_pos + 5, val_end - (src_pos + 5));
          if (!raw_src.empty()) {
            bool already = false;
            for (const auto& im : images) {
              if (im.original_src == raw_src) { already = true; break; }
            }
            if (!already) {
              ImgEntry e;
              e.original_src = raw_src;
              e.disk_path = probe_disk_path(raw_src);
              if (!e.disk_path.empty()) {
                e.exists = true;
                size_t sl = e.disk_path.find_last_of(L"\\/");
                e.leaf_name = (sl == std::wstring::npos) ? e.disk_path : e.disk_path.substr(sl + 1);
              }
              images.push_back(e);
            }
          }
        }
      }
    }
    pos = tag_end + 1;
  }

  const bool is_docx = (dst_file.size() >= 5 && _wcsicmp(dst_file.c_str() + dst_file.size() - 5, L".docx") == 0) ||
                       (dst_file.size() >= 4 && _wcsicmp(dst_file.c_str() + dst_file.size() - 4, L".doc") == 0);
  const bool is_pdf = (dst_file.size() >= 4 && _wcsicmp(dst_file.c_str() + dst_file.size() - 4, L".pdf") == 0);
  const bool is_html = (dst_file.size() >= 5 && _wcsicmp(dst_file.c_str() + dst_file.size() - 5, L".html") == 0) ||
                       (dst_file.size() >= 4 && _wcsicmp(dst_file.c_str() + dst_file.size() - 4, L".htm") == 0);

  int images_copied = 0;

  if (is_docx) {
    std::wstring err;
    if (!ExportMarkdownToDocx(md_text, safe_name, base_dir, dst_file, &err)) {
      MistAlert(hwnd_, L"导出失败", (L"导出 Word 文档失败：" + (err.empty() ? L"未知错误" : err)).c_str(), MistMsgTone::Danger);
      return false;
    }
    std::wstring tip = L"已成功导出「" + safe_name + L"」为 Word 文档";
    if (!images.empty()) {
      tip += L"\n已完整内嵌 " + std::to_wstring(images.size()) + L" 张图片资源";
    }
    tip += L"\n\n保存位置：\n" + dst_file;
    MistAlert(hwnd_, L"导出成功", tip.c_str(), MistMsgTone::Info);
    ShellExecuteW(nullptr, L"open", dst_file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
  } else if (is_pdf) {
    if (!preview_web_.Available()) {
      preview_web_.SetKind(NotesWebView::Kind::kViewer);
      preview_web_.SetTheme(L"light");
      if (!preview_web_.Create(hwnd_)) {
        MistAlert(hwnd_, L"导出失败", L"无法启动 PDF 导出（WebView2 不可用）。", MistMsgTone::Warn);
        return false;
      }
      RegisterDropOnSubtree(preview_web_.Host());
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
    preview_web_.SetPreview(md_text, base_dir);
    // 等预览真正渲染完成（mermaid 图表与图片都是异步的），超时再退回固定延时。
    if (!preview_web_.WaitRendered(5000)) {
      const DWORD wait_start = GetTickCount();
      MSG msg{};
      while (GetTickCount() - wait_start < 450) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        Sleep(15);
      }
    }
    std::wstring err;
    if (!preview_web_.PrintToPdf(dst_file, &err)) {
      MistAlert(hwnd_, L"导出失败", (L"PDF 导出失败：" + (err.empty() ? L"未知错误" : err)).c_str(), MistMsgTone::Danger);
      return false;
    }
    std::wstring tip = L"已成功导出「" + title + L"」为 PDF 文档\n\n保存位置：\n" + dst_file;
    MistAlert(hwnd_, L"导出成功", tip.c_str(), MistMsgTone::Info);
    ShellExecuteW(nullptr, L"open", dst_file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
  } else if (is_html) {
    std::wstring html_content = MarkdownToHtmlDocument(md_text, title, base_dir);
    for (const auto& im : images) {
      if (!im.exists || im.disk_path.empty()) {
        continue;
      }
      std::string bytes;
      if (ReadBinaryFile(im.disk_path, &bytes) && !bytes.empty()) {
        std::wstring b64 = EncodeBase64(bytes);
        std::wstring mime = GetImageMimeType(im.leaf_name);
        std::wstring data_uri = L"data:" + mime + L";base64," + b64;

        std::string raw_u8 = NarrowUtf8(im.original_src);
        std::string data_u8 = NarrowUtf8(data_uri);
        std::string html_u8 = NarrowUtf8(html_content);
        std::string file_uri = ResolveImageSrc(raw_u8, base_dir);

        ReplaceAllA(html_u8, file_uri, data_u8);
        ReplaceAllA(html_u8, raw_u8, data_u8);

        html_content = WidenUtf8(html_u8);
        images_copied++;
      }
    }

    std::ofstream out(dst_file, std::ios::binary);
    if (!out) {
      MistAlert(hwnd_, L"导出失败", (L"无法写入文件：" + dst_file).c_str(), MistMsgTone::Danger);
      return false;
    }
    out << NarrowUtf8(html_content);
  } else {
    std::wstring target_assets_dir = JoinPath(dst_dir, L"assets");
    if (!images.empty()) {
      CreateDirectoryW(target_assets_dir.c_str(), nullptr);
    }

    std::wstring final_md = md_text;
    for (const auto& im : images) {
      if (!im.exists || im.disk_path.empty()) {
        continue;
      }
      std::wstring target_img = JoinPath(target_assets_dir, im.leaf_name);
      if (_wcsicmp(NormalizePathKey(im.disk_path).c_str(), NormalizePathKey(target_img).c_str()) != 0) {
        CopyFileW(im.disk_path.c_str(), target_img.c_str(), FALSE);
      }
      images_copied++;

      std::wstring norm_ref = L"assets/" + im.leaf_name;
      if (im.original_src != norm_ref) {
        ReplaceAllW(final_md, im.original_src, norm_ref);
      }
    }

    std::ofstream out(dst_file, std::ios::binary);
    if (!out) {
      MistAlert(hwnd_, L"导出失败", (L"无法写入文件：" + dst_file).c_str(), MistMsgTone::Danger);
      return false;
    }
    out << NarrowUtf8(final_md);
  }

  std::wstring tip = L"已成功导出「" + title + L"」";
  if (images_copied > 0) {
    tip += L"\n已完整打包 " + std::to_wstring(images_copied) + L" 张图片资源";
  }
  tip += L"\n\n保存位置：\n" + dst_file;
  MistAlert(hwnd_, L"导出成功", tip.c_str(), MistMsgTone::Info);
  return true;
}

void NotesWindow::ExportHtml(const std::wstring& target_path) {
  const std::wstring note_path = target_path.empty() ? path_ : target_path;
  ExportNoteWithAssets(note_path, L"html");
}

void NotesWindow::ExportMarkdown(const std::wstring& target_path) {
  const std::wstring note_path = target_path.empty() ? path_ : target_path;
  ExportNoteWithAssets(note_path, L"md");
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
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
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
    DeleteFileW(src.c_str());
    return;
  }
  const DWORD wait_rc = WaitForSingleObject(pi.hProcess, 60000);
  DWORD exit = 1;
  if (wait_rc == WAIT_TIMEOUT) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 2000);
  } else {
    GetExitCodeProcess(pi.hProcess, &exit);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  DeleteFileW(src.c_str());

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
    {
      POINT pt{};
      GetCursorPos(&pt);
      const int dpi = DpiAtPoint(pt);
      SetWindowPos(hwnd_, HWND_TOPMOST, ScalePxForDpi(40, dpi),
                   ScalePxForDpi(40, dpi), ScalePxForDpi(280, dpi),
                   ScalePxForDpi(140, dpi), SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
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
    {
      const RECT work = PrimaryWorkArea();
      POINT mid{(work.left + work.right) / 2, (work.top + work.bottom) / 2};
      const int dpi = DpiAtPoint(mid);
      const int max_w =
          (std::max)(480, static_cast<int>(work.right - work.left) * 92 / 100);
      const int max_h =
          (std::max)(360, static_cast<int>(work.bottom - work.top) * 92 / 100);
      int ww = (std::min)(max_w, ScalePxForDpi(960, dpi));
      int wh = (std::min)(max_h, ScalePxForDpi(640, dpi));
      const int x =
          work.left + (std::max)(0, static_cast<int>(work.right - work.left - ww) / 2);
      const int y =
          work.top + (std::max)(0, static_cast<int>(work.bottom - work.top - wh) / 2);
      SetWindowPos(hwnd_, HWND_NOTOPMOST, x, y, ww, wh,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
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
  if (md.empty()) {
    return;
  }
  // Mirror full document into RichEdit (source of truth when leaving WW).
  const std::wstring full = ComposeMarkdownWithNativeTitle(md);
  ApplyNativeTitleFromMarkdown(full);
  if (!edit_) {
    return;
  }
  const int len = GetWindowTextLengthW(edit_);
  std::wstring edit_text(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(edit_, edit_text.data(), len + 1);
  edit_text.resize(static_cast<size_t>(len));
  if (full != edit_text) {
    SetWindowTextW(edit_, full.c_str());
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
  EnsureTitleEdit();
  // Layout host first, then push markdown (empty notes still boot Toast).
  if (entering) {
    ApplyWysiwygVisibility();
  } else {
    web_.Show();
    web_.Resize();
  }
  LayoutClient();
  // RichEdit is authoritative after Load/NewTab. CurrentPlainText() would
  // return the previous tab's Toast cache once wysiwyg_ is already true —
  // that leaked the old # title into a freshly opened draft.
  std::wstring md;
  if (edit_) {
    const int len = GetWindowTextLengthW(edit_);
    md.assign(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(edit_, md.data(), len + 1);
    md.resize(static_cast<size_t>(len));
  }
  // Staging may already be body-only — rejoin with native title.
  if (!NativeTitleText().empty() ||
      (edit_ && !md.empty() && md[0] != L'#')) {
    md = ComposeMarkdownWithNativeTitle(md);
  }
  if (md.empty()) {
    md = L"# \n\n";
  }
  {
    const MdTitleBody parts = SplitMarkdownTitleBody(md);
    ApplyNativeTitleFromMarkdown(md);
    if (edit_) {
      SetWindowTextW(edit_, parts.body.c_str());
    }
    // Prefer session title_hint when the file has no leading "# …".
    if (NativeTitleText().empty() && tab_index_ >= 0 &&
        tab_index_ < static_cast<int>(tab_states_.size()) &&
        !tab_states_[static_cast<size_t>(tab_index_)].title_hint.empty()) {
      SetNativeTitleText(
          tab_states_[static_cast<size_t>(tab_index_)].title_hint);
    }
  }
  if (entering || force_reload) {
    // Cache immediately; PostMessage defers JsonEscape + WebView IPC so the
    // tab strip / title can paint first (Typora-class feel).
    web_.SetCachedMarkdown(md, NoteBaseDir());
    pending_tab_markdown_ = md;
    has_pending_tab_markdown_ = true;
    if (hwnd_) {
      PostMessageW(hwnd_, WM_APP + 83, 0, 0);
    } else {
      has_pending_tab_markdown_ = false;
      web_.SetMarkdown(md, NoteBaseDir());
    }
  }
  EnsureTabStates();
  if (tab_index_ >= 0 && tab_index_ < static_cast<int>(tab_states_.size())) {
    tab_states_[static_cast<size_t>(tab_index_)].wysiwyg = true;
  }
  UpdateModeStrip();
  RefreshTitle();
  RaiseTitleEdit();
  if (title_edit_) {
    InvalidateRect(title_edit_, nullptr, TRUE);
  }
}

void NotesWindow::ToggleWysiwyg() {
  EnsureMarkdownEditor(true);
}

std::wstring NotesWindow::NoteBaseDir() const {
  // Online notes ("online://<id>") are virtual paths with no real folder.
  // Images they embed live in the library's 云笔记 folder, so map the asset
  // base there instead of "online:/" (which does not exist → every relative
  // image 404s and silently fails to display).
  if (IsOnlineNotePath(path_)) {
    const std::wstring cloud_dir = JoinPath(NotesLibraryDir(true), L"云笔记");
    CreateDirectoryW(cloud_dir.c_str(), nullptr);
    return cloud_dir;
  }
  const std::wstring d = DirOfPath(path_);
  if (d.empty() || d == L".") {
    return NotesLibraryDir(true);
  }
  return d;
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
  // Non-Markdown must never keep WW / title chrome (Explorer/Notepad parity).
  if (wysiwyg_ && !PathLooksMarkdown()) {
    wysiwyg_ = false;
  }
  if (wysiwyg_) {
    EnsureTitleEdit();
    if (title_edit_) {
      ShowWindow(title_edit_, SW_SHOW);
    }
    // Body: Toast when ready; else RichEdit staging (body-only).
    if (web_.ContentReady()) {
      web_.Show();
      web_.Resize();
      ShowWindow(edit_, SW_HIDE);
      if (gutter_) {
        ShowWindow(gutter_, SW_HIDE);
      }
      RaiseTitleEdit();
    } else {
      if (web_.Host()) {
        ShowWindow(web_.Host(), SW_HIDE);
      }
      ShowWindow(edit_, SW_SHOW);
      RaiseTitleEdit();
    }
  } else {
    web_.Hide();
    if (title_edit_) {
      ShowWindow(title_edit_, SW_HIDE);
    }
    ShowWindow(edit_, SW_SHOW);
  }
  // Caller owns LayoutClient (SwitchToTab / EnsureMarkdownEditor layout once).
}

void NotesWindow::FocusActiveEditor(bool force) {
  if (!hwnd_ || !IsWindow(hwnd_) || !IsWindowVisible(hwnd_) ||
      IsIconic(hwnd_)) {
    return;
  }
  if (!force) {
    const HWND cur = GetFocus();
    if (cur && cur != hwnd_ && cur != tabs_ && cur != status_ &&
        cur != mode_strip_) {
      if (cur == title_edit_ || cur == sidebar_search_ ||
          cur == font_face_combo_ || cur == font_size_combo_ ||
          cur == outline_list_ ||
          (cur == sidebar_ && IsWindowVisible(sidebar_)) ||
          (cur == sidebar_results_ && IsWindowVisible(sidebar_results_)) ||
          (font_face_combo_ && IsChild(font_face_combo_, cur)) ||
          (font_size_combo_ && IsChild(font_size_combo_, cur)) ||
          (library_web_.Host() &&
           (cur == library_web_.Host() || IsChild(library_web_.Host(), cur)))) {
        return;
      }
    }
  }
  const bool want_wysiwyg =
      wysiwyg_ && web_.Available() && PathLooksMarkdown() && !toast_;
  DismissLibraryMenu();
  if (want_wysiwyg && web_.ContentReady()) {
    web_.FocusEditor();
  } else if (edit_ && IsWindow(edit_) && IsWindowVisible(edit_)) {
    SetFocus(edit_);
  } else if (want_wysiwyg) {
    web_.FocusEditor();
  }
}

void NotesWindow::EnsureTitleEdit() {
  if (title_edit_ || !hwnd_) {
    return;
  }
  // CLIPSIBLINGS: WebView2 host must not paint over this strip.
  title_edit_ = CreateWindowExW(
      0, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | ES_LEFT,
      0, 0, 100, 32, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTitleEditId)),
      GetModuleHandleW(nullptr), nullptr);
  if (!title_edit_) {
    return;
  }
  // Cue banner: 「标题」 (Typora placeholder).
  SendMessageW(title_edit_, EM_SETCUEBANNER, TRUE,
               reinterpret_cast<LPARAM>(L"标题"));
  title_edit_proc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      title_edit_, GWLP_WNDPROC,
      reinterpret_cast<LONG_PTR>(NotesWindow::TitleEditProc)));
  ApplyTitleFont();
  ApplyChrome();
}

void NotesWindow::RaiseTitleEdit() {
  if (title_edit_ && IsWindowVisible(title_edit_)) {
    // WebView2 ShowWindow often stacks above prior HWND_TOP siblings.
    SetWindowPos(title_edit_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  if (font_face_combo_ && IsWindowVisible(font_face_combo_)) {
    SetWindowPos(font_face_combo_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  if (font_size_combo_ && IsWindowVisible(font_size_combo_)) {
    SetWindowPos(font_size_combo_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  if (font_smaller_btn_ && IsWindowVisible(font_smaller_btn_)) {
    SetWindowPos(font_smaller_btn_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  if (font_bigger_btn_ && IsWindowVisible(font_bigger_btn_)) {
    SetWindowPos(font_bigger_btn_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
}

void NotesWindow::ApplyTitleFont() {
  if (!title_edit_) {
    return;
  }
  if (title_font_) {
    DeleteObject(title_font_);
    title_font_ = nullptr;
  }
  HDC hdc = GetDC(hwnd_ ? hwnd_ : nullptr);
  const int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
  if (hdc) {
    ReleaseDC(hwnd_ ? hwnd_ : nullptr, hdc);
  }
  // ~20pt at 96dpi; clamp feel via display-adaptivity (not a poster on 200%).
  int pt = 20;
  if (dpi >= 192) {
    pt = 16;
  } else if (dpi >= 144) {
    pt = 18;
  }
  const int height = -MulDiv(pt, dpi, 72);
  title_font_ = CreateFontW(height, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            VARIABLE_PITCH | FF_SWISS, L"Microsoft YaHei");
  if (title_font_) {
    SendMessageW(title_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(title_font_),
                 TRUE);
  }
}

int NotesWindow::TitleChromeHeight() const {
  HDC hdc = GetDC(hwnd_ ? hwnd_ : nullptr);
  const int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
  if (hdc) {
    ReleaseDC(hwnd_ ? hwnd_ : nullptr, hdc);
  }
  // Title field + vertical padding; floor so HiDPI / small work areas stay tappable.
  const int pad = MulDiv(18, dpi, 96);
  int font_px = MulDiv(20, dpi, 72);
  if (dpi >= 192) {
    font_px = MulDiv(16, dpi, 72);
  } else if (dpi >= 144) {
    font_px = MulDiv(18, dpi, 72);
  }
  return (std::max)(MulDiv(48, dpi, 96), font_px + pad);
}

std::wstring NotesWindow::NativeTitleText() const {
  if (!title_edit_) {
    return {};
  }
  const int len = GetWindowTextLengthW(title_edit_);
  std::wstring t(static_cast<size_t>(len) + 1, L'\0');
  GetWindowTextW(title_edit_, t.data(), len + 1);
  t.resize(static_cast<size_t>(len));
  return t;
}

void NotesWindow::SetNativeTitleText(const std::wstring& title) {
  EnsureTitleEdit();
  if (!title_edit_) {
    return;
  }
  title_applying_ = true;
  SetWindowTextW(title_edit_, title.c_str());
  title_applying_ = false;
  // SetWindowTextW leaves the whole text selected when the EDIT holds focus,
  // which paints a system-blue band under the title. Collapse to a caret.
  if (GetFocus() == title_edit_) {
    const int len = GetWindowTextLengthW(title_edit_);
    SendMessageW(title_edit_, EM_SETSEL, len, len);
  } else {
    SendMessageW(title_edit_, EM_SETSEL, 0, 0);
  }
}

void NotesWindow::ApplyNativeTitleFromMarkdown(const std::wstring& md) {
  const MdTitleBody parts = SplitMarkdownTitleBody(md);
  EnsureTabStates();
  if (!parts.title.empty() || parts.emit_title_line) {
    // Real ATX title (including empty "# \n\n" draft seed).
    SetNativeTitleText(parts.title);
    if (tab_index_ >= 0 &&
        tab_index_ < static_cast<int>(tab_states_.size())) {
      tab_states_[static_cast<size_t>(tab_index_)].title_hint = parts.title;
    }
    return;
  }
  // Body-only file: never wipe session title_hint / filename with "".
  if (!NativeTitleText().empty()) {
    return;
  }
  std::wstring fallback;
  if (tab_index_ >= 0 &&
      tab_index_ < static_cast<int>(tab_states_.size())) {
    fallback = tab_states_[static_cast<size_t>(tab_index_)].title_hint;
  }
  if (fallback.empty() && !path_.empty()) {
    const size_t slash = path_.find_last_of(L"\\/");
    fallback =
        slash == std::wstring::npos ? path_ : path_.substr(slash + 1);
    const size_t dot = fallback.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) {
      fallback = fallback.substr(0, dot);
    }
    fallback = LocalizeUntitledStem(fallback);
  }
  if (!fallback.empty()) {
    SetNativeTitleText(fallback);
    if (tab_index_ >= 0 &&
        tab_index_ < static_cast<int>(tab_states_.size())) {
      tab_states_[static_cast<size_t>(tab_index_)].title_hint = fallback;
    }
  }
}

std::wstring NotesWindow::ComposeMarkdownWithNativeTitle(
    const std::wstring& body_or_full) const {
  // If body_or_full still has a leading heading, strip it — native title wins.
  const MdTitleBody parts = SplitMarkdownTitleBody(body_or_full);
  const std::wstring title = NativeTitleText();
  // Do NOT force emit solely because wysiwyg_ is on — that invented
  // "# \n\n" on empty Toast cache and soft-save wiped real notes on disk.
  const bool emit = !title.empty() || parts.emit_title_line;
  return JoinMarkdownTitleBody(title, parts.body, emit);
}

void NotesWindow::PushNativeTitleToWeb() {
  if (!wysiwyg_) {
    return;
  }
  // Keep host cache coherent immediately (Save / tab label before Toast ack).
  web_.SetCachedMarkdown(
      ComposeMarkdownWithNativeTitle(web_.CachedMarkdown()));
  if (!web_.Ready()) {
    return;
  }
  const std::wstring title = NativeTitleText();
  std::wstring js =
      L"try{if(window.__desknoteSetHostTitle)__desknoteSetHostTitle(\"";
  for (wchar_t c : title) {
    if (c == L'\\' || c == L'"') {
      js += L'\\';
      js += c;
    } else if (c == L'\n' || c == L'\r') {
      js += L' ';
    } else if (c < 0x20 || c == 0x2028 || c == 0x2029) {
      wchar_t buf[8];
      swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
      js += buf;
    } else {
      js += c;
    }
  }
  js += L"\")}catch(e){}";
  web_.EvalScript(js);
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
  // Right-side / full-stage preview chrome removed.
  preview_ = false;
  if (preview_web_.Host()) {
    preview_web_.Hide();
  }
  ShowEditMode();
  PersistViewSettings();
}

void NotesWindow::ExportWord(const std::wstring& target_path) {
  const std::wstring note_path = target_path.empty() ? path_ : target_path;
  if (note_path.empty()) {
    return;
  }
  std::wstring title = NoteDisplayName(note_path, false);
  if (title.empty()) title = L"未命名笔记";

  std::wstring safe_name = title;
  for (auto& ch : safe_name) {
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
        ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
      ch = L'_';
    }
  }
  if (safe_name.size() >= 3 && _wcsicmp(safe_name.c_str() + safe_name.size() - 3, L".md") == 0) {
    safe_name.resize(safe_name.size() - 3);
  } else if (safe_name.size() >= 9 && _wcsicmp(safe_name.c_str() + safe_name.size() - 9, L".markdown") == 0) {
    safe_name.resize(safe_name.size() - 9);
  } else if (safe_name.size() >= 4 && _wcsicmp(safe_name.c_str() + safe_name.size() - 4, L".txt") == 0) {
    safe_name.resize(safe_name.size() - 4);
  }
  if (safe_name.empty()) {
    safe_name = L"未命名笔记";
  }

  wchar_t file[MAX_PATH] = {};
  wcsncpy_s(file, (safe_name + L".docx").c_str(), _TRUNCATE);

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter = L"Word 文档 (*.docx)\0*.docx\0所有文件 (*.*)\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
  ofn.lpstrDefExt = L"docx";
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }

  std::wstring md_text;
  const bool is_current = (!path_.empty() && _wcsicmp(NormalizePathKey(path_).c_str(),
                                                      NormalizePathKey(note_path).c_str()) == 0);
  if (is_current) {
    if (wysiwyg_ && web_.Ready()) {
      SyncFromWysiwyg();
    }
    md_text = CurrentPlainText();
  } else {
    if (IsOnlineNotePath(note_path)) {
      const int id = OnlineNoteIdFromPath(note_path);
      auto it = online_body_cache_.find(id);
      if (it != online_body_cache_.end()) {
        md_text = it->second.body;
      } else {
        // Do not block the UI thread on a network fetch here: VaultHttpGetNote
        // retries with blocking sleeps (worst case tens of seconds) and export
        // runs synchronously on the UI thread. If the body is not cached yet,
        // ask the user to open the note first (opening fills the cache in the
        // background). Exporting an empty body would silently produce a blank
        // document.
        MistAlert(hwnd_, L"导出", L"云端笔记正文尚未加载。请先打开该笔记，稍候再导出。",
                  MistMsgTone::Info);
        return;
      }
    } else if (GetFileAttributesW(note_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      std::string bytes;
      if (ReadBinaryFile(note_path, &bytes)) {
        TextFileEncoding enc = TextFileEncoding::Utf8;
        DecodeTextFileBytes(bytes, &md_text, &enc);
      }
    }
  }

  std::wstring base_dir = DirOfPath(note_path);
  if (base_dir.empty() || base_dir == L".") base_dir = NoteBaseDir();

  std::wstring err;
  if (!ExportMarkdownToDocx(md_text, title, base_dir, file, &err)) {
    MistAlert(hwnd_, L"导出 Word 失败",
              (L"导出 Word 文档失败：" + (err.empty() ? L"未知错误" : err)).c_str(),
              MistMsgTone::Danger);
    return;
  }

  MistAlert(hwnd_, L"导出成功", (L"已成功导出 Word 文档：\n" + std::wstring(file)).c_str(),
            MistMsgTone::Info);
  ShellExecuteW(nullptr, L"open", file, nullptr, nullptr, SW_SHOWNORMAL);
}

void NotesWindow::ExportPdf(const std::wstring& target_path) {
  const std::wstring note_path = target_path.empty() ? path_ : target_path;
  if (note_path.empty()) {
    return;
  }
  std::wstring title = NoteDisplayName(note_path, false);
  if (title.empty()) title = L"未命名笔记";

  std::wstring safe_name = title;
  for (auto& ch : safe_name) {
    if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' ||
        ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
      ch = L'_';
    }
  }
  if (safe_name.size() >= 3 && _wcsicmp(safe_name.c_str() + safe_name.size() - 3, L".md") == 0) {
    safe_name.resize(safe_name.size() - 3);
  } else if (safe_name.size() >= 9 && _wcsicmp(safe_name.c_str() + safe_name.size() - 9, L".markdown") == 0) {
    safe_name.resize(safe_name.size() - 9);
  } else if (safe_name.size() >= 4 && _wcsicmp(safe_name.c_str() + safe_name.size() - 4, L".txt") == 0) {
    safe_name.resize(safe_name.size() - 4);
  }
  if (safe_name.empty()) {
    safe_name = L"未命名笔记";
  }

  wchar_t file[MAX_PATH] = {};
  wcsncpy_s(file, (safe_name + L".pdf").c_str(), _TRUNCATE);

  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd_;
  ofn.lpstrFilter = L"PDF 文档 (*.pdf)\0*.pdf\0所有文件 (*.*)\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
  ofn.lpstrDefExt = L"pdf";
  if (!GetSaveFileNameW(&ofn)) {
    return;
  }

  if (!preview_web_.Available()) {
    preview_web_.SetKind(NotesWebView::Kind::kViewer);
    preview_web_.SetTheme(L"light");
    if (!preview_web_.Create(hwnd_)) {
      MistAlert(hwnd_, L"导出 PDF", L"无法启动 PDF 导出（WebView2 不可用）。",
                MistMsgTone::Warn);
      return;
    }
    RegisterDropOnSubtree(preview_web_.Host());
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

  std::wstring md_text;
  const bool is_current = (!path_.empty() && _wcsicmp(NormalizePathKey(path_).c_str(),
                                                      NormalizePathKey(note_path).c_str()) == 0);
  if (is_current) {
    if (wysiwyg_ && web_.Ready()) {
      SyncFromWysiwyg();
    }
    md_text = CurrentPlainText();
  } else {
    if (IsOnlineNotePath(note_path)) {
      const int id = OnlineNoteIdFromPath(note_path);
      auto it = online_body_cache_.find(id);
      if (it != online_body_cache_.end()) {
        md_text = it->second.body;
      } else {
        // Do not block the UI thread on a network fetch here: VaultHttpGetNote
        // retries with blocking sleeps (worst case tens of seconds) and export
        // runs synchronously on the UI thread. If the body is not cached yet,
        // ask the user to open the note first (opening fills the cache in the
        // background). Exporting an empty body would silently produce a blank
        // document.
        MistAlert(hwnd_, L"导出", L"云端笔记正文尚未加载。请先打开该笔记，稍候再导出。",
                  MistMsgTone::Info);
        return;
      }
    } else if (GetFileAttributesW(note_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      std::string bytes;
      if (ReadBinaryFile(note_path, &bytes)) {
        TextFileEncoding enc = TextFileEncoding::Utf8;
        DecodeTextFileBytes(bytes, &md_text, &enc);
      }
    }
  }

  std::wstring base_dir = DirOfPath(note_path);
  if (base_dir.empty() || base_dir == L".") base_dir = NoteBaseDir();

  preview_web_.SetPreview(md_text, base_dir);
  // Wait for the preview to actually finish painting (mermaid diagrams are
  // rendered async, images load async). A fixed delay made large notes export
  // blank / missing images.
  if (!preview_web_.WaitRendered(5000)) {
    // Timed out — keep the old fixed delay as a fallback so the export still
    // goes through with whatever the page managed to paint.
    const DWORD wait_start = GetTickCount();
    MSG msg{};
    while (GetTickCount() - wait_start < 450) {
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      Sleep(15);
    }
  }

  std::wstring err;
  if (!preview_web_.PrintToPdf(file, &err)) {
    MistAlert(hwnd_, L"导出 PDF",
              (L"PDF 导出失败：" + (err.empty() ? L"未知错误" : err)).c_str(),
              MistMsgTone::Danger);
    return;
  }

  MistAlert(hwnd_, L"导出成功", (L"已成功导出 PDF 文档：\n" + std::wstring(file)).c_str(),
            MistMsgTone::Info);
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
  const std::wstring rel = L"assets/" + name;
  if (wysiwyg_ && web_.Ready()) {
    web_.SetAssetBaseDir(NoteBaseDir());
    web_.InsertImage(rel, name.substr(0, name.find_last_of(L'.')));
    MarkDirty();
    return true;
  }
  const std::wstring md = L"![](" + rel + L")";
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
    InvalidateNotesFileCache();
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
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
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
  if (tabs_ && !toast_) {
    const int tabs_w = static_cast<int>(rc.right);
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
    const int account_h = 36;
    const int body_top = top + chrome_h + 4;
    const int body_h = (std::max)(40, bottom - body_top - account_h);
    if (sidebar_account_) {
      MoveWindow(sidebar_account_, 0, bottom - account_h, side, account_h, TRUE);
      ShowWindow(sidebar_account_, SW_SHOW);
    }
    EnsureLibraryRail();
    if (sidebar_results_) {
      ShowWindow(sidebar_results_, SW_HIDE);
    }
    // Unified online+local WebView rail (search box filters titles in HTML).
    if (library_web_.Host()) {
      MoveWindow(library_web_.Host(), 0, body_top, side, body_h, TRUE);
      library_web_.Show();
      library_web_.Resize();
      ShowWindow(sidebar_, SW_HIDE);
    } else {
      MoveWindow(sidebar_, 0, body_top, side, body_h, TRUE);
      ShowWindow(sidebar_, SW_SHOW);
    }
    (void)searching;
    left = side + 1;  // +1 for divider line painted in WM_PAINT
  } else {
    if (sidebar_) {
      ShowWindow(sidebar_, SW_HIDE);
    }
    if (library_web_.Host()) {
      library_web_.Hide();
    }
    if (sidebar_account_) {
      ShowWindow(sidebar_account_, SW_HIDE);
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

  // Right preview chip + outline rail removed — editor uses full width.

  const int stage_l = left + (sidebar_visible_ && !toast_ ? 12 : 8);
  const int stage_r = right - 8;
  const int stage_t = top + 8;
  const int stage_b = bottom - 8;
  const int stage_w = (std::max)(40, stage_r - stage_l);
  const int stage_h = (std::max)(40, stage_b - stage_t);

  const bool md_preview =
      preview_ && looks_md && !wysiwyg_ && !focus_;
  // Native title is immediate; Toast ContentReady only gates the body surface.
  const bool wysiwyg_live = wysiwyg_ && looks_md && web_.ContentReady();
  const bool show_native_title = wysiwyg_ && looks_md && !toast_;
  HDC hdc_pad = GetDC(hwnd_);
  const int dpi_pad = hdc_pad ? GetDeviceCaps(hdc_pad, LOGPIXELSY) : 96;
  if (hdc_pad) {
    ReleaseDC(hwnd_, hdc_pad);
  }
  const int title_pad_x = MulDiv(14, dpi_pad, 96);
  const int font_gap = MulDiv(8, dpi_pad, 96);
  const int face_w = MulDiv(108, dpi_pad, 96);
  const int size_w = MulDiv(56, dpi_pad, 96);
  const int combo_h = MulDiv(24, dpi_pad, 96);
  // Style chrome is Markdown-only. WW uses Toast toolbar; MD source keeps
  // native face/size. Plain files (hosts/.txt) match Notepad — no strip.
  const bool show_font_bar = looks_md && !toast_ && !wysiwyg_;
  if (show_font_bar) {
    EnsureFontControls();
  }
  const int btn_w = MulDiv(26, dpi_pad, 96);
  const int btn_gap = MulDiv(4, dpi_pad, 96);
  const int step_w = btn_w * 2 + btn_gap;
  const int font_tools_w =
      show_font_bar ? (face_w + font_gap + size_w + font_gap + step_w) : 0;
  auto place_font_tools = [&](int combo_y) {
    if (!font_face_combo_ || !font_size_combo_) {
      return;
    }
    const int bigger_x = stage_l + stage_w - title_pad_x - btn_w;
    const int smaller_x = bigger_x - btn_gap - btn_w;
    const int size_x = smaller_x - font_gap - size_w;
    const int face_x = size_x - font_gap - face_w;
    MoveWindow(font_face_combo_, face_x, combo_y, face_w, combo_h + 180, TRUE);
    MoveWindow(font_size_combo_, size_x, combo_y, size_w, combo_h + 220, TRUE);
    ShowWindow(font_face_combo_, SW_SHOW);
    ShowWindow(font_size_combo_, SW_SHOW);
    if (font_smaller_btn_) {
      MoveWindow(font_smaller_btn_, smaller_x, combo_y, btn_w, combo_h, TRUE);
      ShowWindow(font_smaller_btn_, SW_SHOW);
      SetWindowPos(font_smaller_btn_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (font_bigger_btn_) {
      MoveWindow(font_bigger_btn_, bigger_x, combo_y, btn_w, combo_h, TRUE);
      ShowWindow(font_bigger_btn_, SW_SHOW);
      SetWindowPos(font_bigger_btn_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SetWindowPos(font_face_combo_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(font_size_combo_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  };
  auto hide_font_tools = [&]() {
    if (font_face_combo_) {
      ShowWindow(font_face_combo_, SW_HIDE);
    }
    if (font_size_combo_) {
      ShowWindow(font_size_combo_, SW_HIDE);
    }
    if (font_smaller_btn_) {
      ShowWindow(font_smaller_btn_, SW_HIDE);
    }
    if (font_bigger_btn_) {
      ShowWindow(font_bigger_btn_, SW_HIDE);
    }
  };
  int body_t = stage_t;
  int body_h = stage_h;
  if (show_native_title) {
    EnsureTitleEdit();
    const int title_h = TitleChromeHeight();
    const int title_w =
        (std::max)(40, stage_w - title_pad_x * 2 - font_tools_w -
                           (font_tools_w ? font_gap : 0));
    if (title_edit_) {
      MoveWindow(title_edit_, stage_l + title_pad_x, stage_t, title_w, title_h,
                 TRUE);
      ShowWindow(title_edit_, SW_SHOW);
    }
    if (show_font_bar) {
      place_font_tools(stage_t + (std::max)(0, (title_h - combo_h) / 2));
    }
    // Gap under title for a 1px rule (painted in WM_PAINT).
    body_t = stage_t + title_h + MulDiv(6, dpi_pad, 96);
    body_h = (std::max)(40, stage_b - body_t);
  } else {
    if (title_edit_) {
      ShowWindow(title_edit_, SW_HIDE);
    }
    // MD source: font strip above the body. Non-md: full-bleed editor.
    if (show_font_bar) {
      const int bar_h = combo_h + MulDiv(8, dpi_pad, 96);
      place_font_tools(stage_t + MulDiv(4, dpi_pad, 96));
      body_t = stage_t + bar_h;
      body_h = (std::max)(40, stage_b - body_t);
    } else {
      hide_font_tools();
    }
  }
  if (!show_font_bar) {
    hide_font_tools();
  }
  if (wysiwyg_live) {
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
      // CLIPSIBLINGS on host so it cannot cover the title strip.
      SetWindowLongPtrW(web_.Host(), GWL_STYLE,
                        GetWindowLongPtrW(web_.Host(), GWL_STYLE) |
                            WS_CLIPSIBLINGS);
      MoveWindow(web_.Host(), stage_l, body_t, stage_w, body_h, TRUE);
      ShowWindow(web_.Host(), SW_SHOW);
      web_.Resize();
    }
    RaiseTitleEdit();
  } else if (wysiwyg_ && looks_md) {
    // Body staging only — title already in title_edit_.
    if (preview_web_.Host()) {
      preview_web_.Hide();
    }
    if (web_.Host()) {
      MoveWindow(web_.Host(), stage_l, body_t, stage_w, body_h, TRUE);
      ShowWindow(web_.Host(), SW_HIDE);
    }
    if (edit_) {
      // No gutter for body-only staging (cleaner Typora-like interim).
      if (gutter_) {
        ShowWindow(gutter_, SW_HIDE);
      }
      ShowWindow(edit_, SW_SHOW);
      MoveWindow(edit_, stage_l, body_t, stage_w, body_h, TRUE);
    }
    RaiseTitleEdit();
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
      // Respect font-bar offset for MD source; plain files use full stage.
      LayoutGutter(stage_l, body_t, body_h);
      const int edit_left = stage_l + (gutter_ ? gutter_width_ : 0);
      ShowWindow(edit_, SW_SHOW);
      MoveWindow(edit_, edit_left, body_t,
                 (std::max)(40, stage_l + stage_w - edit_left), body_h, TRUE);
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
  // Right outline rail removed.
  outline_ = false;
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
  if (wysiwyg_) {
    SyncFromWysiwyg();
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
  if (wysiwyg_) {
    SyncFromWysiwyg();
  }
  bool replaced = false;
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
          replaced = true;
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
      replaced = true;
    }
  }
  if (replaced && wysiwyg_) {
    int len = GetWindowTextLengthW(edit_);
    std::wstring updated(static_cast<size_t>(len) + 1, L'\0');
    if (len > 0) {
      GetWindowTextW(edit_, updated.data(), len + 1);
    }
    updated.resize(static_cast<size_t>(len));
    web_.SetCachedMarkdown(updated);
    web_.SetMarkdown(updated, NoteBaseDir());
    MarkDirty();
  }
  return RunFindOnce(true, wrap, match_case, whole, status_out);
}

int NotesWindow::RunReplaceAll(bool match_case, bool whole) {
  if (!edit_ || find_what_[0] == 0) {
    return 0;
  }
  if (wysiwyg_) {
    SyncFromWysiwyg();
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
      if (wysiwyg_) {
        web_.SetCachedMarkdown(out);
        web_.SetMarkdown(out, NoteBaseDir());
      }
      MarkDirty();
      RefreshGutter();
      return n;
    } catch (const std::regex_error&) {
      return 0;
    }
  }
  int n = 0;
  const LRESULT old_mask = SendMessageW(edit_, EM_GETEVENTMASK, 0, 0);
  SendMessageW(
      edit_, EM_SETEVENTMASK, 0,
      old_mask & ~static_cast<LRESULT>(ENM_CHANGE | ENM_SELCHANGE | ENM_SCROLL));
  SendMessageW(edit_, WM_SETREDRAW, FALSE, 0);
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
  SendMessageW(edit_, WM_SETREDRAW, TRUE, 0);
  SendMessageW(edit_, EM_SETEVENTMASK, 0, old_mask);
  InvalidateRect(edit_, nullptr, TRUE);
  if (n > 0) {
    if (wysiwyg_) {
      int len = GetWindowTextLengthW(edit_);
      std::wstring updated(static_cast<size_t>(len) + 1, L'\0');
      if (len > 0) {
        GetWindowTextW(edit_, updated.data(), len + 1);
      }
      updated.resize(static_cast<size_t>(len));
      web_.SetCachedMarkdown(updated);
      web_.SetMarkdown(updated, NoteBaseDir());
    }
    status_full_text_dirty_ = true;
    MarkDirty();
    RefreshGutter();
    SchedulePreviewRefresh();
    UpdateStatusBar();
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
    if (AtomicWriteUtf8File(path, bytes, false)) {
      total += n;
    }
  }
  return total;
}

void NotesWindow::ShowFind(bool replace) {
  MistWebDialog dlg;
  dlg.owner = hwnd_;
  dlg.frameless = true;
  dlg.topmost = false;  // Keep DeskTidy desktop band stable (no TOPMOST FG steal).
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
  MistWebDialogRun(&dlg, replace ? L"替换" : L"查找", 460,
                   replace ? 330 : 280);
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
              OFN_EXPLORER | OFN_NOCHANGEDIR;
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
  BringWindowToTop(hwnd_);
  // Windows foreground lock: a background process (file association / shell
  // "Open with") calling SetForegroundWindow directly is often ignored. Attach
  // to the foreground thread briefly so the window actually pops up.
  HWND fg = GetForegroundWindow();
  DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
  DWORD my_tid = GetCurrentThreadId();
  const bool attached = fg_tid && fg_tid != my_tid &&
                        AttachThreadInput(my_tid, fg_tid, TRUE);
  SetForegroundWindow(hwnd_);
  if (attached) {
    AttachThreadInput(my_tid, fg_tid, FALSE);
  }
  SetFocus(hwnd_);
  LayoutClient();
  // Do NOT force a reload here: Present() runs on every activation (tray click,
  // --focus), and force_reload re-pushes the whole document through
  // setMarkdown() → replaceAll(), which drops ProseMirror undo history,
  // selection and scroll position. Only a tab/path change needs the reload.
  EnsureMarkdownEditor(false);
  UpdateWindow(hwnd_);
  FocusActiveEditor(true);
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
  if (in_elevated_save_) {
    PersistTabs();
    PersistViewSettings();
    return true;
  }
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

void NotesWindow::EnsureOnlineNotesPanel() {
  if (online_notes_.Hwnd()) {
    return;
  }
  online_notes_.Create(GetModuleHandleW(nullptr));
  online_notes_.SetOnOpenLocal([this](const std::wstring& p) {
    if (!p.empty()) {
      OpenPaths({p});
      // 「转本地」landed a new .md under 云笔记 — the rail reads its own file
      // list, so without this the new note stayed invisible in the tree until
      // something else rebuilt it. Same refresh every other land-a-file path
      // (NewOnlineNoteInEditor etc.) already does.
      PushLibraryRailState(false);
      Present();
    }
  });
}

bool NotesWindow::ShowAccountLoginDialog() {
  const std::wstring html = MistFindAssetHtml(L"dialogs\\account_login.html");
  if (html.empty()) {
    // Fallback: simple prompt chain (unchanged behaviour).
    const std::wstring login =
        PromptTextWin32(hwnd_, L"在线笔记", L"用户名：", L"");
    if (login.empty()) return false;
    const std::wstring pass =
        PromptTextWin32(hwnd_, L"在线笔记", L"密码：", L"");
    if (pass.empty()) return false;
    return VaultHttpLogin(login, pass);
  }

  bool logged_in = false;

  MistWebDialog dlg;
  dlg.owner = hwnd_;
  dlg.frameless = true;
  dlg.user_data_subdir = L"webview2-mist-account-login";
  dlg.html_path = html;
  dlg.build_state = []() -> std::wstring {
    return L"{\"type\":\"state\",\"mode\":\"login\"}";
  };
  dlg.on_message = [&](const std::wstring& json) -> bool {
    const std::wstring type = MistJsonStringField(json, L"type");
    if (type != L"action") return false;
    const std::wstring id = MistJsonStringField(json, L"id");
    if (id == L"ready") {
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"cancel") return true;

    if (id == L"login") {
      const std::wstring login = MistJsonStringField(json, L"login");
      const std::wstring pass  = MistJsonStringField(json, L"pass");
      VaultLoginError lerr = VaultLoginError::None;
      if (VaultHttpLoginEx(login, pass, &lerr)) {
        logged_in = true;
        // Reply success then close.
        MistWebDialogPostJson(&dlg, L"{\"type\":\"result\",\"ok\":true}");
        return true;
      }
      // Tell the two cases apart. Reporting "wrong password" for a dead
      // network made users retype a perfectly good password.
      const wchar_t* reason =
          lerr == VaultLoginError::Network
              ? L"无法连接服务器，请检查网络后重试。"
              : (lerr == VaultLoginError::Empty ? L"请填写用户名和密码。"
                                                : L"用户名或密码错误。");
      std::wstring reply = L"{\"type\":\"result\",\"ok\":false,\"error\":\"";
      reply += JsonEscLib(reason);
      reply += L"\"}";
      MistWebDialogPostJson(&dlg, reply);
      return false;
    }

    if (id == L"register") {
      const std::wstring login   = MistJsonStringField(json, L"login");
      const std::wstring pass    = MistJsonStringField(json, L"pass");
      const std::wstring display = MistJsonStringField(json, L"display");
      std::wstring err;
      if (!VaultHttpRegister(login, pass, display, &err)) {
        const std::wstring msg =
            err.empty() ? L"注册失败，请稍后重试。" : err;
        std::wstring reply =
            L"{\"type\":\"result\",\"ok\":false,\"error\":\""
            + MistJsonEscape(msg) + L"\"}";
        MistWebDialogPostJson(&dlg, reply);
        return false;
      }
      // Register succeeded — auto login.
      if (!VaultHttpLogin(login, pass)) {
        MistWebDialogPostJson(
            &dlg,
            L"{\"type\":\"result\",\"ok\":false,"
            L"\"error\":\"注册成功，但自动登录失败，请手动登录。\"}");
        return false;
      }
      logged_in = true;
      MistWebDialogPostJson(&dlg, L"{\"type\":\"result\",\"ok\":true}");
      return true;
    }

    return false;
  };

  MistWebDialogRun(&dlg, L"在线笔记", 400, 340);
  return logged_in;
}

void NotesWindow::EnsureLibraryCloudLogin() {
  if (VaultHttpLoggedIn()) return;
  if (ShowAccountLoginDialog()) {
    NoteCloudNotifyPushWorker();
  }
}

void NotesWindow::LoadAndSweepCloudSync() {
  cloud_sync_ = LoadNoteCloudSync();
  const long long now = NoteCloudUnixNow();
  std::vector<int> due_ids;
  std::vector<int> abandoned_ids;
  for (const auto& d : cloud_sync_.pending_deletes) {
    if (d.note_id <= 0 || d.delete_after <= 0 || now < d.delete_after) {
      continue;
    }
    //「转本地」schedules this delete as the tail of a copy-then-remove dance.
    // If the local file is gone by now the user did not end up with a local
    // copy either (deleted it, or the write was rolled back) — deleting the
    // cloud note here would erase the only remaining copy. Keep it instead.
    const NoteCloudLink* link = nullptr;
    for (const auto& L : cloud_sync_.links) {
      if (L.note_id == d.note_id) {
        link = &L;
        break;
      }
    }
    const bool has_local = link && !link->path.empty() &&
                           GetFileAttributesW(link->path.c_str()) !=
                               INVALID_FILE_ATTRIBUTES;
    if (has_local) {
      due_ids.push_back(d.note_id);
    } else {
      abandoned_ids.push_back(d.note_id);
    }
  }
  if (!abandoned_ids.empty()) {
    // Stop scheduling deletes that can never become safe.
    std::set<int> drop(abandoned_ids.begin(), abandoned_ids.end());
    cloud_sync_.pending_deletes.erase(
        std::remove_if(cloud_sync_.pending_deletes.begin(),
                       cloud_sync_.pending_deletes.end(),
                       [&drop](const NoteCloudPendingDelete& d) {
                         return drop.count(d.note_id) > 0;
                       }),
        cloud_sync_.pending_deletes.end());
    SaveNoteCloudSync(cloud_sync_);
    LogLineA("[cloud] kept %d note(s) whose local copy is gone",
             static_cast<int>(abandoned_ids.size()));
  }
  if (!due_ids.empty()) {
    std::thread([hwnd = hwnd_, due_ids = std::move(due_ids)]() {
      if (!VaultHttpLoggedIn()) {
        return;
      }
      auto* deleted_ids = new std::vector<int>();
      for (int id : due_ids) {
        if (VaultHttpDeleteNote(id)) {
          deleted_ids->push_back(id);
        }
      }
      if (deleted_ids->empty() || !hwnd || !IsWindow(hwnd) ||
          !PostMessageW(hwnd, WM_APP + 88, 0,
                        reinterpret_cast<LPARAM>(deleted_ids))) {
        delete deleted_ids;
      }
    }).detach();
  }
}

bool NotesWindow::PathIsCloudSynced(const std::wstring& path) const {
  if (path.empty() || IsVaultAccountsNotePath(path)) {
    return false;
  }
  if (IsOnlineNotePath(path)) {
    return true;
  }
  const auto* L = FindNoteCloudLink(cloud_sync_, path);
  return L && L->sync && L->note_id > 0;
}

namespace {
// Worker→UI notification (WM_APP+87): the push outcome, so a failed upload is
// visible instead of silently leaving local and cloud diverged.
constexpr UINT kMsgCloudPushFailed = WM_APP + 87;
// Cloud delete failed after the local row/tab was already removed.
constexpr UINT kMsgCloudDeleteFailed = WM_APP + 94;
// Backoff before retrying a failed batch. The queue keeps the latest body per
// note, so a retry never resurrects stale content.
constexpr unsigned kCloudPushRetryMs = 5000;
// Exponential backoff for repeated failures: a long offline stretch otherwise
// retries every 5s forever (thread wakeups + WinHTTP session churn). Capped so
// reconnecting still uploads within a minute.
constexpr unsigned kCloudPushRetryMaxMs = 60000;
std::atomic<HWND> g_cloud_push_notify_hwnd{nullptr};
}  // namespace

// Cap the in-memory body cache. Every opened online note stays here for the
// whole session otherwise; a long-running window walking a large library could
// hold hundreds of full note bodies. The current tab and any note whose push is
// still queued must survive, so eviction skips those and drops the oldest
// others (std::map keeps ids ascending, not LRU, but the working set is tiny).
void NotesWindow::RememberOnlineBody(int note_id, const std::wstring& title,
                                    const std::wstring& body) {
  if (note_id <= 0) {
    return;
  }
  online_body_cache_[note_id] = OnlineBodyCache{title, body};
  constexpr size_t kMaxBodies = 24;
  if (online_body_cache_.size() <= kMaxBodies) {
    return;
  }
  const int keep_current =
      IsOnlineNotePath(path_) ? OnlineNoteIdFromPath(path_) : 0;
  for (auto it = online_body_cache_.begin();
       it != online_body_cache_.end() &&
       online_body_cache_.size() > kMaxBodies;) {
    if (it->first == keep_current || it->first == note_id) {
      ++it;
      continue;
    }
    it = online_body_cache_.erase(it);
  }
}

void NotesWindow::QueueCloudNotePush(int note_id, std::wstring title,
                                     std::wstring body) {
  if (note_id <= 0) {
    return;
  }
  if (title.empty()) {
    title = L"未命名";
  }
  RememberOnlineBody(note_id, title, body);
  g_cloud_push_notify_hwnd.store(hwnd_, std::memory_order_release);
  // One worker thread + latest-wins queue (state lives in notes_cloud_sync so
  // it survives this window). The old detached-thread-per-save spawned
  // unbounded threads and let an older request finish after a newer one.
  NoteCloudPushEnqueue(note_id, title, body);
  if (!NoteCloudPushMarkWorkerStarted()) {
    return;
  }
  // The push outcome goes back via PostMessage to the latest active window.
  std::thread([]() {
    unsigned backoff_ms = kCloudPushRetryMs;
    for (;;) {
      // Blocks until something is queued.
      NoteCloudPushWaitForRetry(1000u * 60u * 60u);
      auto batch = NoteCloudPushTakeAll();
      if (batch.empty()) {
        continue;
      }
      // Not logged in: keep the batch queued instead of dropping it. The old
      // code cleared the queue here and relied on "the next save re-queues" —
      // if the user stopped editing, those edits never left the machine.
      if (!VaultHttpLoggedIn()) {
        // Keep the batch queued — dropping it silently lost edits.
        NoteCloudPushRequeue(batch);
        NoteCloudPushWaitForRetry(backoff_ms);
        backoff_ms = (std::min)(backoff_ms * 2, kCloudPushRetryMaxMs);
        continue;
      }
      std::map<int, std::pair<std::wstring, std::wstring>> failed_batch;
      for (const auto& kv : batch) {
        VaultHttpNote updated;
        if (!VaultHttpUpdateNote(kv.first, kv.second.first,
                                 kv.second.second, &updated)) {
          failed_batch.emplace(kv.first, kv.second);
        }
      }
      if (!failed_batch.empty()) {
        // Re-queue only what failed so succeeded notes are not re-uploaded.
        const int failed = static_cast<int>(failed_batch.size());
        NoteCloudPushRequeue(failed_batch);
        HWND hwnd = g_cloud_push_notify_hwnd.load(std::memory_order_acquire);
        if (hwnd && IsWindow(hwnd)) {
          PostMessageW(hwnd, kMsgCloudPushFailed,
                       static_cast<WPARAM>(failed), 0);
        }
        NoteCloudPushWaitForRetry(backoff_ms);
        backoff_ms = (std::min)(backoff_ms * 2, kCloudPushRetryMaxMs);
      } else {
        backoff_ms = kCloudPushRetryMs;  // healthy again — reset the ramp
      }
    }
  }).detach();
}

void NotesWindow::EnsureLibraryRail() {
  if (library_web_.Host()) {
    return;
  }
  library_web_.SetKind(NotesWebView::Kind::kLibrary);
  if (!library_web_.Create(hwnd_)) {
    return;
  }
  library_web_.on_library_action = [this](const std::wstring& json) {
    // Nested MistConfirm / PromptText inside WebMessageReceived freezes the
    // host (same pattern as settings / notes_library / snapshot_ui).
    pending_library_action_json_ = json;
    if (hwnd_) {
      PostMessageW(hwnd_, WM_APP + 84, 0, 0);
    }
  };
  library_web_.ShowLibrary(L"{\"logged_in\":false,\"online\":[],\"local\":[]}");
}

void NotesWindow::UpdateLibraryRailActiveOnly() {
  if (!library_web_.Host() || !library_web_.Ready()) {
    return;
  }
  // Selection highlight only — no disk peek, no cloud list round-trip.
  // applyActive only flips .on classes. The old applyState(window.__lib)
  // re-rendered the whole list on every tab switch (innerHTML rebuild → scroll
  // jump, hover loss) just to move the highlight.
  std::wstring script =
      L"(function(){try{if(typeof window.applyActive!=='function'){"
      L"return;}window.applyActive(\"";
  script += JsonEscLib(path_);
  script += L"\");}catch(e){}})();";
  library_web_.EvalScript(script);
}

void NotesWindow::DismissLibraryMenu() {
  if (library_web_.Host() && library_web_.Ready()) {
    library_web_.EvalScript(
        L"if(typeof window.hideMenu==='function')window.hideMenu();");
  }
}

void NotesWindow::EnsureLibraryListFetch() {
  // A worker whose PostMessageW failed (window mid-teardown) never clears the
  // busy flag, and then the online list can never refresh again for the life of
  // the session. Treat a "busy" state older than the request budget as stale.
  if (library_list_busy_) {
    constexpr ULONGLONG kStaleMs = 120000;  // 2 min >> worst-case retry budget
    if (GetTickCount64() - library_list_busy_since_ < kStaleMs) {
      library_list_refresh_pending_ = true;
      return;
    }
    library_list_busy_ = false;  // stale marker — fall through and retry
  }
  library_list_busy_ = true;
  library_list_busy_since_ = GetTickCount64();
  // VaultHttp* is cookie-mutex guarded and safe off-thread. Only hwnd is
  // captured (no `this`): if the window dies before completion, PostMessage
  // fails and the payload is freed here.
  std::thread([hwnd = hwnd_]() {
    std::vector<VaultHttpNote> remote;
    const bool ok = VaultHttpListNotes(&remote);
    auto* res =
        new std::pair<bool, std::vector<VaultHttpNote>>(ok, std::move(remote));
    if (!PostMessageW(hwnd, WM_APP + 85, 0,
                      reinterpret_cast<LPARAM>(res))) {
      delete res;
    }
  }).detach();
}

void NotesWindow::PushLibraryRailState(bool refresh_online) {
  if (!library_web_.Host()) {
    return;
  }
  if (!VaultHttpLoggedIn()) {
    library_online_cache_.clear();
    library_online_stale_ = true;
    library_list_loaded_once_ = false;
  } else if (refresh_online || library_online_stale_ ||
             !library_list_loaded_once_) {
    // Async fetch — see EnsureLibraryListFetch. The rail renders whatever the
    // cache holds (possibly empty) and refreshes when WM_APP+85 lands.
    //
    // The old `cache.empty()` condition re-fetched on every push once the
    // account genuinely had no online notes: WM_APP+85 clears the cache, then
    // PushLibraryRailState sees it empty and starts another request, forever.
    // `library_list_loaded_once_` marks "we have an answer"; staleness alone
    // drives refresh after that.
    EnsureLibraryListFetch();
  }
  wchar_t qbuf[256]{};
  if (sidebar_search_) {
    GetWindowTextW(sidebar_search_, qbuf, 256);
  }
  const std::wstring query = qbuf;
  // One prefs read per push — NotesLibraryDir loads the prefs JSON each call.
  const std::wstring root = NotesLibraryDir(false);
  // Per-push existence cache. This function stats every cloud link and every
  // matched file, and ran several times per user action (title keystroke,
  // save, sidebar rebuild). One map lookup per path instead of repeated disk
  // round-trips; invalidated on every push so it never goes stale.
  std::map<std::wstring, bool> exists_cache;
  auto path_exists = [&](const std::wstring& p) -> bool {
    if (p.empty()) {
      return false;
    }
    const auto it = exists_cache.find(p);
    if (it != exists_cache.end()) {
      return it->second;
    }
    const bool ok = GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    exists_cache.emplace(p, ok);
    return ok;
  };
  std::vector<NoteSearchHit> body_hits;
  const bool local_matched = !query.empty();
  if (local_matched) {
    // Load search prefs once per window — LoadAppPrefs() re-reads the JSON
    // from disk on every debounced keystroke otherwise.
    if (!sidebar_search_prefs_loaded_) {
      const AppPrefs prefs = LoadAppPrefs();
      notes_search_case_sensitive_ = prefs.notes_search_case_sensitive;
      notes_search_max_hits_ = prefs.notes_search_max_hits;
      notes_search_max_file_bytes_ = prefs.notes_search_max_file_bytes;
      sidebar_search_prefs_loaded_ = true;
    }
    // Full-text (file name + body) match on the host side. The WebView rail
    // only holds titles, so body hits must be computed here — the rail shows
    // exactly the hit set, line + preview included.
    body_hits = SearchNotesLibrary(root, query,
                                   notes_search_case_sensitive_,
                                   notes_search_max_hits_,
                                   notes_search_max_file_bytes_);
  }
  std::wstring json = L"{\"query\":\"";
  json += JsonEscLib(query);
  json += L"\",\"active\":\"";
  json += JsonEscLib(path_);
  json += L"\",\"logged_in\":";
  json += VaultHttpLoggedIn() ? L"true" : L"false";
  json += L",\"local_matched\":";
  json += local_matched ? L"true" : L"false";
  json += L",\"online\":[";
  bool first_online = true;
  std::set<std::wstring> pinned_set;
  for (const auto& p : pinned_paths_) {
    pinned_set.insert(NormalizePathKey(p));
  }
  std::set<std::wstring> locked_set;
  for (const auto& p : locked_paths_) {
    locked_set.insert(NormalizePathKey(p));
  }
  auto fast_is_pinned = [&](const std::wstring& p) -> bool {
    return !pinned_set.empty() && pinned_set.count(NormalizePathKey(p)) > 0;
  };
  auto fast_is_locked = [&](const std::wstring& p) -> bool {
    if (p.empty()) return false;
    if (IsVaultAccountsNotePath(p)) return true;
    return !locked_set.empty() && locked_set.count(NormalizePathKey(p)) > 0;
  };

  std::map<std::wstring, const NoteSearchHit*> hit_map;
  if (local_matched) {
    for (const auto& hit : body_hits) {
      std::wstring k = hit.path;
      for (auto& c : k) {
        if (c == L'/') c = L'\\';
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
      }
      hit_map[k] = &hit;
    }
  }

  // Synced local files (same path, sync attribute). Keys are normalized the
  // same way as the `key` lookups below (lowercase + backslash), otherwise a
  // link stored with forward slashes / different case silently misses.
  std::set<std::wstring> synced_keys;
  for (const auto& L : cloud_sync_.links) {
    if (!L.sync || L.note_id <= 0 || L.path.empty()) {
      continue;
    }
    if (!path_exists(L.path)) {
      continue;
    }
    std::wstring nkey = L.path;
    for (auto& c : nkey) {
      if (c == L'/') {
        c = L'\\';
      }
      if (c >= L'A' && c <= L'Z') {
        c = static_cast<wchar_t>(c - L'A' + L'a');
      }
    }
    synced_keys.insert(nkey);

    const NoteSearchHit* hit_ptr = nullptr;
    if (local_matched) {
      auto it_hit = hit_map.find(nkey);
      if (it_hit != hit_map.end()) {
        hit_ptr = it_hit->second;
      } else {
        // 在搜索模式下，如果该已同步文件未命中文件名或正文，则不列出
        continue;
      }
    }

    if (!first_online) {
      json += L',';
    }
    first_online = false;
    json += L"{\"kind\":\"synced\",\"id\":";
    json += std::to_wstring(L.note_id);
    json += L",\"path\":\"";
    json += JsonEscLib(L.path);
    json += L"\",\"title\":\"";
    json += JsonEscLib(NoteDisplayName(L.path, false));
    json += L"\",\"updated_at\":\"已同步\"";
    if (hit_ptr) {
      std::wstring sub;
      if (hit_ptr->line >= 0) {
        sub = L"行 " + std::to_wstring(hit_ptr->line + 1) + L"：";
      } else {
        sub = L"文件名匹配";
      }
      if (!hit_ptr->preview.empty()) {
        sub += hit_ptr->preview;
      }
      json += L",\"folder\":\"";
      json += JsonEscLib(sub);
      json += L"\",\"line\":";
      json += std::to_wstring(hit_ptr->line);
    }
    json += L",\"pinned\":";
    json += fast_is_pinned(L.path) ? L"true" : L"false";
    json += L",\"locked\":";
    json += fast_is_locked(L.path) ? L"true" : L"false";
    json += L"}";
  }
  // Pure remote rows only when never bound locally. After「转本地」sync=false
  // + pending_deletes must NOT reappear here (was leaking as online:// ghosts).
  // If a bound local file was deleted on disk, do not suppress the online item.
  // Set-based: the old per-item nested scans were O(online × links).
  std::set<int> linked_ids;
  for (const auto& L : cloud_sync_.links) {
    if (L.sync && !L.path.empty() && path_exists(L.path)) {
      linked_ids.insert(L.note_id);
    }
  }
  std::set<int> pending_delete_ids;
  for (const auto& d : cloud_sync_.pending_deletes) {
    pending_delete_ids.insert(d.note_id);
  }
  for (const auto& n : library_online_cache_) {
    if (linked_ids.count(n.id) || pending_delete_ids.count(n.id)) {
      continue;
    }
    if (!first_online) {
      json += L',';
    }
    first_online = false;
    const std::wstring online_path = OnlineNotePathFromId(n.id);
    json += L"{\"kind\":\"online\",\"id\":";
    json += std::to_wstring(n.id);
    json += L",\"path\":\"";
    json += JsonEscLib(online_path);
    json += L"\",\"title\":\"";
    json += JsonEscLib(n.title);
    json += L"\",\"updated_at\":\"";
    json += JsonEscLib(n.updated_at);
    json += L"\",\"pinned\":";
    json += fast_is_pinned(online_path) ? L"true" : L"false";
    json += L",\"locked\":";
    json += fast_is_locked(online_path) ? L"true" : L"false";
    json += L"}";
  }
  json += L"],\"local\":[";
  bool first_local = true;
  if (local_matched) {
    // Search mode: list only full-text hits (file name + body). Each hit
    // carries the match line and a body preview so the rail explains why the
    // file matched (file-name hits have line == -1 and no preview).
    for (const auto& hit : body_hits) {
      const std::wstring& full = hit.path;
      if (full.empty() || IsOnlineNotePath(full) ||
          IsVaultAccountsNotePath(full)) {
        continue;
      }
      if (!path_exists(full)) {
        continue;
      }
      std::wstring key = full;
      for (auto& c : key) {
        if (c == L'/') {
          c = L'\\';
        }
        if (c >= L'A' && c <= L'Z') {
          c = static_cast<wchar_t>(c - L'A' + L'a');
        }
      }
      if (synced_keys.count(key) || PathIsCloudSynced(full)) {
        continue;
      }
      if (!first_local) {
        json += L',';
      }
      first_local = false;
      std::wstring sub;
      if (hit.line >= 0) {
        sub = L"行 " + std::to_wstring(hit.line + 1) + L"：";
      } else {
        sub = L"文件名匹配";
      }
      if (!hit.preview.empty()) {
        sub += hit.preview;
      }
      json += L"{\"kind\":\"local\",\"path\":\"";
      json += JsonEscLib(full);
      json += L"\",\"title\":\"";
      json += JsonEscLib(NoteDisplayName(full, false));
      json += L"\",\"folder\":\"";
      json += JsonEscLib(sub);
      json += L"\",\"pinned\":";
      json += fast_is_pinned(full) ? L"true" : L"false";
      json += L",\"locked\":";
      json += fast_is_locked(full) ? L"true" : L"false";
      json += L",\"line\":";
      json += std::to_wstring(hit.line);
      json += L"}";
    }
  } else {
    // Enumerate the library on disk, not sidebar_paths_. That array is only
    // filled by RebuildSidebar (the legacy TreeView rail); the WebView rail never
    // calls it, so anything that lands a file without a TreeView rebuild — most
    // visibly「转本地」— stayed missing from the tree. ListNoteFiles is
    // mtime-cached, so the per-push cost is one FindFirstFile per directory.
    for (const auto& full : ListNoteFiles(root)) {
      // SidebarRemovePaths clears slots to "" (keeps TreeView lParam); never
      // paint those ghosts — and drop paths already recycled off disk.
      if (full.empty() || IsOnlineNotePath(full) ||
          IsVaultAccountsNotePath(full)) {
        continue;
      }
      if (!path_exists(full)) {
        continue;
      }
      std::wstring key = full;
      for (auto& c : key) {
        if (c == L'/') {
          c = L'\\';
        }
        if (c >= L'A' && c <= L'Z') {
          c = static_cast<wchar_t>(c - L'A' + L'a');
        }
      }
      if (synced_keys.count(key) || PathIsCloudSynced(full)) {
        continue;
      }
      if (!first_local) {
        json += L',';
      }
      first_local = false;
      std::wstring folder = L"本地库";
      if (!root.empty() && full.size() > root.size() + 1 &&
          _wcsnicmp(full.c_str(), root.c_str(), root.size()) == 0) {
        std::wstring rel = full.substr(root.size() + 1);
        const size_t slash = rel.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
          folder = rel.substr(0, slash);
        }
      }
      json += L"{\"kind\":\"local\",\"path\":\"";
      json += JsonEscLib(full);
      json += L"\",\"title\":\"";
      json += JsonEscLib(NoteDisplayName(full, false));
      json += L"\",\"folder\":\"";
      json += JsonEscLib(folder);
      json += L"\",\"pinned\":";
      json += fast_is_pinned(full) ? L"true" : L"false";
      json += L",\"locked\":";
      json += fast_is_locked(full) ? L"true" : L"false";
      json += L"}";
    }
  }
  json += L"]}";
  // Identical state → skip EvalScript + the page's full innerHTML rebuild.
  // Pushes fire from many paths (tab switches, title ticks, actions); without
  // this gate every one of them re-rendered the rail for nothing.
  if (json != last_library_json_) {
    last_library_json_ = json;
    library_web_.PushLibraryState(json);
  }
}

void NotesWindow::HandleLibraryRailAction(const std::wstring& json) {
  const std::wstring id = LibJsonString(json, L"id");
  if (id == L"login") {
    EnsureLibraryCloudLogin();
    if (VaultHttpLoggedIn()) {
      library_online_stale_ = true;
      PushLibraryRailState(true);
    } else {
      PushLibraryRailState(false);
    }
    return;
  }
  if (id == L"open_online") {
    OpenOnlineNoteInEditor(LibJsonInt(json, L"note_id"));
    return;
  }
  if (id == L"open_local") {
    const std::wstring path = LibJsonString(json, L"path");
    if (!path.empty()) {
      OpenPaths({path});
      // Search hits carry the match line — jump there once the tab loads.
      const std::wstring line_key = L"line";
      const std::wstring needle = std::wstring(L"\"") + line_key + L"\"";
      if (json.find(needle) != std::wstring::npos) {
        const int line = LibJsonInt(json, L"line");
        if (line >= 0) {
          if (PathLooksMarkdown() && wysiwyg_) {
            // Markdown 默认打开在只读预览/WYSIWYG；跳行依赖 RichEdit 行号，
            // 先切到源码编辑模式再 GotoLine。
            wysiwyg_ = false;
            ApplyWysiwygVisibility();
            LayoutClient();
          }
          if (preview_ && preview_web_.Host()) {
            preview_web_.Hide();
          }
          pending_goto_line_ = line;
          PostMessageW(hwnd_, WM_APP + 60, 0, 0);
        }
      }
    }
    return;
  }
  if (id == L"to_cloud") {
    ConvertPathToCloud(LibJsonString(json, L"path"));
    return;
  }
  if (id == L"to_local") {
    ConvertPathToLocal(LibJsonString(json, L"path"),
                       LibJsonInt(json, L"note_id"));
    return;
  }
  if (id == L"pin") {
    const std::wstring path = LibJsonString(json, L"path");
    if (!path.empty()) {
      TogglePinPath(path);
      PushLibraryRailState(false);
    }
    return;
  }
  if (id == L"lock") {
    const std::wstring path = LibJsonString(json, L"path");
    if (!path.empty()) {
      ToggleLockPath(path);
      PushLibraryRailState(false);
    }
    return;
  }
  if (id == L"rename") {
    const std::wstring path = LibJsonString(json, L"path");
    const int note_id = LibJsonInt(json, L"note_id");
    if (note_id > 0 && (path.empty() || IsOnlineNotePath(path))) {
      RenameOnlineNote(note_id, LibJsonString(json, L"title"));
      return;
    }
    if (path.empty() || IsVaultAccountsNotePath(path)) {
      return;
    }
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
      PushLibraryRailState(false);
    }
    return;
  }
  if (id == L"delete") {
    const std::wstring path = LibJsonString(json, L"path");
    const int note_id = LibJsonInt(json, L"note_id");
    if (note_id > 0 && (path.empty() || IsOnlineNotePath(path))) {
      DeleteOnlineNote(note_id, LibJsonString(json, L"title"));
      return;
    }
    if (path.empty()) {
      return;
    }
    sidebar_multi_.clear();
    sidebar_multi_.push_back(path);
    sidebar_anchor_path_ = path;
    // DeleteSidebarSelected → RebuildSidebar refreshes the WebView list.
    DeleteSidebarSelected();
    return;
  }
  if (id == L"reveal") {
    const std::wstring path = LibJsonString(json, L"path");
    if (!path.empty()) {
      RevealPathInExplorer(path);
    }
    return;
  }
  if (id == L"export_docx") {
    const std::wstring path = LibJsonString(json, L"path");
    if (!path.empty()) {
      ExportWord(path);
    }
    return;
  }
  if (id == L"export_pdf") {
    const std::wstring path = LibJsonString(json, L"path");
    if (!path.empty()) {
      ExportPdf(path);
    }
    return;
  }
  if (id == L"export_md") {
    const std::wstring path = LibJsonString(json, L"path");
    if (!path.empty()) {
      ExportMarkdown(path);
    }
    return;
  }
  if (id == L"export_html") {
    const std::wstring path = LibJsonString(json, L"path");
    if (!path.empty()) {
      ExportHtml(path);
    }
    return;
  }
}

void NotesWindow::ConvertPathToCloud(const std::wstring& path) {
  if (path.empty() || IsVaultAccountsNotePath(path) || IsOnlineNotePath(path)) {
    return;
  }
  if (!VaultHttpLoggedIn()) {
    EnsureLibraryCloudLogin();
    if (!VaultHttpLoggedIn()) {
      return;
    }
  }
  if (_wcsicmp(NormalizePathKey(path).c_str(),
               NormalizePathKey(path_).c_str()) == 0 &&
      dirty_) {
    Save(true, false);
  }
  std::wstring body;
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      MistAlert(hwnd_, L"转云", L"无法读取本地文件。", MistMsgTone::Warn);
      return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string raw = ss.str();
    TextFileEncoding enc = TextFileEncoding::Utf8;
    if (!DecodeTextFileBytes(raw, &body, &enc)) {
      body = WidenUtf8(raw);
    }
  }
  std::wstring title = NoteDisplayName(path, true);
  if (title.empty()) {
    title = L"未命名";
  }
  // Strip .md suffix from display name for cloud title.
  if (title.size() > 3 && _wcsicmp(title.c_str() + title.size() - 3, L".md") == 0) {
    title.resize(title.size() - 3);
  }
  int note_id = 0;
  if (const auto* existing = FindNoteCloudLink(cloud_sync_, path)) {
    note_id = existing->note_id;
  }
  // Uploading blocks the UI thread; show progress so the click does not look
  // ignored while the request is in flight.
  if (status_) {
    SetWindowTextW(status_, L"正在上传到云端…");
    UpdateWindow(status_);
  }
  VaultHttpNote remote;
  if (note_id > 0) {
    if (!VaultHttpUpdateNote(note_id, title, body, &remote)) {
      if (status_) {
        SetWindowTextW(status_, L"");
      }
      MistAlert(hwnd_, L"转云",
                L"上传失败。可能是登录已过期或网络不可达，"
                L"请点击侧边栏底部的账号条重新登录后重试。",
                MistMsgTone::Danger);
      return;
    }
  } else {
    if (!VaultHttpCreateNote(title, body, &remote) || remote.id <= 0) {
      if (status_) {
        SetWindowTextW(status_, L"");
      }
      MistAlert(hwnd_, L"转云",
                L"上传失败。可能是登录已过期或网络不可达，"
                L"请点击侧边栏底部的账号条重新登录后重试。",
                MistMsgTone::Danger);
      return;
    }
    note_id = remote.id;
  }
  NoteCloudUpsertLink(&cloud_sync_, path, note_id, true);
  // Cancel pending delete if re-clouded.
  cloud_sync_.pending_deletes.erase(
      std::remove_if(cloud_sync_.pending_deletes.begin(),
                     cloud_sync_.pending_deletes.end(),
                     [note_id](const NoteCloudPendingDelete& d) {
                       return d.note_id == note_id;
                     }),
      cloud_sync_.pending_deletes.end());
  SaveNoteCloudSync(cloud_sync_);
  RememberOnlineBody(note_id, title, body);
  if (status_) {
    SetWindowTextW(status_, L"已转云（同一文件，已开启同步）");
  }
  PushLibraryRailState(false);
}

void NotesWindow::ConvertPathToLocal(const std::wstring& path, int note_id) {
  if (IsVaultAccountsNotePath(path)) {
    return;
  }
  int id = note_id;
  std::wstring local_path = path;
  if (IsOnlineNotePath(path) || (path.empty() && id > 0)) {
    if (IsOnlineNotePath(path)) {
      id = OnlineNoteIdFromPath(path);
    }
    // Materialize online:// into a library file, then mark unsynced.
    VaultHttpNote n;
    std::wstring body;
    std::wstring title = L"未命名";
    bool have_body = false;
    auto cit = online_body_cache_.find(id);
    if (cit != online_body_cache_.end() && !cit->second.body.empty()) {
      body = cit->second.body;
      title = cit->second.title.empty() ? title : cit->second.title;
      have_body = true;
    } else if (id > 0 && VaultHttpLoggedIn() && VaultHttpGetNote(id, &n) &&
               !n.body.empty()) {
      title = n.title.empty() ? title : n.title;
      body = n.body;
      have_body = true;
    }
    // The cloud copy is deleted after this conversion, so a stub body would
    // replace the user's note with an empty file. Refuse instead.
    if (!have_body) {
      MistAlert(hwnd_, L"转本地",
                L"未能取到云端内容（可能已离线或登录过期）。\n"
                L"为避免云端笔记被删除后内容丢失，本次未执行转换，请联网后重试。",
                MistMsgTone::Danger);
      return;
    }
    const std::wstring dir = JoinPath(NotesLibraryDir(true), L"云笔记");
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring stem = title;
    for (wchar_t& c : stem) {
      if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
          c == L'"' || c == L'<' || c == L'>' || c == L'|') {
        c = L'_';
      }
    }
    if (stem.empty()) {
      stem = L"未命名";
    }
    local_path = JoinPath(dir, stem + L".md");
    if (GetFileAttributesW(local_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      for (int i = 1; i < 1000; ++i) {
        wchar_t suf[32];
        swprintf_s(suf, L"_%02d.md", i);
        local_path = JoinPath(dir, stem + suf);
        if (GetFileAttributesW(local_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
          break;
        }
      }
    }
    if (!AtomicWriteUtf8File(local_path, NarrowUtf8(body), false)) {
      MistAlert(hwnd_, L"转本地", L"写入本地文件失败，云端笔记未删除。",
                MistMsgTone::Danger);
      return;
    }
    // Retarget every place that may hold the online:// key. Missing pin/lock
    // here left entries pointing at a path that no longer exists, so the
    // converted note silently lost its pinned/locked state.
    for (auto& tp : tab_paths_) {
      if (_wcsicmp(tp.c_str(), path.c_str()) == 0) {
        tp = local_path;
      }
    }
    for (auto& pp : pinned_paths_) {
      if (_wcsicmp(pp.c_str(), path.c_str()) == 0) {
        pp = local_path;
      }
    }
    for (auto& lp : locked_paths_) {
      if (_wcsicmp(lp.c_str(), path.c_str()) == 0) {
        lp = local_path;
      }
    }
    if (_wcsicmp(path_.c_str(), path.c_str()) == 0) {
      path_ = local_path;
    }
    NoteCloudUpsertLink(&cloud_sync_, local_path, id, false);
  } else {
    if (id <= 0) {
      if (const auto* L = FindNoteCloudLink(cloud_sync_, path)) {
        id = L->note_id;
      }
    }
    if (id <= 0) {
      MistAlert(hwnd_, L"转本地", L"该文件尚未关联云笔记。", MistMsgTone::Info);
      return;
    }
    // The library rail's online rows carry only an id, so `path` can be empty
    // here. Without a real local file there is nothing to convert *to*, and
    // scheduling the cloud delete would destroy the note outright.
    if (local_path.empty() ||
        GetFileAttributesW(local_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      MistAlert(hwnd_, L"转本地",
                L"找不到对应的本地文件，云端笔记未删除。", MistMsgTone::Danger);
      return;
    }
    NoteCloudUpsertLink(&cloud_sync_, path, id, false);
  }
  if (id > 0) {
    NoteCloudScheduleDelete(&cloud_sync_, id, NoteCloudUnixAfterHours(24));
    // Drop from online list cache immediately (cloud file still lives 24h).
    library_online_cache_.erase(
        std::remove_if(library_online_cache_.begin(),
                       library_online_cache_.end(),
                       [id](const LibraryOnlineItem& it) {
                         return it.id == id;
                       }),
        library_online_cache_.end());
  }
  SaveNoteCloudSync(cloud_sync_);
  RebuildSidebar();
  RebuildTabControl();
  if (status_) {
    SetWindowTextW(status_, L"已转本地；云端副本将在 24 小时后删除");
  }
}

void NotesWindow::RenameOnlineNote(int note_id,
                                   const std::wstring& fallback_title) {
  if (note_id <= 0) {
    return;
  }
  std::wstring cur_title = fallback_title;
  if (cur_title.empty()) {
    for (const auto& n : library_online_cache_) {
      if (n.id == note_id) {
        cur_title = n.title;
        break;
      }
    }
  }
  const std::wstring online_path = OnlineNotePathFromId(note_id);
  if (cur_title.empty()) {
    cur_title = NoteDisplayName(online_path, false);
  }
  const std::wstring new_title =
      PromptText(hwnd_, L"重命名云端笔记", L"新名称：", cur_title);
  if (new_title.empty() || new_title == cur_title) {
    return;
  }

  for (auto& n : library_online_cache_) {
    if (n.id == note_id) {
      n.title = new_title;
      break;
    }
  }
  auto it_b = online_body_cache_.find(note_id);
  std::wstring body;
  if (it_b != online_body_cache_.end()) {
    it_b->second.title = new_title;
    body = it_b->second.body;
  }

  if (!path_.empty() && _wcsicmp(NormalizePathKey(path_).c_str(),
                                 NormalizePathKey(online_path).c_str()) == 0) {
    SetNativeTitleText(new_title);
    RefreshTitle();
  }
  RebuildTabControl();
  PushLibraryRailState(false);

  std::thread([hwnd = hwnd_, note_id, new_title, body]() {
    if (!VaultHttpLoggedIn()) {
      return;
    }
    std::wstring send_body = body;
    bool have_body = !send_body.empty();
    if (!have_body) {
      VaultHttpNote cur;
      if (VaultHttpGetNote(note_id, &cur)) {
        send_body = cur.body;
        have_body = true;
      }
    }
    // Without the real body, an update would overwrite the cloud note's
    // content with an empty/short placeholder (the "content cross-write" class
    // of data loss). Refuse the push and leave the cloud copy untouched.
    if (!have_body) {
      if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, kMsgCloudPushFailed, 0, 0);
      }
      return;
    }
    // On success the local row was already updated optimistically before this
    // worker started, so no callback is needed. Posting WM_APP+86 here (the
    // SyncCloudResult message) with a null payload made the handler clear
    // sync_cloud_busy_ for an unrelated in-flight sync.
    (void)VaultHttpUpdateNote(note_id, new_title, send_body, &updated);
  }).detach();
}

void NotesWindow::DeleteOnlineNote(int note_id,
                                   const std::wstring& fallback_title) {
  if (note_id <= 0) {
    return;
  }
  const std::wstring online_path = OnlineNotePathFromId(note_id);
  if (PathIsLocked(online_path)) {
    NotifyTabDeleteLocked();
    return;
  }
  std::wstring title = fallback_title;
  if (title.empty()) {
    for (const auto& n : library_online_cache_) {
      if (n.id == note_id) {
        title = n.title;
        break;
      }
    }
  }
  if (title.empty()) {
    title = NoteDisplayName(online_path, false);
  }
  if (title.empty()) {
    title = L"在线笔记";
  }

  const std::wstring prompt =
      L"确定要从云端删除笔记「" + title + L"」吗？\n删除后不可恢复。";
  if (!MistConfirm(hwnd_, L"删除云端笔记", prompt.c_str(), MistMsgTone::Danger)) {
    return;
  }

  const std::wstring norm_target = NormalizePathKey(online_path);
  const std::wstring cur_key = NormalizePathKey(path_);
  bool current_gone = false;
  if (!cur_key.empty() && _wcsicmp(norm_target.c_str(), cur_key.c_str()) == 0) {
    current_gone = true;
    dirty_ = false;
  }

  auto erase_path = [](std::vector<std::wstring>* list, const std::wstring& p) {
    const std::wstring key = NormalizePathKey(p);
    for (size_t i = 0; i < list->size(); ++i) {
      if (_wcsicmp(NormalizePathKey((*list)[i]).c_str(), key.c_str()) == 0) {
        list->erase(list->begin() + static_cast<ptrdiff_t>(i));
        return true;
      }
    }
    return false;
  };
  erase_path(&pinned_paths_, online_path);
  erase_path(&locked_paths_, online_path);

  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(), norm_target.c_str()) == 0) {
      if (i < tab_states_.size()) {
        tab_states_.erase(tab_states_.begin() + static_cast<ptrdiff_t>(i));
      }
      tab_paths_.erase(tab_paths_.begin() + static_cast<ptrdiff_t>(i));
      if (tab_index_ >= static_cast<int>(i) && tab_index_ > 0) {
        --tab_index_;
      }
      break;
    }
  }

  online_body_cache_.erase(note_id);
  cloud_sync_.links.erase(
      std::remove_if(cloud_sync_.links.begin(), cloud_sync_.links.end(),
                     [note_id](const auto& L) { return L.note_id == note_id; }),
      cloud_sync_.links.end());
  SaveNoteCloudSync(cloud_sync_);

  library_online_cache_.erase(
      std::remove_if(library_online_cache_.begin(), library_online_cache_.end(),
                     [note_id](const auto& it) { return it.id == note_id; }),
      library_online_cache_.end());

  if (current_gone) {
    if (!tab_paths_.empty()) {
      if (tab_index_ >= static_cast<int>(tab_paths_.size())) {
        tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
      }
      SwitchToTab(tab_index_, false);
    } else {
      path_.clear();
      if (edit_) {
        SetWindowTextW(edit_, L"");
      }
      SetNativeTitleText(L"");
      RefreshTitle();
      UpdatePreview();
    }
  }
  RebuildTabControl();
  PersistTabs();
  PushLibraryRailState(false);

  std::thread([hwnd = hwnd_, note_id]() {
    if (!VaultHttpLoggedIn()) {
      if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, kMsgCloudDeleteFailed, 0, 0);
      }
      return;
    }
    if (VaultHttpDeleteNote(note_id)) {
      // Local row/tab already removed; nothing to update on success. (Sending
      // WM_APP+86 here used to reset sync_cloud_busy_ for an unrelated sync.)
    } else if (hwnd && IsWindow(hwnd)) {
      // The local row/tab was already removed optimistically; the cloud copy
      // still exists, so surface the divergence and re-pull the list, otherwise
      // the note silently reappears on the next refresh.
      PostMessageW(hwnd, kMsgCloudDeleteFailed, 0, 0);
    }
  }).detach();
}

namespace {
// Worker→UI payload for「同步」(WM_APP+86). Bodies are fetched off-thread and
// applied (file writes + editor reload) on the UI thread, so a slow server
// never blocks typing and edits made while the fetch runs are respected.
struct SyncCloudBody {
  std::wstring path;
  int note_id = 0;
  std::wstring title;
  std::wstring body;
  // File identity at fetch start: if the local copy changed while the cloud
  // request was in flight, the user's edit wins and the cloud body is skipped.
  ULONGLONG local_size = 0;
  ULONGLONG local_mtime = 0;
};
struct SyncCloudResult {
  bool list_ok = false;
  std::vector<VaultHttpNote> list;
  std::vector<SyncCloudBody> bodies;
};
}  // namespace

void NotesWindow::SyncRefreshFromCloud() {
  if (!VaultHttpLoggedIn()) {
    EnsureLibraryCloudLogin();
    if (!VaultHttpLoggedIn()) {
      return;
    }
  }
  if (sync_cloud_busy_) {
    // Same latch risk as library_list_busy_: WM_APP+86 clears it, and if that
    // post is lost the button stays dead for the session.
    constexpr ULONGLONG kStaleMs = 180000;  // 3 min >> worst-case retry budget
    if (GetTickCount64() - sync_cloud_busy_since_ < kStaleMs) {
      if (status_) {
        SetWindowTextW(status_, L"同步正在进行…");
      }
      return;
    }
    sync_cloud_busy_ = false;  // stale marker — allow a fresh attempt
  }
  library_online_stale_ = true;
  // Snapshot the links: cloud_sync_ may mutate while the fetch is in flight.
  std::vector<NoteCloudLink> links;
  for (const auto& L : cloud_sync_.links) {
    if (L.sync && L.note_id > 0 && !L.path.empty()) {
      links.push_back(L);
    }
  }
  std::vector<int> online_ids;
  for (const auto& tp : tab_paths_) {
    if (IsOnlineNotePath(tp)) {
      const int oid = OnlineNoteIdFromPath(tp);
      if (oid > 0 && std::find(online_ids.begin(), online_ids.end(), oid) == online_ids.end()) {
        online_ids.push_back(oid);
      }
    }
  }
  sync_cloud_busy_ = true;
  sync_cloud_busy_since_ = GetTickCount64();
  // Only hwnd captured (no `this`); result marshalled via WM_APP+86.
  std::thread([hwnd = hwnd_, links = std::move(links), online_ids = std::move(online_ids)]() {
    auto* res = new SyncCloudResult();
    res->list_ok = VaultHttpListNotes(&res->list);
    if (res->list_ok) {
      for (const auto& L : links) {
        VaultHttpNote n;
        if (VaultHttpGetNote(L.note_id, &n) && n.id == L.note_id) {
          SyncCloudBody b;
          b.path = L.path;
          b.note_id = L.note_id;
          b.title = n.title;
          WIN32_FILE_ATTRIBUTE_DATA fad{};
          if (GetFileAttributesExW(L.path.c_str(), GetFileExInfoStandard,
                                   &fad)) {
            b.local_size = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) |
                           fad.nFileSizeLow;
            b.local_mtime =
                (static_cast<ULONGLONG>(fad.ftLastWriteTime.dwHighDateTime)
                 << 32) |
                fad.ftLastWriteTime.dwLowDateTime;
          }
          b.body = n.body.empty()
                       ? (L"# " + (n.title.empty() ? std::wstring(L"未命名")
                                                   : n.title) +
                          L"\n\n")
                       : n.body;
          res->bodies.push_back(std::move(b));
        }
      }
      for (int oid : online_ids) {
        VaultHttpNote n;
        if (VaultHttpGetNote(oid, &n)) {
          SyncCloudBody b;
          b.path = OnlineNotePathFromId(oid);
          b.note_id = oid;
          b.title = n.title;
          b.body = n.body.empty()
                       ? (L"# " + (n.title.empty() ? std::wstring(L"未命名")
                                                   : n.title) +
                          L"\n\n")
                       : n.body;
          res->bodies.push_back(std::move(b));
        }
      }
    }
    if (!PostMessageW(hwnd, WM_APP + 86, 0, reinterpret_cast<LPARAM>(res))) {
      delete res;
    }
  }).detach();
}

void NotesWindow::OpenOnlineNoteInEditor(int note_id) {
  if (note_id <= 0) {
    return;
  }
  if (!VaultHttpLoggedIn()) {
    EnsureLibraryCloudLogin();
    if (!VaultHttpLoggedIn()) {
      return;
    }
  }
  // Fetch body only on cache miss (first open). Tab switches reuse cache.
  // The fetch itself runs on the worker inside Load() (WM_APP+89), so opening
  // an online note never blocks the UI thread on a retrying HTTPS call.
  const std::wstring opath = OnlineNotePathFromId(note_id);
  for (size_t i = 0; i < tab_paths_.size(); ++i) {
    if (_wcsicmp(tab_paths_[i].c_str(), opath.c_str()) == 0) {
      SwitchToTab(static_cast<int>(i), true);
      return;
    }
  }
  if (tab_paths_.size() >= 12) {
    MistAlert(hwnd_, L"DeskNote", L"最多 12 个标签。", MistMsgTone::Info);
    return;
  }
  Save(true);
  tab_paths_.push_back(opath);
  EnsureTabStates();
  tab_states_.back() = TabState{};
  tab_states_.back().wrap = word_wrap_;
  tab_states_.back().wysiwyg = true;
  auto cit = online_body_cache_.find(note_id);
  if (cit != online_body_cache_.end() && !cit->second.title.empty()) {
    tab_states_.back().title_hint = cit->second.title;
  }
  RebuildTabControl();
  SwitchToTab(static_cast<int>(tab_paths_.size()) - 1, false);
}

void NotesWindow::NewOnlineNoteInEditor() {
  if (!VaultHttpLoggedIn()) {
    EnsureLibraryCloudLogin();
    if (!VaultHttpLoggedIn()) {
      return;
    }
  }
  const std::wstring seed = L"# 未命名\n\n";
  // This call blocks the UI thread on the network (bounded now by the tightened
  // WinHTTP timeouts). Say what is happening instead of leaving the editor
  // looking hung for up to a minute.
  const std::wstring prev_status =
      status_ ? [&] {
        const int n = GetWindowTextLengthW(status_);
        std::wstring t(static_cast<size_t>(n) + 1, L'\0');
        GetWindowTextW(status_, t.data(), n + 1);
        t.resize(static_cast<size_t>(n));
        return t;
      }()
              : std::wstring();
  if (status_) {
    SetWindowTextW(status_, L"正在创建在线笔记…");
    UpdateWindow(status_);
  }
  VaultHttpNote created;
  const bool created_ok = VaultHttpCreateNote(L"未命名", seed, &created);
  if (status_) {
    SetWindowTextW(status_, prev_status.c_str());
  }
  if (!created_ok || created.id <= 0) {
    MistAlert(hwnd_, L"在线笔记", L"新建失败，请稍后重试。",
              MistMsgTone::Danger);
    return;
  }
  // Same-file model: local .md + sync attribute.
  const std::wstring dir = JoinPath(NotesLibraryDir(true), L"云笔记");
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring path = JoinPath(dir, L"未命名.md");
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    for (int i = 1; i < 1000; ++i) {
      wchar_t suf[32];
      swprintf_s(suf, L"未命名_%02d.md", i);
      path = JoinPath(dir, suf);
      if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        break;
      }
    }
  }
  if (!AtomicWriteUtf8File(path, NarrowUtf8(seed), false)) {
    // The cloud note already exists but nothing landed on disk. Delete it again
    // rather than leaving an orphan the user can never open from the library.
    if (VaultHttpDeleteNote(created.id)) {
      MistAlert(hwnd_, L"在线笔记",
                L"写入本地文件失败，已撤销本次新建。请检查笔记库目录权限。",
                MistMsgTone::Danger);
    } else {
      MistAlert(hwnd_, L"在线笔记",
                L"写入本地文件失败，且未能撤销云端笔记，请稍后在「在线笔记」中手动删除",
                MistMsgTone::Danger);
      // Keep it visible so the user can remove it deliberately.
      LibraryOnlineItem orphan;
      orphan.id = created.id;
      orphan.title = L"未命名";
      orphan.updated_at = created.updated_at;
      library_online_cache_.insert(library_online_cache_.begin(),
                                   std::move(orphan));
      PushLibraryRailState(false);
    }
    return;
  }
  NoteCloudUpsertLink(&cloud_sync_, path, created.id, true);
  SaveNoteCloudSync(cloud_sync_);
  RememberOnlineBody(created.id, L"未命名", seed);
  LibraryOnlineItem row;
  row.id = created.id;
  row.title = L"未命名";
  row.updated_at = created.updated_at;
  library_online_cache_.insert(library_online_cache_.begin(), std::move(row));
  library_online_stale_ = false;
  OpenPaths({path});
  PushLibraryRailState(false);
}

void NotesWindow::OpenOnlineNotes(bool create_new) {
  // Unified rail: no separate list popup. Create or just show the sidebar.
  if (!sidebar_visible_) {
    ToggleSidebar();  // calls LayoutClient()
  }
  EnsureLibraryRail();
  LayoutClient();  // position/show library WebView even if sidebar was already open
  if (create_new) {
    NewOnlineNoteInEditor();
  } else {
    // Pull the online list and refresh the WebView rail.
    PushLibraryRailState(true);
  }
}

std::wstring NotesWindow::SidebarPathFromItem(HTREEITEM item) const {
  if (!sidebar_ || !item) {
    return {};
  }
  TVITEMW it{};
  it.hItem = item;
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

std::wstring NotesWindow::SidebarSelectedPath() {
  if (!sidebar_multi_.empty()) {
    return sidebar_multi_.back();
  }
  return SidebarPathFromItem(TreeView_GetSelection(sidebar_));
}

bool NotesWindow::SidebarPathInMulti(const std::wstring& path) const {
  if (path.empty()) {
    return false;
  }
  const std::wstring key = NormalizePathKey(path);
  for (const auto& p : sidebar_multi_) {
    if (_wcsicmp(NormalizePathKey(p).c_str(), key.c_str()) == 0) {
      return true;
    }
  }
  return false;
}

void NotesWindow::SidebarClearMultiSelect() {
  if (sidebar_multi_.empty() && sidebar_anchor_path_.empty()) {
    return;
  }
  sidebar_multi_.clear();
  sidebar_anchor_path_.clear();
  if (sidebar_) {
    InvalidateRect(sidebar_, nullptr, FALSE);
  }
}

std::vector<std::wstring> NotesWindow::SidebarSelectedPaths() const {
  if (!sidebar_multi_.empty()) {
    return sidebar_multi_;
  }
  std::vector<std::wstring> one;
  // const_cast-free: duplicate GetSelection path
  if (!sidebar_) {
    return one;
  }
  HTREEITEM sel = TreeView_GetSelection(sidebar_);
  if (!sel) {
    return one;
  }
  TVITEMW it{};
  it.hItem = sel;
  it.mask = TVIF_PARAM;
  if (!TreeView_GetItem(sidebar_, &it) || it.lParam == 0) {
    return one;
  }
  const size_t idx = static_cast<size_t>(it.lParam) - 1;
  if (idx < sidebar_paths_.size()) {
    one.push_back(sidebar_paths_[idx]);
  }
  return one;
}

void NotesWindow::SidebarApplyClickSelect(HTREEITEM item, bool ctrl,
                                          bool shift) {
  if (!sidebar_ || !item) {
    return;
  }
  const std::wstring path = SidebarPathFromItem(item);
  if (path.empty()) {
    // Folder / root — clear multi, keep default tree focus.
    SidebarClearMultiSelect();
    TreeView_SelectItem(sidebar_, item);
    return;
  }
  if (shift && !sidebar_anchor_path_.empty()) {
    // Range select like Explorer: file items from anchor → click (visible order).
    std::vector<std::wstring> files;
    int ia = -1;
    int ib = -1;
    for (HTREEITEM cur = TreeView_GetRoot(sidebar_); cur;
         cur = TreeView_GetNextVisible(sidebar_, cur)) {
      const std::wstring p = SidebarPathFromItem(cur);
      if (p.empty()) {
        continue;
      }
      if (_wcsicmp(NormalizePathKey(p).c_str(),
                   NormalizePathKey(sidebar_anchor_path_).c_str()) == 0) {
        ia = static_cast<int>(files.size());
      }
      if (cur == item) {
        ib = static_cast<int>(files.size());
      }
      files.push_back(p);
    }
    if (ib < 0) {
      ib = ia >= 0 ? ia : 0;
    }
    if (ia < 0) {
      ia = ib;
    }
    const int lo = (std::min)(ia, ib);
    const int hi = (std::max)(ia, ib);
    sidebar_multi_.clear();
    for (int i = lo; i <= hi && i < static_cast<int>(files.size()); ++i) {
      sidebar_multi_.push_back(files[static_cast<size_t>(i)]);
    }
    TreeView_SelectItem(sidebar_, item);
    InvalidateRect(sidebar_, nullptr, FALSE);
    return;
  }
  if (ctrl) {
    // Toggle membership (Explorer / VS Code library).
    if (SidebarPathInMulti(path)) {
      const std::wstring key = NormalizePathKey(path);
      sidebar_multi_.erase(
          std::remove_if(sidebar_multi_.begin(), sidebar_multi_.end(),
                         [&](const std::wstring& p) {
                           return _wcsicmp(NormalizePathKey(p).c_str(),
                                           key.c_str()) == 0;
                         }),
          sidebar_multi_.end());
    } else {
      sidebar_multi_.push_back(path);
    }
    if (sidebar_anchor_path_.empty()) {
      sidebar_anchor_path_ = path;
    }
    TreeView_SelectItem(sidebar_, item);
    InvalidateRect(sidebar_, nullptr, FALSE);
    return;
  }
  sidebar_multi_.clear();
  sidebar_multi_.push_back(path);
  sidebar_anchor_path_ = path;
  TreeView_SelectItem(sidebar_, item);
  InvalidateRect(sidebar_, nullptr, FALSE);
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

bool NotesWindow::PathIsLocked(const std::wstring& path) const {
  if (path.empty()) {
    return false;
  }
  // 账号库 is permanently locked (cannot unlock).
  if (IsVaultAccountsNotePath(path)) {
    return true;
  }
  const std::wstring key = NormalizePathKey(path);
  for (const auto& p : locked_paths_) {
    if (_wcsicmp(NormalizePathKey(p).c_str(), key.c_str()) == 0) {
      return true;
    }
  }
  return false;
}

void NotesWindow::ToggleLockPath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  // 账号库不准解锁（也不需要再锁一次）。
  if (IsVaultAccountsNotePath(path)) {
    NotifyAccountsTabLocked();
    return;
  }
  const std::wstring norm = NormalizePathKey(path);
  for (size_t i = 0; i < locked_paths_.size(); ++i) {
    if (_wcsicmp(NormalizePathKey(locked_paths_[i]).c_str(), norm.c_str()) ==
        0) {
      locked_paths_.erase(locked_paths_.begin() + static_cast<ptrdiff_t>(i));
      PersistTabs();
      RebuildTabControl();
      if (status_) {
        SetWindowTextW(status_, L"页签已解锁，可以删除");
      }
      return;
    }
  }
  locked_paths_.push_back(norm);
  PersistTabs();
  RebuildTabControl();
  if (status_) {
    SetWindowTextW(status_, L"页签已锁定，不可删除");
  }
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
  // 账号库 stays pinned like a system sticky tab.
  if (IsVaultAccountsNotePath(path)) {
    NotifyAccountsTabLocked();
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
      RebuildTabControl();
      return;
    }
  }
  pinned_paths_.push_back(norm);
  PersistTabs();
  ApplyPinnedTabOrder();
  RebuildSidebar();
  RebuildTabControl();
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
  // Load search prefs once per window — LoadAppPrefs() re-reads the JSON from
  // disk on every debounced keystroke otherwise.
  if (!sidebar_search_prefs_loaded_) {
    const AppPrefs prefs = LoadAppPrefs();
    notes_search_case_sensitive_ = prefs.notes_search_case_sensitive;
    notes_search_max_hits_ = prefs.notes_search_max_hits;
    notes_search_max_file_bytes_ = prefs.notes_search_max_file_bytes;
    sidebar_search_prefs_loaded_ = true;
  }
  sidebar_hits_ =
      SearchNotesLibrary(NotesLibraryDir(true), query,
                         notes_search_case_sensitive_,
                         notes_search_max_hits_,
                         notes_search_max_file_bytes_);
  for (const auto& hit : sidebar_hits_) {
    std::wstring label = NoteDisplayName(hit.path, true);
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
  if (!PathLooksMarkdown()) {
    if (wysiwyg_) {
      wysiwyg_ = false;
      ApplyWysiwygVisibility();
      LayoutClient();
    }
    if (preview_ && preview_web_.Host()) {
      preview_web_.Hide();
    }
    pending_goto_line_ = hit.line;
    PostMessageW(hwnd_, WM_APP + 60, 0, 0);
  }
}

void NotesWindow::RebuildSidebar() {
  if (!sidebar_) {
    return;
  }
  // Ensure Explorer-visible names match UI before we rebuild labels.
  HealUntitledDraftsOnDisk();
  // Freeze paint while wiping/rebuilding — TreeView per-item redraw is the
  // main hitch after delete / pin / rename (Explorer-class notes rails).
  SendMessageW(sidebar_, WM_SETREDRAW, FALSE, 0);
  // DeleteAllItems invalidates every HTREEITEM; a drag still holding one would
  // use freed handles. Drop the gesture outright here (rather than finishing it)
  // because finishing would call ApplySidebarDrop → RebuildSidebar again.
  if (sidebar_drag_item_ || sidebar_drag_target_) {
    sidebar_drag_item_ = nullptr;
    sidebar_drag_target_ = nullptr;
    if (GetCapture() == hwnd_) {
      ReleaseCapture();
    }
  }
  TreeView_DeleteAllItems(sidebar_);
  sidebar_paths_.clear();
  sidebar_multi_.clear();
  sidebar_anchor_path_.clear();
  const std::wstring root = NotesLibraryDir(true);
  CreateDirectoryW(root.c_str(), nullptr);

  // Precompute the pinned set once: PathIsPinned() re-normalizes every pinned
  // path per call, and the walk below asks once per file. Rebuilding a tree of
  // N notes with P pins cost N×P mutex-guarded path normalizations.
  std::set<std::wstring> pinned_keys;
  for (const auto& p : pinned_paths_) {
    pinned_keys.insert(NormalizePathKey(p));
  }
  auto is_pinned_fast = [&pinned_keys](const std::wstring& full) {
    return !pinned_keys.empty() &&
           pinned_keys.count(NormalizePathKey(full)) != 0;
  };

  bool sidebar_healed = false;
  auto retarget_after_heal = [&](const std::wstring& old_path,
                                 const std::wstring& new_path) {
    if (old_path.empty() || new_path.empty() ||
        _wcsicmp(NormalizePathKey(old_path).c_str(),
                 NormalizePathKey(new_path).c_str()) == 0) {
      return;
    }
    RetargetNotePath(old_path, new_path);
    sidebar_healed = true;
  };

  auto insert = [&](HTREEITEM parent, const std::wstring& full, bool is_dir,
                    const std::wstring& display) {
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    std::wstring label = display;
    if (label.empty()) {
      const size_t slash = full.find_last_of(L"\\/");
      label = slash == std::wstring::npos ? full : full.substr(slash + 1);
    }
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
            std::wstring full = dir + L"\\" + fd.cFileName;
            // DeskNote does not list 账号库.md (DeskTidy vault UI owns it).
            if (IsVaultAccountsNotePath(full)) {
              continue;
            }
            const std::wstring old_full = full;
            if (HealEnglishUntitledPath(&full)) {
              retarget_after_heal(old_full, full);
            }
            // Match window title / tab: document title → localized stem.
            Entry e{NoteDisplayName(full, true), full, false, false, 0};
            e.pinned = is_pinned_fast(e.full);
            ULARGE_INTEGER u{};
            u.LowPart = fd.ftCreationTime.dwLowDateTime;
            u.HighPart = fd.ftCreationTime.dwHighDateTime;
            e.ctime = u.QuadPart;
            // Lead pad for thumbtack paint (same glyph as tab chips — no ★).
            if (e.pinned) {
              e.name = L"   " + e.name;
            }
            entries.push_back(std::move(e));
          }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        // Dirs first; then files honour the user's drag order (sidebar_order_),
        // with any unlisted file falling back to pinned → newest → name.
        const auto ord_it = sidebar_order_.find(NormalizePathKey(dir));
        const std::vector<std::wstring>* order =
            (ord_it == sidebar_order_.end()) ? nullptr : &ord_it->second;
        auto rank_of = [order](const std::wstring& full) -> size_t {
          if (!order) {
            return (std::numeric_limits<size_t>::max)();
          }
          const std::wstring key = NormalizePathKey(full);
          for (size_t i = 0; i < order->size(); ++i) {
            if (_wcsicmp((*order)[i].c_str(), key.c_str()) == 0) {
              return i;
            }
          }
          return (std::numeric_limits<size_t>::max)();
        };
        std::sort(entries.begin(), entries.end(),
                  [&](const Entry& a, const Entry& b) {
                    if (a.dir != b.dir) {
                      return a.dir;
                    }
                    if (order) {
                      const size_t ra = rank_of(a.full);
                      const size_t rb = rank_of(b.full);
                      if (ra != rb) {
                        return ra < rb;
                      }
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
          HTREEITEM it = insert(parent, e.full, e.dir, e.name);
          if (e.dir && depth < 2) {
            walk(e.full, it, depth + 1);
          }
        }
      };

  HTREEITEM root_item = insert(TVI_ROOT, root, true, L"");
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
  SendMessageW(sidebar_, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(sidebar_, nullptr, nullptr,
               RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
  if (sidebar_healed) {
    SchedulePersistTabs();
    RebuildTabControl();
    RefreshTitle();
  }
  if (!path_.empty()) {
    HighlightSidebarPath(path_);
  }
  EnsureLibraryRail();
  // Local tree changed; keep online cache unless marked stale (refresh/login).
  PushLibraryRailState(false);
}

// --- Library tree drag-to-order -------------------------------------------
// TVN_BEGINDRAG starts a modal-ish drag: we own the loop with SetCapture and
// move the dragged item to the hovered sibling on each WM_MOUSEMOVE. Order is
// persisted per parent folder in sidebar_order_ so it survives rebuilds.

void NotesWindow::StartSidebarDrag(HTREEITEM item) {
  if (!item || !sidebar_) {
    return;
  }
  // Only files reorder — moving a folder under a file would break the tree.
  TVITEMW ti{};
  ti.mask = TVIF_HANDLE | TVIF_PARAM;
  ti.hItem = item;
  if (!TreeView_GetItem(sidebar_, &ti) || ti.lParam == 0) {
    return;
  }
  sidebar_drag_item_ = item;
  SetCapture(hwnd_);
  SetCursor(LoadCursor(nullptr, IDC_SIZENS));
}

void NotesWindow::MoveSidebarItem(HTREEITEM drag_item, HTREEITEM target_item,
                                  bool after) {
  if (!sidebar_ || !drag_item || !target_item || drag_item == target_item) {
    return;
  }
  // Siblings only — the order list is keyed by the parent folder, and moving a
  // file into another folder is a file-system operation we do not do here.
  if (TreeView_GetParent(sidebar_, drag_item) !=
      TreeView_GetParent(sidebar_, target_item)) {
    return;
  }
  TVITEMW dt{};
  dt.mask = TVIF_HANDLE | TVIF_PARAM;
  dt.hItem = drag_item;
  if (!TreeView_GetItem(sidebar_, &dt) || dt.lParam == 0) {
    return;
  }
  TVITEMW tt{};
  tt.mask = TVIF_HANDLE | TVIF_PARAM;
  tt.hItem = target_item;
  if (!TreeView_GetItem(sidebar_, &tt) || tt.lParam == 0) {
    return;
  }
  // Only record the intent here. Re-inserting the item on every WM_MOUSEMOVE
  // destroyed and recreated the node under the cursor (flicker, and the drag
  // item handle went stale mid-gesture). The reorder is applied once on drop.
  sidebar_drag_target_ = target_item;
  sidebar_drag_after_ = after;
}

// Apply the recorded drop: rebuild the sibling order, persist it, and reorder
// the visible items without a full RebuildSidebar (which would also walk disk).
void NotesWindow::ApplySidebarDrop() {
  HTREEITEM drag_item = sidebar_drag_item_;
  HTREEITEM target_item = sidebar_drag_target_;
  const bool after = sidebar_drag_after_;
  sidebar_drag_item_ = nullptr;
  sidebar_drag_target_ = nullptr;
  if (!sidebar_ || !drag_item || !target_item || drag_item == target_item) {
    return;
  }
  const HTREEITEM drag_parent = TreeView_GetParent(sidebar_, drag_item);
  if (drag_parent != TreeView_GetParent(sidebar_, target_item)) {
    return;
  }
  TVITEMW dt{};
  dt.mask = TVIF_HANDLE | TVIF_PARAM;
  dt.hItem = drag_item;
  if (!TreeView_GetItem(sidebar_, &dt) || dt.lParam == 0) {
    return;
  }
  const size_t drag_idx = static_cast<size_t>(dt.lParam) - 1;
  if (drag_idx >= sidebar_paths_.size()) {
    return;
  }
  const std::wstring drag_path = sidebar_paths_[drag_idx];
  const size_t ds = drag_path.find_last_of(L"\\/");
  const std::wstring parent_dir = ds == std::wstring::npos
                                      ? NotesLibraryDir(true)
                                      : drag_path.substr(0, ds);

  // Rebuild the ordering list from what is on screen, then move the drag entry
  // next to the drop target.
  std::vector<std::wstring> keys;
  for (HTREEITEM c = TreeView_GetChild(sidebar_, drag_parent); c;
       c = TreeView_GetNextSibling(sidebar_, c)) {
    TVITEMW ct{};
    ct.mask = TVIF_HANDLE | TVIF_PARAM;
    ct.hItem = c;
    if (TreeView_GetItem(sidebar_, &ct) && ct.lParam > 0) {
      const size_t idx = static_cast<size_t>(ct.lParam) - 1;
      if (idx < sidebar_paths_.size()) {
        keys.push_back(NormalizePathKey(sidebar_paths_[idx]));
      }
    }
  }
  const std::wstring drag_key = NormalizePathKey(drag_path);
  keys.erase(std::remove(keys.begin(), keys.end(), drag_key), keys.end());

  TVITEMW tt{};
  tt.mask = TVIF_HANDLE | TVIF_PARAM;
  tt.hItem = target_item;
  if (TreeView_GetItem(sidebar_, &tt) && tt.lParam > 0) {
    const size_t ti = static_cast<size_t>(tt.lParam) - 1;
    if (ti < sidebar_paths_.size()) {
      const std::wstring tgt_key = NormalizePathKey(sidebar_paths_[ti]);
      auto pos = std::find(keys.begin(), keys.end(), tgt_key);
      if (pos == keys.end()) {
        keys.push_back(drag_key);
      } else {
        keys.insert(after ? std::next(pos) : pos, drag_key);
      }
    }
  }
  sidebar_order_[NormalizePathKey(parent_dir)] = keys;
  PersistSidebarOrder();
  // Re-sort the tree once; cheap because it reuses the already-built rows.
  RebuildSidebar();
}

void NotesWindow::EndSidebarDrag() {
  if (GetCapture() == hwnd_) {
    ReleaseCapture();
  }
  // Pending drop is applied only if the gesture actually moved somewhere.
  if (sidebar_drag_item_ && sidebar_drag_target_) {
    ApplySidebarDrop();
  }
  sidebar_drag_item_ = nullptr;
  sidebar_drag_target_ = nullptr;
}

void NotesWindow::PersistSidebarOrder() {
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_sidebar_order.json");
  std::ostringstream out;
  out << "{\"dirs\":{";
  bool first_dir = true;
  for (const auto& kv : sidebar_order_) {
    if (kv.second.empty()) {
      continue;
    }
    out << (first_dir ? "\n  " : ",\n  ");
    out << "\"" << EscapeJson(NarrowUtf8(kv.first)) << "\":[";
    bool first_item = true;
    for (const auto& p : kv.second) {
      out << (first_item ? "\n    " : ",\n    ");
      out << "\"" << EscapeJson(NarrowUtf8(p)) << "\"";
      first_item = false;
    }
    out << "\n  ]";
    first_dir = false;
  }
  out << "\n}}\n";
  AtomicWriteUtf8File(file, out.str(), false);
}

void NotesWindow::LoadSidebarOrder() {
  sidebar_order_.clear();
  const std::wstring file = JoinPath(NativeDataDir(), L"notes_sidebar_order.json");
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string raw = ss.str();
  // Minimal parser: {"dirs":{"<dir>":["<file>",...], ...}}
  size_t pos = raw.find("\"dirs\"");
  if (pos == std::string::npos) {
    return;
  }
  pos = raw.find('{', pos);
  if (pos == std::string::npos) {
    return;
  }
  // Match the closing brace by depth: a plain find('}') stops at the first
  // nested object's brace and truncates the parse. String literals are
  // skipped so a '}' inside a path cannot unbalance the count.
  size_t end = std::string::npos;
  {
    int depth = 0;
    bool in_str = false;
    for (size_t k = pos; k < raw.size(); ++k) {
      const char c = raw[k];
      if (in_str) {
        if (c == '\\') {
          ++k;  // skip escaped char
        } else if (c == '"') {
          in_str = false;
        }
        continue;
      }
      if (c == '"') {
        in_str = true;
      } else if (c == '{') {
        ++depth;
      } else if (c == '}') {
        if (--depth == 0) {
          end = k;
          break;
        }
      }
    }
  }
  if (end == std::string::npos) {
    return;
  }
  size_t i = pos + 1;
  while (i < end) {
    const size_t kq = raw.find('"', i);
    if (kq == std::string::npos || kq >= end) {
      break;
    }
    const size_t kq2 = raw.find('"', kq + 1);
    if (kq2 == std::string::npos) {
      break;
    }
    const std::string dir_u8 = raw.substr(kq + 1, kq2 - kq - 1);
    const size_t arr_open = raw.find('[', kq2);
    if (arr_open == std::string::npos || arr_open > end) {
      break;  // no array for this key — stop rather than search past the end
    }
    const size_t arr_close = raw.find(']', arr_open);
    if (arr_close == std::string::npos || arr_close > end) {
      break;
    }
    std::vector<std::wstring> items;
    size_t j = arr_open + 1;
    while (j < arr_close) {
      const size_t q1 = raw.find('"', j);
      if (q1 == std::string::npos || q1 >= arr_close) {
        break;
      }
      const size_t q2 = raw.find('"', q1 + 1);
      if (q2 == std::string::npos) {
        break;
      }
      items.push_back(NormalizePathKey(
          WidenUtf8(raw.substr(q1 + 1, q2 - q1 - 1))));
      j = q2 + 1;
    }
    if (!items.empty()) {
      sidebar_order_[NormalizePathKey(WidenUtf8(dir_u8))] = std::move(items);
    }
    i = arr_close + 1;
  }
}

void NotesWindow::SidebarUpsertFile(const std::wstring& path) {
  if (!sidebar_ || path.empty()) {
    return;
  }
  const std::wstring key = NormalizePathKey(path);
  // Already present → refresh label + select (common after close-last).
  HTREEITEM found = nullptr;
  std::function<void(HTREEITEM)> walk = [&](HTREEITEM item) {
    while (item && !found) {
      TVITEMW ti{};
      ti.mask = TVIF_HANDLE | TVIF_PARAM;
      ti.hItem = item;
      if (TreeView_GetItem(sidebar_, &ti) && ti.lParam > 0) {
        const size_t idx = static_cast<size_t>(ti.lParam) - 1;
        if (idx < sidebar_paths_.size() &&
            _wcsicmp(NormalizePathKey(sidebar_paths_[idx]).c_str(),
                     key.c_str()) == 0) {
          found = item;
          return;
        }
      }
      if (HTREEITEM child = TreeView_GetChild(sidebar_, item)) {
        walk(child);
      }
      item = TreeView_GetNextSibling(sidebar_, item);
    }
  };
  walk(TreeView_GetRoot(sidebar_));
  std::wstring label = NoteDisplayName(path, false);
  if (PathIsPinned(path)) {
    label = L"   " + label;
  }
  if (found) {
    TVITEMW set{};
    set.mask = TVIF_HANDLE | TVIF_TEXT;
    set.hItem = found;
    set.pszText = label.data();
    TreeView_SetItem(sidebar_, &set);
    HighlightSidebarPath(path);
    return;
  }
  HTREEITEM root = TreeView_GetRoot(sidebar_);
  if (!root) {
    return;
  }
  sidebar_paths_.push_back(path);
  TVINSERTSTRUCTW ins{};
  ins.hParent = root;
  ins.hInsertAfter = TVI_FIRST;  // newest draft on top (ctime sort parity)
  ins.item.mask = TVIF_TEXT | TVIF_PARAM;
  ins.item.pszText = label.data();
  ins.item.lParam = static_cast<LPARAM>(sidebar_paths_.size());
  HTREEITEM it = TreeView_InsertItem(sidebar_, &ins);
  if (it) {
    TreeView_Expand(sidebar_, root, TVE_EXPAND);
    HighlightSidebarPath(path);
  }
}

void NotesWindow::SidebarRemovePaths(const std::vector<std::wstring>& paths) {
  if (paths.empty()) {
    return;
  }
  std::vector<std::wstring> keys;
  keys.reserve(paths.size());
  for (const auto& p : paths) {
    if (!p.empty()) {
      keys.push_back(NormalizePathKey(p));
    }
  }
  if (keys.empty()) {
    return;
  }
  auto matches = [&](const std::wstring& full) {
    if (full.empty()) {
      return false;
    }
    const std::wstring key = NormalizePathKey(full);
    for (const auto& k : keys) {
      if (_wcsicmp(key.c_str(), k.c_str()) == 0) {
        return true;
      }
    }
    return false;
  };
  // Always clear path slots first — WebView rail may leave the TreeView stale
  // or hidden; early-return on empty doomed used to keep deleted rows visible.
  for (auto& slot : sidebar_paths_) {
    if (!slot.empty() && matches(slot)) {
      slot.clear();
    }
  }
  for (auto it = sidebar_multi_.begin(); it != sidebar_multi_.end();) {
    if (matches(*it)) {
      it = sidebar_multi_.erase(it);
    } else {
      ++it;
    }
  }
  if (!sidebar_anchor_path_.empty() && matches(sidebar_anchor_path_)) {
    sidebar_anchor_path_.clear();
  }
  if (!sidebar_) {
    return;
  }
  std::vector<HTREEITEM> doomed;
  std::function<void(HTREEITEM)> walk = [&](HTREEITEM item) {
    while (item) {
      const HTREEITEM next = TreeView_GetNextSibling(sidebar_, item);
      if (HTREEITEM child = TreeView_GetChild(sidebar_, item)) {
        walk(child);
      }
      TVITEMW ti{};
      ti.mask = TVIF_HANDLE | TVIF_PARAM;
      ti.hItem = item;
      if (TreeView_GetItem(sidebar_, &ti) && ti.lParam > 0) {
        const size_t idx = static_cast<size_t>(ti.lParam) - 1;
        if (idx < sidebar_paths_.size() &&
            (sidebar_paths_[idx].empty() || matches(sidebar_paths_[idx]))) {
          doomed.push_back(item);
        }
      }
      item = next;
    }
  };
  walk(TreeView_GetRoot(sidebar_));
  if (doomed.empty()) {
    return;
  }
  SendMessageW(sidebar_, WM_SETREDRAW, FALSE, 0);
  for (HTREEITEM it : doomed) {
    TreeView_DeleteItem(sidebar_, it);
  }
  SendMessageW(sidebar_, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(sidebar_, nullptr, nullptr,
               RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
                                     const std::wstring& new_stem_raw,
                                     bool quiet) {
  if (path.empty()) {
    return false;
  }
  // Never rename the system hosts file (or any hosts basename) — content lines
  // must not become the filename.
  if (IsHostsPath(path)) {
    if (!quiet) {
      MistAlert(hwnd_, L"重命名", L"系统 hosts 文件不能改名。", MistMsgTone::Warn);
    }
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
  // Sanitize like SyncFilenameToTitle (inline rename can paste illegal chars).
  {
    std::wstring clean = SanitizeNoteFileStem(new_stem);
    if (!clean.empty()) {
      new_stem = std::move(clean);
    }
  }
  if (new_stem.empty() || new_stem == stem) {
    return false;
  }
  // Disallow path separators in the stem.
  if (new_stem.find_first_of(L"\\/") != std::wstring::npos) {
    if (!quiet) {
      MistAlert(hwnd_, L"重命名", L"名称不能包含路径分隔符。", MistMsgTone::Warn);
    }
    return false;
  }
  const std::wstring new_path = dir + L"\\" + new_stem + suffix;
  if (GetFileAttributesW(new_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    if (!quiet) {
      MistAlert(hwnd_, L"重命名", L"该名称已存在。", MistMsgTone::Warn);
    }
    return false;
  }
  if (!TryRenameNoteFile(path, new_path)) {
    if (!quiet) {
      MistAlert(hwnd_, L"重命名", L"无法重命名。", MistMsgTone::Warn);
    }
    return false;
  }
  InvalidateNotesFileCache();
  NotifyShellFileRenamed(path, new_path);
  RetargetNotePath(path, new_path);
  if (quiet) {
    // Keep focus in title field while typing (no full tree/tab rebuild).
    RefreshSidebarLabelForPath(new_path);
    RefreshCurrentTabLabel();
    RefreshTitle();
    UpdateStatusBar();
    if (tabs_) {
      InvalidateRect(tabs_, nullptr, FALSE);
    }
    // Filename changed — refresh left rail titles/paths (TreeView is hidden).
    if (library_web_.Host() && IsWindowVisible(library_web_.Host())) {
      PushLibraryRailState(false);
    }
    return true;
  }
  RebuildSidebar();
  RebuildTabControl();
  PersistTabs();
  UpdateStatusBar();
  RefreshTitle();
  return true;
}

void NotesWindow::DeleteSidebarSelected() {
  std::vector<std::wstring> paths = SidebarSelectedPaths();
  if (paths.empty()) {
    return;
  }
  // Dedupe (Ctrl toggle can leave duplicates if paths differ only by slash).
  {
    std::vector<std::wstring> uniq;
    for (const auto& p : paths) {
      bool seen = false;
      for (const auto& u : uniq) {
        if (_wcsicmp(NormalizePathKey(u).c_str(),
                     NormalizePathKey(p).c_str()) == 0) {
          seen = true;
          break;
        }
      }
      if (!seen) {
        uniq.push_back(p);
      }
    }
    paths = std::move(uniq);
  }
  // 账号库 / user-locked notes cannot be recycled from the library rail.
  {
    std::vector<std::wstring> allowed;
    allowed.reserve(paths.size());
    bool blocked_vault = false;
    bool blocked_lock = false;
    for (const auto& p : paths) {
      if (IsVaultAccountsNotePath(p)) {
        blocked_vault = true;
        continue;
      }
      if (PathIsLocked(p)) {
        blocked_lock = true;
        continue;
      }
      allowed.push_back(p);
    }
    if (blocked_vault && allowed.empty()) {
      NotifyAccountsTabLocked();
      return;
    }
    if (blocked_lock && allowed.empty()) {
      NotifyTabDeleteLocked();
      return;
    }
    if (blocked_vault) {
      NotifyAccountsTabLocked();
    } else if (blocked_lock) {
      NotifyTabDeleteLocked();
    }
    paths = std::move(allowed);
  }
  if (paths.empty()) {
    return;
  }
  std::wstring prompt;
  if (paths.size() == 1) {
    prompt = L"删除此文件？将移入回收站。";
  } else {
    prompt = L"删除选中的 " + std::to_wstring(paths.size()) +
             L" 个文件？将移入回收站。";
  }
  if (!MistConfirm(hwnd_, L"删除", prompt.c_str(), MistMsgTone::Danger) ||
      !RecyclePathsQuiet(hwnd_, paths)) {
    return;
  }
  const std::wstring cur_key = NormalizePathKey(path_);
  bool current_gone = false;
  auto erase_path = [](std::vector<std::wstring>* list, const std::wstring& path) {
    const std::wstring key = NormalizePathKey(path);
    for (size_t i = 0; i < list->size(); ++i) {
      if (_wcsicmp(NormalizePathKey((*list)[i]).c_str(), key.c_str()) == 0) {
        list->erase(list->begin() + static_cast<ptrdiff_t>(i));
        return true;
      }
    }
    return false;
  };
  for (const auto& path : paths) {
    const std::wstring key = NormalizePathKey(path);
    if (!cur_key.empty() && _wcsicmp(key.c_str(), cur_key.c_str()) == 0) {
      current_gone = true;
      dirty_ = false;
    }
    erase_path(&pinned_paths_, path);
    erase_path(&locked_paths_, path);
    for (size_t i = 0; i < tab_paths_.size(); ++i) {
      if (_wcsicmp(NormalizePathKey(tab_paths_[i]).c_str(), key.c_str()) == 0) {
        if (i < tab_states_.size()) {
          tab_states_.erase(tab_states_.begin() + static_cast<ptrdiff_t>(i));
        }
        tab_paths_.erase(tab_paths_.begin() + static_cast<ptrdiff_t>(i));
        if (tab_index_ >= static_cast<int>(i) && tab_index_ > 0) {
          --tab_index_;
        }
        break;
      }
    }
  }
  bool need_switch = current_gone;
  const bool seeded_blank = tab_paths_.empty();
  if (seeded_blank) {
    if (!SeedBlankDraftTab()) {
      return;  // nothing valid to switch to; the alert already explained why
    }
    need_switch = true;
  } else if (tab_index_ >= static_cast<int>(tab_paths_.size())) {
    tab_index_ = static_cast<int>(tab_paths_.size()) - 1;
    need_switch = true;
  }
  SidebarClearMultiSelect();
  if (seeded_blank) {
    ActivateSeededBlankDraft(paths.size() == 1 ? paths.front() : L"");
    if (paths.size() > 1) {
      // Multi-delete emptied tabs: drop remaining recycled rows then upsert.
      SidebarRemovePaths(paths);
      SidebarUpsertFile(path_);
    }
    // Rescan disk so WebView rail does not keep cleared "" ghost rows.
    if (sidebar_visible_) {
      RebuildSidebar();
    }
  } else if (need_switch) {
    SidebarRemovePaths(paths);
    // rebuild_tree: library WebView reads sidebar_paths_ — must rescan disk.
    SyncTabsAndSidebar(true, sidebar_visible_);
  } else {
    SidebarRemovePaths(paths);
    tab_chips_dirty_ = true;
    RebuildTabControl();
    if (sidebar_visible_) {
      RebuildSidebar();
    } else if (!path_.empty()) {
      HighlightSidebarPath(path_);
    }
    SchedulePersistTabs();
  }
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
    const std::wstring hit_path = SidebarPathFromItem(hit);
    // Right-click outside the multi-set replaces selection (Explorer-like).
    if (!hit_path.empty() && !SidebarPathInMulti(hit_path)) {
      sidebar_multi_.clear();
      sidebar_multi_.push_back(hit_path);
      sidebar_anchor_path_ = hit_path;
    }
    TreeView_SelectItem(sidebar_, hit);
    InvalidateRect(sidebar_, nullptr, FALSE);
  }
  const auto paths = SidebarSelectedPaths();
  const std::wstring path = paths.empty() ? L"" : paths.front();
  const bool is_file = !path.empty();
  const bool multi = paths.size() > 1;

  // Keep pin / rename / delete / reveal only — open / new / copy path / refresh
  // duplicate the sidebar + toolbar (Typora / VS Code file-tree menus).
  const bool vault_locked =
      is_file && !multi && IsVaultAccountsNotePath(path);
  bool any_deletable = false;
  for (const auto& p : paths) {
    if (!PathIsLocked(p)) {
      any_deletable = true;
      break;
    }
  }
  HMENU m = CreatePopupMenu();
  if (is_file && !multi && !vault_locked) {
    AppendMenuW(m, MF_STRING, kSidePin,
                PathIsPinned(path) ? L"取消置顶" : L"置顶");
    AppendMenuW(m, MF_STRING, kTabLock,
                PathIsLocked(path) ? L"解锁" : L"锁定");
  }
  AppendMenuW(m, MF_STRING | (is_file && !multi && !vault_locked ? MF_ENABLED
                                                                : MF_GRAYED),
              kSideRename, L"重命名\tF2");
  if (is_file && !multi) {
    HMENU exp = CreatePopupMenu();
    AppendMenuW(exp, MF_STRING, kSideExportWord, L"Word 文档 (*.docx)…");
    AppendMenuW(exp, MF_STRING, kSideExportPdf, L"PDF 文档 (*.pdf)…");
    AppendMenuW(exp, MF_STRING, kSideExportHtml, L"HTML 网页 (*.html)…");
    AppendMenuW(exp, MF_STRING, kSideExportMd, L"Markdown (含图片) (*.md)…");
    AppendMenuW(exp, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(exp, MF_STRING, kSideExport, L"更多导出选项…");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(exp), L"导出");
  }
  // 锁定/账号库不出现「删除」；多选里若还有可删项则保留入口。
  if (is_file && any_deletable) {
    std::wstring del_label = L"删除\tDelete";
    if (multi) {
      del_label = L"删除 " + std::to_wstring(paths.size()) + L" 项\tDelete";
    }
    AppendMenuW(m, MF_STRING, kSideDelete, del_label.c_str());
  }
  AppendMenuW(m, MF_STRING | (is_file && !multi ? MF_ENABLED : MF_GRAYED),
              kSideReveal, L"在资源管理器中显示");
  const int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                                 hwnd_, nullptr);
  DestroyMenu(m);

  if (cmd == kSidePin) {
    TogglePinPath(path);
  } else if (cmd == kTabLock) {
    ToggleLockPath(path);
  } else if (cmd == kSideRename) {
    RenameSidebarSelected();
  } else if (cmd == kSideExportWord) {
    ExportWord(path);
  } else if (cmd == kSideExportPdf) {
    ExportPdf(path);
  } else if (cmd == kSideExportHtml) {
    ExportHtml(path);
  } else if (cmd == kSideExportMd) {
    ExportMarkdown(path);
  } else if (cmd == kSideExport) {
    ExportNoteWithAssets(path);
  } else if (cmd == kSideDelete) {
    DeleteSidebarSelected();
  } else if (cmd == kSideReveal) {
    RevealPathInExplorer(path);
  }
}

LRESULT CALLBACK NotesWindow::SidebarProc(HWND hwnd, UINT msg, WPARAM wparam,
                                          LPARAM lparam) {
  auto* self = reinterpret_cast<NotesWindow*>(
      GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (self && msg == WM_DROPFILES) {
    return SendMessageW(self->hwnd_, WM_DROPFILES, wparam, lparam);
  }
  if (self && msg == WM_LBUTTONDOWN) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl || shift) {
      TVHITTESTINFO ht{};
      ht.pt.x = GET_X_LPARAM(lparam);
      ht.pt.y = GET_Y_LPARAM(lparam);
      HTREEITEM hit = TreeView_HitTest(hwnd, &ht);
      if (hit && (ht.flags & (TVHT_ONITEM | TVHT_ONITEMLABEL |
                              TVHT_ONITEMICON | TVHT_ONITEMSTATEICON |
                              TVHT_ONITEMINDENT | TVHT_ONITEMBUTTON))) {
        // Still expand/collapse on the +/- glyph without multi-select churn.
        if (ht.flags & TVHT_ONITEMBUTTON) {
          /* fall through to default */
        } else {
          self->SidebarApplyClickSelect(hit, ctrl, shift);
          SetFocus(hwnd);
          return 0;
        }
      }
    } else {
      TVHITTESTINFO ht{};
      ht.pt.x = GET_X_LPARAM(lparam);
      ht.pt.y = GET_Y_LPARAM(lparam);
      HTREEITEM hit = TreeView_HitTest(hwnd, &ht);
      if (hit && !(ht.flags & TVHT_ONITEMBUTTON)) {
        self->SidebarApplyClickSelect(hit, false, false);
        // Single-click file → switch/open tab (Notepad++ / Typora file tree).
        // Folders keep expand/collapse via default proc; multi-select skips open.
        const std::wstring path = self->SidebarPathFromItem(hit);
        if (!path.empty()) {
          self->OpenOrActivatePath(path);
        }
      } else if (!hit) {
        self->SidebarClearMultiSelect();
      }
    }
  }
  if (self && msg == WM_KEYDOWN) {
    if (wparam == VK_F2) {
      self->RenameSidebarSelected();
      return 0;
    }
    if (wparam == VK_DELETE) {
      self->DeleteSidebarSelected();
      return 0;
    }
    if (wparam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
      // Ctrl+A — select all files under the library root.
      self->sidebar_multi_.clear();
      for (const auto& p : self->sidebar_paths_) {
        self->sidebar_multi_.push_back(p);
      }
      if (!self->sidebar_multi_.empty()) {
        self->sidebar_anchor_path_ = self->sidebar_multi_.front();
      }
      InvalidateRect(hwnd, nullptr, FALSE);
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
    case WM_DROPFILES:
      return SendMessageW(self->hwnd_, WM_DROPFILES, wparam, lparam);
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
      self->DismissLibraryMenu();
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
          const std::wstring& tp =
              self->tab_paths_[static_cast<size_t>(hit)];
          if (self->PathIsLocked(tp)) {
            // Lock glyph: unlock (账号库 refuses inside ToggleLockPath).
            self->ToggleLockPath(tp);
          } else {
            self->CloseTabAt(hit);
          }
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
        } else {
          self->FocusActiveEditor(true);
        }
      }
      self->tab_drag_from_ = hit;
      self->tab_dragging_ = false;
      self->tab_drag_origin_ = pt;
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (self->sidebar_drag_item_ && (wparam & MK_LBUTTON)) {
        // Screen coords (we hold capture): hit-test the tree under the cursor.
        POINT sp = pt;
        ClientToScreen(hwnd, &sp);
        POINT cp = sp;
        ScreenToClient(self->sidebar_, &cp);
        TVHITTESTINFO ht{};
        ht.pt = cp;
        TreeView_HitTest(self->sidebar_, &ht);
        if (ht.hItem && (ht.flags & TVHT_ONITEM) &&
            ht.hItem != self->sidebar_drag_item_) {
          RECT ir{};
          TreeView_GetItemRect(self->sidebar_, ht.hItem, &ir, TRUE);
          const bool after = cp.y > (ir.top + ir.bottom) / 2;
          self->MoveSidebarItem(self->sidebar_drag_item_, ht.hItem, after);
        }
        return 0;
      }
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
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        // Set once here — doing HAND/ARROW in WM_MOUSEMOVE shifts the hotspot
        // and can bounce hit-test zones forever (tab flicker).
        const bool hand = self->tab_hover_zone_ == 1 ||
                          self->tab_hover_zone_ == 2 ||
                          self->tab_hover_index_ == -2;
        SetCursor(LoadCursor(nullptr, hand ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_MOUSELEAVE:
      if (self->tab_hover_index_ != -1 || self->tab_hover_zone_ != 0) {
        self->tab_hover_index_ = -1;
        self->tab_hover_zone_ = 0;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
      if (self->sidebar_drag_item_) {
        self->EndSidebarDrag();
      }
      self->tab_drag_from_ = -1;
      self->tab_dragging_ = false;
      return 0;
    case WM_CAPTURECHANGED:
      // Capture lost mid-drag (Alt+Tab, focus steal) — never leave the tree
      // stuck in drag state.
      if (self->sidebar_drag_item_) {
        self->EndSidebarDrag();
      }
      break;
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
      // Empty bar: new tab. Chip body: rename (× / middle-click still close).
      // Pin/close hit plates: ignore dblclk (single-click already handles them).
      if (hit < 0) {
        self->NewTab();
      } else if (zone == 0) {
        self->RenameTabAt(hit);
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

LRESULT CALLBACK NotesWindow::TitleEditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                            LPARAM lparam) {
  auto* self = reinterpret_cast<NotesWindow*>(
      GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (self && (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
               msg == WM_SETFOCUS)) {
    self->DismissLibraryMenu();
  }
  if (self && msg == WM_KEYDOWN &&
      (wparam == VK_RETURN || wparam == VK_DOWN)) {
    // Typora: Enter / Down from title → body.
    if (self->wysiwyg_ && self->web_.ContentReady() && self->web_.Host()) {
      SetFocus(self->web_.Host());
      self->web_.EvalScript(
          L"try{var e=document.querySelector('.ProseMirror');"
          L"if(e)e.focus()}catch(ex){}");
    } else if (self->edit_) {
      SetFocus(self->edit_);
    }
    return 0;
  }
  // Cue banner is flaky under WebView2 sibling paint — draw placeholder ourselves.
  if (self && msg == WM_PAINT) {
    const LRESULT r =
        self->title_edit_proc_
            ? CallWindowProcW(self->title_edit_proc_, hwnd, msg, wparam, lparam)
            : DefWindowProcW(hwnd, msg, wparam, lparam);
    if (GetWindowTextLengthW(hwnd) == 0 && GetFocus() != hwnd) {
      HDC hdc = GetDC(hwnd);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      SetBkMode(hdc, TRANSPARENT);
      SetTextColor(hdc, self->muted_color_);
      HFONT old = nullptr;
      if (self->title_font_) {
        old = static_cast<HFONT>(SelectObject(hdc, self->title_font_));
      }
      DrawTextW(hdc, L"标题", -1, &rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      if (old) {
        SelectObject(hdc, old);
      }
      // Bottom rule so the strip reads as chrome even when empty.
      HPEN pen = CreatePen(PS_SOLID, 1, self->border_color_);
      HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
      MoveToEx(hdc, 0, rc.bottom - 1, nullptr);
      LineTo(hdc, rc.right, rc.bottom - 1);
      SelectObject(hdc, old_pen);
      DeleteObject(pen);
      ReleaseDC(hwnd, hdc);
    }
    return r;
  }
  if (self && self->title_edit_proc_) {
    return CallWindowProcW(self->title_edit_proc_, hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK NotesWindow::EditProc(HWND hwnd, UINT msg, WPARAM wparam,
                                        LPARAM lparam) {
  auto* self = reinterpret_cast<NotesWindow*>(
      GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (self && (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
               msg == WM_SETFOCUS)) {
    self->DismissLibraryMenu();
  }
  if (self && msg == WM_DROPFILES) {
    return SendMessageW(self->hwnd_, WM_DROPFILES, wparam, lparam);
  }
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
    // Shift+Enter: soft break at caret (Typora) — newline without list continue.
    if (wparam == VK_RETURN && shift && !ctrl) {
      self->InsertSoftLineBreak();
      return 0;
    }
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
        self->Save(true);
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
      const bool is_account = id == static_cast<UINT>(kSidebarAccountId);
      if (!is_new && !is_ref && !is_mode && !is_outline && !is_account) {
        break;
      }
      // --- Account strip (bottom of sidebar) --------------------------------
      if (is_account) {
        HDC hdc = dis->hDC;
        RECT rc = dis->rcItem;
        const bool logged_in = VaultHttpLoggedIn();
        HBRUSH back = CreateSolidBrush(rail_bg_);
        FillRect(hdc, &rc, back);
        DeleteObject(back);
        // top divider line
        HPEN pen = CreatePen(PS_SOLID, 1, border_color_);
        HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
        MoveToEx(hdc, rc.left, rc.top, nullptr);
        LineTo(hdc, rc.right, rc.top);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);
        SetBkMode(hdc, TRANSPARENT);
        if (ui_font_) SelectObject(hdc, ui_font_);
        const std::wstring label =
            logged_in ? (L"● " + VaultHttpSessionLogin()) : L"登录 / 注册";
        SetTextColor(hdc, logged_in ? RGB(47, 111, 237) : muted_color_);
        RECT text_rc = rc;
        text_rc.left += 14;
        DrawTextW(hdc, label.c_str(), -1, &text_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
        return TRUE;
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
          const auto hItem =
              reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec);
          const UINT state =
              TreeView_GetItemState(sidebar_, hItem, TVIS_SELECTED);
          bool multi_on = false;
          {
            TVITEMW ti{};
            ti.mask = TVIF_HANDLE | TVIF_PARAM;
            ti.hItem = hItem;
            if (TreeView_GetItem(sidebar_, &ti) && ti.lParam > 0) {
              const size_t idx = static_cast<size_t>(ti.lParam) - 1;
              if (idx < sidebar_paths_.size()) {
                multi_on = SidebarPathInMulti(sidebar_paths_[idx]);
              }
            }
          }
          if ((state & TVIS_SELECTED) || multi_on) {
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
          return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
        }
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
          const auto hItem =
              reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec);
          TVITEMW ti{};
          ti.mask = TVIF_HANDLE | TVIF_PARAM;
          ti.hItem = hItem;
          if (TreeView_GetItem(sidebar_, &ti) && ti.lParam > 0) {
            const size_t idx = static_cast<size_t>(ti.lParam) - 1;
            if (idx < sidebar_paths_.size()) {
              const std::wstring& sp = sidebar_paths_[idx];
              RECT tr{};
              if (TreeView_GetItemRect(sidebar_, hItem, &tr, TRUE)) {
                Gdiplus::Graphics g(cd->nmcd.hdc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                if (PathIsPinned(sp)) {
                  DrawTabPushPin(
                      g, static_cast<float>(tr.left) + 6.f,
                      static_cast<float>(tr.top + tr.bottom) * 0.5f, true,
                      false);
                }
                if (PathIsCloudSynced(sp)) {
                  const float cx = static_cast<float>(tr.right) + 6.f;
                  const float cy = static_cast<float>(tr.top + tr.bottom) * 0.5f;
                  Gdiplus::Pen cpen(Gdiplus::Color(255, 47, 111, 237), 1.4f);
                  cpen.SetLineJoin(Gdiplus::LineJoinRound);
                  cpen.SetStartCap(Gdiplus::LineCapRound);
                  cpen.SetEndCap(Gdiplus::LineCapRound);
                  Gdiplus::GraphicsPath cp;
                  cp.AddArc(cx + 0.f, cy - 1.f, 5.f, 5.f, 90.f, 180.f);
                  cp.AddArc(cx + 2.f, cy - 5.f, 6.5f, 6.5f, 195.f, 160.f);
                  cp.AddArc(cx + 7.f, cy - 2.f, 5.f, 6.f, 260.f, 190.f);
                  cp.CloseFigure();
                  g.DrawPath(&cpen, &cp);
                }
              }
            }
          }
          return CDRF_DODEFAULT;
        }
        return CDRF_DODEFAULT;
      }
      // Single-click already opens/switches tabs (SidebarProc) — no NM_DBLCLK.
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
      if (hdr && hdr->idFrom == kSidebarId && hdr->code == TVN_BEGINDRAG) {
        auto* tv = reinterpret_cast<NMTREEVIEWW*>(lparam);
        StartSidebarDrag(tv->itemNew.hItem);
        return 0;
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
        case kMenuOnlineNotes:
          OpenOnlineNotes(false);
          return 0;
        case kMenuNewOnlineNote:
          OpenOnlineNotes(true);
          return 0;
        case kMenuToOnline: {
          // Push current local note to cloud online notes.
          Save(true);
          std::wstring title = TabLabel(path_);
          if (title.rfind(L"☁ ", 0) == 0) {
            title = title.substr(2);
          }
          if (title.empty()) {
            title = L"未命名";
          }
          // WYSIWYG 之外 web_ 里可能留着上一篇笔记的缓存；只有当前确实处于
          // WYSIWYG 模式时才用它，否则一律读 RichEdit 正文。
          std::wstring body = wysiwyg_ && web_.Ready()
                                  ? web_.PullMarkdown(300, web_.CachedMarkdown())
                                  : std::wstring();
          if (body.empty() && edit_) {
            const int len = GetWindowTextLengthW(edit_);
            if (len > 0) {
              body.assign(static_cast<size_t>(len) + 1, L'\0');
              GetWindowTextW(edit_, body.data(), len + 1);
              body.resize(wcslen(body.c_str()));
            }
          }
          if (!VaultHttpLoggedIn()) {
            EnsureLibraryCloudLogin();
            if (!VaultHttpLoggedIn()) {
              MistAlert(hwnd_, L"转在线", L"请先登录云账号，再点一次「转在线」。",
                        MistMsgTone::Warn);
              break;
            }
          }
          VaultHttpNote created;
          if (!VaultHttpCreateNote(title, body, &created) || created.id <= 0) {
            MistAlert(hwnd_, L"转在线", L"上传失败，请稍后重试。",
                      MistMsgTone::Danger);
            break;
          }
          // Local list patch after create — no ListNotes.
          {
            LibraryOnlineItem row;
            row.id = created.id;
            row.title = title;
            row.updated_at = created.updated_at;
            library_online_cache_.insert(library_online_cache_.begin(),
                                        std::move(row));
            RememberOnlineBody(created.id, title, body);
            library_online_stale_ = false;
          }
          OpenOnlineNoteInEditor(created.id);
          PushLibraryRailState(false);
          break;
        }
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
        case kMenuExportWord:
          ExportWord();
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
        case kFontFaceId:
          if (HIWORD(wparam) == CBN_SELCHANGE && font_face_combo_) {
            const int sel = static_cast<int>(
                SendMessageW(font_face_combo_, CB_GETCURSEL, 0, 0));
            SetFontFace(sel == 1 ? L"SimSun" : L"Microsoft YaHei");
            return 0;
          }
          break;
        case kFontSizeId:
          if (HIWORD(wparam) == CBN_SELCHANGE && font_size_combo_) {
            const int sel = static_cast<int>(
                SendMessageW(font_size_combo_, CB_GETCURSEL, 0, 0));
            if (sel >= 0) {
              wchar_t buf[16] = {};
              SendMessageW(font_size_combo_, CB_GETLBTEXT, sel,
                           reinterpret_cast<LPARAM>(buf));
              const int pt = _wtoi(buf);
              if (pt >= 8) {
                SetFontSize(pt);
              }
            }
            return 0;
          }
          break;
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
      if (LOWORD(wparam) == kSidebarAccountId && HIWORD(wparam) == BN_CLICKED) {
        if (!VaultHttpLoggedIn()) {
          EnsureLibraryCloudLogin();
          if (sidebar_account_) {
            InvalidateRect(sidebar_account_, nullptr, TRUE);
          }
          if (VaultHttpLoggedIn()) {
            OpenOnlineNotes(false);
          }
        } else {
          // 已登录 — 询问是否退出
          const std::wstring name = VaultHttpSessionLogin();
          const std::wstring account_msg =
              L"当前账号：" + name + L"\n\n是否退出登录？";
          if (MistConfirm(hwnd_, L"在线笔记", account_msg.c_str(),
                          MistMsgTone::Warn)) {
            VaultHttpLogout();
            // Drop everything tied to the previous session. Without this the
            // rail kept listing the signed-out account's online notes and the
            // body cache still short-circuited later fetches.
            library_online_cache_.clear();
            online_body_cache_.clear();
            library_online_stale_ = true;
            library_list_loaded_once_ = false;
            if (sidebar_account_) {
              InvalidateRect(sidebar_account_, nullptr, TRUE);
            }
            PushLibraryRailState(true);
          }
        }
        return 0;
      }
      if (LOWORD(wparam) == kSidebarRefreshId &&
          HIWORD(wparam) == BN_CLICKED) {
        SyncRefreshFromCloud();
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
      if (LOWORD(wparam) == kTitleEditId && HIWORD(wparam) == EN_CHANGE) {
        if (title_applying_ || loading_) {
          return 0;
        }
        MarkDirty();
        EnsureTabStates();
        if (tab_index_ >= 0 &&
            tab_index_ < static_cast<int>(tab_states_.size())) {
          tab_states_[static_cast<size_t>(tab_index_)].title_hint =
              NativeTitleText();
        }
        RefreshCurrentTabLabel();
        RefreshTitle();
        RefreshSidebarLabelForPath(path_);
        SchedulePersistTabs();
        ScheduleSyncFilenameToTitle();
        // Debounce the WYSIWYG host-title push (O(doc) ComposeMarkdown + JS) —
        // run once per input burst instead of on every keystroke.
        SetTimer(hwnd_, kTitleSyncTimer, 200, nullptr);
        return 0;
      }
      if (LOWORD(wparam) == kEditId && HIWORD(wparam) == EN_CHANGE) {
        status_full_text_dirty_ = true;
        MarkDirty();
        // In WYSIWYG, title lives in title_edit_ — don't re-parse # from body.
        // Debounce title sync: SetTimer replaces any pending timer, so the
        // full-text GetWindowTextW + ExtractMarkdownTitle runs once per input
        // burst instead of on every keystroke.
        if (!wysiwyg_) {
          SetTimer(hwnd_, kTitleSyncTimer, 200, nullptr);
        }
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
      std::vector<std::wstring> paths;
      paths.reserve(count);
      for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len + 1, L'\0');
        DragQueryFileW(drop, i, p.data(), len + 1);
        p.resize(len);
        paths.push_back(std::move(p));
      }
      DragFinish(drop);
      HandleDroppedPaths(paths);
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
      if (ctl == title_edit_) {
        SetTextColor(hdc, text_color_);
        SetBkColor(hdc, toast_ ? RGB(255, 248, 220) : content_bg_);
        return reinterpret_cast<LRESULT>(bg_brush_ ? bg_brush_ : card_brush_);
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
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_NCLBUTTONDOWN:
    case WM_NCRBUTTONDOWN:
      DismissLibraryMenu();
      break;
    case WM_ACTIVATE:
      DismissLibraryMenu();
      if (LOWORD(wparam) != WA_INACTIVE && !IsIconic(hwnd_)) {
        FocusActiveEditor(false);
        return 0;
      }
      break;
    case WM_SETFOCUS:
      DismissLibraryMenu();
      FocusActiveEditor(false);
      return 0;
    case WM_SIZE:
      LayoutClient();
      return 0;
    case WM_APP + 83:
      if (has_pending_tab_markdown_) {
        has_pending_tab_markdown_ = false;
        web_.SetMarkdown(pending_tab_markdown_, NoteBaseDir());
      }
      return 0;
    case WM_APP + 84: {
      std::wstring json = std::move(pending_library_action_json_);
      pending_library_action_json_.clear();
      if (!json.empty()) {
        HandleLibraryRailAction(json);
      }
      return 0;
    }
    case WM_APP + 85: {
      // Cloud list fetch done (EnsureLibraryListFetch). Apply on UI thread.
      auto* res = reinterpret_cast<
          std::pair<bool, std::vector<VaultHttpNote>>*>(lparam);
      library_list_busy_ = false;
      bool refreshed = false;
      if (res) {
        if (res->first) {
          library_online_cache_.clear();
          library_online_cache_.reserve(res->second.size());
          for (const auto& n : res->second) {
            LibraryOnlineItem it;
            it.id = n.id;
            it.title = n.title.empty() ? L"未命名" : n.title;
            it.updated_at = n.updated_at;
            library_online_cache_.push_back(std::move(it));
          }
          library_online_stale_ = false;
          library_list_loaded_once_ = true;
          refreshed = true;
        } else {
          // Keep showing the stale cache; the next explicit action retries.
          library_online_stale_ = true;
        }
        delete res;
      }
      if (library_list_refresh_pending_) {
        library_list_refresh_pending_ = false;
        EnsureLibraryListFetch();
      }
      if (refreshed) {
        PushLibraryRailState(false);
      }
      return 0;
    }
    case kMsgElevatedSaveDone: {
      // 提权助手已经写完目标文件：恢复原窗口，只把焦点/视图还给用户，
      // 不重新读取文件（那会把光标重置到开头，也可能把刚保存的内容覆盖）。
      auto* payload = reinterpret_cast<ElevatedSaveDone*>(lparam);
      if (!payload) {
        return 0;
      }
      std::unique_ptr<ElevatedSaveDone> done(payload);
      in_elevated_save_ = false;
      if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
      }
      // 等待期间用户可能切换了标签：只给目标标签恢复光标/滚动，
      // 但全局提权状态总要解除，否则之后所有保存都被挡住。
      const bool target_current =
          _wcsicmp(NormalizePathKey(path_).c_str(), done->target.c_str()) == 0;
      if (!done->ok) {
        // 等待期间可能已经切换标签：必须恢复目标标签自己的 dirty 状态，
        // 否则切回去会按“已保存”从磁盘重载旧内容，吞掉用户编辑。
        const int n = static_cast<int>(tab_paths_.size());
        bool target_restored = false;
        if (done->target_tab >= 0 && done->target_tab < n &&
            _wcsicmp(NormalizePathKey(
                         tab_paths_[static_cast<size_t>(done->target_tab)])
                         .c_str(),
                     done->target.c_str()) == 0) {
          // dirty_ is per-active-tab; switch back so the edited buffer is the
          // live one again, then mark it unsaved. Otherwise returning to this
          // tab later would reload the old on-disk body and lose the edit.
          if (done->target_tab != tab_index_) {
            SwitchToTab(done->target_tab, /*save_current=*/false);
          }
          dirty_ = true;
          EnsureTabStates();
          TabState& st = tab_states_[static_cast<size_t>(done->target_tab)];
          st.caret = done->caret;
          st.first_visible_line = done->first_visible_line;
          target_restored = true;
        }
        if (!target_restored) {
          dirty_ = true;
        }
        RefreshCurrentTabLabel();
        RefreshTitle();
        MistAlert(hwnd_, L"保存失败",
                  L"提权写入未完成，文件未被修改。请检查权限或安全软件后重试。",
                  MistMsgTone::Warn);
        UpdateStatusBar();
        return 0;
      }
      if (target_current && edit_) {
        const int total = static_cast<int>(
            SendMessageW(edit_, EM_GETLINECOUNT, 0, 0));
        GETTEXTLENGTHEX tl{};
        tl.flags = GTL_DEFAULT | GTL_NUMCHARS;
        tl.codepage = 1200;
        const LRESULT len = SendMessageW(
            edit_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&tl), 0);
        const LONG caret = len > 0 ? (std::min)(done->caret,
                                               static_cast<LONG>(len))
                                   : done->caret;
        CHARRANGE cr{caret, caret};
        SendMessageW(edit_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&cr));
        const int first = (std::min)(done->first_visible_line,
                                     (std::max)(0, total - 1));
        const int cur = static_cast<int>(
            SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0));
        if (first != cur) {
          SendMessageW(edit_, EM_LINESCROLL, 0, first - cur);
        }
        SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
        if (done->typed_text_mode) {
          SetFocus(edit_);
        }
      }
      RefreshTitle();
      UpdateStatusBar();
      return 0;
    }
    case WM_APP + 86: {
      // 「同步」fetch done (SyncRefreshFromCloud). Apply on UI thread.
      auto* res = reinterpret_cast<SyncCloudResult*>(lparam);
      sync_cloud_busy_ = false;
      if (!res) {
        return 0;
      }
      if (!res->list_ok) {
        library_online_stale_ = true;
        MistAlert(hwnd_, L"同步", L"拉取云端列表失败。", MistMsgTone::Warn);
        delete res;
        return 0;
      }
      library_online_cache_.clear();
      library_online_cache_.reserve(res->list.size());
      for (const auto& n : res->list) {
        LibraryOnlineItem it;
        it.id = n.id;
        it.title = n.title.empty() ? L"未命名" : n.title;
        it.updated_at = n.updated_at;
        library_online_cache_.push_back(std::move(it));
      }
      library_online_stale_ = false;
      int updated = 0;
      for (auto& b : res->bodies) {
        if (IsOnlineNotePath(b.path)) {
          RememberOnlineBody(b.note_id, b.title, b.body);
          if (_wcsicmp(path_.c_str(), b.path.c_str()) == 0) {
            loading_ = true;
            SetWindowTextW(edit_, b.body.c_str());
            ApplyNativeTitleFromMarkdown(b.body);
            loading_ = false;
            dirty_ = false;
            SyncCurrentTabTitleHint();
            UpdateStatusBar();
            RefreshGutter();
            if (wysiwyg_) {
              web_.SetMarkdown(b.body, NoteBaseDir());
            }
          }
          ++updated;
          continue;
        }
        // Edited since the fetch started — keep the user's version instead of
        // clobbering it with the cloud copy. Two independent guards:
        //   1) file fingerprint taken at fetch start (covers edits made in a
        //      different, now-inactive tab that were autosaved meanwhile);
        //   2) active tab's dirty flag (covers unsaved edits not yet on disk).
        const bool active_dirty =
            _wcsicmp(path_.c_str(), b.path.c_str()) == 0 && dirty_;
        bool local_changed = false;
        if (!b.path.empty() && b.local_mtime != 0) {
          WIN32_FILE_ATTRIBUTE_DATA now{};
          if (!GetFileAttributesExW(b.path.c_str(), GetFileExInfoStandard,
                                    &now)) {
            local_changed = true;  // deleted / renamed while fetching
          } else {
            const ULONGLONG size =
                (static_cast<ULONGLONG>(now.nFileSizeHigh) << 32) |
                now.nFileSizeLow;
            const ULONGLONG mtime =
                (static_cast<ULONGLONG>(now.ftLastWriteTime.dwHighDateTime)
                 << 32) |
                now.ftLastWriteTime.dwLowDateTime;
            local_changed = size != b.local_size || mtime != b.local_mtime;
          }
        }
        if (active_dirty || local_changed) {
          continue;
        }
        if (AtomicWriteUtf8File(b.path, NarrowUtf8(b.body), false)) {
          RememberOnlineBody(b.note_id, b.title, b.body);
          ++updated;
          if (_wcsicmp(path_.c_str(), b.path.c_str()) == 0) {
            loading_ = true;
            SetWindowTextW(edit_, b.body.c_str());
            ApplyNativeTitleFromMarkdown(b.body);
            loading_ = false;
            dirty_ = false;
            if (wysiwyg_) {
              web_.SetMarkdown(b.body, NoteBaseDir());
            }
          }
        }
      }
      delete res;
      SaveNoteCloudSync(cloud_sync_);
      RebuildSidebar();
      PushLibraryRailState(false);
      if (status_) {
        SetWindowTextW(
            status_,
            (L"已从云端同步 " + std::to_wstring(updated) + L" 个文件").c_str());
      }
      return 0;
    }
    case kMsgCloudPushFailed: {
      // A cloud push attempt failed (network / expired session). The worker
      // already re-queued the batch and will retry; this only makes the
      // divergence visible so the user is not told everything synced.
      if (status_) {
        SetWindowTextW(status_, L"云端同步失败，稍后自动重试…");
      }
      ShowToast(L"云笔记上传失败，将在稍后自动重试");
      return 0;
    }
    case kMsgCloudDeleteFailed: {
      // Cloud delete failed after the row/tab was removed locally. Tell the
      // user and re-pull the cloud list so the still-existing note reappears
      // instead of vanishing then silently coming back on the next refresh.
      ShowToast(L"云端删除失败，笔记仍在云端，正在刷新列表…");
      if (VaultHttpLoggedIn()) {
        SyncRefreshFromCloud();
      }
      return 0;
    }
    case WM_APP + 95: {
      // A cloud note image finished downloading; repaint the active preview
      // so the <img> which previously 404'd now resolves.
      UpdatePreview();
      if (wysiwyg_) {
        web_.SetMarkdown(CurrentPlainText(), NoteBaseDir());
      }
      return 0;
    }
    case WM_APP + 88: {
      auto* deleted_ids = reinterpret_cast<std::vector<int>*>(lparam);
      if (deleted_ids) {
        if (!deleted_ids->empty()) {
          std::set<int> del_set(deleted_ids->begin(), deleted_ids->end());
          cloud_sync_.pending_deletes.erase(
              std::remove_if(cloud_sync_.pending_deletes.begin(),
                             cloud_sync_.pending_deletes.end(),
                             [&del_set](const NoteCloudPendingDelete& d) {
                               return del_set.count(d.note_id) > 0;
                             }),
              cloud_sync_.pending_deletes.end());
          SaveNoteCloudSync(cloud_sync_);
        }
        delete deleted_ids;
      }
      return 0;
    }
    case WM_APP + 89: {
      // Online note body fetched on a worker (see Load()).
      std::unique_ptr<VaultHttpNote> n(reinterpret_cast<VaultHttpNote*>(lparam));
      if (!n || n->id <= 0) {
        return 0;
      }
      pending_online_load_ids_.erase(n->id);
      if (wparam == 0) {
        // Record the failure: the editor still holds the '# ' placeholder, and
        // saving it would overwrite the real cloud body with an empty one.
        online_load_failed_ids_.insert(n->id);
        if (status_ && IsOnlineNotePath(path_) && OnlineNoteIdFromPath(path_) == n->id) {
          SetWindowTextW(status_, L"云端笔记加载失败，请检查网络或登录状态（已禁止保存以免覆盖云端正文）");
        }
        return 0;
      }
      online_load_failed_ids_.erase(n->id);
      std::wstring wide =
          n->body.empty() ? (L"# " + n->title + L"\n\n") : n->body;
      if (!n->title.empty() && wide.rfind(L"# ", 0) != 0) {
        wide = L"# " + n->title + L"\n\n" + wide;
      }
      RememberOnlineBody(n->id, n->title, wide);
      // Cloud bodies reference assets/... but the binaries never left the
      // local library. Gather them before the WebView renders this body.
      MaterializeOnlineNoteAssets(
          hwnd_, n->id, wide,
          JoinPath(JoinPath(NotesLibraryDir(true), L"云笔记"), L"assets"));
      if (!IsOnlineNotePath(path_) || OnlineNoteIdFromPath(path_) != n->id) {
        return 0;
      }
      std::wstring cur_plain = CurrentPlainText();
      bool is_placeholder = LooksLikeEmptyMarkdownSeed(cur_plain) || cur_plain.empty();
      if (!is_placeholder) {
        std::wstring trimmed = cur_plain;
        while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
        while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(trimmed.begin());
        if (trimmed == L"#" || trimmed == (L"# " + n->title) || trimmed == (L"#" + n->title)) {
          is_placeholder = true;
        }
      }
      if (dirty_ && !is_placeholder) {
        return 0;
      }
      text_enc_ = TextFileEncoding::Utf8;
      loading_ = true;
      SetWindowTextW(edit_, wide.c_str());
      ApplyNativeTitleFromMarkdown(wide);
      loading_ = false;
      dirty_ = false;
      SyncCurrentTabTitleHint();
      UpdateStatusBar();
      RefreshGutter();
      if (wysiwyg_) {
        web_.SetMarkdown(wide, NoteBaseDir());
      }
      return 0;
    }
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
        // Title filter only — reuse online cache, no network.
        PushLibraryRailState(false);
        return 0;
      }
      if (wparam == kOutlineTimer) {
        KillTimer(hwnd_, kOutlineTimer);
        RebuildOutline();
        return 0;
      }
      if (wparam == kStatusTimer) {
        // Skip the periodic full-text re-read/count while hidden or minimized;
        // the timer stays scheduled (cheap) so restore never misses an update.
        if (IsWindowVisible(hwnd_) && !IsIconic(hwnd_)) {
          UpdateStatusBar();
        }
        // Keep periodic refresh at the steadier cadence.
        SetTimer(hwnd_, kStatusTimer, 800, nullptr);
        return 0;
      }
      if (wparam == kPersistTabsTimer) {
        PersistTabs();
        return 0;
      }
      if (wparam == kPersistViewTimer) {
        KillTimer(hwnd_, kPersistViewTimer);
        PersistViewSettings();
        return 0;
      }
      if (wparam == kSyncFilenameTimer) {
        KillTimer(hwnd_, kSyncFilenameTimer);
        SyncFilenameToTitle();
        return 0;
      }
      if (wparam == kGutterTimer) {
        KillTimer(hwnd_, kGutterTimer);
        RefreshGutter();
        return 0;
      }
      if (wparam == kTitleSyncTimer) {
        KillTimer(hwnd_, kTitleSyncTimer);
        SyncCurrentTabTitleHint();
        PushNativeTitleToWeb();  // WYSIWYG host title (no-op in markdown mode)
        // Title EN_CHANGE already wrote title_hint — SyncCurrentTabTitleHint
        // may no-op; still refresh the WebView library list labels.
        if (library_web_.Host() && IsWindowVisible(library_web_.Host())) {
          PushLibraryRailState(false);
        }
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
      Destroy();
      if (quit_on_close_) {
        PostQuitMessage(0);
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}
