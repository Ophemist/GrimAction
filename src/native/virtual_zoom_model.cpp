#include "virtual_zoom_model.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float radians_per_degree = 0.01745329238474369F; // Game.dll .rdata 0x7770A4, used by UpdatePitch
constexpr std::int32_t maximum_clicks_per_frame = 5;
constexpr float field_epsilon = 1.0e-6F;
// Reading the target back after the native setter: the setter derives the blend itself, so allow for
// its own arithmetic rather than demanding our exact division.
constexpr float acquire_tolerance = 1.0e-4F;

bool finite_vec(const gdtpc::CollisionVec3& value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
}

bool gdtpc::valid_virtual_zoom_settings(const VirtualZoomSettings& s) noexcept
{
    return std::isfinite(s.engine_distance) && std::isfinite(s.visual_minimum) && std::isfinite(s.visual_default) &&
        s.visual_minimum >= 0.5F && s.engine_distance <= 100.0F && s.visual_minimum < s.engine_distance &&
        std::isfinite(s.ratio) && s.ratio > 1.0F && s.ratio <= 4.0F &&
        std::isfinite(s.engine_step) && s.engine_step > 0.0F && s.engine_step <= 1.0F &&
        std::isfinite(s.step_tolerance) && s.step_tolerance > 0.0F && s.step_tolerance < s.engine_step &&
        std::isfinite(s.smoothing_per_second) && s.smoothing_per_second > 0.0F && s.smoothing_per_second <= 120.0F &&
        s.fault_limit >= 1 && s.fault_limit <= 64;
}

float gdtpc::maximum_hold_blend(const VirtualZoomSettings& settings) noexcept
{
    // Half a click below the far end: an outward click then reaches the clamp at 1 and is still a
    // visible, clipped move.
    return 1.0F - settings.engine_step * 0.5F;
}

bool gdtpc::pitch_for_distance(const PitchProfileFields& p, const float distance, float& radians) noexcept
{
    if (!std::isfinite(distance) || !std::isfinite(p.distance_default) || !std::isfinite(p.pitch_default) ||
        !std::isfinite(p.distance_minimum) || !std::isfinite(p.distance_maximum) ||
        !std::isfinite(p.pitch_minimum) || !std::isfinite(p.pitch_maximum))
        return false;
    // `jae` in UpdatePitch: the default distance itself takes the upper branch.
    float degrees = 0.0F;
    if (distance < p.distance_default)
    {
        const auto span = p.distance_default - p.distance_minimum;
        if (!(std::abs(span) > 0.0F)) return false;
        degrees = (distance - p.distance_minimum) / span * (p.pitch_default - p.pitch_minimum) + p.pitch_minimum;
    }
    else
    {
        const auto span = p.distance_maximum - p.distance_default;
        if (!(std::abs(span) > 0.0F)) return false;
        degrees = (distance - p.distance_default) / span * (p.pitch_maximum - p.pitch_default) + p.pitch_default;
    }
    if (!std::isfinite(degrees)) return false;
    radians = std::clamp(degrees, 0.0F, 89.0F) * radians_per_degree;
    return std::isfinite(radians);
}

gdtpc::CollisionVec3 gdtpc::eye_direction(const float yaw, const float pitch) noexcept
{
    const auto horizontal = std::cos(pitch);
    return {horizontal * std::sin(yaw), std::sin(pitch), horizontal * std::cos(yaw)};
}

bool gdtpc::eye_pull(const float engine_distance, const float arm, const float yaw, const float pitch,
    CollisionVec3& pull) noexcept
{
    if (!std::isfinite(engine_distance) || !std::isfinite(arm) || !std::isfinite(yaw) || !std::isfinite(pitch) ||
        engine_distance <= 0.0F || arm <= 0.0F)
        return false;
    const auto amount = engine_distance - arm;
    if (std::abs(amount) > 200.0F) return false;
    const auto direction = eye_direction(yaw, pitch);
    const CollisionVec3 value{-amount * direction.x, -amount * direction.y, -amount * direction.z};
    if (!finite_vec(value)) return false;
    pull = value;
    return true;
}

gdtpc::VirtualZoomModel::VirtualZoomModel(const VirtualZoomSettings settings) noexcept
    : settings_{settings}, valid_{settings.enabled && valid_virtual_zoom_settings(settings)}
{
    visual_ = default_visual();
    smoothed_ = visual_;
}

