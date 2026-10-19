#include "pet_window.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "pet_ledges.h"
#include "toast.h"
#include "paths_util.h"
#include "tool_paths.h"

#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {
constexpr wchar_t kClassName[] = L"DeskTidyNativePet";
constexpr int kSize = 72;
bool g_registered = false;
}  // namespace

void PetWindow::TryLoadSprite() {
  const std::wstring candidates[] = {
      JoinPath(NativeDataDir(), L"pet.png"),
      JoinPath(ExeDir(), L"pet.png"),
      JoinPath(ExeDir(), L"assets\\pet.png"),
  };
  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartup(&token, &input, nullptr);
  for (const auto& path : candidates) {
    Gdiplus::Bitmap src(path.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) {
      continue;
    }
    sprite_w_ = static_cast<int>(src.GetWidth());
    sprite_h_ = static_cast<int>(src.GetHeight());
    if (sprite_w_ > 128) {
      sprite_h_ = sprite_h_ * 128 / sprite_w_;
      sprite_w_ = 128;
    }
    Gdiplus::Bitmap scaled(sprite_w_, sprite_h_);
    Gdiplus::Graphics g(&scaled);
    g.DrawImage(&src, 0, 0, sprite_w_, sprite_h_);
    HBITMAP hb = nullptr;
    scaled.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hb);
    if (hb) {
      sprite_ = hb;
      break;
    }
  }
  Gdiplus::GdiplusShutdown(token);
}

void PetWindow::ClearFrames() {
  for (auto* hb : frames_) {
    if (hb) {
      DeleteObject(hb);
    }
  }
  frames_.clear();
  frame_i_ = 0;
  dissolve_ = 0;
}

void PetWindow::LoadPrefs() {
  std::ifstream in(JoinPath(NativeDataDir(), L"pet.json"));
  if (!in) {
    return;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string json = ss.str();
  auto field = [&](const char* key) -> std::string {
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
    std::string v;
    while (p < json.size() && json[p] != '"') {
      if (json[p] == '\\' && p + 1 < json.size()) {
        ++p;
      }
      v.push_back(json[p++]);
    }
    return v;
  };
  const std::string ch = field("character");
  const std::string pose = field("pose");
  if (!ch.empty()) {
    character_ = WidenUtf8(ch);
  }
  if (!pose.empty()) {
    pose_ = WidenUtf8(pose);
  }
}

void PetWindow::PersistPrefs() {
  std::ofstream out(JoinPath(NativeDataDir(), L"pet.json"), std::ios::binary);
  if (!out) {
    return;
  }
  out << "{\n  \"character\":\"" << NarrowUtf8(character_) << "\",\n  \"pose\":\""
      << NarrowUtf8(pose_) << "\"\n}\n";
}

void PetWindow::LoadCharacters() {
  character_ids_.clear();
  const std::wstring roots[] = {
      JoinPath(NativeDataDir(), L"pets"),
      JoinPath(ExeDir(), L"pets"),
      JoinPath(ExeDir(), L"assets\\pets"),
  };
  for (const auto& root : roots) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(root, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        continue;
      }
      if (fd.cFileName[0] == L'.') {
        continue;
      }
      // Character dirs contain pose subdirs or pngs; skip known pose names.
      const std::wstring name = fd.cFileName;
      if (name == L"idle" || name == L"walk" || name == L"sleep" ||
          name == L"wait" || name == L"play" || name == L"fall" ||
          name == L"hold" || name == L"trash") {
        continue;
      }
      character_ids_.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!character_ids_.empty()) {
      break;
    }
  }
  if (character_ids_.empty()) {
    character_ids_.push_back(L"default");
  }
  std::sort(character_ids_.begin(), character_ids_.end());
}

