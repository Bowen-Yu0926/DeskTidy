#include "screen_record.h"

#include "app_prefs.h"
#include "display_geom.h"
#include "ffmpeg_fetch.h"
#include "i18n.h"
#include "log.h"
#include "mist_webview.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "tool_paths.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <objbase.h>
#include <ocidl.h>
#include <windowsx.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kBadgeClass[] = L"DeskTidyNativeRecBadge";
constexpr UINT kBadgeTimer = 1;

HANDLE g_proc = nullptr;
HANDLE g_stdin_wr = nullptr;
std::vector<HWND> g_badges;
std::wstring g_out_path;  // current (segment) output path
bool g_class_reg = false;
bool g_paused = false;
DWORD g_rec_start_tick = 0;
DWORD g_paused_accum_ms = 0;
DWORD g_pause_tick = 0;

// DirectShow device-enumeration interfaces (declared manually so we never need
// strmiids.lib — only ole32). Matches ffmpeg `-f dshow` device friendly names.
const GUID CLSID_SystemDeviceEnum = {0x62BE5D10, 0x60EB, 0x11d0,
                                     {0xBD, 0x3B, 0x00, 0xA0, 0xC9, 0x11,
                                      0xCE, 0x86}};
const GUID CLSID_AudioInputDeviceCategory = {0x33d9a762, 0x90c8, 0x11d0,
                                             {0xbd, 0x43, 0x00, 0xa0, 0xc9,
                                              0x11, 0xce, 0x86}};
MIDL_INTERFACE("29840822-5B84-11D0-BD3B-00A0C911CE86")
ICreateDevEnum : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE CreateClassEnumerator(
      REFCLSID clsidDeviceClass, IEnumMoniker** ppEnumMoniker, DWORD dwFlags) = 0;
};

// Immutable per-session command template so pause can spawn later segments.
struct RecCmd {
  std::wstring ff;
  std::wstring region;  // gdigrab -offset_x/-offset_y/-video_size (may be empty)
  int fps = 30;
  int crf = 23;
  bool with_audio = false;
  std::wstring audio_in;  // -f dshow -i audio="..."
  std::wstring out_dir;
  std::wstring stem;  // DeskTidy_YYYYMMDD_HHMMSS
  int seconds = 0;    // >0 = -t auto-stop on the FIRST segment
};
RecCmd g_cmd;
std::vector<std::wstring> g_segments;

std::wstring SegmentPath(int index) {
  wchar_t n[16];
  swprintf_s(n, L".part%03d.mp4", index);
  return JoinPath(g_cmd.out_dir, g_cmd.stem + n);
}

std::wstring FinalOutputPath() {
  return JoinPath(g_cmd.out_dir, g_cmd.stem + L".mp4");
}

std::wstring BuildCmd(const std::wstring& out_path, bool timed) {
  std::wstring cmd = L"\"" + g_cmd.ff + L"\" -y -f gdigrab -framerate " +
                     std::to_wstring(g_cmd.fps) + g_cmd.region + L" -i desktop" +
                     g_cmd.audio_in;
  if (timed && g_cmd.seconds > 0) {
    cmd += L" -t " + std::to_wstring(g_cmd.seconds);
  }
  // ultrafast + zerolatency keep the desktop responsive during capture.
  cmd += L" -c:v libx264 -preset ultrafast -tune zerolatency -crf " +
         std::to_wstring(g_cmd.crf);
  if (g_cmd.with_audio) {
    cmd += L" -c:a aac -shortest";
  }
  cmd += L" \"" + out_path + L"\"";
  return cmd;
}

