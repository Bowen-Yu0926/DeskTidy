#include "app_prefs.h"
#include "desknote_launch.h"
#include "elevated_save.h"
#include "file_assoc.h"
#include "i18n.h"
#include "log.h"
#include "mist_msg.h"
#include "notes_webview.h"
#include "notes_window.h"
#include "paths_util.h"
#include "ui_theme.h"

#include <Windows.h>
#include <ole2.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

NotesWindow* g_notes = nullptr;
HWND g_msg = nullptr;

struct CmdParse {
  std::vector<std::wstring> paths;
  std::wstring elev_save_target;
  bool elev_reopen = false;
};

CmdParse ParseCmd() {
  // Prefer GetCommandLineW (full line) over wWinMain's lpCmdLine: some
  // launchers leave lpCmdLine empty or oddly quoted for GUI subsystems.
  CmdParse out;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return out;
  }
  // argv[0] is the executable path.
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i] ? argv[i] : L"";
    if (a == L"--elevated-save" || a == L"--elevated-save-reopen") {
      out.elev_reopen = (a == L"--elevated-save-reopen");
      if (i + 1 < argc && argv[i + 1]) {
        out.elev_save_target = argv[++i];
      }
      continue;
    }
    if (a.empty() || a[0] == L'-') {
      continue;
    }
    out.paths.push_back(a);
  }
  LocalFree(argv);
  return out;
}

std::vector<std::wstring> PathsFromOpenAction(const std::wstring& action) {
  std::vector<std::wstring> paths;
  if (action.rfind(L"--open", 0) != 0) {
    return paths;
  }
  size_t pos = 0;
  while (true) {
    const size_t nl = action.find(L'\n', pos);
    if (nl == std::wstring::npos) {
      break;
    }
    pos = nl + 1;
    size_t end = action.find(L'\n', pos);
    if (end == std::wstring::npos) {
      end = action.size();
    }
    std::wstring p = action.substr(pos, end - pos);
    if (!p.empty()) {
      paths.push_back(p);
    }
    pos = end;
  }
  return paths;
}

void HandleDeskNoteAction(const std::wstring& action) {
  if (!g_notes) {
    return;
  }
  if (action == L"--focus" || action.empty()) {
    g_notes->Present();
    return;
  }
  if (action.rfind(L"--open", 0) == 0) {
    const auto paths = PathsFromOpenAction(action);
    if (paths.empty()) {
      g_notes->Present();
    } else {
      g_notes->OpenPaths(paths);
    }
    return;
  }
  g_notes->Present();
}

LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_COPYDATA) {
    const auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lparam);
    if (cds && cds->dwData == kDeskNoteCliMsg && cds->lpData &&
        cds->cbData >= sizeof(wchar_t)) {
      const size_t n = cds->cbData / sizeof(wchar_t);
      std::wstring action(static_cast<const wchar_t*>(cds->lpData), n);
      while (!action.empty() && action.back() == L'\0') {
        action.pop_back();
      }
      HandleDeskNoteAction(action);
    }
    return TRUE;
  }
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool CreateMsgWindow(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = MsgWndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kDeskNoteMsgClass;
  RegisterClassExW(&wc);
  g_msg = CreateWindowExW(0, kDeskNoteMsgClass, kDeskNoteMsgTitle, WS_POPUP, 0,
                          0, 0, 0, nullptr, nullptr, instance, nullptr);
  return g_msg != nullptr;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  LogInit();
  SetProcessAppUserModelId(kDeskNoteAumid);
  using SetDpiCtxFn = BOOL(WINAPI*)(void*);
  HMODULE user32 = GetModuleHandleW(L"user32.dll");
  if (user32) {
    auto set_ctx = reinterpret_cast<SetDpiCtxFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_ctx) {
      set_ctx(reinterpret_cast<void*>(static_cast<INT_PTR>(-4)));
    }
  }

  const CmdParse cmd = ParseCmd();
  LogLine(L"[deskNote] boot cmdline=%s", GetCommandLineW());

  // Elevated handoff must run before the single-instance check so a busy
  // forward path cannot silently drop the save the user just confirmed.
  bool elev_save_ok = false;
  const bool elev_save = !cmd.elev_save_target.empty();
  if (elev_save) {
    elev_save_ok = ConsumeElevatedPending(cmd.elev_save_target);
    LogLine(L"[elevated] consume target=%s ok=%s", cmd.elev_save_target.c_str(),
            elev_save_ok ? L"yes" : L"no");
  } else {
    PurgeOrphanElevatedPending();
  }

  // 提权助手只写盘：原窗口保持存活并占用单实例锁，助手不能抢锁、
  // 建窗口或转发 --open，否则会干扰原窗口恢复。
  if (elev_save) {
    LogLine(L"[elevated] helper write done ok=%d", elev_save_ok ? 1 : 0);
    LogShutdown();
    return elev_save_ok ? 0 : 1;
  }
  PurgeOrphanElevatedPending();

  bool lock_acquired = false;
  for (int i = 0; i < 1; ++i) {
    if (AcquireDeskNoteInstanceLock()) {
      lock_acquired = true;
      break;
    }
    Sleep(50);
  }

  if (!lock_acquired) {
    std::wstring action = L"--focus";
    if (!cmd.paths.empty()) {
      action = L"--open";
      for (const auto& p : cmd.paths) {
        action.push_back(L'\n');
        action += p;
      }
    }
    ForwardToDeskNote(action);
    LogShutdown();
    return 0;
  }

  const HRESULT hr = OleInitialize(nullptr);
  if (FAILED(hr) && hr != S_FALSE && hr != OLE_E_WRONGCOMPOBJ) {
    ReleaseDeskNoteInstanceLock();
    LogShutdown();
    return 1;
  }

  LoadLanguage();
  RegisterNoteFileAssociations();
  // Overlap Edge/WebView2 profile bring-up with window chrome construction.
  NotesWarmWebViewEnv();
  MistWarmMessageEnv();
  if (!CreateMsgWindow(instance)) {
    OleUninitialize();
    ReleaseDeskNoteInstanceLock();
    LogShutdown();
    return 1;
  }

  NotesWindow notes;
  g_notes = &notes;
  notes.SetQuitOnClose(true);
  if (!notes.Create(instance)) {
    g_notes = nullptr;
    DestroyWindow(g_msg);
    OleUninitialize();
    ReleaseDeskNoteInstanceLock();
    LogShutdown();
    return 1;
  }
  {
    AppPrefs prefs = LoadAppPrefs();
    // DeskNote 作为独立笔记视窗，保持独立素雅浅色高对比度，不与全局主题联动
  }

  if (cmd.paths.empty()) {
    notes.Present();
  } else {
    notes.OpenPaths(cmd.paths);
  }

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (notes.TranslateAccel(&msg)) {
      continue;
    }
    if (notes.Hwnd() && IsDialogMessageW(notes.Hwnd(), &msg)) {
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  g_notes = nullptr;
  notes.Destroy();
  if (g_msg) {
    DestroyWindow(g_msg);
    g_msg = nullptr;
  }
  OleUninitialize();
  ReleaseDeskNoteInstanceLock();
  LogShutdown();
  return 0;
}
