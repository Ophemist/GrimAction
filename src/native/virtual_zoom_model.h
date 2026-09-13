#pragma once

// Pure virtual-zoom policy (IMPLEMENTATION_PLAN.md section 8). Free of Win32 like the collision, shoulder
// and mouse-look models: the runtime reads and writes camera memory, and every decision here is provable
// offline.
//
// Third-person targeting reach follows the ENGINE zoom distance, not the rendered eye (live, 2026-09-13).
// So the engine zoom is held far (E) and the player's scroll drives a separate visual distance (V).
// Collision shortens V to the visual arm (A), and the rendered eye is pulled toward the target by
// P = D - A through the eye offset WorldCamera+0x60, where D is the physical engine distance (+0x08).
//
// Engine facts encoded here, each checked against live telemetry before use:
//  - Eye direction (target toward eye) is (cos p sin y, sin p, cos p cos y) for rendered yaw +0x0C and
//    pitch +0x10; the camera position +0x94 is the focus plus D times that plus +0x60.
//  - GameCamera::UpdatePitch interpolates degrees between +0x578/+0x110 below the default distance +0x10C
//    and +0x110/+0x57C at or above it, converts with 0.017453292 (Game.dll .rdata 0x7770A4), and
//    UpdateFromInputImpl then clamps the result to [0, 89] degrees. Matched telemetry to 0.00000 degrees.
//  - A scroll click moves the zoom target blend +0x584 by exactly 0.1, clamped to [0, 1]. A target held
//    at blend 1 therefore cannot show an outward click, which is why E must leave room below the far end.

#include "camera_collision_model.h"

#include <cstdint>

namespace gdtpc
{
struct VirtualZoomSettings
{
    bool enabled{false};
    float engine_distance{85.0F};   // E: where the engine zoom is held
    float visual_minimum{4.0F};     // V never below this (profile distance_min)
    float visual_default{42.0F};    // V on a new session (profile distance_default), clamped to [min, E]
    float ratio{1.14F};             // V multiplier per click (zoom_step_percent)
    float engine_step{0.1F};        // blend delta of one native click
    float step_tolerance{0.02F};
    // Exponential approach of the smoothed visual distance to V, per second. The engine used to animate
    // a scroll; holding its zoom removes that, so the model supplies the glide.
    float smoothing_per_second{12.0F};
    std::uint32_t fault_limit{4};   // latch off for the process after this many failed writes
};

[[nodiscard]] bool valid_virtual_zoom_settings(const VirtualZoomSettings& settings) noexcept;

// Largest target blend at which an outward click is still visible to click detection.
[[nodiscard]] float maximum_hold_blend(const VirtualZoomSettings& settings) noexcept;

// The six profile fields GameCamera::UpdatePitch reads. Distances in units, pitches in degrees.
struct PitchProfileFields
{
    float distance_default{};  // +0x10C
    float pitch_default{};     // +0x110
    float distance_minimum{};  // +0x570
    float distance_maximum{};  // +0x574
    float pitch_minimum{};     // +0x578
    float pitch_maximum{};     // +0x57C
};

// Pitch in radians the engine would derive for `distance`, including UpdateFromInputImpl's [0, 89] degree
// clamp. False for nonfinite input or a degenerate interpolation span on the branch taken.
[[nodiscard]] bool pitch_for_distance(const PitchProfileFields& profile, float distance, float& radians) noexcept;

// Unit vector from the camera target toward the eye.
[[nodiscard]] CollisionVec3 eye_direction(float yaw, float pitch) noexcept;

// Eye-offset vector that moves the eye from distance `engine_distance` to `arm` along the eye direction:
// -(engine_distance - arm) * eye_direction. False when anything is nonfinite or the pull is implausible.
[[nodiscard]] bool eye_pull(float engine_distance, float arm, float yaw, float pitch, CollisionVec3& pull) noexcept;

enum class VirtualZoomState : std::uint32_t
{
    disabled = 0,   // not configured, or invalid settings
    released = 1,   // configured but not applying (not third person, ineligible, unreadable)
    acquiring = 2,  // applying; the engine is being moved to E this frame
    holding = 3,    // engine held at E, scroll drives V
    latched_off = 4 // too many failed writes; the accepted non-virtual path takes over for the process
};

struct VirtualZoomDecision
{
    VirtualZoomState state{VirtualZoomState::disabled};
    // Move the engine to E through the validated zoom setter (current and target blend), then report the
    // target blend read back with record_acquire_result.
    bool acquire{};
    // Put `target_blend` back into +0x584: a click (or a foreign change) was absorbed. Report the outcome
    // with record_target_write_result.
    bool write_target{};
    // Holding ended this frame: hand the engine the visual distance back (distance and blend below).
    bool release{};
    float target_blend{};     // blend of E for the current endpoints
    float visual_distance{};  // V after this frame's clicks
    float release_blend{};    // blend of V, valid when release is set
    std::int32_t clicks{};    // signed clicks consumed this frame; positive zooms out
};

class VirtualZoomModel final
{
public:
    explicit VirtualZoomModel(VirtualZoomSettings settings) noexcept;

