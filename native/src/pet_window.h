#pragma once

#include "ole_drag.h"
#include "pet_ledges.h"

#include <Windows.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

// Desktop-band pet with optional character/pose packs and light physics.
class PetWindow {
 public:
  // Return true when paths were recycled (OLE may advertise MOVE).
  using TrashFn = std::function<bool(const std::vector<std::wstring>&)>;
  using PageFn = std::function<void(int)>;
  using VoidFn = std::function<void()>;

  bool Create(HINSTANCE instance);
  void Destroy();
  void Show();
  void Hide();
  void SetVisible(bool on);
  HWND Hwnd() const { return hwnd_; }
  HWND PagesHwnd() const { return pages_; }
  HWND MenuHwnd() const { return menu_; }
  void AttachToDesktopLayer();
  // Re-upload ULW after SoftSeat/Raise (same cure as FenceWindow::RefreshLayer).
  void RefreshLayer();
  void Tick();
  // P2-24: 33ms when active/hovered, 100ms when idle pose and pointer away.
  int WantedTickMs() const;
  void StartClimb();
  void SetOnTrash(TrashFn fn) { on_trash_ = std::move(fn); }
  void SetOnPage(PageFn fn) { on_page_ = std::move(fn); }
  // Right-click page chip → create fence on that page (Python create_fence_requested).
  void SetOnCreateFence(PageFn fn) { on_create_fence_ = std::move(fn); }
  void SetOnHide(VoidFn fn) { on_hide_ = std::move(fn); }
  // Push sprite below overlapping fence bottoms (P1-17).
  void NudgeClearOfFenceBottoms(const std::vector<RECT>& fence_screen_rects);
  // Float-bar tools hosted on the pet (Python: pages + tools on pet).
  void SetOnTool(std::function<void(const std::wstring&)> fn) {
    on_tool_ = std::move(fn);
  }
  // Python float_bar_tool_flags: record / minutes / calculator / todo / vault.
  void SetToolFlags(bool record, bool minutes, bool calculator, bool todo,
                    bool vault = false);
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
  // Drag hot-path: re-ULW with the last present DIB (no Paint/premultiply).
  void PresentMoveOnly();
  void ReleasePresentBuffer();
  bool EnsurePresentBuffer(int w, int h);
  // Cached left-facing flip of the current sprite frame (avoid
  // CreateDIBSection×2 + per-pixel flip every Paint when facing left).
  HDC EnsureFlipBuffer(HDC hdc, HBITMAP bmp, int bw, int bh);
  void ReleaseFlipBuffer();
  // Mirror frames_pargb_[idx] into frames_pargb_flip_ (lazy, once per cel).
  void EnsureFlippedPargbCel(size_t idx);
  // Rebuild + apply the sprite-shaped hit region (cached per frame).
  void ApplyHitRegion(const std::vector<BYTE>& alpha, int bw, int bh, int cw,
                      int ch);
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
  // Integer cell multiplier for pixehood (crisp nearest upscale).
  int PixelDisplayMul() const;
  // Ground clearance: DPI-scaled floor + sprite fraction (display-adaptivity).
  int GroundMarginPx() const;
  // Bubble-to-head gap so the pointer tip never sits on hair.
  int PageHeadClearancePx(int pet_h) const;
  void ApplyScaledWindowSize();
  void EnterHappy(DWORD now);
  void EnterSleep(DWORD now);
  void EnterWait(DWORD now);
  void ScheduleIdle(DWORD now);
  void ChooseNextAfterIdle(DWORD now);
  void RunAi(DWORD now);
  void SpawnHearts(int count);
  // Speech bubble above the pet (not the screen-top toast strip).
  void Say(const char* action);
  void AdvanceFrames(DWORD elapsed_ms, DWORD now, bool* frame_changed);
  void StepPhysics(DWORD elapsed_ms);
  // Drag chrome: Detach+TOPMOST while moving (Progman-owned blanks mid-drag);
  // SoftSeat + double Present on drop (no SetWindowRgn).
  void BeginDragFloat();
  void EndDragFloat();
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
  // P2-22: continuous anim clock for breath/sway/FX (ticks forward every Tick).
  int anim_tick_ = 0;
  int base_y_ = 0;
  bool dragging_ = false;
  // Pose at drag start — restore after gentle drop (sleep keeps tree).
  std::wstring pose_before_drag_;
  // Sleep ping-pong direction (+1 / -1) so tree cels don't hard-cut 7→0.
  int sleep_anim_dir_ = 1;
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
  PageFn on_create_fence_;
  VoidFn on_hide_;
  std::function<void(const std::wstring&)> on_tool_;
  std::function<void(const std::wstring&)> on_folder_;
  HWND menu_ = nullptr;
  bool menu_open_ = false;
  int pose_hold_ticks_ = 0;
  std::vector<std::wstring> page_names_;
  std::vector<ChromeItem> chrome_;
  std::vector<FolderChip> folders_;
  int chrome_hover_ = -1;
  int current_page_ = 0;
  bool tool_record_ = false;
  bool tool_minutes_ = false;
  bool tool_calculator_ = false;
  bool tool_todo_ = false;
  bool tool_vault_ = false;
  bool auto_wander_ = true;
  bool follow_cursor_ = false;
  bool show_page_bubbles_ = true;
  float size_scale_ = 1.f;
  // character id → enabled action ids (pet/wait/sleep/fall).
  std::map<std::wstring, std::vector<std::string>> enabled_actions_;
  HBITMAP sprite_ = nullptr;
  int sprite_w_ = 0;
  // Base (idle) sprite height captured at load, kept stable across poses so the
  // fence-nudge margin matches Python's fixed _sprite_h (P1-17).
  int base_sprite_h_ = 0;
  // Cached hit-test region built from the sprite's own PNG alpha (P0-12).
  // HBITMAP round-trips lose alpha, so masks are captured at load time.
  HRGN mask_rgn_ = nullptr;
  int mask_w_ = 0;
  int mask_h_ = 0;
  int mask_facing_ = 1;
  int mask_index_ = -1;
  int mask_gen_ = -1;
  int mask_gen_counter_ = 0;
  std::vector<std::vector<BYTE>> frames_alpha_;
  // Full BGRA PARGB cels (HBITMAP round-trips drop alpha → ULW blank pet).
  std::vector<std::vector<BYTE>> frames_pargb_;
  // Horizontal mirrors of frames_pargb_ (lazy); avoids GDI+ flip every Paint.
  std::vector<std::vector<BYTE>> frames_pargb_flip_;
  std::vector<BYTE> sprite_alpha_;
  std::vector<BYTE> sprite_pargb_;
  std::vector<BYTE> sprite_pargb_flip_;
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
  // Cached window-ledge scan (ListWindowLedges → EnumWindows, ~1Hz). TTL-cache
  // so climb probing reuses the last scan instead of re-enumerating every tick.
  std::vector<WindowLedge> ledge_cache_;
  ULONGLONG ledge_cache_ms_ = 0;
  int walk_dir_ = 1;
  // 1 = face right (default art), -1 = face left (mirrored) — P1-14.
  int facing_ = 1;
  HWND buoy_ = nullptr;
  HWND pages_ = nullptr;
  // ---- P1/P3/P4/P5/P6 additions ----
  DWORD last_tick_ms_ = 0;
  DWORD anim_accum_ms_ = 0;
  DWORD last_soft_present_ms_ = 0;
  bool present_dirty_ = false;
  bool chrome_dirty_ = false;
  // Reused ULW DIB (Fences-style layer buffer; avoid CreateDIBSection every tick).
  HDC present_dc_ = nullptr;
  HBITMAP present_dib_ = nullptr;
  HGDIOBJ present_old_ = nullptr;
  void* present_bits_ = nullptr;
  int present_buf_w_ = 0;
  int present_buf_h_ = 0;
  // True after a full Present filled present_bits_ for current size/frame/facing.
  bool present_content_valid_ = false;
  int present_frame_i_ = -1;
  int present_facing_ = 0;
  int present_anim_tick_ = -1;
  HCURSOR drag_size_cursor_ = nullptr;
  // Left-facing flip cache (dst DIB held selected in flip_dc_).
  HDC flip_dc_ = nullptr;
  HBITMAP flip_dib_ = nullptr;
  HGDIOBJ flip_old_ = nullptr;
  void* flip_bits_ = nullptr;
  HBITMAP flip_src_ = nullptr;  // source frame the cache was built from
  int flip_w_ = 0;
  int flip_h_ = 0;
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
