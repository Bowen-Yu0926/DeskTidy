#include "screen_record.h"

#include "app_prefs.h"
#include "display_geom.h"
#include "ffmpeg_fetch.h"
#include "i18n.h"
#include "log.h"
#include "mist_msg.h"
#include "mist_webview.h"
#include "paths_util.h"
#include "prompt_text.h"
#include "toast.h"
#include "tool_paths.h"

#include <Shellapi.h>
#include <ShlObj.h>
#include <objbase.h>
#include <ocidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// Forward declaration (defined in app_prefs.cpp): normalizes record mic device.
std::wstring NormalizeRecordMic(const std::wstring& mic);

namespace {

constexpr wchar_t kBadgeClass[] = L"DeskTidyNativeRecBadge";
constexpr wchar_t kBorderClass[] = L"DeskTidyNativeRecBorder";
constexpr UINT kBadgeTimer = 1;
constexpr int kBorderPx = 3;

// Market parity (OBS / ShareX / Game Bar): recording chrome stays on screen for
// the user but must not appear in gdigrab / Desktop Duplication output.
void ExcludeOverlayFromCapture(HWND hwnd) {
  if (!hwnd) {
    return;
  }
  SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
}

HANDLE g_proc = nullptr;
DWORD g_proc_pid = 0;
ULONGLONG g_proc_create_ms = 0;
HANDLE g_stdin_wr = nullptr;
std::vector<HWND> g_badges;
std::vector<HWND> g_borders;
RECT g_capture_rc{};
bool g_capture_rc_set = false;
std::wstring g_out_path;  // current (segment) output path
bool g_class_reg = false;
bool g_border_class_reg = false;
bool g_paused = false;
DWORD g_rec_start_tick = 0;
DWORD g_paused_accum_ms = 0;
DWORD g_pause_tick = 0;
// B2: heartbeat for crash recovery trimming — epoch seconds at start and the
// most recent 2s heartbeat, so a killed parent leaves a bounded cut point.
ULONGLONG g_started_at = 0;
ULONGLONG g_last_beat = 0;
bool g_adopted = false;  // orphan reopen: no stdin pipe to send 'q'
bool g_badge_dragging = false;
POINT g_badge_drag_origin{};
POINT g_badge_win_origin{};

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
  int mf_quality = 70;  // h264_mf -quality 0–100
  bool use_h264_mf = false;
  bool with_audio = false;
  std::wstring audio_in;  // -f dshow -i audio="..."
  std::wstring out_dir;
  std::wstring stem;  // DeskTidy_YYYYMMDD_HHMMSS
  int seconds = 0;    // >0 = -t auto-stop on the FIRST segment
  int max_mb = 0;     // P2-38: soft size cap (0 = off)
};
RecCmd g_cmd;
std::vector<std::wstring> g_segments;

std::wstring RecordStatePath() {
  return JoinPath(NativeDataDir(), L"screen_record_state.json");
}

ULONGLONG FileTimeToMs(const FILETIME& ft) {
  ULARGE_INTEGER u{};
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return u.QuadPart / 10000ULL;  // 100ns → ms
}

ULONGLONG ProcessCreateTimeMs(HANDLE proc) {
  FILETIME create{}, exit_t{}, kernel{}, user{};
  if (!GetProcessTimes(proc, &create, &exit_t, &kernel, &user)) {
    return 0;
  }
  return FileTimeToMs(create);
}

bool IsFfmpegImage(DWORD pid, const std::wstring& expected_exe) {
  HANDLE proc =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) {
    return false;
  }
  wchar_t path[MAX_PATH] = {};
  DWORD n = MAX_PATH;
  const BOOL ok = QueryFullProcessImageNameW(proc, 0, path, &n);
  CloseHandle(proc);
  if (!ok || n == 0) {
    return false;
  }
  const std::wstring image = path;
  const size_t slash = image.find_last_of(L"\\/");
  const std::wstring base =
      slash == std::wstring::npos ? image : image.substr(slash + 1);
  if (_wcsicmp(base.c_str(), L"ffmpeg.exe") != 0) {
    return false;
  }
  if (!expected_exe.empty()) {
    // Accept either exact path match or same basename under a different dir.
    if (_wcsicmp(image.c_str(), expected_exe.c_str()) != 0) {
      const size_t es = expected_exe.find_last_of(L"\\/");
      const std::wstring ebase = es == std::wstring::npos
                                     ? expected_exe
                                     : expected_exe.substr(es + 1);
      if (_wcsicmp(base.c_str(), ebase.c_str()) != 0) {
        return false;
      }
    }
  }
  return true;
}

void ClearRecordState() {
  DeleteFileW(RecordStatePath().c_str());
}

