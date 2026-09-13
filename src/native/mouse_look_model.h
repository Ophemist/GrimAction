#pragma once

// Pure mouse-look arbitration and cursor/yaw arithmetic (IMPLEMENTATION_PLAN.md section 7). Free of
// Win32 like the collision and shoulder models: the runtime supplies cursor, window and game state and
// applies the decision, so every priority rule and every re-anchor is provable offline.
//
// Priority, fixed by the player: not eligible > Left Alt (hard override) > menu open and not in combat >
// captured. While captured the cursor is held at the aim point; horizontal movement becomes camera yaw
// and vertical movement slides the aim point within a screen band. The first captured frame after any
// release only re-anchors, so movement made while the cursor was free can never rotate the camera.

#include <cstdint>

namespace gdtpc
{
struct MouseLookSettings
{
    bool enabled{false};
    float yaw_radians_per_pixel{0.003F};
    bool invert_x{false};
    float aim_start{0.5F};        // fraction of client height from the top
    float aim_band_top{0.25F};
    float aim_band_bottom{0.75F};
    float aim_vertical_scale{1.0F}; // aim-point pixels per mouse pixel
    std::uint32_t menu_release_frames{3};
    // Extra exponential catch-up of the rendered yaw toward requested yaw, per second, on top of the
    // engine's own follow. 0 keeps the native glide.
    float yaw_catchup_per_second{6.0F};
    // Vertical look: mouse movement first changes a pitch offset (degrees, positive looks further down)
    // within [pitch_offset_min, pitch_offset_max]; movement beyond those limits slides the aim point.
    float pitch_degrees_per_pixel{0.1F};
    float pitch_offset_min{-60.0F};
    float pitch_offset_max{25.0F};
    float pitch_floor_degrees{-45.0F}; // final pitch never below this; negative looks above horizontal
};

[[nodiscard]] bool valid_mouse_look_settings(const MouseLookSettings& settings) noexcept;

// Per-frame fraction of the remaining yaw gap to close for a catch-up rate and a frame time. Returns 0
// for a zero rate or an unusable frame time, so a stalled or huge frame never jumps the camera.
[[nodiscard]] float yaw_catchup_fraction(float per_second, float delta_seconds) noexcept;

// Pitch-offset degrees for one frame of right-stick Y (XInput sThumbRY, up positive): standard XInput right-thumb deadzone
// (8689) rescaled to [0, 1], stick up looks up (negative offset) unless inverted. 0 for an unusable frame time or speed.
[[nodiscard]] float stick_pitch_degrees(std::int16_t thumb_y, float degrees_per_second, bool invert, float delta_seconds) noexcept;

// Final pitch in radians for a native (engine-derived) pitch and an offset in degrees, clamped to
// [floor, 89 degrees]; floor may be negative (above horizontal). Returns native if anything is nonfinite.
[[nodiscard]] float apply_pitch_offset(float native_radians, float offset_degrees, float floor_degrees) noexcept;

// Ownership of the per-frame pitch overlay. The engine re-applies its 0..89 degree clamp to the pitch
// field every frame through a radians->degrees->radians round trip, which perturbs our written value in
// the last bits. Live, an exact-equality ownership test therefore mistook our own write for a fresh
// native pitch every frame and compounded the offset (52 -> 66 -> 81 degrees in three frames). A value
// within `tolerance_radians` of our last write, or of the engine's [0, 89] clamp of it, is ours; anything
// further is a new native pitch.
class PitchOverlay final
{
public:
    static constexpr float tolerance_radians = 0.0001F;
    // Returns the value to write for the field's current value, or the current value unchanged when it
    // is nonfinite.
    [[nodiscard]] float step(float current_radians, float offset_degrees, float floor_degrees) noexcept;
    // Explicit-base mode for virtual zoom: the engine derives pitch from its held-far distance, so the base
    // is supplied (pitch for the visual arm) instead of being read from the field. Returns the base unchanged
    // when it is nonfinite; native() reports the base so the floor logic sees the pitch actually built on.
    [[nodiscard]] float step_from_base(float base_radians, float offset_degrees, float floor_degrees) noexcept;
    void release() noexcept { owned_ = false; }
    [[nodiscard]] bool owned() const noexcept { return owned_; }
    [[nodiscard]] float native() const noexcept { return native_; }

private:
    bool owned_{};
    float native_{};
    float written_{};
};

enum class MouseLookState : std::uint32_t
{
    disabled = 0,   // feature off in configuration
    ineligible = 1, // not third person, not foreground, stopping, unreadable menu state, bad window...
    alt = 2,        // Left Alt held: cursor free
    menu = 3,       // menu open and not in combat: cursor free
    captured = 4
};

struct MouseLookRect
{
    std::int32_t left{}, top{}, right{}, bottom{}; // client area in screen coordinates
};

struct MouseLookInput
{
    bool eligible{};
    bool alt_down{};
    bool menu_raw{};     // this frame's undebounced menu signal
    bool combat{};       // phase 1: always false
    bool cursor_valid{}; // GetCursorPos succeeded
    // The engine-derived pitch (degrees) the overlay is applied to, when known. With it the pitch offset
    // stops exactly where the final pitch reaches pitch_floor_degrees, so movement past the floor spills
    // into the aim band instead of accumulating an invisible offset (a dead zone on the way back).
    bool native_pitch_valid{};
    float native_pitch_degrees{};
    std::int32_t cursor_x{}, cursor_y{};
    MouseLookRect client{};
    // Right-stick vertical look this frame, in pitch-offset degrees (positive looks further down); applied while captured,
    // including re-anchor frames, within the same limits and floor as the mouse. 0 when unused.
    float pitch_stick_degrees{};
};

struct MouseLookDecision
{
    MouseLookState state{MouseLookState::ineligible};
    bool captured{};
    bool capture_edge{};  // first captured frame: acquire the clip
    bool release_edge{};  // first frame after capture ended: restore the clip
    bool reanchor{};      // this captured frame produced no rotation by design
    bool warp{};
    std::int32_t warp_x{}, warp_y{};
    float yaw_delta{};    // radians to add to requested yaw; 0 unless captured and not re-anchoring
    float aim_y{};        // current aim fraction
    bool menu{};          // debounced menu state
    // Pitch offset to apply this frame. Held (not reset) while Alt or a menu frees the cursor so the
    // camera does not snap; not applied at all when disabled or ineligible.
    bool apply_pitch{};
    float pitch_offset_degrees{};
};

class MouseLookModel final
{
public:
    explicit MouseLookModel(MouseLookSettings settings) noexcept;

