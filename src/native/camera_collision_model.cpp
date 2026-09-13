#include "camera_collision_model.h"

#include <algorithm>
#include <cmath>

namespace
{
bool finite_vec(const gdtpc::CollisionVec3& value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

// Below this the arm has not meaningfully moved and the camera is where we put it.
constexpr float write_epsilon = 0.01F;
// The engine animates its own zoom back toward the player's target every frame, so holding the
// camera in against geometry takes continuous force, not one write. Beyond this much drift from the
// commanded arm the write is re-asserted.
constexpr float reassert_epsilon = 0.05F;
gdtpc::CollisionVec3 add(const gdtpc::CollisionVec3& a, const gdtpc::CollisionVec3& b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

gdtpc::CollisionVec3 subtract(const gdtpc::CollisionVec3& a, const gdtpc::CollisionVec3& b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
}

gdtpc::ShoulderOffsetDecision gdtpc::ShoulderOffsetModel::relinquish(const CollisionVec3 current) noexcept
{
    ShoulderOffsetDecision result{};
    result.side = side_;
    if (owns_ && finite_vec(current) && current == last_written_)
    {
        result.value = last_base_;
        result.translation = subtract(result.value, last_written_);
        result.write = current != result.value;
    }
    owns_ = false;
    last_base_ = {};
    last_written_ = {};
    return result;
}

gdtpc::ShoulderOffsetDecision gdtpc::ShoulderOffsetModel::step(const CollisionVec3 current,
    const float yaw, const float units, const float height, const bool eligible,
    const bool cycle_key_down) noexcept
{
    auto changed = false;
    if (cycle_key_down && !key_down_ && eligible)
    {
        side_ = side_ == ShoulderSide::center ? ShoulderSide::right :
            side_ == ShoulderSide::right ? ShoulderSide::left : ShoulderSide::center;
        ++edge_count_;
        changed = true;
    }
    key_down_ = cycle_key_down;
    const auto owned_before = owns_;
    if (!eligible || !finite_vec(current) || !std::isfinite(yaw) || !std::isfinite(units) || units < 0.0F ||
        !std::isfinite(height) || height < 0.0F || (side_ == ShoulderSide::center && height == 0.0F))
    {
        auto result = relinquish(current);
        result.side_changed = changed;
        result.force_query = changed || (owned_before && result.write);
        return result;
    }

    // Center is built without the trig so it carries exact +0 laterally, not a -0 from 0 * -sin.
    const auto sign = side_ == ShoulderSide::right ? 1.0F : -1.0F;
    const auto overlay = side_ == ShoulderSide::center ? CollisionVec3{0.0F, height, 0.0F} :
        CollisionVec3{sign * units * std::cos(yaw), height, -sign * units * std::sin(yaw)};
    const auto base = owns_ && current == last_written_ ? last_base_ : current;
    const auto value = add(base, overlay);
    ShoulderOffsetDecision result{};
    result.value = value;
    result.translation = owns_ ? subtract(value, last_written_) : overlay;
    result.side = side_;
    result.write = current != value;
    result.side_changed = changed;
    result.force_query = changed || !owned_before;
    owns_ = true;
    last_base_ = base;
    last_written_ = value;
    return result;
}

void gdtpc::ShoulderOffsetModel::reset() noexcept
{
    side_ = ShoulderSide::center;
    last_base_ = {};
    last_written_ = {};
    owns_ = false;
    key_down_ = false;
    edge_count_ = 0;
}

bool gdtpc::valid_collision_settings(const CollisionSettings& settings) noexcept
{
    return std::isfinite(settings.skin) && settings.skin >= 0.0F && settings.skin <= 8.0F &&
        std::isfinite(settings.minimum_distance) && settings.minimum_distance >= 0.5F &&
        settings.minimum_distance <= 40.0F &&
        std::isfinite(settings.extend_units_per_second) && settings.extend_units_per_second > 0.0F &&
        settings.extend_units_per_second <= 500.0F &&
        settings.query_interval >= 1 && settings.query_interval <= 30 &&
        settings.release_confirm_queries >= 1 && settings.release_confirm_queries <= 30 &&
        std::isfinite(settings.release_immediate_units) && settings.release_immediate_units >= 0.0F &&
        settings.release_immediate_units <= 10.0F &&
        settings.fault_limit >= 1 && settings.fault_limit <= 64;
}

gdtpc::CameraCollisionModel::CameraCollisionModel(const CollisionSettings settings) noexcept
    : settings_{settings}
{
    // Invalid configuration must not silently produce a half-working arm. Latch off immediately;
    // step() then returns the desired distance untouched for the life of the model.
    if (!valid_collision_settings(settings_)) state_ = CollisionState::latched_off;
}

// Arbitrates between the player's own zoom and a collision override.
//
// The player's zoom arrives as its own argument, taken from the engine's zoom TARGET, which nothing
// here ever writes. It therefore needs no latching, no hand-back tracking and no deadband: it is
// simply correct on every frame, including across alt-tab, ineligibility and session changes. The
// earlier design inferred it from the camera's live distance, which is the engine's animated
// position and is exactly what collision writes to, and that confusion is what ratcheted the
// player's zoom onto the character.
gdtpc::CollisionDecision gdtpc::CameraCollisionModel::decide(LevelQuery& query, const CollisionVec3& focus,
    const CollisionVec3& toward_camera, const float player_distance, const float observed_distance,
    const float delta_seconds, const bool eligible) noexcept
{
    CollisionDecision decision{};
    if (std::isfinite(player_distance) && player_distance > 0.0F) desired_ = player_distance;
    decision.desired = desired_;

    if (!eligible || state_ == CollisionState::latched_off || !std::isfinite(desired_) || desired_ <= 0.0F)
    {
        // Letting go is all that is required. The engine's zoom target is still the player's own
        // choice, so it animates the camera home without us writing anything.
        overriding_ = false;
        has_written_ = false;
        retry_write_ = false;
        decision.arm = desired_;
        decision.state = state_ == CollisionState::latched_off ? CollisionState::latched_off
                                                              : CollisionState::ineligible;
        if (state_ != CollisionState::latched_off) state_ = CollisionState::ineligible;
        return decision;
    }

    const auto arm = step(query, focus, toward_camera, desired_, delta_seconds, true);
    return finish_decision(arm, observed_distance,
        std::isfinite(observed_distance) && observed_distance > 0.0F);
}

gdtpc::CollisionDecision gdtpc::CameraCollisionModel::decide_camera(CameraLevelQuery& query,
    const float player_distance, const float observed_distance, const float delta_seconds,
    const bool eligible) noexcept
{
    CollisionDecision decision{};
    if (std::isfinite(player_distance) && player_distance > 0.0F) desired_ = player_distance;
    decision.desired = desired_;

    if (!eligible || state_ == CollisionState::latched_off || !std::isfinite(desired_) || desired_ <= 0.0F)
    {
        // Letting go is all that is required. The engine's zoom target is still the player's own
        // choice, so it animates the camera home without us writing anything.
        overriding_ = false;
        has_written_ = false;
        retry_write_ = false;
        decision.arm = desired_;
        decision.state = state_ == CollisionState::latched_off ? CollisionState::latched_off
                                                              : CollisionState::ineligible;
        if (state_ != CollisionState::latched_off) state_ = CollisionState::ineligible;
        return decision;
    }

    return finish_decision(step_camera(query, desired_, delta_seconds, true), observed_distance,
        std::isfinite(observed_distance) && observed_distance > 0.0F);
}

gdtpc::CollisionDecision gdtpc::CameraCollisionModel::finish_decision(const float arm, const float observed,
    const bool observed_valid) noexcept
{
    CollisionDecision decision{};
    decision.arm = arm;
    decision.desired = desired_;
    decision.state = state_;

    // Treat "back at the player's distance" as the end of the override rather than a write, so an
    // unobstructed camera is handed back to the game completely.
    const auto obstructed = std::isfinite(arm) && arm < desired_ - write_epsilon;
    if (!obstructed)
    {
        // Nothing to do but let go. The engine's zoom target is the player's own choice and we never
        // write it, so the camera is already being animated to exactly the right place. A parting
        // write here would only fight that.
        overriding_ = false;
        has_written_ = false;
        retry_write_ = false;
        return decision;
    }

    overriding_ = true;
    // A single write does not hold the camera. The engine keeps animating its own zoom back toward
    // the player's target, so the camera slides straight back out and is yanked in again on the next
    // write: in the third live session it slid from 4.54 to 8.42 over 48 frames, was snapped back by
    // one write, and repeated, which is the bounce. Re-assert whenever the camera is not where we
    // put it, as well as when the arm itself moves.
    const auto drifted = observed_valid && std::abs(observed - arm) > reassert_epsilon;
    if (retry_write_ || !has_written_ || std::abs(arm - last_written_) > write_epsilon || drifted)
    {
        decision.write = true;
        last_written_ = arm;
        has_written_ = true;
    }
    return decision;
}

void gdtpc::CameraCollisionModel::abandon_override() noexcept
{
    overriding_ = false;
    has_written_ = false;
    retry_write_ = false;
    has_arm_ = false;
    since_query_ = 0;
    clear_streak_ = 0;
    if (state_ != CollisionState::latched_off) state_ = CollisionState::ineligible;
}

void gdtpc::CameraCollisionModel::record_write_result(const bool success) noexcept
{
    if (success) { retry_write_ = false; return; }
    retry_write_ = true;
    overriding_ = true;
    record_fault();
    if (state_ == CollisionState::latched_off) { overriding_ = false; retry_write_ = false; }
}

void gdtpc::CameraCollisionModel::reset_session() noexcept
{
    has_arm_ = false;
    clear_streak_ = 0;
    pending_target_ = 0.0F;
    desired_ = 0.0F;
    overriding_ = false;
    has_written_ = false;
    retry_write_ = false;
    last_written_ = 0.0F;
    arm_ = 0.0F;
    target_ = 0.0F;
    since_query_ = 0;
    // The fault latch deliberately survives a session change. A runtime that faulted repeatedly in
    // one world has not earned a fresh chance in the next one.
    if (state_ != CollisionState::latched_off) state_ = CollisionState::ineligible;
}

void gdtpc::CameraCollisionModel::apply_query_target(const float candidate) noexcept
{
    if (!std::isfinite(candidate)) return; // a sample the caller could not qualify never moves the arm

    // Pulling in, or holding, takes effect at once: clipping through geometry is worse than a jump.
    // With no arm established yet there is nothing to bounce against, so the first sample after the
    // camera becomes eligible is taken as-is rather than waiting to be confirmed.
    if (!has_arm_ || candidate <= target_)
    {
        target_ = candidate;
        clear_streak_ = 0;
        return;
    }

    // A small outward step is not chatter. Chatter alternates between a near hit and a far miss, so it
    // is always a large move; an obstruction receding as the player walks away produces a trickle of
    // small ones. Confirming those stalled the arm for nine frames at a time between advances.
    if (candidate - target_ <= settings_.release_immediate_units)
    {
        target_ = candidate;
        clear_streak_ = 0;
        return;
    }

    // Extending outward by more than that. Hold the target where it is until the configured run of
    // queries agrees, and then release only as far as the nearest sample in that run, so a run
    // containing one close reading does not hand back the full arm.
    pending_target_ = clear_streak_ == 0 ? candidate : std::min(pending_target_, candidate);
    if (++clear_streak_ < settings_.release_confirm_queries) return;
    target_ = pending_target_;
    clear_streak_ = 0;
}

void gdtpc::CameraCollisionModel::record_fault() noexcept
{
    // A query that could not be qualified is not evidence of clearance, so it breaks a release run.
    clear_streak_ = 0;
    ++fault_count_;
    if (fault_count_ >= settings_.fault_limit) state_ = CollisionState::latched_off;
}

float gdtpc::CameraCollisionModel::step(LevelQuery& query, const CollisionVec3& focus,
    const CollisionVec3& toward_camera, const float desired_distance, const float delta_seconds,
    const bool eligible) noexcept
{
    if (state_ == CollisionState::latched_off) return desired_distance;

    // Reject every nonfinite input before anything else. A NaN origin fed into a physics raycast is
    // a crash, not a rejected sample, so nothing downstream may ever see one.
    if (!eligible || !std::isfinite(desired_distance) || desired_distance <= 0.0F ||
        !finite_vec(focus) || !finite_vec(toward_camera))
    {
        state_ = CollisionState::ineligible;
        has_arm_ = false;
        since_query_ = 0;
        clear_streak_ = 0;
        // The caller's desired distance is returned untouched, including when it is itself invalid:
        // this model never invents a value, it only ever shortens a good one.
        return desired_distance;
    }

    const auto length_squared = toward_camera.x * toward_camera.x + toward_camera.y * toward_camera.y +
        toward_camera.z * toward_camera.z;
    if (!std::isfinite(length_squared) || length_squared < 1.0e-8F)
    {
        // A degenerate direction cannot produce a unit ray. Not a fault, just nothing to ask.
        state_ = CollisionState::ineligible;
        return desired_distance;
    }

    const auto inverse_length = 1.0F / std::sqrt(length_squared);
    CollisionRay ray{};
    ray.origin = focus;
    ray.direction = {toward_camera.x * inverse_length, toward_camera.y * inverse_length,
        toward_camera.z * inverse_length};
    if (!finite_vec(ray.direction))
    {
        state_ = CollisionState::ineligible;
        return desired_distance;
    }

    // Throttle: the ray is the expensive part and the arm does not need per-frame precision.
    const auto due = !has_arm_ || since_query_ + 1 >= settings_.query_interval;
    if (due)
    {
        since_query_ = 0;
        ++query_count_;
        auto distance = 0.0F;
        if (!query.raycast(ray, desired_distance, distance))
        {
            record_fault();
            if (state_ == CollisionState::latched_off) return desired_distance;
            // A single failed query keeps the previous target rather than snapping the camera.
            if (!has_arm_) return desired_distance;
        }
        else if (!std::isfinite(distance))
        {
            // +infinity is the engine's documented miss sentinel, so a clean miss lands here and is
            // not a fault: it simply means nothing is in the way. Any other nonfinite value, NaN in
            // particular, is invalid and is counted, because the engine's own test would call NaN a
            // hit and drive the camera into the character.
            if (std::isinf(distance) && distance > 0.0F) apply_query_target(desired_distance);
            else
            {
                record_fault();
                if (state_ == CollisionState::latched_off) return desired_distance;
                if (!has_arm_) return desired_distance;
            }
        }
        else if (distance < 0.0F)
        {
            record_fault();
            if (state_ == CollisionState::latched_off) return desired_distance;
            if (!has_arm_) return desired_distance;
        }
        else
        {
            ++hit_count_;
            apply_query_target(std::clamp(distance - settings_.skin, settings_.minimum_distance, desired_distance));
        }
    }
    else ++since_query_;

    if (!has_arm_)
    {
        arm_ = std::clamp(target_, settings_.minimum_distance, desired_distance);
        has_arm_ = true;
    }
    else
    {
        const auto clamped_target = std::clamp(target_, settings_.minimum_distance, desired_distance);
        if (clamped_target <= arm_) arm_ = clamped_target; // pull in immediately; clipping is worse than a jump
        else if (std::isfinite(delta_seconds) && delta_seconds > 0.0F && delta_seconds <= 0.1F)
            arm_ = std::min(clamped_target, arm_ + settings_.extend_units_per_second * delta_seconds);
        // An implausible timestep leaves the arm where it is rather than making up a step.
    }

    arm_ = std::clamp(arm_, settings_.minimum_distance, desired_distance);
    state_ = arm_ < desired_distance ? CollisionState::shortened : CollisionState::idle;
    return arm_;
}


float gdtpc::CameraCollisionModel::step_camera(CameraLevelQuery& query, const float desired_distance,
    const float delta_seconds, const bool eligible) noexcept
{
    if (state_ == CollisionState::latched_off) return desired_distance;
    if (!eligible || !std::isfinite(desired_distance) || desired_distance <= 0.0F)
    {
        state_ = CollisionState::ineligible;
        has_arm_ = false;
        since_query_ = 0;
        clear_streak_ = 0;
        return desired_distance;
    }

    const auto due = !has_arm_ || since_query_ + 1 >= settings_.query_interval;
    if (due)
    {
        since_query_ = 0;
        ++query_count_;
        auto distance = 0.0F;
        CollisionRay ray{};
        if (!query.raycast_from_camera(desired_distance, distance, ray) ||
            !finite_vec(ray.origin) || !finite_vec(ray.direction))
        {
            record_fault();
            if (state_ == CollisionState::latched_off) return desired_distance;
            if (!has_arm_) return desired_distance;
        }
        else
        {
            const auto length_squared = ray.direction.x * ray.direction.x + ray.direction.y * ray.direction.y +
                ray.direction.z * ray.direction.z;
            if (!std::isfinite(length_squared) || std::abs(length_squared - 1.0F) > 0.001F)
            {
                record_fault();
                if (state_ == CollisionState::latched_off) return desired_distance;
                if (!has_arm_) return desired_distance;
            }
            else if (!std::isfinite(distance))
            {
                if (std::isinf(distance) && distance > 0.0F) apply_query_target(desired_distance);
                else
                {
                    record_fault();
                    if (state_ == CollisionState::latched_off) return desired_distance;
                    if (!has_arm_) return desired_distance;
                }
            }
            else if (distance < 0.0F)
            {
                record_fault();
                if (state_ == CollisionState::latched_off) return desired_distance;
                if (!has_arm_) return desired_distance;
            }
            else
            {
                ++hit_count_;
                apply_query_target(std::clamp(distance - settings_.skin, settings_.minimum_distance, desired_distance));
            }
        }
    }
    else ++since_query_;

    if (!has_arm_)
    {
        arm_ = std::clamp(target_, settings_.minimum_distance, desired_distance);
        has_arm_ = true;
    }
    else
    {
        const auto clamped_target = std::clamp(target_, settings_.minimum_distance, desired_distance);
        if (clamped_target <= arm_) arm_ = clamped_target;
        else if (std::isfinite(delta_seconds) && delta_seconds > 0.0F && delta_seconds <= 0.1F)
            arm_ = std::min(clamped_target, arm_ + settings_.extend_units_per_second * delta_seconds);
    }
    state_ = arm_ < desired_distance - 0.001F ? CollisionState::shortened : CollisionState::idle;
    return arm_;
}
