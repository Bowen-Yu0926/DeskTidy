#include "screen_record.h"

#include "app_prefs.h"
#include "display_geom.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "tool_paths.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <windowsx.h>

#include <string>
#include <vector>

namespace {

constexpr wchar_t kBadgeClass[] = L"DeskTidyNativeRecBadge";
HANDLE g_proc = nullptr;
HANDLE g_stdin_wr = nullptr;
HWND g_badge = nullptr;
std::wstring g_out_path;
bool g_class_reg = false;
bool g_paused = false;

using NtSuspendFn = LONG(NTAPI*)(HANDLE);
using NtResumeFn = LONG(NTAPI*)(HANDLE);

bool SuspendProcess(HANDLE proc, bool pause) {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) {
    return false;
  }
  auto susp = reinterpret_cast<NtSuspendFn>(
      GetProcAddress(ntdll, "NtSuspendProcess"));
  auto resu =
      reinterpret_cast<NtResumeFn>(GetProcAddress(ntdll, "NtResumeProcess"));
  if (!susp || !resu) {
    return false;
  }
  return (pause ? susp(proc) : resu(proc)) >= 0;
}

std::wstring ResolveOutDir(const AppPrefs& prefs) {
  if (!prefs.record_output_dir.empty()) {
    CreateDirectoryW(prefs.record_output_dir.c_str(), nullptr);
    return prefs.record_output_dir;
  }
  PWSTR vids = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &vids)) &&
      vids) {
    std::wstring dir = JoinPath(vids, L"DeskTidyNative");
    CoTaskMemFree(vids);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
  }
  return NativeDataDir();
}

void DestroyBadge() {
  if (g_badge) {
    DestroyWindow(g_badge);
    g_badge = nullptr;
  }
}

void PaintBadge(HWND hwnd, HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int mid = rc.right / 2;
  RECT left{0, 0, mid - 1, rc.bottom};
  RECT right{mid + 1, 0, rc.right, rc.bottom};
  HBRUSH pause_br =
      CreateSolidBrush(g_paused ? RGB(180, 140, 40) : RGB(40, 120, 80));
  HBRUSH stop_br = CreateSolidBrush(RGB(180, 32, 32));
  FillRect(hdc, &left, pause_br);
  FillRect(hdc, &right, stop_br);
  DeleteObject(pause_br);
  DeleteObject(stop_br);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(255, 255, 255));
  DrawTextW(hdc, g_paused ? L"继续" : L"暂停", -1, &left,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  DrawTextW(hdc, L"停止", -1, &right, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK BadgeProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      RECT rc{};
      GetClientRect(hwnd, &rc);
      if (pt.x < rc.right / 2) {
        PauseScreenRecord(!g_paused);
      } else {
        StopScreenRecord();
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      PaintBadge(hwnd, hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      g_badge = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ShowBadge(HINSTANCE instance) {
  if (!g_class_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BadgeProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = kBadgeClass;
    RegisterClassExW(&wc);
    g_class_reg = true;
  }
  const int ww = 168;
  const int hh = 28;
  const RECT work = PrimaryWorkArea();
  g_badge = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kBadgeClass, L"REC",
      WS_POPUP, work.right - ww - 16, work.top + 12, ww, hh, nullptr, nullptr,
      instance, nullptr);
  if (g_badge) {
    ShowWindow(g_badge, SW_SHOWNOACTIVATE);
  }
}

bool StartRecording(HWND owner) {
  if (g_proc) {
    return false;
  }
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

  const AppPrefs prefs = LoadAppPrefs();
  const auto mons = EnumMonitors();
  int mon_index = -1;
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
      mon_index = (ans == L"0") ? -1 : (_wtoi(ans.c_str()) - 1);
    }
  }

  const std::wstring out_dir = ResolveOutDir(prefs);
  wchar_t name[64];
  SYSTEMTIME st{};
  GetLocalTime(&st);
  swprintf_s(name, L"rec_%04d%02d%02d_%02d%02d%02d.mp4", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  g_out_path = JoinPath(out_dir, name);

  std::wstring region;
  if (mon_index >= 0 && mon_index < static_cast<int>(mons.size())) {
    const RECT& r = mons[static_cast<size_t>(mon_index)].monitor;
    wchar_t off[160];
    swprintf_s(off, L" -offset_x %d -offset_y %d -video_size %dx%d", r.left,
               r.top, r.right - r.left, r.bottom - r.top);
    region = off;
  }

  std::wstring dur;
  if (prefs.record_seconds > 0) {
    dur = L" -t " + std::to_wstring(prefs.record_seconds);
  }

  const int crf = prefs.record_quality == 0   ? 28
                  : prefs.record_quality == 2 ? 18
                                              : 23;
  std::wstring audio_in;
  std::wstring audio_out;
  if (!prefs.record_mic.empty()) {
    audio_in = L" -f dshow -i audio=\"" + prefs.record_mic + L"\"";
    audio_out = L" -c:a aac -shortest";
  }
  std::wstring cmd = L"\"" + ff + L"\" -y -f gdigrab -framerate 30" + region +
                     L" -i desktop" + audio_in + dur + L" -c:v libx264 -crf " +
                     std::to_wstring(crf) + audio_out + L" \"" + g_out_path +
                     L"\"";

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr;
  HANDLE wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
    MessageBoxW(owner, L"无法创建管道。", L"录屏", MB_OK | MB_ICONWARNING);
    g_out_path.clear();
    return false;
  }
  SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = rd;
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(rd);
    CloseHandle(wr);
    MessageBoxW(owner, L"无法启动 ffmpeg。", L"录屏", MB_OK | MB_ICONWARNING);
    g_out_path.clear();
    return false;
  }
  CloseHandle(rd);
  CloseHandle(pi.hThread);
  g_stdin_wr = wr;
  g_proc = pi.hProcess;
  g_paused = false;
  ShowBadge(GetModuleHandleW(nullptr));
  return true;
}

}  // namespace

