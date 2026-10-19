#pragma once

#include "ole_drag.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

// Desktop-band pet with optional character/pose packs and light physics.
class PetWindow {
 public:
  using TrashFn = std::function<void(const std::vector<std::wstring>&)>;
  using PageFn = std::function<void(int)>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  void AttachToDesktopLayer();
  void Tick();
  void StartClimb();
  void SetOnTrash(TrashFn fn) { on_trash_ = std::move(fn); }
  void SetOnPage(PageFn fn) { on_page_ = std::move(fn); }
  void SetPages(const std::vector<std::wstring>& names, int current);

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void EnsureDropTarget();
  void TryLoadSprite();
  void LoadFramePack();
  void LoadPosePacks();
  void LoadCharacters();
  void SetCharacter(const std::wstring& character);
  void SetPose(const std::wstring& pose);
  void ClearFrames();
  void ShowContextMenu(POINT screen);
  void StartFall();
  void StartThrow(float vx, float vy);
  void StartWalk();
  void PushDragSample(POINT screen);
  void CreateBuoy(HINSTANCE instance);
  void CreatePages(HINSTANCE instance);
  void PlacePages();
  static LRESULT CALLBACK BuoyProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK PagesProc(HWND, UINT, WPARAM, LPARAM);
  void PaintBuoy(HDC hdc);
  void PaintPages(HDC hdc);
  void PersistPrefs();
  void LoadPrefs();

  HWND hwnd_ = nullptr;
  int bob_ = 0;
  int base_y_ = 0;
  bool dragging_ = false;
  POINT drag_offset_{};
  struct DragSample {
    DWORD tick = 0;
    POINT pt{};
  };
  DragSample drag_samples_[8]{};
  int drag_sample_n_ = 0;
  int drag_sample_i_ = 0;
  PathDropTarget* drop_target_ = nullptr;
  TrashFn on_trash_;
  PageFn on_page_;
  std::vector<std::wstring> page_names_;
  int current_page_ = 0;
  HBITMAP sprite_ = nullptr;
  int sprite_w_ = 0;
  int sprite_h_ = 0;
  std::vector<HBITMAP> frames_;
  int frame_i_ = 0;
  int dissolve_ = 0;
  std::wstring character_ = L"default";
  std::wstring pose_ = L"idle";
  std::vector<std::wstring> character_ids_;
  std::vector<std::wstring> pose_names_;
  std::vector<std::vector<std::wstring>> pose_files_;
  // Physics
  float vel_x_ = 0.f;
  float vel_y_ = 0.f;
  float pos_x_ = 0.f;
  float pos_y_ = 0.f;
  bool falling_ = false;
  bool walking_ = false;
  bool climbing_ = false;
  bool on_ledge_ = false;
  int ledge_left_ = 0;
  int ledge_right_ = 0;
  int ledge_top_ = 0;
  HWND ledge_hwnd_ = nullptr;
  int climb_cooldown_ = 0;
  int walk_dir_ = 1;
  HWND buoy_ = nullptr;
  HWND pages_ = nullptr;
};