void PetWindow::LoadPosePacks() {
  pose_names_.clear();
  pose_files_.clear();
  const std::wstring roots[] = {
      JoinPath(NativeDataDir(), L"pets"),
      JoinPath(ExeDir(), L"pets"),
      JoinPath(ExeDir(), L"assets\\pets"),
  };
  const wchar_t* poses[] = {L"idle", L"walk", L"sleep", L"wait", L"play",
                            L"fall"};
  auto scan_pose_dir = [&](const std::wstring& dir, const wchar_t* pose) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      return;
    }
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        files.push_back(JoinPath(dir, fd.cFileName));
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!files.empty()) {
      std::sort(files.begin(), files.end());
      pose_names_.push_back(pose);
      pose_files_.push_back(std::move(files));
    }
  };
  for (const auto& root : roots) {
    // Prefer pets/<character>/<pose>/
    if (character_ != L"default") {
      const std::wstring char_root = JoinPath(root, character_);
      for (const wchar_t* pose : poses) {
        scan_pose_dir(JoinPath(char_root, pose), pose);
      }
      // Also pets/<character>_idle.png style
      if (pose_names_.empty()) {
        for (const wchar_t* pose : poses) {
          const std::wstring pattern =
              JoinPath(root, character_ + L"_" + pose + L"*.png");
          WIN32_FIND_DATAW fd{};
          HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
          if (h == INVALID_HANDLE_VALUE) {
            continue;
          }
          std::vector<std::wstring> files;
          do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
              files.push_back(JoinPath(root, fd.cFileName));
            }
          } while (FindNextFileW(h, &fd));
          FindClose(h);
          if (!files.empty()) {
            std::sort(files.begin(), files.end());
            pose_names_.push_back(pose);
            pose_files_.push_back(std::move(files));
          }
        }
      }
    }
    if (!pose_names_.empty()) {
      break;
    }
    // Flat pets/<pose>/
    for (const wchar_t* pose : poses) {
      scan_pose_dir(JoinPath(root, pose), pose);
    }
    if (!pose_names_.empty()) {
      break;
    }
  }
}

void PetWindow::SetCharacter(const std::wstring& character) {
  character_ = character;
  LoadPosePacks();
  if (!pose_names_.empty()) {
    SetPose(pose_names_[0]);
  }
  PersistPrefs();
}

void PetWindow::SetPose(const std::wstring& pose) {
  pose_ = pose;
  ClearFrames();
  std::vector<std::wstring> files;
  for (size_t i = 0; i < pose_names_.size(); ++i) {
    if (pose_names_[i] == pose) {
      files = pose_files_[i];
      break;
    }
  }
  if (files.empty()) {
    return;
  }
  Gdiplus::GdiplusStartupInput input;
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartup(&token, &input, nullptr);
  for (const auto& path : files) {
    if (frames_.size() >= 16) {
      break;
    }
    Gdiplus::Bitmap src(path.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) {
      continue;
    }
    int w = static_cast<int>(src.GetWidth());
    int h = static_cast<int>(src.GetHeight());
    if (w > 128) {
      h = h * 128 / w;
      w = 128;
    }
    Gdiplus::Bitmap scaled(w, h);
    Gdiplus::Graphics g(&scaled);
    g.DrawImage(&src, 0, 0, w, h);
    HBITMAP hb = nullptr;
    scaled.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hb);
    if (hb) {
      frames_.push_back(hb);
      sprite_w_ = w;
      sprite_h_ = h;
    }
  }
  Gdiplus::GdiplusShutdown(token);
  PersistPrefs();
  if (hwnd_) {
    SetWindowPos(hwnd_, nullptr, 0, 0, sprite_w_, sprite_h_,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, TRUE);
  }
}

void PetWindow::LoadFramePack() {
  LoadPrefs();
  LoadCharacters();
  bool known = false;
  for (const auto& id : character_ids_) {
    if (id == character_) {
      known = true;
      break;
    }
  }
  if (!known && !character_ids_.empty()) {
    character_ = character_ids_[0];
  }
  LoadPosePacks();
  if (!pose_names_.empty()) {
    bool pose_ok = false;
    for (const auto& p : pose_names_) {
      if (p == pose_) {
        pose_ok = true;
        break;
      }
    }
    SetPose(pose_ok ? pose_ : pose_names_[0]);
    return;
  }
  // Flat pets\*.png fallback (no pose subdirs).
  const std::wstring roots[] = {
      JoinPath(NativeDataDir(), L"pets"),
      JoinPath(ExeDir(), L"pets"),
      JoinPath(ExeDir(), L"assets\\pets"),
  };
  std::vector<std::wstring> files;
  for (const auto& root : roots) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(root, L"*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        files.push_back(JoinPath(root, fd.cFileName));
      }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!files.empty()) {
      break;
    }
  }
  std::sort(files.begin(), files.end());
  if (files.size() < 2) {
    return;
  }
  pose_files_ = {files};
  pose_names_ = {L"idle"};
  SetPose(L"idle");
}

