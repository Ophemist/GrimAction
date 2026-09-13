#include "camera_memory_model.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace
{
constexpr std::size_t distance_default_offset = 0x10c;
constexpr std::size_t pitch_default_offset = 0x110;
constexpr std::size_t distance_minimum_offset = 0x570;
constexpr std::size_t distance_maximum_offset = 0x574;
constexpr std::size_t pitch_minimum_offset = 0x578;
constexpr std::size_t pitch_maximum_offset = 0x57c;
constexpr std::size_t zoom_blend_offset = 0x580;
constexpr std::size_t zoom_endpoint_a_offset = 0x590;
constexpr std::size_t zoom_endpoint_b_offset = 0x594;

float read_float(const void* base, const std::size_t offset) noexcept
{
    float value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof(value));
    return value;
}

void write_float(void* base, const std::size_t offset, const float value) noexcept
{
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof(value));
}

bool valid_zoom(const gdtpc::ZoomProfile& value) noexcept
{
    return std::isfinite(value.minimum) && std::isfinite(value.maximum) && std::isfinite(value.initial) &&
        value.minimum <= value.initial && value.initial <= value.maximum;
}

bool valid_snapshot(const gdtpc::CameraMemorySnapshot& value) noexcept
{
    return gdtpc::valid_camera_profile(value.profile) && std::isfinite(value.zoom_blend) &&
        value.zoom_blend >= 0.0F && value.zoom_blend <= 1.0F &&
        std::isfinite(value.zoom_endpoint_a) && std::isfinite(value.zoom_endpoint_b) &&
        value.zoom_endpoint_a <= value.zoom_endpoint_b;
}
}

bool gdtpc::valid_zoom_step_settings(const ZoomStepSettings& settings) noexcept
{
    return std::isfinite(settings.ratio) && settings.ratio > 1.0F && settings.ratio <= 4.0F &&
        std::isfinite(settings.engine_step) && settings.engine_step > 0.0F && settings.engine_step <= 1.0F &&
        std::isfinite(settings.step_tolerance) && settings.step_tolerance > 0.0F &&
        settings.step_tolerance < settings.engine_step;
}

gdtpc::ZoomStepModel::ZoomStepModel(const ZoomStepSettings settings) noexcept
    : settings_{settings}
{
    // Invalid settings must not produce erratic zooming. Disable instead, and the engine's own
    // stepping is then left completely alone.
    if (!valid_zoom_step_settings(settings_)) disabled_ = true;
}

bool gdtpc::ZoomStepModel::step(const float engine_target_blend, const float endpoint_a,
    const float endpoint_b, float& blend_to_write) noexcept
{
    if (disabled_ || !std::isfinite(engine_target_blend) || engine_target_blend < 0.0F ||
        engine_target_blend > 1.0F || !std::isfinite(endpoint_a) || !std::isfinite(endpoint_b) ||
        endpoint_a <= 0.0F || endpoint_b <= endpoint_a)
        return false;

    if (!has_commanded_)
    {
        player_ = engine_target_blend;
        field_ = engine_target_blend;
        has_commanded_ = true;
        ++adopted_count_;
        return false;
    }

    // Measured against the FIELD, not the player's zoom. While collision holds the field at the arm,
    // a scroll still shows up as one engine step away from that held value.
    const auto delta = engine_target_blend - field_;
    if (std::abs(delta) <= 1.0e-6F) return false; // nothing moved; leave the field alone

    // A click is the engine's own step, or a shorter move that it clipped against an endpoint.
    const auto clipped_inward = delta < 0.0F && engine_target_blend <= 0.0F;
    const auto clipped_outward = delta > 0.0F && engine_target_blend >= 1.0F;
    const auto is_click = std::abs(std::abs(delta) - settings_.engine_step) <= settings_.step_tolerance ||
        clipped_inward || clipped_outward;
    if (!is_click)
    {
        // Something other than a scroll moved the target: a profile entry, or the player's zoom being
        // set by the game. That is not ours to reinterpret, so adopt it and step from there next time.
        player_ = engine_target_blend;
        field_ = engine_target_blend;
        ++adopted_count_;
        return false;
    }

    // A click steps the PLAYER's zoom, which is unaffected by whatever collision may have parked in
    // the field.
    const auto span = endpoint_b - endpoint_a;
    const auto current = endpoint_a + span * player_;
    const auto scaled = delta > 0.0F ? current * settings_.ratio : current / settings_.ratio;
    if (!std::isfinite(scaled)) return false;
    const auto clamped = std::clamp(scaled, endpoint_a, endpoint_b);
    const auto blend = std::clamp((clamped - endpoint_a) / span, 0.0F, 1.0F);
    if (!std::isfinite(blend)) return false;

    // Already hard against the endpoint the player is pushing toward: let the engine's own clamped
    // value stand rather than writing the same thing every click.
    const auto unchanged = std::abs(blend - player_) <= 1.0e-6F;
    player_ = blend;
    field_ = blend;
    blend_to_write = blend;
    ++click_count_;
    return unchanged ? engine_target_blend != blend : true;
}

