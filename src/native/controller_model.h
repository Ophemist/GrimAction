#pragma once

#include <cstdint>

namespace gdtpc
{
enum class CameraMode : std::uint32_t { isometric, third_person };

struct Vec2 { float x; float y; };
struct ZoomProfile { float minimum; float maximum; float initial; };
struct CameraProfile { ZoomProfile distance; ZoomProfile pitch; };
struct ControllerSettings
{
    CameraProfile isometric_camera;
    CameraProfile third_person_camera;
    float follow_delay_seconds{0.65F};
    float follow_smooth_time_seconds{0.32F};
    float follow_maximum_radians_per_second{4.5F};
    float manual_look_radians_per_second{2.8F};
    float look_dead_zone{0.08F};
};
struct FrameInput
{
    float delta_seconds;
    Vec2 movement;
    Vec2 manual_look;
    bool toggle_down;
    bool menu_owns_input;
    float aim_heading_radians;
    float camera_yaw_radians;
};
struct FrameOutput
{
    CameraMode mode;
    bool apply_camera_profile;
    CameraProfile active_camera_profile;
    bool override_movement;
    Vec2 world_movement;
    bool override_camera_yaw;
    float camera_yaw_radians;
    float zoom_distance;
};

class ControllerModel final
{
public:
    explicit ControllerModel(ControllerSettings settings);
    [[nodiscard]] CameraMode mode() const noexcept { return mode_; }
    void set_zoom(float distance) noexcept;
    [[nodiscard]] FrameOutput step(const FrameInput& input);
private:
    ControllerSettings settings_;
    CameraMode mode_{CameraMode::isometric};
    bool toggle_was_down_{false};
    float follow_delay_remaining_{0.0F};
    float yaw_velocity_{0.0F};
    float isometric_zoom_;
    float third_person_zoom_;
};
}
