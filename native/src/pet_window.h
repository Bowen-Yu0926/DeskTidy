#pragma once

#include "ole_drag.h"

#include <Windows.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

// Desktop-band pet with optional character/pose packs and light physics.
class PetWindow {
 public:
  using TrashFn = std::function<void(const std::vector<std::wstring>&)>;
  using PageFn = std::function<void(int)>;
  using VoidFn = std::function<void()>;

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
  void SetOnHide(VoidFn fn) { on_hide_ = std::move(fn); }
  // Float-bar tools hosted on the pet (Python: pages + tools on pet).
  void SetOnTool(std::function<void(const std::wstring&)> fn) {
    on_tool_ = std::move(fn);
  }
  // Python float_bar_tool_flags: record / minutes / calculator / todo.
  void SetToolFlags(bool record, bool minutes, bool calculator, bool todo);
  void SetPages(const std::vector<std::wstring>& names, int current);
  struct FolderChip {
    std::wstring name;
    std::wstring path;
  };
  void SetFolders(const std::vector<FolderChip>& folders);
  void SetOnFolder(std::function<void(const std::wstring&)> fn) {
    on_folder_ = std::move(fn);
  }
  // Settings parity with Python desktop_pet.
  void SetAutoWander(bool on);
  bool AutoWander() const { return auto_wander_; }
  void SetShowPageBubbles(bool on);
  bool ShowPageBubbles() const { return show_page_bubbles_; }
  void SetSizeScale(float scale);
  float SizeScale() const { return size_scale_; }
  const std::wstring& Character() const { return character_; }
  const std::vector<std::wstring>& Characters() const { return character_ids_; }
  void ApplyCharacter(const std::wstring& id) { SetCharacter(id); }
  // Per-character optional ⋯ actions (Python pet_enabled_actions). pet/fall always on.
  bool ActionEnabled(const std::wstring& character, const std::string& action) const;
  std::vector<std::string> EnabledActions(const std::wstring& character) const;
  void SetEnabledActions(const std::wstring& character,
                         const std::vector<std::string>& actions);
  // Settings toggles only (excludes always-on pet/fall).
  std::vector<std::pair<std::string, std::wstring>> SettingsActionOptions(
      const std::wstring& character) const;

 private:
  struct ChromeItem {
    enum class Kind { Page, Folder, Tool };
    Kind kind = Kind::Page;
    int page_index = 0;
    std::wstring label;
    std::wstring tool_id;  // record / minutes / calculator / todo
    std::wstring folder_path;
  };
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  void Paint(HDC hdc);
  void Present();
  void EnsureDropTarget();
  void TryLoadSprite();
  void LoadFramePack();
  void LoadPosePacks();
  void LoadCharacters();
  void SetCharacter(const std::wstring& character);
  void SetPose(const std::wstring& pose);
  void ClearFrames();
  // P1/P3/P4/P5/P6 helpers
  bool HasPose(const std::wstring& pose) const;
  int FrameIntervalMs(const std::wstring& pose) const;
  int ScaledW() const;
  int ScaledH() const;
  void ApplyScaledWindowSize();
  void EnterHappy(DWORD now);
  void EnterSleep(DWORD now);
  void EnterWait(DWORD now);
  void ScheduleIdle(DWORD now);
  void ChooseNextAfterIdle(DWORD now);
  void RunAi(DWORD now);
  void SpawnHearts(int count);
  void AdvanceFrames(DWORD elapsed_ms, DWORD now, bool* frame_changed);
  void StepPhysics(DWORD elapsed_ms);
  void EndDrag(POINT release);
  void RebaseFloor();
  bool RectVisibleOnAnyMonitor(const RECT& rc) const;
  void RestorePosition();
  void ShowContextMenu(POINT screen);
  void StartFall();
  void StartThrow(float vx, float vy);
  void StartWalk();
  void PushDragSample(POINT screen);
  void CreateBuoy(HINSTANCE instance);
  void CreatePages(HINSTANCE instance);
  void CreateMenu(HINSTANCE instance);
  void PlacePages();
  void PlaceMenu();
  void PaintMenu(HDC hdc);
  void RunMenuAction(int index);
  static LRESULT CALLBACK MenuProc(HWND, UINT, WPARAM, LPARAM);
  void RebuildChromeItems();
  void LayoutChrome(std::vector<RECT>* out_rects, int* out_w, int* out_h) const;
  int HitTestChrome(int local_x, int local_y) const;
  void ActivateChrome(int index);
  static LRESULT CALLBACK BuoyProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK PagesProc(HWND, UINT, WPARAM, LPARAM);
  void PaintBuoy(HDC hdc);
  void PaintPages(HDC hdc);
  void PersistPrefs();
  void LoadPrefs();
  void EnsureDefaultActions(const std::wstring& character);
  std::vector<int> VisibleMenuIndices() const;
  static std::vector<std::wstring> PetAssetRoots();

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
  VoidFn on_hide_;
  std::function<void(const std::wstring&)> on_tool_;
  std::function<void(const std::wstring&)> on_folder_;
  HWND menu_ = nullptr;
  bool menu_open_ = false;
  int pose_hold_ticks_ = 0;
  std::vector<std::wstring> page_names_;
  std::vector<ChromeItem> chrome_;
  std::vector<FolderChip> folders_;
  int current_page_ = 0;
  bool tool_record_ = false;
  bool tool_minutes_ = false;
  bool tool_calculator_ = false;
  bool tool_todo_ = false;
  bool auto_wander_ = true;
  bool show_page_bubbles_ = true;
  float size_scale_ = 1.f;
  // character id → enabled action ids (pet/wait/sleep/fall).
  std::map<std::wstring, std::vector<std::string>> enabled_actions_;
  HBITMAP sprite_ = nullptr;
  int sprite_w_ = 0;
  int sprite_h_ = 0;
  std::vector<HBITMAP> frames_;
  int frame_i_ = 0;
  int dissolve_ = 0;
  std::wstring character_ = L"hoodie";
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
  // ---- P1/P3/P4/P5/P6 additions ----
  DWORD last_tick_ms_ = 0;
  DWORD anim_accum_ms_ = 0;
  bool present_dirty_ = false;
  bool chrome_dirty_ = false;
  // Click vs drag (P3/P5)
  bool press_captured_ = false;
  POINT press_origin_{};
  // Mood / autonomous behavior (P3/P4)
  float mood_ = 50.f;
  DWORD last_interact_ms_ = 0;
  DWORD state_until_ms_ = 0;
  bool happy_active_ = false;
  int rest_flip_ = 0;
  bool post_trash_wait_ = false;
  bool cursor_was_near_ = false;
  struct Heart {
    float fx = 0.f;
    float fy = 0.f;
    DWORD born = 0;
  };
  std::vector<Heart> hearts_;
  // Position persistence / DPI (P2/P6)
  bool has_saved_pos_ = false;
  int current_dpi_ = 0;
};