void PetWindow::StartClimb() {
  if (!hwnd_) {
    return;
  }
  RECT self{};
  GetWindowRect(hwnd_, &self);
  const auto ledges = ListWindowLedges(hwnd_);
  if (ledges.empty()) {
    ShowToast(L"附近没有可攀的窗台");
    return;
  }
  // Prefer a ledge above the pet and horizontally overlapping; else nearest.
  const WindowLedge* pick = nullptr;
  int best_score = 0x7fffffff;
  for (const auto& L : ledges) {
    if (L.y >= self.top) {
      continue;
    }
    const int mid = self.left + (self.right - self.left) / 2;
    const int lx = L.x;
    const int rx = L.x + L.width;
    int dx = 0;
    if (mid < lx) {
      dx = lx - mid;
    } else if (mid > rx) {
      dx = mid - rx;
    }
    const int dy = self.top - L.y;
    const int score = dx * 2 + dy;
    if (score < best_score) {
      best_score = score;
      pick = &L;
    }
  }
  if (!pick) {
    pick = &ledges.front();
  }
  ledge_left_ = pick->x;
  ledge_right_ = pick->x + pick->width;
  ledge_top_ = pick->y;
  ledge_hwnd_ = pick->hwnd;
  climbing_ = true;
  on_ledge_ = false;
  walking_ = false;
  falling_ = false;
  climb_cooldown_ = 40;
  pos_x_ = static_cast<float>(self.left);
  pos_y_ = static_cast<float>(self.top);
  // Walk sideways toward the ledge while climbing.
  if (pos_x_ < static_cast<float>(ledge_left_)) {
    walk_dir_ = 1;
  } else if (pos_x_ + 40 > static_cast<float>(ledge_right_)) {
    walk_dir_ = -1;
  }
}

void PetWindow::StartWalk() {
  walking_ = true;
  falling_ = false;
  climbing_ = false;
  on_ledge_ = false;
  vel_x_ = 2.5f * static_cast<float>(walk_dir_);
  vel_y_ = 0.f;
  if (std::find(pose_names_.begin(), pose_names_.end(), L"walk") !=
      pose_names_.end()) {
    SetPose(L"walk");
  }
}

void PetWindow::StartFall() {
  falling_ = true;
  walking_ = false;
  vel_y_ = -2.f;
  vel_x_ = 0.f;
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  pos_x_ = static_cast<float>(rc.left);
  pos_y_ = static_cast<float>(rc.top);
  if (std::find(pose_names_.begin(), pose_names_.end(), L"fall") !=
      pose_names_.end()) {
    SetPose(L"fall");
  }
}

void PetWindow::ShowContextMenu(POINT screen) {
  HMENU menu = CreatePopupMenu();
  HMENU poses = CreatePopupMenu();
  for (size_t i = 0; i < pose_names_.size(); ++i) {
    const UINT flags =
        MF_STRING | (pose_ == pose_names_[i] ? MF_CHECKED : 0);
    AppendMenuW(poses, flags, static_cast<UINT>(100 + i),
                pose_names_[i].c_str());
  }
  if (!pose_names_.empty()) {
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(poses), L"姿态");
  }
  HMENU chars = CreatePopupMenu();
  for (size_t i = 0; i < character_ids_.size(); ++i) {
    const UINT flags =
        MF_STRING | (character_ == character_ids_[i] ? MF_CHECKED : 0);
    AppendMenuW(chars, flags, static_cast<UINT>(200 + i),
                character_ids_[i].c_str());
  }
  if (!character_ids_.empty()) {
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(chars), L"角色");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 1, L"走走");
  AppendMenuW(menu, MF_STRING, 4, L"攀上窗台");
  AppendMenuW(menu, MF_STRING, 2, L"跌落");
  AppendMenuW(menu, MF_STRING, 3, L"待机");
  SetForegroundWindow(hwnd_);
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0,
                     hwnd_, nullptr);
  DestroyMenu(menu);
  if (cmd == 1) {
    StartWalk();
  } else if (cmd == 4) {
    StartClimb();
  } else if (cmd == 2) {
    StartFall();
    climbing_ = false;
    on_ledge_ = false;
  } else if (cmd == 3) {
    walking_ = false;
    falling_ = false;
    climbing_ = false;
    on_ledge_ = false;
    if (!pose_names_.empty()) {
      SetPose(L"idle");
    }
  } else if (cmd >= 100 &&
             cmd < 100 + static_cast<int>(pose_names_.size())) {
    walking_ = false;
    falling_ = false;
    SetPose(pose_names_[static_cast<size_t>(cmd - 100)]);
  } else if (cmd >= 200 &&
             cmd < 200 + static_cast<int>(character_ids_.size())) {
    SetCharacter(character_ids_[static_cast<size_t>(cmd - 200)]);
  }
}