std::wstring ResolveOutDir(const AppPrefs& prefs) {
  if (!prefs.record_output_dir.empty()) {
    CreateDirectoryW(prefs.record_output_dir.c_str(), nullptr);
    return prefs.record_output_dir;
  }
  // Python default: <install_root>/录屏 beside the exe.
  const std::wstring dir = JoinPath(ExeDir(), L"录屏");
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

void DestroyBadges() {
  for (HWND h : g_badges) {
    if (h) {
      KillTimer(h, kBadgeTimer);
      DestroyWindow(h);
    }
  }
  g_badges.clear();
}

DWORD ElapsedRecMs() {
  if (!g_rec_start_tick) {
    return 0;
  }
  DWORD now = GetTickCount();
  DWORD paused = g_paused_accum_ms;
  if (g_paused && g_pause_tick) {
    paused += now - g_pause_tick;
  }
  const DWORD total = now - g_rec_start_tick;
  return total > paused ? total - paused : 0;
}

void PaintBadge(HWND hwnd, HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int time_w = 72;
  const int btn_w = (rc.right - time_w) / 2;
  RECT time_rc{0, 0, time_w, rc.bottom};
  RECT left{time_w, 0, time_w + btn_w - 1, rc.bottom};
  RECT right{time_w + btn_w + 1, 0, rc.right, rc.bottom};
  HBRUSH time_br = CreateSolidBrush(RGB(28, 28, 32));
  HBRUSH pause_br =
      CreateSolidBrush(g_paused ? RGB(180, 140, 40) : RGB(40, 120, 80));
  HBRUSH stop_br = CreateSolidBrush(RGB(180, 32, 32));
  FillRect(hdc, &time_rc, time_br);
  FillRect(hdc, &left, pause_br);
  FillRect(hdc, &right, stop_br);
  DeleteObject(time_br);
  DeleteObject(pause_br);
  DeleteObject(stop_br);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(255, 255, 255));
  const DWORD ms = ElapsedRecMs();
  const int sec = static_cast<int>(ms / 1000);
  wchar_t clock[32];
  swprintf_s(clock, L"REC %02d:%02d", sec / 60, sec % 60);
  DrawTextW(hdc, clock, -1, &time_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  const std::wstring pause =
      g_paused ? Tr("record_resume") : Tr("record_pause");
  const std::wstring stop = Tr("record_stop");
  DrawTextW(hdc, pause.c_str(), -1, &left,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  DrawTextW(hdc, stop.c_str(), -1, &right, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK BadgeProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_TIMER:
      if (wparam == kBadgeTimer) {
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      RECT rc{};
      GetClientRect(hwnd, &rc);
      const int time_w = 72;
      if (pt.x < time_w) {
        return 0;
      }
      const int mid = time_w + (rc.right - time_w) / 2;
      if (pt.x < mid) {
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
      KillTimer(hwnd, kBadgeTimer);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// One REC indicator per monitor so a recording is visible no matter where the
// user is looking.
void ShowBadges(HINSTANCE instance) {
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
  DestroyBadges();
  const int ww = 220;
  const int hh = 28;
  for (const auto& m : EnumMonitors()) {
    const RECT& work = m.work;
    HWND b = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kBadgeClass, L"REC",
        WS_POPUP, work.right - ww - 16, work.top + 12, ww, hh, nullptr, nullptr,
        instance, nullptr);
    if (b) {
      SetTimer(b, kBadgeTimer, 500, nullptr);
      ShowWindow(b, SW_SHOWNOACTIVATE);
      g_badges.push_back(b);
    }
  }
}

bool PickRecordMonitor(HWND owner, const std::vector<MonitorInfo>& mons,
                       int* mon_index) {
  if (!mon_index) {
    return false;
  }
  *mon_index = -1;
  if (mons.size() <= 1) {
    return true;
  }
  std::vector<std::wstring> labels;
  labels.push_back(L"整桌面（所有显示器）");
  for (size_t i = 0; i < mons.size(); ++i) {
    const auto& m = mons[i];
    std::wstring lab = L"显示器 " + std::to_wstring(i + 1) + L"： " +
                       std::to_wstring(m.monitor.right - m.monitor.left) + L"x" +
                       std::to_wstring(m.monitor.bottom - m.monitor.top);
    if (m.primary) {
      lab += L"（主屏）";
    }
    labels.push_back(std::move(lab));
  }
  bool ok = false;
  int choice = -1;
  MistWebDialog dlg;
  dlg.owner = owner;
  dlg.user_data_subdir = L"webview2-rec-mon";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\pick_list.html");
  if (dlg.html_path.empty()) {
    return true;
  }
  dlg.build_state = [&]() {
    std::wstring json =
        L"{\"type\":\"state\",\"title\":\"录屏\",\"hint\":\"选择录制目标\","
        L"\"open_label\":\"开始\",\"allow_new\":false,\"sel\":0,\"items\":[";
    for (size_t i = 0; i < labels.size(); ++i) {
      if (i) {
        json += L',';
      }
      json += L'"';
      json += MistJsonEscape(labels[i]);
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
    if (id == L"ready") {
      MistWebDialogPushState(&dlg);
      return false;
    }
    if (id == L"open") {
      const int index = MistJsonIntField(json, L"index", -1);
      if (index >= 0 && index < static_cast<int>(labels.size())) {
        choice = index - 1;
        ok = true;
        return true;
      }
      return false;
    }
    if (id == L"cancel") {
      return true;
    }
    return false;
  };
  MistWebDialogRun(&dlg, L"录屏", 480, 400);
  if (!ok) {
    return false;
  }
  *mon_index = choice;
  return true;
}

bool SpawnSegment(int index, bool timed) {
  g_out_path = SegmentPath(index);
  const std::wstring cmd = BuildCmd(g_out_path, timed);
  LogLine(L"[record] spawn segment %d: %s", index, cmd.c_str());

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr;
  HANDLE wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) {
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
    return false;
  }
  CloseHandle(rd);
  CloseHandle(pi.hThread);
  g_stdin_wr = wr;
  g_proc = pi.hProcess;
  if (index == 1) {
    g_rec_start_tick = GetTickCount();
    g_paused_accum_ms = 0;
    g_pause_tick = 0;
  }
  ShowBadges(GetModuleHandleW(nullptr));
  return true;
}

void FinalizeCurrentSegment() {
  if (g_proc) {
    if (g_stdin_wr) {
      DWORD written = 0;
      const char q = 'q';
      WriteFile(g_stdin_wr, &q, 1, &written, nullptr);
      CloseHandle(g_stdin_wr);
      g_stdin_wr = nullptr;
    }
    if (WaitForSingleObject(g_proc, 3000) == WAIT_TIMEOUT) {
      LogLineA("[record] ffmpeg segment did not exit in time, terminating");
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
  if (!g_out_path.empty()) {
    g_segments.push_back(g_out_path);
  }
}

bool RunConcat(const std::wstring& list_path, const std::wstring& out_path) {
  std::wstring cmd = L"\"" + g_cmd.ff + L"\" -y -f concat -safe 0 -i \"" +
                     list_path + L"\" -c copy \"" + out_path + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    LogLineA("[record] concat: CreateProcess failed");
    return false;
  }
  WaitForSingleObject(pi.hProcess, 30000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

std::wstring MergeSegments() {
  if (g_segments.empty()) {
    return {};
  }
  const std::wstring final_path = FinalOutputPath();
  if (g_segments.size() == 1) {
    const std::wstring seg = g_segments[0];
    if (_wcsicmp(seg.c_str(), final_path.c_str()) != 0) {
      DeleteFileW(final_path.c_str());
      if (!MoveFileW(seg.c_str(), final_path.c_str())) {
        LogLineA("[record] rename to final failed, keep segment path");
        return seg;
      }
    }
    return final_path;
  }
  // Concat demuxer playlist (forward slashes for safe parsing).
  std::string playlist;
  for (const auto& s : g_segments) {
    std::wstring p = s;
    for (auto& c : p) {
      if (c == L'\\') {
        c = L'/';
      }
    }
    playlist += "file '" + NarrowUtf8(p) + "'\n";
  }
  const std::wstring list_path =
      JoinPath(g_cmd.out_dir, g_cmd.stem + L".concat.txt");
  FILE* f = nullptr;
  if (_wfopen_s(&f, list_path.c_str(), L"wb") != 0 || !f) {
    LogLineA("[record] concat: cannot write playlist");
    return g_segments.front();
  }
  fwrite(playlist.data(), 1, playlist.size(), f);
  fclose(f);

  DeleteFileW(final_path.c_str());
  if (!RunConcat(list_path, final_path)) {
    LogLineA("[record] concat failed, keeping segments");
    DeleteFileW(list_path.c_str());
    return g_segments.front();
  }
  DeleteFileW(list_path.c_str());
  for (const auto& s : g_segments) {
    DeleteFileW(s.c_str());
  }
  return final_path;
}

ULONGLONG FileSizeOf(const std::wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return 0;
  }
  LARGE_INTEGER li{};
  const bool ok = GetFileSizeEx(f, &li);
  CloseHandle(f);
  return ok ? static_cast<ULONGLONG>(li.QuadPart) : 0;
}

void PromptKeepDiscardRename(const std::wstring& saved) {
  const ULONGLONG size = FileSizeOf(saved);
  if (size == 0) {
    LogLineA("[record] output empty, discarding");
    DeleteFileW(saved.c_str());
    MessageBoxW(nullptr, L"录屏未能保存。（文件为空或未生成，可再试一次）",
                L"录屏", MB_OK | MB_ICONWARNING);
    return;
  }
  const int choice = MessageBoxW(
      nullptr,
      (L"录屏已保存：\n" + saved + L"\n\n是 = 保留\n否 = 丢弃\n取消 = 重命名")
          .c_str(),
      L"录屏", MB_YESNOCANCEL | MB_ICONINFORMATION);
  if (choice == IDNO) {
    DeleteFileW(saved.c_str());
    LogLineA("[record] user discarded output");
    return;
  }
  if (choice == IDCANCEL) {
    std::wstring stem = PromptText(nullptr, L"重命名", L"新文件名（不含扩展名）",
                                   L"DeskTidy_recording");
    if (stem.empty()) {
      return;
    }
    std::wstring dest = saved;
    const size_t dot = dest.find_last_of(L'.');
    std::wstring ext = (dot != std::wstring::npos) ? dest.substr(dot) : L".mp4";
    const size_t slash = dest.find_last_of(L'\\');
    dest = dest.substr(0, slash + 1) + stem + ext;
    if (_wcsicmp(dest.c_str(), saved.c_str()) != 0) {
      DeleteFileW(dest.c_str());
      if (!MoveFileW(saved.c_str(), dest.c_str())) {
        MessageBoxW(nullptr, L"重命名失败。", L"录屏", MB_OK | MB_ICONWARNING);
        return;
      }
      LogLine(L"[record] renamed to %s", dest.c_str());
    }
  }
  ShellExecuteW(nullptr, L"open", L"explorer.exe",
                (L"/select,\"" + saved + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

bool StartRecording(HWND owner) {
  if (g_proc || g_paused) {
    return false;
  }
  std::wstring ff;
  if (!EnsureFfmpegForRecording(owner, &ff) || ff.empty()) {
    return false;
  }
  const AppPrefs prefs = LoadAppPrefs();
  const auto mons = EnumMonitors();
  int mon_index = -1;
  if (!PickRecordMonitor(owner, mons, &mon_index)) {
    return false;
  }

  g_cmd = RecCmd{};
  g_cmd.ff = ff;
  g_cmd.fps = prefs.record_fps >= 15 && prefs.record_fps <= 60
                  ? prefs.record_fps
                  : 30;
  g_cmd.crf = prefs.record_quality == 0   ? 28
              : prefs.record_quality == 2 ? 18
                                          : 23;
  g_cmd.seconds = prefs.record_seconds;
  g_cmd.out_dir = ResolveOutDir(prefs);

  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t stem[64];
  swprintf_s(stem, L"DeskTidy_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  g_cmd.stem = stem;

  if (mon_index >= 0 && mon_index < static_cast<int>(mons.size())) {
    const RECT& r = mons[static_cast<size_t>(mon_index)].monitor;
    wchar_t off[160];
    swprintf_s(off, L" -offset_x %d -offset_y %d -video_size %dx%d", r.left,
               r.top, r.right - r.left, r.bottom - r.top);
    g_cmd.region = off;
  }
  if (!prefs.record_mic.empty()) {
    g_cmd.with_audio = true;
    g_cmd.audio_in = L" -f dshow -i audio=\"" + prefs.record_mic + L"\"";
  }

  g_segments.clear();
  g_out_path.clear();
  g_paused = false;
  if (!SpawnSegment(1, g_cmd.seconds > 0)) {
    MessageBoxW(owner, L"无法启动 ffmpeg。", L"录屏", MB_OK | MB_ICONWARNING);
    g_out_path.clear();
    g_cmd = RecCmd{};
    return false;
  }
  return true;
}

}  // namespace

std::vector<std::wstring> ListAudioCaptureDevices() {
  std::vector<std::wstring> out;
  ICreateDevEnum* dev_enum = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr,
                                CLSCTX_INPROC_SERVER, __uuidof(ICreateDevEnum),
                                reinterpret_cast<void**>(&dev_enum));
  if (FAILED(hr) || !dev_enum) {
    return out;
  }
  IEnumMoniker* en = nullptr;
  hr = dev_enum->CreateClassEnumerator(CLSID_AudioInputDeviceCategory, &en, 0);
  if (FAILED(hr) || !en) {
    dev_enum->Release();
    return out;
  }
  IMoniker* mon = nullptr;
  ULONG fetched = 0;
  while (en->Next(1, &mon, &fetched) == S_OK && mon) {
    IPropertyBag* bag = nullptr;
    if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_IPropertyBag,
                                     reinterpret_cast<void**>(&bag))) &&
        bag) {
      VARIANT var;
      VariantInit(&var);
      if (SUCCEEDED(bag->Read(L"FriendlyName", &var, nullptr)) &&
          var.vt == VT_BSTR && var.bstrVal) {
        out.push_back(var.bstrVal);
      }
      VariantClear(&var);
      bag->Release();
    }
    mon->Release();
  }
  en->Release();
  dev_enum->Release();
  return out;
}

bool IsScreenRecordPaused() { return g_paused; }

void PauseScreenRecord(bool pause) {
  if (pause == g_paused) {
    return;
  }
  if (pause) {
    if (!g_proc) {
      return;
    }
    g_pause_tick = GetTickCount();
    g_paused = true;
    // Segment + concat (Python parity): finalize the current segment, then
    // resume by spawning the next. Timestamps and file length stay correct.
    FinalizeCurrentSegment();
    if (g_proc == nullptr && !g_segments.empty()) {
      LogLineA("[record] paused (segment finalized)");
    }
    if (!g_badges.empty()) {
      for (HWND b : g_badges) {
        InvalidateRect(b, nullptr, TRUE);
      }
    }
    return;
  }
  // resume
  if (g_paused) {
    g_paused_accum_ms += GetTickCount() - g_pause_tick;
    g_pause_tick = 0;
    g_paused = false;
    if (!g_proc) {
      if (!SpawnSegment(static_cast<int>(g_segments.size()) + 1, false)) {
        LogLineA("[record] resume: spawn failed");
        g_paused = true;
      }
    }
    if (!g_badges.empty()) {
      for (HWND b : g_badges) {
        InvalidateRect(b, nullptr, TRUE);
      }
    }
  }
}

bool IsScreenRecording() {
  if (g_paused) {
    return true;
  }
  if (!g_proc) {
    return false;
  }
  const DWORD wait = WaitForSingleObject(g_proc, 0);
  if (wait != WAIT_TIMEOUT) {
    CloseHandle(g_proc);
    g_proc = nullptr;
    if (g_stdin_wr) {
      CloseHandle(g_stdin_wr);
      g_stdin_wr = nullptr;
    }
    if (!g_out_path.empty()) {
      g_segments.push_back(g_out_path);
    }
    DestroyBadges();
    const std::wstring saved = MergeSegments();
    g_out_path.clear();
    g_segments.clear();
    if (!saved.empty()) {
      PromptKeepDiscardRename(saved);
    }
    return false;
  }
  return true;
}

void StopScreenRecord() {
  if (!g_proc && !g_paused) {
    return;
  }
  if (g_proc) {
    FinalizeCurrentSegment();
  }
  g_paused = false;
  g_pause_tick = 0;
  DestroyBadges();
  const std::wstring saved = MergeSegments();
  g_out_path.clear();
  g_segments.clear();
  if (!saved.empty()) {
    PromptKeepDiscardRename(saved);
  }
}

std::wstring ScreenRecordOutputPath() {
  return g_out_path.empty() ? FinalOutputPath() : g_out_path;
}

void ReapScreenRecordOnExit() {
  if (g_proc) {
    FinalizeCurrentSegment();
  }
  g_paused = false;
  DestroyBadges();
  for (const auto& s : g_segments) {
    LogLine(L"[record] 丢弃未合并分段：%s", s.c_str());
    DeleteFileW(s.c_str());
  }
  g_segments.clear();
  g_out_path.clear();
  g_cmd = RecCmd{};
}

bool ToggleScreenRecord(HWND owner) {
  if (IsScreenRecording()) {
    StopScreenRecord();
    return true;
  }
  return StartRecording(owner);
}