// Escape a UTF-8 string for embedding inside a JSON string literal. Windows
// paths contain backslashes that must be written as \\ for a correct round-trip
// (the reader unescapes them).
std::string JsonEscapeUtf8(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x",
                   static_cast<unsigned int>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

void WriteRecordState() {
  if (g_proc_pid == 0 || g_out_path.empty()) {
    return;
  }
  std::ofstream out(RecordStatePath(), std::ios::binary);
  if (!out) {
    return;
  }
  out << "{\n  \"pid\": " << g_proc_pid << ",\n  \"output\": \""
      << JsonEscapeUtf8(NarrowUtf8(g_out_path)) << "\",\n  \"exe\": \""
      << JsonEscapeUtf8(NarrowUtf8(g_cmd.ff)) << "\",\n  \"create_time_ms\": "
      << g_proc_create_ms << ",\n  \"started_at\": " << g_started_at
      << ",\n  \"last_beat\": " << g_last_beat << ",\n  \"out_dir\": \""
      << JsonEscapeUtf8(NarrowUtf8(g_cmd.out_dir)) << "\",\n  \"stem\": \""
      << JsonEscapeUtf8(NarrowUtf8(g_cmd.stem)) << "\",\n  \"segments\": [";
  for (size_t i = 0; i < g_segments.size(); ++i) {
    if (i) {
      out << ", ";
    }
    out << "\"" << JsonEscapeUtf8(NarrowUtf8(g_segments[i])) << "\"";
  }
  out << "]\n}\n";
}

bool ReadRecordState(DWORD* pid, std::wstring* output, std::wstring* exe,
                     ULONGLONG* create_ms, std::wstring* out_dir,
                     std::wstring* stem, std::vector<std::wstring>* segments,
                     ULONGLONG* started_at, ULONGLONG* last_beat) {
  std::ifstream in(RecordStatePath(), std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  auto find_num = [&](const char* key, ULONGLONG* out_v) -> bool {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) {
      return false;
    }
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) {
      return false;
    }
    ++p;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
      ++p;
    }
    ULONGLONG v = 0;
    if (p >= json.size() || json[p] < '0' || json[p] > '9') {
      return false;
    }
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') {
      v = v * 10 + static_cast<ULONGLONG>(json[p] - '0');
      ++p;
    }
    *out_v = v;
    return true;
  };
  auto find_str = [&](const char* key, std::wstring* out_s) -> bool {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) {
      return false;
    }
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) {
      return false;
    }
    p = json.find('"', p + 1);
    if (p == std::string::npos) {
      return false;
    }
    ++p;
    std::string v;
    while (p < json.size() && json[p] != '"') {
      if (json[p] == '\\' && p + 1 < json.size() &&
          (json[p + 1] == '\\' || json[p + 1] == '"')) {
        ++p;  // consume escape char, push the escaped char below
      }
      v.push_back(json[p++]);
    }
    *out_s = WidenUtf8(v);
    return true;
  };
  ULONGLONG pid_u = 0;
  if (!find_num("pid", &pid_u) || pid_u == 0) {
    return false;
  }
  *pid = static_cast<DWORD>(pid_u);
  if (!find_str("output", output) || output->empty()) {
    return false;
  }
  find_str("exe", exe);
  ULONGLONG cms = 0;
  find_num("create_time_ms", &cms);
  *create_ms = cms;
  ULONGLONG sat = 0;
  if (started_at && find_num("started_at", &sat)) {
    *started_at = sat;
  }
  ULONGLONG lb = 0;
  if (last_beat && find_num("last_beat", &lb)) {
    *last_beat = lb;
  }
  find_str("out_dir", out_dir);
  find_str("stem", stem);
  if (segments) {
    segments->clear();
    const size_t arr = json.find("\"segments\"");
    if (arr != std::string::npos) {
      size_t p = json.find('[', arr);
      if (p != std::string::npos) {
        ++p;
        while (p < json.size() && json[p] != ']') {
          while (p < json.size() && json[p] != '"' && json[p] != ']') {
            ++p;
          }
          if (p >= json.size() || json[p] == ']') {
            break;
          }
          ++p;
          std::string v;
          while (p < json.size() && json[p] != '"') {
            if (json[p] == '\\' && p + 1 < json.size() &&
                (json[p + 1] == '\\' || json[p + 1] == '"')) {
              ++p;  // consume escape char, push the escaped char below
            }
            v.push_back(json[p++]);
          }
          if (p < json.size() && json[p] == '"') {
            ++p;
          }
          if (!v.empty()) {
            segments->push_back(WidenUtf8(v));
          }
        }
      }
    }
  }
  return true;
}

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
  // P1-24: prefer h264_mf when probed (Python probe_preferred_encoder).
  if (g_cmd.use_h264_mf) {
    cmd += L" -c:v h264_mf -rate_control quality -quality " +
           std::to_wstring(g_cmd.mf_quality) + L" -pix_fmt nv12";
  } else {
    // ultrafast + zerolatency keep the desktop responsive during capture.
    cmd += L" -c:v libx264 -preset ultrafast -tune zerolatency -crf " +
           std::to_wstring(g_cmd.crf) + L" -pix_fmt yuv420p";
  }
  if (g_cmd.with_audio) {
    cmd += L" -c:a aac -shortest";
  }
  // P1-25 crash safety: fragmented mp4 writes the moov up front and flushes
  // each fragment as it is produced, so a segment left behind by a hard kill
  // still carries a recoverable header (recoverable via `-c copy`). Without
  // this, a `taskkill /F` orphan leaves an unfinalized moov that -c copy cannot
  // read ("moov atom not found").
  cmd += L" -movflags +frag_keyframe+empty_moov -flush_packets 1";
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

void DestroyBorders() {
  for (HWND h : g_borders) {
    if (h) {
      DestroyWindow(h);
    }
  }
  g_borders.clear();
}

void DestroyBadges() {
  for (HWND h : g_badges) {
    if (h) {
      KillTimer(h, kBadgeTimer);
      DestroyWindow(h);
    }
  }
  g_badges.clear();
  DestroyBorders();
  g_capture_rc_set = false;
}