bool IsScreenRecordPaused() { return g_paused && g_proc != nullptr; }

void PauseScreenRecord(bool pause) {
  if (!g_proc || !IsScreenRecording()) {
    return;
  }
  if (pause == g_paused) {
    return;
  }
  if (!SuspendProcess(g_proc, pause)) {
    return;
  }
  g_paused = pause;
  if (g_badge) {
    InvalidateRect(g_badge, nullptr, TRUE);
  }
}

bool IsScreenRecording() {
  if (!g_proc) {
    return false;
  }
  if (g_paused) {
    return true;
  }
  const DWORD wait = WaitForSingleObject(g_proc, 0);
  if (wait != WAIT_TIMEOUT) {
    CloseHandle(g_proc);
    g_proc = nullptr;
    g_paused = false;
    if (g_stdin_wr) {
      CloseHandle(g_stdin_wr);
      g_stdin_wr = nullptr;
    }
    DestroyBadge();
    if (!g_out_path.empty()) {
      const std::wstring msg = L"录屏已完成。\n" + g_out_path;
      MessageBoxW(nullptr, msg.c_str(), L"录屏", MB_OK | MB_ICONINFORMATION);
      ShellExecuteW(nullptr, L"open", g_out_path.c_str(), nullptr, nullptr,
                    SW_SHOWNORMAL);
    }
    return false;
  }
  return true;
}

void StopScreenRecord() {
  if (g_proc && g_paused) {
    SuspendProcess(g_proc, false);
    g_paused = false;
  }
  if (g_proc) {
    if (g_stdin_wr) {
      DWORD written = 0;
      const char q = 'q';
      WriteFile(g_stdin_wr, &q, 1, &written, nullptr);
      CloseHandle(g_stdin_wr);
      g_stdin_wr = nullptr;
    }
    if (WaitForSingleObject(g_proc, 4000) == WAIT_TIMEOUT) {
      TerminateProcess(g_proc, 0);
      WaitForSingleObject(g_proc, 2000);
    }
    CloseHandle(g_proc);
    g_proc = nullptr;
  }
  if (g_stdin_wr) {
    CloseHandle(g_stdin_wr);
    g_stdin_wr = nullptr;
  }
  g_paused = false;
  DestroyBadge();
  if (!g_out_path.empty()) {
    const std::wstring msg = L"录屏已停止。\n" + g_out_path;
    MessageBoxW(nullptr, msg.c_str(), L"录屏", MB_OK | MB_ICONINFORMATION);
    ShellExecuteW(nullptr, L"open", g_out_path.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
  }
}

std::wstring ScreenRecordOutputPath() { return g_out_path; }

bool ToggleScreenRecord(HWND owner) {
  if (IsScreenRecording()) {
    StopScreenRecord();
    return true;
  }
  return StartRecording(owner);
}
