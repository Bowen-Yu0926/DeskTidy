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

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  void AttachToDesktopLayer();
  void Tick();
  void SetOnTrash(TrashFn fn) { on_trash_ = std::move(fn); }

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
  void StartWalk();
  void PersistPrefs();
  void LoadPrefs();

  HWND hwnd_ = nullptr;
  int bob_ = 0;
  int base_y_ = 0;
  bool dragging_ = false;
  POINT drag_offset_{};
  PathDropTarget* drop_target_ = nullptr;
  TrashFn on_trash_;
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
  int walk_dir_ = 1;
};
