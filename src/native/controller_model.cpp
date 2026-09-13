#include "controller_model.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace
{
float clamp_zoom(const gdtpc::ZoomProfile& profile, const float value) noexcept
{
    return std::clamp(value, profile.minimum, profile.maximum);
}

void validate_zoom(const gdtpc::ZoomProfile& profile)
{
    if (!std::isfinite(profile.minimum) || !std::isfinite(profile.maximum) || !std::isfinite(profile.initial) ||
        profile.minimum > profile.maximum)
        throw std::invalid_argument("Invalid zoom profile.");
}

void validate_camera(const gdtpc::CameraProfile& profile)
{
    validate_zoom(profile.distance);
    validate_zoom(profile.pitch);
}

float wrap_angle(const float angle) noexcept
{
    auto wrapped = std::remainder(angle, 2.0F * std::numbers::pi_v<float>);
    if (wrapped <= -std::numbers::pi_v<float>) wrapped += 2.0F * std::numbers::pi_v<float>;
    return wrapped;
}

float smooth_damp_angle(const float current, const float target, float& velocity, const float delta_time,
    const float smooth_time, const float maximum_speed) noexcept
{
    auto change = wrap_angle(current - target);
    const auto omega = 2.0F / smooth_time;
    const auto x = omega * delta_time;
    const auto decay = 1.0F / (1.0F + x + 0.48F * x * x + 0.235F * x * x * x);
    change = std::clamp(change, -maximum_speed * smooth_time, maximum_speed * smooth_time);
    const auto adjusted_target = current - change;
    const auto temporary = (velocity + omega * change) * delta_time;
    velocity = (velocity - omega * temporary) * decay;
    return wrap_angle(adjusted_target + (change + temporary) * decay);
}
}

gdtpc::ControllerModel::ControllerModel(ControllerSettings settings)
    : settings_(settings),
      isometric_zoom_(clamp_zoom(settings.isometric_camera.distance, settings.isometric_camera.distance.initial)),
      third_person_zoom_(clamp_zoom(settings.third_person_camera.distance, settings.third_person_camera.distance.initial))
{
    validate_camera(settings_.isometric_camera);
    validate_camera(settings_.third_person_camera);
    if (settings_.follow_delay_seconds < 0.0F || settings_.follow_smooth_time_seconds <= 0.0F ||
        settings_.follow_maximum_radians_per_second <= 0.0F || settings_.manual_look_radians_per_second <= 0.0F ||
        settings_.look_dead_zone < 0.0F || settings_.look_dead_zone >= 1.0F)
        throw std::invalid_argument("Invalid controller settings.");
}

void gdtpc::ControllerModel::set_zoom(const float distance) noexcept
{
    if (mode_ == CameraMode::isometric) isometric_zoom_ = clamp_zoom(settings_.isometric_camera.distance, distance);
    else third_person_zoom_ = clamp_zoom(settings_.third_person_camera.distance, distance);
}

gdtpc::FrameOutput gdtpc::ControllerModel::step(const FrameInput& input)
{
    if (!std::isfinite(input.delta_seconds) || input.delta_seconds < 0.0F)
        throw std::invalid_argument("Delta time must be finite and non-negative.");

    const auto toggle_edge = input.toggle_down && !toggle_was_down_;
    auto apply_camera_profile = false;
    toggle_was_down_ = input.toggle_down;
    if (toggle_edge && !input.menu_owns_input)
    {
        mode_ = mode_ == CameraMode::isometric ? CameraMode::third_person : CameraMode::isometric;
        follow_delay_remaining_ = settings_.follow_delay_seconds;
        yaw_velocity_ = 0.0F;
        apply_camera_profile = true;
    }

    const auto active_profile = mode_ == CameraMode::isometric ? settings_.isometric_camera : settings_.third_person_camera;
    if (mode_ == CameraMode::isometric || input.menu_owns_input)
    {
        const auto zoom = mode_ == CameraMode::isometric ? isometric_zoom_ : third_person_zoom_;
        return {mode_, apply_camera_profile, active_profile, false, input.movement, false, input.camera_yaw_radians, zoom};
    }

    auto yaw = wrap_angle(input.camera_yaw_radians);
    const auto look_length_squared = input.manual_look.x * input.manual_look.x + input.manual_look.y * input.manual_look.y;
    if (look_length_squared > settings_.look_dead_zone * settings_.look_dead_zone)
    {
        yaw = wrap_angle(yaw + input.manual_look.x * settings_.manual_look_radians_per_second * input.delta_seconds);
        follow_delay_remaining_ = settings_.follow_delay_seconds;
        yaw_velocity_ = 0.0F;
    }
    else if (follow_delay_remaining_ > 0.0F)
    {
        follow_delay_remaining_ = std::max(0.0F, follow_delay_remaining_ - input.delta_seconds);
    }
    else
    {
        yaw = smooth_damp_angle(yaw, input.aim_heading_radians, yaw_velocity_, input.delta_seconds,
            settings_.follow_smooth_time_seconds, settings_.follow_maximum_radians_per_second);
    }
    return {mode_, apply_camera_profile, active_profile, false, input.movement, true, yaw, third_person_zoom_};
}