float gdtpc::VirtualZoomModel::default_visual() const noexcept
{
    if (!valid_virtual_zoom_settings(settings_)) return settings_.visual_default;
    return std::clamp(settings_.visual_default, settings_.visual_minimum, settings_.engine_distance);
}

gdtpc::VirtualZoomDecision gdtpc::VirtualZoomModel::step(const bool applies, const float engine_target_blend,
    const float endpoint_a, const float endpoint_b) noexcept
{
    VirtualZoomDecision decision{};
    decision.visual_distance = visual_;
    if (!valid_) return decision;

    const auto endpoints_valid = std::isfinite(endpoint_a) && std::isfinite(endpoint_b) && endpoint_a > 0.0F &&
        endpoint_b > endpoint_a;
    const auto hold_blend = endpoints_valid ? (settings_.engine_distance - endpoint_a) / (endpoint_b - endpoint_a) : 0.0F;
    const auto hold_valid = endpoints_valid && std::isfinite(hold_blend) && hold_blend >= 0.0F &&
        hold_blend <= maximum_hold_blend(settings_);

    if (!applies || latched_ || !hold_valid)
    {
        if (holding_ || release_pending_)
        {
            holding_ = false;
            release_pending_ = false;
            decision.release = endpoints_valid && visual_blend(endpoint_a, endpoint_b, decision.release_blend);
        }
        decision.state = latched_ ? VirtualZoomState::latched_off : VirtualZoomState::released;
        return decision;
    }

    decision.target_blend = hold_blend;
    pending_blend_ = hold_blend;
    if (!holding_)
    {
        decision.acquire = true;
        decision.state = VirtualZoomState::acquiring;
        smoothed_ = visual_; // the engine is at V (or wherever the profile put it); start the glide at V
        return decision;
    }

    decision.state = VirtualZoomState::holding;
    if (!std::isfinite(engine_target_blend))
    {
        decision.write_target = true;
        return decision;
    }
    const auto delta = engine_target_blend - held_blend_;
    if (std::abs(delta) <= field_epsilon && std::abs(held_blend_ - hold_blend) <= field_epsilon) return decision;

    // Any change is put back: the field is ours while holding. Only the engine's own clicks move V; a
    // change of any other size (a profile entry, a native zoom call) is absorbed without reinterpretation.
    decision.write_target = true;
    const auto clicks = std::abs(delta) <= field_epsilon ? 0 : classify_clicks(delta, engine_target_blend);
    if (clicks != 0)
    {
        const auto factor = std::pow(settings_.ratio, static_cast<float>(std::abs(clicks)));
        const auto scaled = clicks > 0 ? visual_ * factor : visual_ / factor;
        if (std::isfinite(scaled))
            visual_ = std::clamp(scaled, settings_.visual_minimum, settings_.engine_distance);
        decision.clicks = clicks;
        click_count_ += static_cast<std::uint64_t>(std::abs(clicks));
    }
    decision.visual_distance = visual_;
    return decision;
}

std::int32_t gdtpc::VirtualZoomModel::classify_clicks(const float delta, const float engine_target_blend) const noexcept
{
    // The engine clamps its target to [0, 1], so a click against an end arrives as a shorter move.
    if (delta > 0.0F && engine_target_blend >= 1.0F) return 1;
    if (delta < 0.0F && engine_target_blend <= 0.0F) return -1;
    const auto magnitude = std::abs(delta);
    const auto count = static_cast<std::int32_t>(std::lround(magnitude / settings_.engine_step));
    if (count < 1 || count > maximum_clicks_per_frame) return 0;
    if (std::abs(magnitude - static_cast<float>(count) * settings_.engine_step) > settings_.step_tolerance) return 0;
    return delta > 0.0F ? count : -count;
}

void gdtpc::VirtualZoomModel::record_acquire_result(const bool success, const float observed_target_blend) noexcept
{
    if (success && std::isfinite(observed_target_blend) &&
        std::abs(observed_target_blend - pending_blend_) <= acquire_tolerance)
    {
        holding_ = true;
        release_pending_ = false;
        held_blend_ = observed_target_blend;
        return;
    }
    // The setter may have moved the engine part of the way; if this latches, the engine is handed back.
    holding_ = false;
    release_pending_ = true;
    record_fault();
}

void gdtpc::VirtualZoomModel::record_target_write_result(const bool success) noexcept
{
    if (success)
    {
        held_blend_ = pending_blend_;
        return;
    }
    // Whatever is in the field now is unknown; acquire again from scratch next frame, or hand back.
    holding_ = false;
    release_pending_ = true;
    record_fault();
}

