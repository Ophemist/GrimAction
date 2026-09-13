#pragma once

#include "camera_collision_model.h"
#include "camera_memory_model.h"
#include "controller_model.h"
#include "mouse_look_model.h"
#include "virtual_zoom_model.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace gdtpc
{
struct RuntimeConfig
{
    std::uint32_t schema_version{1};
    CameraProfile third_person_camera{{12.0F, 90.0F, 42.0F}, {20.0F, 44.0F, 30.0F}};
    float third_person_fov_degrees{45.0F};
    float follow_delay_seconds{0.25F};
    float follow_smooth_seconds{0.22F};
    float follow_max_radians_per_second{5.5F};
    float aim_yaw_offset_degrees{0.0F};
    // Camera collision assistance. Off by default so a build that carries the capability still
    // behaves exactly like the accepted Gate 2 runtime until it is deliberately enabled.
    bool collision_enabled{false};
    CollisionSettings collision{};

    // Finer zoom stepping. Off by default, so a build that carries the capability behaves exactly
    // like the accepted runtime until it is deliberately enabled.
    bool zoom_step_enabled{false};
    // Percent added per scroll click, applied multiplicatively: 14 means each click changes the
    // distance by a factor of 1.14. The engine's own step is a fixed tenth of the blend, which is a
    // fixed number of units and therefore far too coarse close in and far too fine far out.
    std::uint32_t zoom_step_percent{14};
    bool shoulder_offset_enabled{false};
    float shoulder_offset_units{0.75F};
    // Lifts GameCamera's framing target on every shoulder side, center included. The engine clamps the
    // vertical target offset to +/-15; this setting is bounded well inside that.
    float shoulder_height_units{1.5F};
    // Read-only menu-detection research. F10 marks "a panel is open", F11 "everything is closed";
    // each mark snapshots UI memory to a side file. Never writes game memory.
    bool ui_probe_enabled{false};
    // Mouse look (IMPLEMENTATION_PLAN.md section 7). Off by default; requires collision.
    MouseLookSettings mouse_look{};
    // Virtual zoom (IMPLEMENTATION_PLAN.md section 8). Off by default; requires mouse look. The engine zoom
    // is held at this distance for targeting reach while scroll drives the visual distance.
    bool virtual_zoom_enabled{false};
    float virtual_zoom_engine_distance{85.0F};
    // Third-person cap of the camera far plane WorldCamera+0x18, percent of native (50..100, 100 = off). Reduces the
    // view-dependent loading hitch of a low camera; the player chose 95.
    std::uint32_t third_person_far_plane_percent{100};
    // Free the cursor while the player controller is in its talk-to-NPC state, like an open panel. Requires mouse look.
    bool npc_dialog_releases_cursor{false};
    // Show a small white dot instead of the game cursor while mouse look captures it. Requires mouse look.
    bool mouse_look_dot_cursor{false};
    // Right-stick vertical look while mouse look captures (XInput read only). Requires mouse look.
    bool right_stick_pitch_enabled{false};
    float right_stick_pitch_degrees_per_second{90.0F};
    bool right_stick_pitch_invert{false};
    std::uint32_t toggle_virtual_key{0x77};
    bool start_in_third_person{false};
    bool persist_mode{false};
};

// The stepping settings a configuration implies. One place, so the parser validates exactly what the
// runtime will construct.
[[nodiscard]] ZoomStepSettings zoom_step_settings(const RuntimeConfig& config) noexcept;
// Likewise for virtual zoom: visual bounds and click ratio come from the profile and zoom-step keys.
[[nodiscard]] VirtualZoomSettings virtual_zoom_settings(const RuntimeConfig& config) noexcept;

[[nodiscard]] bool parse_runtime_config(std::string_view text, RuntimeConfig& result, std::string& error);
}