LRESULT CALLBACK BorderProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      HBRUSH br = CreateSolidBrush(RGB(220, 38, 38));
      FillRect(hdc, &rc, br);
      DeleteObject(br);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // clicks pass through to desktop/apps
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void ShowBorders(HINSTANCE instance, const RECT& target, bool clear_first) {
  if (clear_first) {
    DestroyBorders();
  }
  if (!g_border_class_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BorderProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kBorderClass;
    RegisterClassExW(&wc);
    g_border_class_reg = true;
  }
  const int b = kBorderPx;
  const int x = target.left;
  const int y = target.top;
  const int w = target.right - target.left;
  const int h = target.bottom - target.top;
  if (w <= 0 || h <= 0) {
    return;
  }
  const RECT strips[4] = {
      {x, y, x + w, y + b},
      {x, y + h - b, x + w, y + h},
      {x, y, x + b, y + h},
      {x + w - b, y, x + w, y + h},
  };
  for (const RECT& s : strips) {
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
            WS_EX_NOACTIVATE,
        kBorderClass, L"", WS_POPUP, s.left, s.top, s.right - s.left,
        s.bottom - s.top, nullptr, nullptr, instance, nullptr);
    if (hwnd) {
      SetLayeredWindowAttributes(hwnd, 0, 220, LWA_ALPHA);
      ExcludeOverlayFromCapture(hwnd);
      ShowWindow(hwnd, SW_SHOWNOACTIVATE);
      g_borders.push_back(hwnd);
    }
  }
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

ULONGLONG TotalRecordedBytes();

LRESULT CALLBACK BadgeProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_TIMER:
      if (wparam == kBadgeTimer) {
        // P2-38: stop when the cumulative size across all segments + the live
        // output exceeds record_max_mb (pause/resume cannot reset the budget).
        if (g_cmd.max_mb > 0 && !g_paused) {
          const ULONGLONG limit =
              static_cast<ULONGLONG>(g_cmd.max_mb) * 1024ull * 1024ull;
          if (TotalRecordedBytes() >= limit) {
            LogLine(L"[record] size cap %d MiB reached — stopping",
                    g_cmd.max_mb);
            StopScreenRecord();
            ShowToast(L"录制文件已达大小上限，已自动停止");
            return 0;
          }
        }
        // B2: heartbeat — refresh last_beat every 2s so crash recovery can
        // bound the orphan ffmpeg's overrun to a tight cut point.
        if (!g_paused && g_proc) {
          const ULONGLONG now = static_cast<ULONGLONG>(time(nullptr));
          if (now >= g_last_beat + 2) {
            g_last_beat = now;
            WriteRecordState();
          }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      RECT rc{};
      GetClientRect(hwnd, &rc);
      // Drag from the REC clock strip; buttons keep click semantics.
      if (pt.x < 72) {
        SetCapture(hwnd);
        g_badge_dragging = true;
        GetCursorPos(&g_badge_drag_origin);
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        g_badge_win_origin = {wr.left, wr.top};
        return 0;
      }
      break;
    }
    case WM_MOUSEMOVE:
      if (g_badge_dragging && (wparam & MK_LBUTTON)) {
        POINT cur{};
        GetCursorPos(&cur);
        SetWindowPos(hwnd, nullptr,
                     g_badge_win_origin.x + (cur.x - g_badge_drag_origin.x),
                     g_badge_win_origin.y + (cur.y - g_badge_drag_origin.y), 0,
                     0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      break;
    case WM_LBUTTONUP: {
      if (g_badge_dragging) {
        g_badge_dragging = false;
        ReleaseCapture();
        return 0;
      }
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
  // P2-36: red capture frame + one movable REC badge on the target (or primary).
  RECT badge_work = PrimaryWorkArea();
  if (g_capture_rc_set) {
    ShowBorders(instance, g_capture_rc, true);
    badge_work = g_capture_rc;
  } else {
    // Full desktop: frame each monitor; badge on primary.
    DestroyBorders();
    for (const auto& m : EnumMonitors()) {
      ShowBorders(instance, m.monitor, false);
    }
  }
  HWND b = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kBadgeClass, L"REC",
      WS_POPUP, badge_work.right - ww - 16, badge_work.top + 12, ww, hh,
      nullptr, nullptr, instance, nullptr);
  if (b) {
    ExcludeOverlayFromCapture(b);
    SetTimer(b, kBadgeTimer, 500, nullptr);
    ShowWindow(b, SW_SHOWNOACTIVATE);
    g_badges.push_back(b);
  }
}

bool PickRecordMonitor(HWND owner, const std::vector<MonitorInfo>& mons,
                       int* mon_index) {
  if (!mon_index || mons.empty()) {
    return false;
  }
  // Default / single display: primary (or the only) monitor — no "整桌面".
  int primary = 0;
  for (size_t i = 0; i < mons.size(); ++i) {
    if (mons[i].primary) {
      primary = static_cast<int>(i);
      break;
    }
  }
  *mon_index = primary;
  if (mons.size() == 1) {
    return true;
  }
  std::vector<std::wstring> labels;
  labels.reserve(mons.size());
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
  int choice = primary;
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
        L"\"open_label\":\"开始\",\"allow_new\":false,\"sel\":";
    json += std::to_wstring(primary);
    json += L",\"items\":[";
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
        choice = index;
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
  MistWebDialogRun(&dlg, L"录屏", 480, 360);
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
  g_proc_pid = pi.dwProcessId;
  g_proc_create_ms = ProcessCreateTimeMs(g_proc);
  g_adopted = false;
  if (index == 1) {
    g_rec_start_tick = GetTickCount();
    g_paused_accum_ms = 0;
    g_pause_tick = 0;
    g_started_at = static_cast<ULONGLONG>(time(nullptr));
    g_last_beat = g_started_at;
  }
  WriteRecordState();
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
    } else if (g_adopted) {
      // Orphan reopen has no stdin pipe — terminate gracefully enough for concat.
      TerminateProcess(g_proc, 0);
    }
    if (WaitForSingleObject(g_proc, 3000) == WAIT_TIMEOUT) {
      LogLineA("[record] ffmpeg segment did not exit in time, terminating");
      TerminateProcess(g_proc, 0);
      WaitForSingleObject(g_proc, 2000);
    }
    CloseHandle(g_proc);
    g_proc = nullptr;
    g_proc_pid = 0;
    g_proc_create_ms = 0;
    g_adopted = false;
  }
  if (g_stdin_wr) {
    CloseHandle(g_stdin_wr);
    g_stdin_wr = nullptr;
  }
  if (!g_out_path.empty()) {
    g_segments.push_back(g_out_path);
    g_out_path.clear();
  }
  WriteRecordState();
}