    // Zoom intent, once per callback after the native update. `applies` means third person with mouse
    // look applying (captured, Alt or menu) and a readable zoom state.
    [[nodiscard]] VirtualZoomDecision step(bool applies, float engine_target_blend, float endpoint_a,
        float endpoint_b) noexcept;
    void record_acquire_result(bool success, float observed_target_blend) noexcept;
    void record_target_write_result(bool success) noexcept;

    // Advances the smoothed visual distance toward V. An unusable frame time leaves it where it is.
    [[nodiscard]] float advance(float delta_seconds) noexcept;

    // Stop holding without a write: the engine was handed back elsewhere, or the camera is no longer ours.
    void relinquish() noexcept;
    // New session: V returns to its default. The fault latch survives, as collision's does.
    void reset_session() noexcept;
    void record_fault() noexcept;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool holding() const noexcept { return holding_; }
    [[nodiscard]] bool latched() const noexcept { return latched_; }
    [[nodiscard]] float visual_distance() const noexcept { return visual_; }
    [[nodiscard]] float smoothed_distance() const noexcept { return smoothed_; }
    [[nodiscard]] std::uint64_t click_count() const noexcept { return click_count_; }
    [[nodiscard]] std::uint64_t fault_count() const noexcept { return fault_count_; }
    // Blend of V between the endpoints, for handing the engine back. False when not expressible.
    [[nodiscard]] bool visual_blend(float endpoint_a, float endpoint_b, float& blend) const noexcept;

private:
    [[nodiscard]] std::int32_t classify_clicks(float delta, float engine_target_blend) const noexcept;
    [[nodiscard]] float default_visual() const noexcept;

    VirtualZoomSettings settings_{};
    bool valid_{};
    bool holding_{};
    bool release_pending_{}; // a failed write may have left the engine away from V
    bool latched_{};
    float held_blend_{};     // what +0x584 holds while holding
    float pending_blend_{};  // blend asked for by the last acquire/write decision
    float visual_{};
    float smoothed_{};
    std::uint64_t click_count_{};
    std::uint64_t fault_count_{};
};

// Ownership of the eye offset WorldCamera+0x60, the same pattern as the shoulder and pitch overlays: a
// field within `tolerance` of our last write still holds our pull and is recomposed from the saved base;
// anything else is fresh native state and becomes the base. Release restores the saved base bytes exactly.
class EyeOffsetOverlay final
{
public:
    static constexpr float tolerance = 0.001F;
    // Value to write for the field's current value and this frame's pull. False when either is nonfinite.
    [[nodiscard]] bool step(const CollisionVec3& current, const CollisionVec3& pull, CollisionVec3& value) noexcept;
    // True, with the base to write, when the field still holds our pull. Ownership ends either way.
    [[nodiscard]] bool relinquish(const CollisionVec3& current, CollisionVec3& restore) noexcept;
    // The last write did not land: ownership is unknown, so claim nothing.
    void forget() noexcept { owned_ = false; }
    [[nodiscard]] bool owned() const noexcept { return owned_; }
    [[nodiscard]] CollisionVec3 base() const noexcept { return base_; }
    [[nodiscard]] CollisionVec3 pull() const noexcept { return owned_ ? pull_ : CollisionVec3{}; }

private:
    [[nodiscard]] bool ours(const CollisionVec3& current) const noexcept;

    bool owned_{};
    CollisionVec3 base_{};
    CollisionVec3 written_{};
    CollisionVec3 pull_{};
};

// Third-person far-plane cap (2026-09-13). The camera far plane WorldCamera+0x18 gates the view-dependent loading that
// hitches a low third-person view: live, 70% of native removed the bridge hitch but looked too close, and the player
// chose 95% (fire hitch tolerable, horizon cut acceptable). `third_person_far_plane_percent` sets it; 100 is off.
// Ownership follows the other overlays: a field within tolerance of our last write is ours and is recomposed from
// the saved native value; anything else (a sector's SetViewDistance) becomes the new native value.
[[nodiscard]] bool valid_far_plane_percent(std::uint32_t percent) noexcept; // 50..100
// Fraction to apply, or 0 when the percent is 100 (off) or invalid.
[[nodiscard]] float far_plane_fraction(std::uint32_t percent) noexcept;

class FarPlaneOverlay final
{
public:
    static constexpr float tolerance = 0.01F;
    // Value to write for the field's current value and a fraction in (0, 1]. False (write nothing) for a nonfinite
    // or implausible field or fraction.
    [[nodiscard]] bool step(float current, float fraction, float& value) noexcept;
    // True, with the native value to write, when the field still holds our cap. Ownership ends either way.
    [[nodiscard]] bool relinquish(float current, float& restore) noexcept;
    void forget() noexcept { owned_ = false; }
    [[nodiscard]] bool owned() const noexcept { return owned_; }
    [[nodiscard]] float native() const noexcept { return native_; }

private:
    bool owned_{};
    float native_{};
    float written_{};
};
}