bool PetWindow::Create(HINSTANCE instance) {
  if (!g_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PetWindow::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);
    g_registered = true;
  }
  LoadPrefs();
  TryLoadSprite();
  LoadCharacters();
  LoadFramePack();
  const int ww = (sprite_ || !frames_.empty()) ? sprite_w_ : kSize;
  const int hh = (sprite_ || !frames_.empty()) ? sprite_h_ : kSize;
  const RECT work = PrimaryWorkArea();
  base_y_ = work.bottom - hh - 120;
  pos_x_ = static_cast<float>(work.right - ww - 40);
  pos_y_ = static_cast<float>(base_y_);
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName, L"Pet",
      WS_POPUP, static_cast<int>(pos_x_), base_y_, ww, hh, nullptr, nullptr,
      instance, this);
  if (!hwnd_) {
    return false;
  }
  if (sprite_ || !frames_.empty()) {
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
  } else {
    SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), 0, LWA_COLORKEY);
  }
  SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  EnsureDropTarget();
  CreateBuoy(instance);
  CreatePages(instance);
  return true;
}

void PetWindow::CreateBuoy(HINSTANCE instance) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PetWindow::BuoyProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = L"DeskTidyNativePetBuoy";
    RegisterClassExW(&wc);
    registered = true;
  }
  const RECT work = PrimaryWorkArea();
  buoy_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
      L"DeskTidyNativePetBuoy", L"PetBuoy", WS_POPUP, work.left + 12,
      work.bottom - 44, 132, 28, nullptr, nullptr, instance, this);
  if (!buoy_) {
    return;
  }
  SetLayeredWindowAttributes(buoy_, 0, 220, LWA_ALPHA);
  SetWindowLongPtrW(buoy_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

void PetWindow::SetPages(const std::vector<std::wstring>& names, int current) {
  page_names_ = names;
  current_page_ = current;
  if (pages_) {
    InvalidateRect(pages_, nullptr, FALSE);
    PlacePages();
  }
}

void PetWindow::CreatePages(HINSTANCE instance) {
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PetWindow::PagesProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = L"DeskTidyNativePetPages";
    RegisterClassExW(&wc);
    registered = true;
  }
  pages_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
      L"DeskTidyNativePetPages", L"PetPages", WS_POPUP, 0, 0, 80, 26, nullptr,
      nullptr, instance, this);
  if (!pages_) {
    return;
  }
  SetLayeredWindowAttributes(pages_, 0, 230, LWA_ALPHA);
  SetWindowLongPtrW(pages_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

void PetWindow::PlacePages() {
  if (!pages_ || !hwnd_) {
    return;
  }
  if (!IsWindowVisible(hwnd_) || page_names_.empty()) {
    ShowWindow(pages_, SW_HIDE);
    return;
  }
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  const int n = (std::min)(static_cast<int>(page_names_.size()), 8);
  const int w = n * 36 + 8;
  const int h = 26;
  SetWindowPos(pages_, nullptr, rc.left, rc.top - h - 6, w, h,
               SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
}

void PetWindow::PaintPages(HDC hdc) {
  RECT rc{};
  GetClientRect(pages_, &rc);
  HBRUSH bg = CreateSolidBrush(RGB(28, 32, 40));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  SetBkMode(hdc, TRANSPARENT);
  const int n = (std::min)(static_cast<int>(page_names_.size()), 8);
  for (int i = 0; i < n; ++i) {
    RECT r{i * 36 + 4, 3, (i + 1) * 36, rc.bottom - 3};
    HBRUSH b = CreateSolidBrush(i == current_page_ ? RGB(70, 130, 200)
                                                   : RGB(55, 70, 90));
    FillRect(hdc, &r, b);
    DeleteObject(b);
    std::wstring label = std::to_wstring(i + 1);
    if (static_cast<size_t>(i) < page_names_.size() &&
        !page_names_[static_cast<size_t>(i)].empty()) {
      label = page_names_[static_cast<size_t>(i)].substr(0, 2);
    }
    SetTextColor(hdc, RGB(240, 240, 240));
    DrawTextW(hdc, label.c_str(), -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }
}

LRESULT CALLBACK PetWindow::PagesProc(HWND hwnd, UINT msg, WPARAM wparam,
                                      LPARAM lparam) {
  auto* self =
      reinterpret_cast<PetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    self->PaintPages(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_LBUTTONUP) {
    const int index = GET_X_LPARAM(lparam) / 36;
    if (index >= 0 && index < static_cast<int>(self->page_names_.size()) &&
        self->on_page_) {
      self->on_page_(index);
    }
    return 0;
  }
  if (msg == WM_DESTROY) {
    self->pages_ = nullptr;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void PetWindow::PaintBuoy(HDC hdc) {
  RECT rc{};
  GetClientRect(buoy_, &rc);
  HBRUSH bg = CreateSolidBrush(RGB(28, 32, 40));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  const wchar_t* labels[] = {L"走", L"爬", L"落"};
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(230, 230, 230));
  for (int i = 0; i < 3; ++i) {
    RECT r{i * 44 + 2, 2, (i + 1) * 44 - 2, rc.bottom - 2};
    HBRUSH b = CreateSolidBrush(RGB(55, 70, 90));
    FillRect(hdc, &r, b);
    DeleteObject(b);
    DrawTextW(hdc, labels[i], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
}

LRESULT CALLBACK PetWindow::BuoyProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  auto* self =
      reinterpret_cast<PetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    self->PaintBuoy(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_LBUTTONUP) {
    const int x = GET_X_LPARAM(lparam);
    if (x < 44) {
      self->StartWalk();
    } else if (x < 88) {
      self->StartClimb();
    } else {
      self->StartFall();
    }
    return 0;
  }
  if (msg == WM_DESTROY) {
    self->buoy_ = nullptr;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void PetWindow::EnsureDropTarget() {
  if (!hwnd_ || drop_target_) {
    return;
  }
  drop_target_ =
      new PathDropTarget([this](const std::vector<std::wstring>& paths) {
        if (on_trash_) {
          on_trash_(paths);
        }
      });
  if (!RegisterPathDropTarget(hwnd_, drop_target_)) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
}

void PetWindow::Destroy() {
  if (buoy_) {
    DestroyWindow(buoy_);
    buoy_ = nullptr;
  }
  if (pages_) {
    DestroyWindow(pages_);
    pages_ = nullptr;
  }
  if (hwnd_) {
    RevokePathDropTarget(hwnd_);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (drop_target_) {
    drop_target_->Release();
    drop_target_ = nullptr;
  }
  if (sprite_) {
    DeleteObject(sprite_);
    sprite_ = nullptr;
  }
  ClearFrames();
}

void PetWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
  if (buoy_) {
    ShowWindow(buoy_, SW_SHOWNOACTIVATE);
    AttachOverlayToDesktop(buoy_, true);
    PlaceOverlayInDesktopBand(buoy_, true);
    RaiseOverlayInDesktopBand(buoy_);
  }
  if (pages_) {
    ShowWindow(pages_, SW_SHOWNOACTIVATE);
    AttachOverlayToDesktop(pages_, true);
    PlaceOverlayInDesktopBand(pages_, true);
    RaiseOverlayInDesktopBand(pages_);
    PlacePages();
  }
}

void PetWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
  }
  if (buoy_) {
    ShowWindow(buoy_, SW_HIDE);
  }
  if (pages_) {
    ShowWindow(pages_, SW_HIDE);
  }
}

void PetWindow::SetVisible(bool on) {
  if (on) {
    Show();
  } else {
    Hide();
  }
}

void PetWindow::AttachToDesktopLayer() {
  if (!hwnd_) {
    return;
  }
  AttachOverlayToDesktop(hwnd_, true);
  PlaceOverlayInDesktopBand(hwnd_, true);
  RaiseOverlayInDesktopBand(hwnd_);
}

void PetWindow::Tick() {
  if (!hwnd_) {
    return;
  }
  PlacePages();
  if (!IsWindowVisible(hwnd_) || dragging_) {
    return;
  }
  if (frames_.size() >= 2) {
    dissolve_ += 18;
    if (dissolve_ >= 255) {
      dissolve_ = 0;
      frame_i_ = (frame_i_ + 1) % static_cast<int>(frames_.size());
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  const RECT work = PrimaryWorkArea();
  const int ww = sprite_w_ > 0 ? sprite_w_ : kSize;
  const int hh = sprite_h_ > 0 ? sprite_h_ : kSize;

  if (climb_cooldown_ > 0) {
    --climb_cooldown_;
  }

  if (climbing_) {
    pos_y_ -= 3.f;
    pos_x_ += 1.2f * static_cast<float>(walk_dir_);
    if (pos_x_ < static_cast<float>(ledge_left_)) {
      pos_x_ = static_cast<float>(ledge_left_);
    }
    if (pos_x_ + ww > static_cast<float>(ledge_right_)) {
      pos_x_ = static_cast<float>(ledge_right_ - ww);
    }
    if (pos_y_ <= static_cast<float>(ledge_top_ - hh)) {
      pos_y_ = static_cast<float>(ledge_top_ - hh);
      climbing_ = false;
      on_ledge_ = true;
      walking_ = true;
      vel_x_ = 2.2f * static_cast<float>(walk_dir_);
    }
    SetWindowPos(hwnd_, nullptr, static_cast<int>(pos_x_),
                 static_cast<int>(pos_y_), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return;
  }

  if (on_ledge_) {
    if (ledge_hwnd_ && !IsWindow(ledge_hwnd_)) {
      on_ledge_ = false;
      climbing_ = false;
      StartFall();
      return;
    }
    if (ledge_hwnd_) {
      RECT lr{};
      if (GetWindowRect(ledge_hwnd_, &lr)) {
        ledge_top_ = lr.top;
        ledge_left_ = lr.left + 12;
        ledge_right_ = lr.right - 12;
      }
    }
    pos_x_ += vel_x_;
    if (pos_x_ < static_cast<float>(ledge_left_) - 4 ||
        pos_x_ + ww > static_cast<float>(ledge_right_) + 4) {
      on_ledge_ = false;
      StartFall();
      return;
    }
    pos_y_ = static_cast<float>(ledge_top_ - hh);
    SetWindowPos(hwnd_, nullptr, static_cast<int>(pos_x_),
                 static_cast<int>(pos_y_), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return;
  }

  if (falling_) {
    vel_y_ += 0.55f;
    pos_y_ += vel_y_;
    pos_x_ += vel_x_;
    if (pos_y_ >= static_cast<float>(base_y_)) {
      pos_y_ = static_cast<float>(base_y_);
      falling_ = false;
      vel_y_ = 0.f;
      if (std::find(pose_names_.begin(), pose_names_.end(), L"idle") !=
          pose_names_.end()) {
        SetPose(L"idle");
      }
    }
    SetWindowPos(hwnd_, nullptr, static_cast<int>(pos_x_),
                 static_cast<int>(pos_y_), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return;
  }

  if (walking_) {
    pos_x_ += vel_x_;
    if (pos_x_ < static_cast<float>(work.left) ||
        pos_x_ + static_cast<float>(ww) > static_cast<float>(work.right)) {
      walk_dir_ = -walk_dir_;
      vel_x_ = 2.5f * static_cast<float>(walk_dir_);
      pos_x_ = (std::max)(static_cast<float>(work.left),
                          (std::min)(pos_x_, static_cast<float>(work.right - ww)));
    }
    // Auto-climb when walking under a nearby ledge.
    if (climb_cooldown_ == 0) {
      const auto ledges = ListWindowLedges(hwnd_, 8);
      const int mid = static_cast<int>(pos_x_) + ww / 2;
      for (const auto& L : ledges) {
        if (L.y >= base_y_ - 20) {
          continue;
        }
        if (mid >= L.x && mid <= L.x + L.width && base_y_ - L.y < 220) {
          StartClimb();
          return;
        }
      }
      climb_cooldown_ = 20;
    }
    bob_ = (bob_ + 1) % 40;
    const int dy = (bob_ < 20) ? (bob_ / 4) : ((40 - bob_) / 4);
    SetWindowPos(hwnd_, nullptr, static_cast<int>(pos_x_), base_y_ - dy, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return;
  }

  bob_ = (bob_ + 1) % 40;
  const int dy = (bob_ < 20) ? (bob_ / 4) : ((40 - bob_) / 4);
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  pos_x_ = static_cast<float>(rc.left);
  SetWindowPos(hwnd_, nullptr, rc.left, base_y_ - dy, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  (void)hh;
}

LRESULT CALLBACK PetWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam) {
  PetWindow* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<PetWindow*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    self->hwnd_ = hwnd;
  } else {
    self = reinterpret_cast<PetWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleMessage(msg, wparam, lparam);
}

LRESULT PetWindow::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      HDC hdc = BeginPaint(hwnd_, &ps);
      Paint(hdc);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      dragging_ = true;
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(hwnd_, &pt);
      RECT rc{};
      GetWindowRect(hwnd_, &rc);
      drag_offset_.x = pt.x - rc.left;
      drag_offset_.y = pt.y - rc.top;
      SetCapture(hwnd_);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (dragging_ && (wparam & MK_LBUTTON)) {
        POINT pt{};
        GetCursorPos(&pt);
        SetWindowPos(hwnd_, nullptr, pt.x - drag_offset_.x,
                     pt.y - drag_offset_.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        base_y_ = pt.y - drag_offset_.y;
        PlacePages();
      }
      return 0;
    case WM_LBUTTONUP: {
      dragging_ = false;
      ReleaseCapture();
      RECT rc{};
      GetWindowRect(hwnd_, &rc);
      base_y_ = rc.top;
      pos_x_ = static_cast<float>(rc.left);
      pos_y_ = static_cast<float>(rc.top);
      walking_ = false;
      falling_ = false;
      climbing_ = false;
      on_ledge_ = false;
      AttachToDesktopLayer();
      return 0;
    }
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ClientToScreen(hwnd_, &pt);
      ShowContextMenu(pt);
      return 0;
    }
    case WM_DESTROY:
      if (drop_target_) {
        RevokePathDropTarget(hwnd_);
        drop_target_->Release();
        drop_target_ = nullptr;
      }
      hwnd_ = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void PetWindow::Paint(HDC hdc) {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  if (frames_.size() >= 2) {
    const int a = frame_i_;
    const int b = (frame_i_ + 1) % static_cast<int>(frames_.size());
    const int w = sprite_w_;
    const int h = sprite_h_;
    auto blit = [&](HBITMAP bmp, BYTE alpha) {
      HDC mem = CreateCompatibleDC(hdc);
      HGDIOBJ old = SelectObject(mem, bmp);
      BLENDFUNCTION bf{};
      bf.BlendOp = AC_SRC_OVER;
      bf.SourceConstantAlpha = alpha;
      AlphaBlend(hdc, 0, 0, w, h, mem, 0, 0, w, h, bf);
      SelectObject(mem, old);
      DeleteDC(mem);
    };
    blit(frames_[static_cast<size_t>(a)],
         static_cast<BYTE>(255 - dissolve_));
    blit(frames_[static_cast<size_t>(b)], static_cast<BYTE>(dissolve_));
    return;
  }
  if (sprite_) {
    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mem, sprite_);
    BitBlt(hdc, 0, 0, sprite_w_, sprite_h_, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    return;
  }
  HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0));
  FillRect(hdc, &rc, clear);
  DeleteObject(clear);

  HBRUSH body = CreateSolidBrush(RGB(120, 200, 140));
  HPEN pen = CreatePen(PS_SOLID, 2, RGB(40, 80, 50));
  HGDIOBJ old_b = SelectObject(hdc, body);
  HGDIOBJ old_p = SelectObject(hdc, pen);
  Ellipse(hdc, 8, 10, rc.right - 8, rc.bottom - 6);
  SelectObject(hdc, old_b);
  SelectObject(hdc, old_p);
  DeleteObject(body);
  DeleteObject(pen);

  HBRUSH eye = CreateSolidBrush(RGB(20, 20, 20));
  SelectObject(hdc, eye);
  Ellipse(hdc, 24, 28, 34, 38);
  Ellipse(hdc, 40, 28, 50, 38);
  DeleteObject(eye);
}