void gdtpc::ZoomStepModel::note_field_write(const float blend) noexcept
{
    if (!std::isfinite(blend) || blend < 0.0F || blend > 1.0F) return;
    field_ = blend;
    // The player's zoom is deliberately untouched: this records only what is in the field.
}

void gdtpc::ZoomStepModel::reset() noexcept
{
    player_ = 0.0F;
    field_ = 0.0F;
    has_commanded_ = false;
}

bool gdtpc::valid_camera_profile(const CameraProfile& profile) noexcept
{
    return valid_zoom(profile.distance) && valid_zoom(profile.pitch);
}

bool gdtpc::capture_camera_memory(const void* camera, CameraMemorySnapshot& snapshot) noexcept
{
    if (camera == nullptr) return false;
    CameraMemorySnapshot candidate{
        {{read_float(camera, distance_minimum_offset), read_float(camera, distance_maximum_offset),
             read_float(camera, distance_default_offset)},
            {read_float(camera, pitch_minimum_offset), read_float(camera, pitch_maximum_offset),
             read_float(camera, pitch_default_offset)}},
        read_float(camera, zoom_blend_offset), read_float(camera, zoom_endpoint_a_offset),
        read_float(camera, zoom_endpoint_b_offset)};
    if (!valid_snapshot(candidate)) return false;
    snapshot = candidate;
    return true;
}

bool gdtpc::apply_camera_profile(void* camera, const CameraProfile& profile, const float zoom_distance) noexcept
{
    if (camera == nullptr || !valid_camera_profile(profile) || !std::isfinite(zoom_distance)) return false;
    const auto distance = std::clamp(zoom_distance, profile.distance.minimum, profile.distance.maximum);
    const auto span = profile.distance.maximum - profile.distance.minimum;
    const auto blend = span > 0.0F ? (distance - profile.distance.minimum) / span : 0.0F;

    write_float(camera, distance_default_offset, profile.distance.initial);
    write_float(camera, pitch_default_offset, profile.pitch.initial);
    write_float(camera, distance_minimum_offset, profile.distance.minimum);
    write_float(camera, distance_maximum_offset, profile.distance.maximum);
    write_float(camera, pitch_minimum_offset, profile.pitch.minimum);
    write_float(camera, pitch_maximum_offset, profile.pitch.maximum);
    write_float(camera, zoom_blend_offset, blend);
    write_float(camera, zoom_endpoint_a_offset, profile.distance.minimum);
    write_float(camera, zoom_endpoint_b_offset, profile.distance.maximum);
    return true;
}

bool gdtpc::restore_camera_memory(void* camera, const CameraMemorySnapshot& snapshot) noexcept
{
    if (camera == nullptr || !valid_snapshot(snapshot)) return false;
    write_float(camera, distance_default_offset, snapshot.profile.distance.initial);
    write_float(camera, pitch_default_offset, snapshot.profile.pitch.initial);
    write_float(camera, distance_minimum_offset, snapshot.profile.distance.minimum);
    write_float(camera, distance_maximum_offset, snapshot.profile.distance.maximum);
    write_float(camera, pitch_minimum_offset, snapshot.profile.pitch.minimum);
    write_float(camera, pitch_maximum_offset, snapshot.profile.pitch.maximum);
    write_float(camera, zoom_blend_offset, snapshot.zoom_blend);
    write_float(camera, zoom_endpoint_a_offset, snapshot.zoom_endpoint_a);
    write_float(camera, zoom_endpoint_b_offset, snapshot.zoom_endpoint_b);
    return true;
}

float gdtpc::zoom_distance_from_memory(const CameraMemorySnapshot& snapshot) noexcept
{
    return snapshot.zoom_endpoint_a + (snapshot.zoom_endpoint_b - snapshot.zoom_endpoint_a) * snapshot.zoom_blend;
}
