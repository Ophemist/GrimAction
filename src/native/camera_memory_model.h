#pragma once

#include "controller_model.h"

#include <cstdint>

namespace gdtpc
{
struct CameraMemorySnapshot
{
    CameraProfile profile;
    float zoom_blend;
    float zoom_endpoint_a;
    float zoom_endpoint_b;
};

// Finer zoom stepping.
//
// The engine moves its zoom target by a fixed 0.1 of the blend per scroll click, measured at 54 of 55
// observed target changes in the accepted collision session, the one exception being a partial step
// clipped at the end of the range. Because distance is linear in the blend between the two endpoints,
// that is a fixed number of units per click: 8.6 across a 4 to 90 range. At the near end that is a
// threefold jump from 4.00 to 12.60, and at the far end it is barely a tenth of the distance. Making
// the span smaller would make the steps finer but costs reach, and the accepted session shows the
// whole range in use.
//
// This model substitutes a PROPORTIONAL step: each click multiplies the distance by a constant, so the
// resolution is fine where the camera is close and coarse where it is far, which is where the player
// asked for it. It is deliberately a pure policy with no memory access: it is handed the engine's
// target blend and the two endpoints, and returns the blend to write, if any.
struct ZoomStepSettings
{
    float ratio{1.14F};       // distance multiplier per click
    float engine_step{0.1F};  // the blend delta the engine itself applies per click
    float step_tolerance{0.02F};
};

[[nodiscard]] bool valid_zoom_step_settings(const ZoomStepSettings& settings) noexcept;

// Two jobs, deliberately separated, because collision may legitimately put a value in the target field
// that is NOT the player's choice:
//
//  - `player_blend` is the player's own zoom, and is authoritative. It changes only on a scroll.
//  - `field_blend` is whatever was last written to the engine's target field, whoever wrote it.
//
// Click detection measures against the field, not against the player's zoom, so a scroll is still
// recognised while collision is holding the field at the arm. When collision is not holding the field,
// the two are kept equal and the behaviour is exactly what was accepted live.
class ZoomStepModel final
{
public:
    explicit ZoomStepModel(ZoomStepSettings settings) noexcept;

    // Returns true and sets `blend_to_write` when the engine's target should be replaced with a finer
    // one. Returns false when the engine's value should stand, which is the case on the first frame,
    // when nothing changed, and when the change was not one of the engine's own clicks.
    [[nodiscard]] bool step(float engine_target_blend, float endpoint_a, float endpoint_b,
        float& blend_to_write) noexcept;

    // Reports a value written to the target field by someone other than this model, which is
    // collision holding the camera in. Without this the next frame would read that value, fail to
    // recognise it as a click, and adopt it as the player's choice.
    void note_field_write(float blend) noexcept;

    void reset() noexcept; // new session: forget what we commanded

    [[nodiscard]] bool has_player_blend() const noexcept { return has_commanded_; }
    [[nodiscard]] float player_blend() const noexcept { return player_; }
    // What the target field was last set to, by this model or by collision. A field that no longer
    // holds this has been moved by the engine, which means the player scrolled.
    [[nodiscard]] float field_blend() const noexcept { return field_; }
    [[nodiscard]] std::uint64_t click_count() const noexcept { return click_count_; }
    [[nodiscard]] std::uint64_t adopted_count() const noexcept { return adopted_count_; }

private:
    ZoomStepSettings settings_{};
    float player_{0.0F};  // the player's own zoom: changes only on a scroll
    float field_{0.0F};   // what the engine's target field was last set to, by anyone
    bool has_commanded_{false};
    bool disabled_{false};
    std::uint64_t click_count_{}, adopted_count_{};
};

[[nodiscard]] bool valid_camera_profile(const CameraProfile& profile) noexcept;
[[nodiscard]] bool capture_camera_memory(const void* camera, CameraMemorySnapshot& snapshot) noexcept;
[[nodiscard]] bool apply_camera_profile(void* camera, const CameraProfile& profile, float zoom_distance) noexcept;
[[nodiscard]] bool restore_camera_memory(void* camera, const CameraMemorySnapshot& snapshot) noexcept;
[[nodiscard]] float zoom_distance_from_memory(const CameraMemorySnapshot& snapshot) noexcept;
}