    [[nodiscard]] MouseLookDecision step(const MouseLookInput& input) noexcept;
    // New session: forget capture and debounce; the aim point returns to its configured start.
    void reset() noexcept;
    [[nodiscard]] bool captured() const noexcept { return captured_; }
    [[nodiscard]] float aim_y() const noexcept { return aim_y_; }
    [[nodiscard]] float pitch_offset() const noexcept { return pitch_offset_; }

private:
    void slide_vertical(float dy_pixels, float height, float pitch_min) noexcept;

    MouseLookSettings settings_{};
    bool valid_{};
    bool captured_{};
    bool menu_{};
    std::uint32_t menu_clear_frames_{};
    float aim_y_{};
    float pitch_offset_{};
    std::int32_t last_warp_x_{}, last_warp_y_{};
    MouseLookRect last_client_{};
};

// NPC conversation signal (2026-09-13). The NPC dialog window is not UI panel state (a probe session found no
// difference anywhere in engine or UI memory). Talking to an NPC instead switches the player controller's current state
// to ControllerPlayerStateTalkToNpc. The runtime samples the controller (found through the game's own object lookup)
// and this pure rule decides; anything unexpected is `unknown`, which never frees the cursor.
enum class NpcTalkSignal : std::uint32_t
{
    disabled = 0,
    unknown = 1,               // not sampled this frame, or not readable
    idle = 2,                  // player controller in any other state
    talking = 3,               // top state is TalkToNpc: treat as an open menu
    not_player_controller = 4, // the lookup returned an object that is not a ControllerPlayer
    faulted = 5                // the lookup or a read raised; latched off for the process
};

struct PlayerControllerSample
{
    bool readable{};
    std::uintptr_t controller_vtable{};
    std::uint64_t state_count{};
    std::uintptr_t top_state_vtable{};
};

[[nodiscard]] NpcTalkSignal classify_npc_talk(const PlayerControllerSample& sample, std::uintptr_t player_controller_vtable,
    std::uintptr_t talk_to_npc_vtable) noexcept;

// Dot cursor while mouse look captures (2026-09-13). The engine keeps its hardware cursor handle in WinWindow+0x20 and applies
// it on every WM_SETCURSOR (WinWindow::WindowProc, the only Win32 SetCursor call in Game/Engine). While captured the runtime
// stores its own dot handle there. A different handle found in the field is the game changing its cursor (attack, pick
// up, ...): it becomes the handle to restore and the dot is put back. Release restores it only if the field still holds the dot.
class CursorHandleOverlay final
{
public:
    // Handle to store for the field's current handle. False (store nothing) when the dot handle is null.
    [[nodiscard]] bool step(std::uintptr_t current, std::uintptr_t dot, std::uintptr_t& value) noexcept;
    // True, with the game's handle, when the field still holds the dot. Ownership ends either way.
    [[nodiscard]] bool relinquish(std::uintptr_t current, std::uintptr_t& restore) noexcept;
    void forget() noexcept { owned_ = false; }
    [[nodiscard]] bool owned() const noexcept { return owned_; }
    [[nodiscard]] std::uintptr_t base() const noexcept { return base_; }

private:
    bool owned_{};
    std::uintptr_t base_{};
    std::uintptr_t dot_{};
};
}
