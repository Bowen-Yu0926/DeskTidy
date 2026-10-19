#include "pet_window.h"

#include "desktop_shell.h"
#include "display_geom.h"
#include "paths_util.h"
#include "tool_paths.h"

#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
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

void PetWindow::LoadFramePack() {
  const std::wstring roots[] = {
      JoinPath(NativeDataDir(), L"pets"),
      JoinPath(ExeDir(), L"pets"),
      JoinPath(ExeDir(), L"assets\\pets"),
  };
  std::vector<std::wstring> files;
  for (const auto& root : roots) {
    const std::wstring pattern = JoinPath(root, L"*.png");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
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
  TryLoadSprite();
  LoadFramePack();
  const int ww = (sprite_ || !frames_.empty()) ? sprite_w_ : kSize;
  const int hh = (sprite_ || !frames_.empty()) ? sprite_h_ : kSize;
  const RECT work = PrimaryWorkArea();
  base_y_ = work.bottom - hh - 120;
  hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kClassName, L"Pet",
      WS_POPUP, work.right - ww - 40, base_y_, ww, hh, nullptr, nullptr,
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
  return true;
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
  for (auto* hb : frames_) {
    if (hb) {
      DeleteObject(hb);
    }
  }
  frames_.clear();
}

void PetWindow::Show() {
  if (!hwnd_) {
    return;
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  AttachToDesktopLayer();
}

void PetWindow::Hide() {
  if (hwnd_) {
    ShowWindow(hwnd_, SW_HIDE);
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
  if (!hwnd_ || !IsWindowVisible(hwnd_) || dragging_) {
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
  bob_ = (bob_ + 1) % 40;
  const int dy = (bob_ < 20) ? (bob_ / 4) : ((40 - bob_) / 4);
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  SetWindowPos(hwnd_, nullptr, rc.left, base_y_ - dy, 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
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
      }
      return 0;
    case WM_LBUTTONUP:
      dragging_ = false;
      ReleaseCapture();
      AttachToDesktopLayer();
      return 0;
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