float gdtpc::VirtualZoomModel::advance(const float delta_seconds) noexcept
{
    if (!std::isfinite(smoothed_)) smoothed_ = visual_;
    if (std::isfinite(delta_seconds) && delta_seconds > 0.0F && delta_seconds <= 0.1F)
    {
        const auto fraction = 1.0F - std::exp(-settings_.smoothing_per_second * delta_seconds);
        smoothed_ += (visual_ - smoothed_) * fraction;
        // Close enough is exactly there, so an idle camera does not creep by a float forever.
        if (std::abs(visual_ - smoothed_) < 0.001F) smoothed_ = visual_;
    }
    return smoothed_;
}

void gdtpc::VirtualZoomModel::relinquish() noexcept
{
    holding_ = false;
    release_pending_ = false;
}

void gdtpc::VirtualZoomModel::reset_session() noexcept
{
    holding_ = false;
    release_pending_ = false;
    held_blend_ = 0.0F;
    pending_blend_ = 0.0F;
    visual_ = default_visual();
    smoothed_ = visual_;
}

void gdtpc::VirtualZoomModel::record_fault() noexcept
{
    ++fault_count_;
    // A failed write also leaves release_pending_ set, so the step after the latch still hands V back.
    if (fault_count_ >= settings_.fault_limit) latched_ = true;
}

bool gdtpc::VirtualZoomModel::visual_blend(const float endpoint_a, const float endpoint_b, float& blend) const noexcept
{
    if (!std::isfinite(endpoint_a) || !std::isfinite(endpoint_b) || !(endpoint_b > endpoint_a) ||
        !std::isfinite(visual_))
        return false;
    const auto value = (visual_ - endpoint_a) / (endpoint_b - endpoint_a);
    if (!std::isfinite(value) || value < -field_epsilon || value > 1.0F + field_epsilon) return false;
    blend = std::clamp(value, 0.0F, 1.0F);
    return true;
}

bool gdtpc::EyeOffsetOverlay::ours(const CollisionVec3& current) const noexcept
{
    return owned_ && std::abs(current.x - written_.x) <= tolerance && std::abs(current.y - written_.y) <= tolerance &&
        std::abs(current.z - written_.z) <= tolerance;
}

bool gdtpc::EyeOffsetOverlay::step(const CollisionVec3& current, const CollisionVec3& pull, CollisionVec3& value) noexcept
{
    if (!finite_vec(current) || !finite_vec(pull)) return false;
    if (!ours(current)) base_ = current;
    const CollisionVec3 next{base_.x + pull.x, base_.y + pull.y, base_.z + pull.z};
    if (!finite_vec(next)) return false;
    written_ = next;
    pull_ = pull;
    owned_ = true;
    value = next;
    return true;
}

bool gdtpc::EyeOffsetOverlay::relinquish(const CollisionVec3& current, CollisionVec3& restore) noexcept
{
    const auto restorable = finite_vec(current) && ours(current);
    restore = base_;
    owned_ = false;
    pull_ = {};
    return restorable;
}

bool gdtpc::valid_far_plane_percent(const std::uint32_t percent) noexcept
{
    return percent >= 50 && percent <= 100;
}

float gdtpc::far_plane_fraction(const std::uint32_t percent) noexcept
{
    if (!valid_far_plane_percent(percent) || percent == 100) return 0.0F;
    return static_cast<float>(percent) / 100.0F;
}

bool gdtpc::FarPlaneOverlay::step(const float current, const float fraction, float& value) noexcept
{
    // A far plane is a positive distance; anything else is not a camera we understand.
    if (!std::isfinite(current) || current <= 0.0F || current > 1.0e6F || !std::isfinite(fraction) ||
        fraction <= 0.0F || fraction > 1.0F)
        return false;
    const auto ours = owned_ && std::abs(current - written_) <= tolerance;
    if (!ours) native_ = current;
    const auto next = native_ * fraction;
    if (!std::isfinite(next) || next <= 0.0F) return false;
    written_ = next;
    owned_ = true;
    value = next;
    return true;
}

bool gdtpc::FarPlaneOverlay::relinquish(const float current, float& restore) noexcept
{
    const auto restorable = owned_ && std::isfinite(current) && std::abs(current - written_) <= tolerance;
    restore = native_;
    owned_ = false;
    return restorable;
}