bool RunConcat(const std::wstring& list_path, const std::wstring& out_path) {
  // No audio → -c copy. With audio, remux desyncs on pause/resume — re-encode
  // like Python build_concat_command (P0-7).
  std::wstring cmd = L"\"" + g_cmd.ff + L"\" -y -loglevel error -f concat -safe 0 -i \"" +
                     list_path + L"\"";
  if (!g_cmd.with_audio) {
    cmd += L" -c copy";
  } else {
    cmd += L" -c:v libx264 -preset ultrafast -crf 23 -pix_fmt yuv420p -c:a aac";
  }
  cmd += L" \"" + out_path + L"\"";
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
  WaitForSingleObject(pi.hProcess, 120000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if (code != 0) {
    LogLineA("[record] concat: ffmpeg exit %lu",
             static_cast<unsigned long>(code));
    return false;
  }
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
    // Failure: keep segments + playlist so the user can recover (P0-7).
    LogLineA("[record] concat failed, keeping segments");
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

// P2-38: cumulative bytes across every finalized segment + the live one, so
// pause/resume cannot reset the record_max_mb budget.
ULONGLONG TotalRecordedBytes() {
  ULONGLONG total = 0;
  for (const auto& seg : g_segments) {
    total += FileSizeOf(seg);
  }
  if (!g_out_path.empty()) {
    total += FileSizeOf(g_out_path);
  }
  return total;
}

// --- crash-recovery remux helpers (moov is never finalized on a hard kill) ---

std::wstring TempSegDir() {
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  return JoinPath(tmp, L"desktidy_rec");
}

bool EnsureDirExists(const std::wstring& dir) {
  return CreateDirectoryW(dir.c_str(), nullptr) != 0 ||
         GetLastError() == ERROR_ALREADY_EXISTS;
}

// Parse the first "Duration: HH:MM:SS.cc" from ffmpeg stderr → milliseconds.
ULONGLONG ParseDurationMs(const std::string& text) {
  const size_t p = text.find("Duration:");
  if (p == std::string::npos) {
    return 0;
  }
  size_t q = p + 9;
  while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) {
    ++q;
  }
  int h = 0;
  int m = 0;
  double s = 0.0;
  if (sscanf_s(text.c_str() + q, "%d:%d:%lf", &h, &m, &s) < 2) {
    return 0;
  }
  return static_cast<ULONGLONG>((h * 3600 + m * 60) * 1000 + s * 1000);
}

// Run ffmpeg with stderr captured to a temp file. Returns false only when the
// process cannot be spawned; otherwise exit_code/stderr_out are filled.
bool RunFfmpegCaptureStderr(const std::wstring& cmd, DWORD timeout_ms,
                            int* exit_code, std::string* stderr_out) {
  if (exit_code) {
    *exit_code = -1;
  }
  if (stderr_out) {
    stderr_out->clear();
  }
  wchar_t tmp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, tmp);
  wchar_t err_name[MAX_PATH] = {};
  swprintf_s(err_name, L"dtff_%lu_%lu.err", GetCurrentProcessId(),
             GetTickCount());
  const std::wstring err_path = JoinPath(tmp, err_name);
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE err_file =
      CreateFileW(err_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (err_file == INVALID_HANDLE_VALUE) {
    return false;
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  si.hStdError = err_file;
  PROCESS_INFORMATION pi{};
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  bool spawned = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) !=
                 0;
  if (!spawned) {
    LogLineA("[record] ffmpeg spawn failed (err=%lu)",
             static_cast<unsigned long>(GetLastError()));
  }
  if (spawned) {
    WaitForSingleObject(pi.hProcess, timeout_ms);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exit_code) {
      *exit_code = static_cast<int>(code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  if (stderr_out) {
    SetFilePointer(err_file, 0, nullptr, FILE_BEGIN);
    char chunk[4096];
    DWORD read = 0;
    std::string s;
    while (ReadFile(err_file, chunk, sizeof(chunk), &read, nullptr) &&
           read > 0) {
      s.append(chunk, read);
    }
    *stderr_out = std::move(s);
  }
  CloseHandle(err_file);
  DeleteFileW(err_path.c_str());
  return spawned;
}

// Remux a crashed segment with `-c copy` to rebuild the moov. On success
// dur_ms receives the parsed input duration; on failure err_tail gets the last
// 200 chars of ffmpeg stderr (or a spawn error).
bool RemuxSegment(const std::wstring& ff, const std::wstring& in_path,
                  const std::wstring& out_path, ULONGLONG* dur_ms,
                  std::string* err_tail, double cut_seconds = 0) {
  if (dur_ms) {
    *dur_ms = 0;
  }
  if (err_tail) {
    err_tail->clear();
  }
  DeleteFileW(out_path.c_str());
  std::wstring cmd = L"\"" + ff + L"\" -y -i \"" + in_path + L"\"";
  if (cut_seconds > 0) {
    wchar_t buf[64];
    swprintf_s(buf, L" -t %.3f", cut_seconds);
    cmd += buf;
  }
  cmd += L" -c copy \"" + out_path + L"\"";
  int code = -1;
  std::string stderr_text;
  if (!RunFfmpegCaptureStderr(cmd, 120000, &code, &stderr_text)) {
    if (err_tail) {
      *err_tail = "CreateProcess failed";
    }
    return false;
  }
  if (dur_ms) {
    *dur_ms = ParseDurationMs(stderr_text);
  }
  if (code != 0 || FileSizeOf(out_path) == 0) {
    if (err_tail) {
      const std::string tail =
          stderr_text.size() > 200 ? stderr_text.substr(stderr_text.size() - 200)
                                   : stderr_text;
      *err_tail = tail;
    }
    return false;
  }
  // A segment with no recoverable duration (0 frames) remuxes to an empty
  // stream; treat it as a failed remux so it is skipped, not concatenated.
  if (dur_ms && *dur_ms == 0) {
    if (err_tail) {
      *err_tail = "0 frames / no duration";
    }
    DeleteFileW(out_path.c_str());
    return false;
  }
  return true;
}

// P1-25: merge segments left behind by a dead ffmpeg into the final mp4.
// A hard-killed ffmpeg never finalizes the mp4 moov, so the old rename/concat
// path failed. Instead: remux each segment with `-c copy` (rebuilds moov), then
// concat. Returns the final path on success, empty on failure — in which case
// the .partNNN.mp4 segments are left untouched for manual recovery.
std::wstring MergeInterruptedRecording(const std::wstring& exe,
                                       const std::wstring& out_dir,
                                       const std::wstring& stem,
                                       const std::vector<std::wstring>& segs,
                                       ULONGLONG started_at,
                                       ULONGLONG last_beat) {
  if (segs.empty() || out_dir.empty() || stem.empty() || exe.empty()) {
    return {};
  }
  const std::wstring final_path = JoinPath(out_dir, stem + L".mp4");

  // B2: with a heartbeat, bound the orphan ffmpeg's overrun. cut = last known
  // parent beat minus start plus a 3s buffer; remux `-t cut` caps the output at
  // cut (a genuinely shorter real duration wins because -t is an upper bound).
  double cut = 0;
  if (started_at > 0 && last_beat >= started_at) {
    const ULONGLONG now = static_cast<ULONGLONG>(time(nullptr));
    cut = static_cast<double>(last_beat - started_at) + 3.0;
    LogLine(L"[record] recovery trim cut=%.2fs beat_age=%.2fs", cut,
            now >= last_beat ? static_cast<double>(now - last_beat) : 0.0);
  }

  if (segs.size() == 1) {
    ULONGLONG dur = 0;
    std::string err;
    if (RemuxSegment(exe, segs[0], final_path, &dur, &err, cut)) {
      LogLine(L"[record] remux ok seg1 dur=%.2fs", dur / 1000.0);
      DeleteFileW(segs[0].c_str());
      return final_path;
    }
    LogLineA("[record] remux seg1 failed: %s", err.c_str());
    return {};
  }

  // Multi-segment: remux each into %TEMP%\desktidy_rec\segN.mp4, skip the ones
  // that remux to zero frames / fail, then concat the survivors.
  const std::wstring temp_dir = TempSegDir();
  EnsureDirExists(temp_dir);
  std::vector<std::wstring> good;
  for (size_t i = 0; i < segs.size(); ++i) {
    wchar_t name[32];
    swprintf_s(name, L"seg%zu.mp4", i + 1);
    const std::wstring tmp_path = JoinPath(temp_dir, name);
    ULONGLONG dur = 0;
    std::string err;
    // Only the final (un-finalized) segment overruns after a parent kill.
    const double seg_cut = (i + 1 == segs.size()) ? cut : 0.0;
    if (RemuxSegment(exe, segs[i], tmp_path, &dur, &err, seg_cut)) {
      LogLine(L"[record] remux ok seg%zu dur=%.2fs", i + 1, dur / 1000.0);
      good.push_back(tmp_path);
    } else {
      LogLineA("[record] remux seg%zu skipped: %s", i + 1, err.c_str());
      DeleteFileW(tmp_path.c_str());
    }
  }
  if (good.empty()) {
    LogLineA("[record] all remux failed — keeping segments");
    return {};
  }

  if (good.size() == 1) {
    // Only one segment survived — promote it straight to the final path.
    DeleteFileW(final_path.c_str());
    if (!MoveFileW(good[0].c_str(), final_path.c_str())) {
      if (!CopyFileW(good[0].c_str(), final_path.c_str(), FALSE)) {
        LogLineA("[record] promote single remuxed segment failed");
        return {};
      }
      DeleteFileW(good[0].c_str());
    }
    for (const auto& s : segs) {
      DeleteFileW(s.c_str());
    }
    LogLine(L"[record] concat ok out=%s (single surviving segment)",
            final_path.c_str());
    return final_path;
  }

  std::string playlist;
  for (const auto& p : good) {
    std::wstring w = p;
    for (auto& c : w) {
      if (c == L'\\') {
        c = L'/';
      }
    }
    playlist += "file '" + NarrowUtf8(w) + "'\n";
  }
  const std::wstring list_path = JoinPath(temp_dir, stem + L".concat.txt");
  FILE* f = nullptr;
  if (_wfopen_s(&f, list_path.c_str(), L"wb") != 0 || !f) {
    LogLineA("[record] concat: cannot write playlist");
    return {};
  }
  fwrite(playlist.data(), 1, playlist.size(), f);
  fclose(f);

  DeleteFileW(final_path.c_str());
  const std::wstring cmd = L"\"" + exe + L"\" -y -f concat -safe 0 -i \"" +
                           list_path + L"\" -c copy \"" + final_path + L"\"";
  int code = -1;
  std::string stderr_text;
  if (!RunFfmpegCaptureStderr(cmd, 120000, &code, &stderr_text) || code != 0 ||
      FileSizeOf(final_path) == 0) {
    const std::string tail = stderr_text.size() > 200
                                 ? stderr_text.substr(stderr_text.size() - 200)
                                 : stderr_text;
    LogLineA("[record] concat failed: %s", tail.c_str());
    return {};
  }
  const ULONGLONG dur = ParseDurationMs(stderr_text);
  LogLine(L"[record] concat ok out=%s dur=%.2fs", final_path.c_str(),
          dur / 1000.0);

  for (const auto& s : segs) {
    DeleteFileW(s.c_str());
  }
  for (const auto& t : good) {
    DeleteFileW(t.c_str());
  }
  DeleteFileW(list_path.c_str());
  return final_path;
}

// P1-25: dead-ffmpeg recovery — merge leftover segments, keep them on failure.
bool RecoverDeadRecord(const std::wstring& exe, const std::wstring& out_dir,
                       const std::wstring& stem,
                       const std::vector<std::wstring>& segs,
                       ULONGLONG started_at, ULONGLONG last_beat) {
  const std::wstring final =
      MergeInterruptedRecording(exe, out_dir, stem, segs, started_at,
                                last_beat);
  if (final.empty()) {
    LogLineA("[record] interrupted merge failed — keeping segments");
    ShowToast(L"上次录制恢复失败，分段已保留：" + JoinPath(out_dir, stem));
    return false;
  }
  ClearRecordState();
  LogLine(L"[record] recovered interrupted recording → %s", final.c_str());
  // Startup / crash recovery: toast only — never the keep/discard dialog
  // (that dialog is only for an intentional stop in this session).
  ShowToast(L"已恢复上次录制：" + final);
  return true;
}

std::wstring SanitizeRecordStem(std::wstring stem) {
  // Strip path / reserved characters so rename stays inside the record folder.
  for (wchar_t& ch : stem) {
    if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) {
      ch = L'_';
    }
  }
  while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) {
    stem.pop_back();
  }
  size_t i = 0;
  while (i < stem.size() && stem[i] == L' ') {
    ++i;
  }
  if (i) {
    stem.erase(0, i);
  }
  return stem;
}

bool ApplyRecordKeepName(std::wstring* path, const std::wstring& stem_in) {
  if (!path || path->empty()) {
    return false;
  }
  const std::wstring stem = SanitizeRecordStem(stem_in);
  if (stem.empty()) {
    return false;
  }
  const size_t slash = path->find_last_of(L'\\');
  const size_t dot = path->find_last_of(L'.');
  const std::wstring dir =
      (slash == std::wstring::npos) ? L"" : path->substr(0, slash + 1);
  const std::wstring ext =
      (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
          ? path->substr(dot)
          : L".mp4";
  const std::wstring dest = dir + stem + ext;
  if (_wcsicmp(dest.c_str(), path->c_str()) == 0) {
    return true;
  }
  DeleteFileW(dest.c_str());
  if (!MoveFileW(path->c_str(), dest.c_str())) {
    MistAlert(nullptr, L"录屏", L"重命名失败，已保留原文件名。",
              MistMsgTone::Warn);
    return true;  // keep original path
  }
  *path = dest;
  LogLine(L"[record] renamed to %s", dest.c_str());
  return true;
}

void RevealRecordFile(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  ShellExecuteW(nullptr, L"open", L"explorer.exe",
                (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

constexpr wchar_t kRecDoneClass[] = L"DeskTidyNativeRecDone";
constexpr UINT kMsgRecDonePrompt = WM_APP + 1;
std::wstring g_pending_done_path;
HWND g_done_hwnd = nullptr;
bool g_done_class_reg = false;
bool g_done_prompt_open = false;

void RunKeepDiscardRenameDialog(std::wstring saved) {
  const ULONGLONG size = FileSizeOf(saved);
  if (size == 0) {
    LogLineA("[record] output empty, discarding");
    DeleteFileW(saved.c_str());
    MistAlert(nullptr, L"录屏", L"录屏未能保存。（文件为空或未生成，可再试一次）",
              MistMsgTone::Warn);
    return;
  }

  const size_t slash = saved.find_last_of(L'\\');
  const size_t dot = saved.find_last_of(L'.');
  const std::wstring folder =
      (slash == std::wstring::npos) ? saved : saved.substr(0, slash);
  std::wstring stem =
      (slash == std::wstring::npos) ? saved : saved.substr(slash + 1);
  std::wstring ext = L".mp4";
  if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
    ext = saved.substr(dot);
    stem = saved.substr(slash == std::wstring::npos ? 0 : slash + 1,
                        dot - (slash == std::wstring::npos ? 0 : slash + 1));
  }

  enum class Choice { Keep, Discard };
  Choice choice = Choice::Keep;
  std::wstring keep_stem = stem;
  bool answered = false;
  bool mist_attempted = false;

  MistWebDialog dlg;
  dlg.owner = nullptr;
  dlg.frameless = true;
  dlg.user_data_subdir = L"webview2-rec-done";
  dlg.html_path = MistFindAssetHtml(L"dialogs\\record_done.html");
  if (!dlg.html_path.empty()) {
    mist_attempted = true;
    dlg.build_state = [&]() {
      std::wstring json =
          L"{\"type\":\"state\",\"title\":\"录屏已保存\",\"hint\":"
          L"\"可修改文件名后保留；丢弃将删除本次录像。关闭窗口也会保留。\","
          L"\"folder\":\"";
      json += MistJsonEscape(folder);
      json += L"\",\"value\":\"";
      json += MistJsonEscape(stem);
      json += L"\",\"ext\":\"";
      json += MistJsonEscape(ext);
      json += L"\"}";
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
      if (id == L"keep") {
        keep_stem = MistJsonStringField(json, L"value");
        choice = Choice::Keep;
        answered = true;
        return true;
      }
      if (id == L"discard") {
        choice = Choice::Discard;
        answered = true;
        return true;
      }
      return false;
    };
    MistWebDialogRun(&dlg, L"录屏", 480, 300);
  }

  if (!answered && mist_attempted) {
    // Host chrome closed without a button — same as Esc：保留。
    choice = Choice::Keep;
    keep_stem = stem;
    answered = true;
  }

  if (!answered) {
    // Mist/WebView2 unavailable — keep quietly (no Yes/No MessageBox; that
    // legend UI is what we removed). Soft toast + reveal matches update UX.
    LogLineA("[record] mist done-dialog unavailable — keep + toast");
    choice = Choice::Keep;
    keep_stem = stem;
    ShowToast(L"录屏已保存：" + saved);
  }

  if (choice == Choice::Discard) {
    DeleteFileW(saved.c_str());
    LogLineA("[record] user discarded output");
    ShowToast(L"已丢弃本次录屏");
    return;
  }
  ApplyRecordKeepName(&saved, keep_stem.empty() ? stem : keep_stem);
  RevealRecordFile(saved);
}

LRESULT CALLBACK RecDoneWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                LPARAM lparam) {
  if (msg == kMsgRecDonePrompt) {
    if (g_done_prompt_open) {
      return 0;
    }
    std::wstring path = std::move(g_pending_done_path);
    g_pending_done_path.clear();
    if (path.empty()) {
      return 0;
    }
    g_done_prompt_open = true;
    RunKeepDiscardRenameDialog(std::move(path));
    g_done_prompt_open = false;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void EnsureRecDoneHwnd() {
  if (g_done_hwnd) {
    return;
  }
  HINSTANCE inst = GetModuleHandleW(nullptr);
  if (!g_done_class_reg) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RecDoneWndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kRecDoneClass;
    RegisterClassExW(&wc);
    g_done_class_reg = true;
  }
  g_done_hwnd = CreateWindowExW(0, kRecDoneClass, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, inst, nullptr);
}

// Defer off hotkey / pet-timer stacks so WebView2 mist dialog can create cleanly
// (nested pumps from IsScreenRecording / ToggleScreenRecord used to strand the
// old Yes/No/Cancel MessageBox on the UI thread).
void PromptKeepDiscardRename(const std::wstring& saved_in) {
  if (saved_in.empty()) {
    return;
  }
  EnsureRecDoneHwnd();
  if (!g_done_hwnd) {
    RunKeepDiscardRenameDialog(saved_in);
    return;
  }
  g_pending_done_path = saved_in;
  PostMessageW(g_done_hwnd, kMsgRecDonePrompt, 0, 0);
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
  g_cmd.mf_quality = prefs.record_quality == 0   ? 55
                     : prefs.record_quality == 2 ? 85
                                                 : 70;
  g_cmd.use_h264_mf =
      PreferredRecordingEncoder(ff) == L"h264_mf";
  g_cmd.seconds = prefs.record_seconds;
  g_cmd.max_mb = prefs.record_max_mb;
  g_cmd.out_dir = ResolveOutDir(prefs);

  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t stem[64];
  swprintf_s(stem, L"DeskTidy_%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute, st.wSecond);
  g_cmd.stem = stem;

  g_capture_rc_set = false;
  if (mon_index >= 0 && mon_index < static_cast<int>(mons.size())) {
    const RECT& r = mons[static_cast<size_t>(mon_index)].monitor;
    wchar_t off[160];
    swprintf_s(off, L" -offset_x %d -offset_y %d -video_size %dx%d", r.left,
               r.top, r.right - r.left, r.bottom - r.top);
    g_cmd.region = off;
    g_capture_rc = r;
    g_capture_rc_set = true;
  }
  std::wstring mic = NormalizeRecordMic(prefs.record_mic);
  if (mic.empty()) {
    const auto mics = ListAudioCaptureDevices();
    if (!mics.empty()) {
      mic = mics.front();
    }
  }
  if (!mic.empty()) {
    g_cmd.with_audio = true;
    g_cmd.audio_in = L" -f dshow -i audio=\"" + mic + L"\"";
  }

  g_segments.clear();
  g_out_path.clear();
  g_paused = false;
  if (!SpawnSegment(1, g_cmd.seconds > 0)) {
    MistAlert(owner, L"录屏", L"无法启动 ffmpeg。", MistMsgTone::Warn);
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
      // P2-37: timed recordings must apply remaining seconds to later segments.
      bool timed = false;
      const int budget = g_cmd.seconds;
      if (budget > 0) {
        const int elapsed_s =
            static_cast<int>(ElapsedRecMs() / 1000);
        const int rem = budget - elapsed_s;
        if (rem <= 0) {
          LogLineA("[record] resume: timed budget exhausted → stop");
          g_paused = false;
          DestroyBadges();
          const std::wstring saved = MergeSegments();
          g_out_path.clear();
          g_segments.clear();
          g_cmd = RecCmd{};
          ClearRecordState();
          if (!saved.empty()) {
            PromptKeepDiscardRename(saved);
          }
          return;
        }
        g_cmd.seconds = rem;
        timed = true;
      }
      if (!SpawnSegment(static_cast<int>(g_segments.size()) + 1, timed)) {
        LogLineA("[record] resume: spawn failed");
        g_paused = true;
      }
      if (budget > 0) {
        g_cmd.seconds = budget;  // keep original budget for next pause/resume
      }
    }
    if (!g_badges.empty()) {
      for (HWND b : g_badges) {
        InvalidateRect(b, nullptr, TRUE);
      }
    }
  }
}

bool ScreenRecordSessionLive() { return g_proc != nullptr || g_paused; }

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
    g_proc_pid = 0;
    g_adopted = false;
    if (g_stdin_wr) {
      CloseHandle(g_stdin_wr);
      g_stdin_wr = nullptr;
    }
    if (!g_out_path.empty()) {
      g_segments.push_back(g_out_path);
      g_out_path.clear();
    }
    DestroyBadges();
    const std::wstring saved = MergeSegments();
    g_segments.clear();
    g_cmd = RecCmd{};
    ClearRecordState();
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
  g_cmd = RecCmd{};
  ClearRecordState();
  if (!saved.empty()) {
    PromptKeepDiscardRename(saved);
  }
}

std::wstring ScreenRecordOutputPath() {
  return g_out_path.empty() ? FinalOutputPath() : g_out_path;
}

bool AdoptOrphanScreenRecord() {
  if (g_proc || g_paused) {
    return false;
  }
  DWORD pid = 0;
  std::wstring output, exe, out_dir, stem;
  ULONGLONG create_ms = 0;
  ULONGLONG started_at = 0;
  ULONGLONG last_beat = 0;
  std::vector<std::wstring> segs;
  if (!ReadRecordState(&pid, &output, &exe, &create_ms, &out_dir, &stem,
                        &segs, &started_at, &last_beat)) {
    return false;
  }
  // Derive stem/out_dir from output when the state predates those fields.
  if (stem.empty() && !output.empty()) {
    const size_t slash = output.find_last_of(L"\\/");
    std::wstring name =
        slash == std::wstring::npos ? output : output.substr(slash + 1);
    const size_t part = name.find(L".part");
    if (part != std::wstring::npos) {
      stem = name.substr(0, part);
    }
  }
  if (out_dir.empty() && !output.empty()) {
    const size_t slash = output.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      out_dir = output.substr(0, slash);
    }
  }

  // The live segment being written at crash time is stored in `output`, not in
  // `segments` (which only holds already-finalized segments). Include it so the
  // recovery remuxes the crashed, un-finalized mp4 too.
  std::vector<std::wstring> all_segs = segs;
  if (!output.empty() &&
      std::find(all_segs.begin(), all_segs.end(), output) == all_segs.end()) {
    all_segs.push_back(output);
  }

  // If the orphan ffmpeg is somehow still alive, terminate it and wait so the
  // merge never reads a file that is still being written (moov never finalizes
  // on a hard kill — the remux path rebuilds it).
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE |
                                SYNCHRONIZE | PROCESS_VM_READ,
                            FALSE, pid);
  if (proc) {
    if (WaitForSingleObject(proc, 0) == WAIT_TIMEOUT) {
      const ULONGLONG live_ms = ProcessCreateTimeMs(proc);
      bool same = create_ms == 0 || live_ms == 0;
      if (!same) {
        const ULONGLONG diff =
            live_ms > create_ms ? live_ms - create_ms : create_ms - live_ms;
        same = diff <= 2000;
      }
      if (same && IsFfmpegImage(pid, exe)) {
        TerminateProcess(proc, 0);
        WaitForSingleObject(proc, 2000);
        LogLine(L"[record] orphan pid=%lu alive — terminated, waiting 2s", pid);
      } else {
        LogLine(L"[record] orphan pid=%lu alive but not our ffmpeg — not terminating",
                pid);
      }
    }
    CloseHandle(proc);
  }
  LogLine(L"[record] orphan pid=%lu dead — recovering segments", pid);
  return RecoverDeadRecord(exe, out_dir, stem, all_segs, started_at,
                           last_beat);
}

// P1-25: startup recovery — adopt a live orphan ffmpeg or merge the segments of
// a dead one. Wired from App::Init by development A (do not edit app.cpp here).
bool RecoverInterruptedRecording() {
  return AdoptOrphanScreenRecord();
}

void ReapScreenRecordOnExit() {
  // P1-25: clean exit merges segments instead of deleting them. Crash path
  // leaves screen_record_state.json so the next launch can AdoptOrphan…
  if (g_proc || g_paused) {
    if (g_proc) {
      FinalizeCurrentSegment();
    }
    g_paused = false;
    DestroyBadges();
    const std::wstring saved = MergeSegments();
    if (!saved.empty()) {
      LogLine(L"[record] exit-merge saved %s", saved.c_str());
    }
  }
  g_out_path.clear();
  g_segments.clear();
  g_cmd = RecCmd{};
  g_proc_pid = 0;
  ClearRecordState();
}

bool ToggleScreenRecord(HWND owner) {
  // MistWebDialogRun pumps the thread queue; WM_HOTKEY / relay can re-enter
  // here and open a second「选择录制目标」dialog (F4 / Ctrl+Alt+3 double UI).
  static bool busy = false;
  if (busy) {
    return false;
  }
  busy = true;
  struct BusyGuard {
    ~BusyGuard() { busy = false; }
  } guard;

  AdoptOrphanScreenRecord();
  if (IsScreenRecording()) {
    StopScreenRecord();
    return true;
  }
  return StartRecording(owner);
}
