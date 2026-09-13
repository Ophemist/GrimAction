#include "camera_write_adapter.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
float field(const gdtpc::CameraRawSnapshot& snapshot, const std::size_t index) noexcept
{
    return std::bit_cast<float>(snapshot.fields[index]);
}

void set_field(gdtpc::CameraRawSnapshot& snapshot, const std::size_t index, const float value) noexcept
{
    snapshot.fields[index] = std::bit_cast<std::array<std::byte, sizeof(float)>>(value);
}

bool read_camera_field(gdtpc::CameraMemoryAccess& access, const void* camera, const std::size_t index,
    float& value) noexcept
{
    std::array<std::byte, sizeof(float)> raw{};
    if (!access.preflight(camera, gdtpc::camera_field_offsets[index], false) ||
        !access.read(camera, gdtpc::camera_field_offsets[index], raw.data(), raw.size()))
        return false;
    value = std::bit_cast<float>(raw);
    return std::isfinite(value);
}

bool same_field(const std::array<std::byte, sizeof(float)>& left,
    const std::array<std::byte, sizeof(float)>& right) noexcept
{
    return std::memcmp(left.data(), right.data(), sizeof(float)) == 0;
}

bool rollback(gdtpc::CameraMemoryAccess& access, void* camera, const gdtpc::CameraRawSnapshot& preimage,
    gdtpc::CameraWriteJournal& journal) noexcept
{
    auto healthy = true;
    for (std::size_t index = 0; index < gdtpc::camera_field_count; ++index)
    {
        const auto bit = static_cast<std::uint16_t>(1U << index);
        journal.restore_attempted |= bit;
        if (access.write(camera, gdtpc::camera_field_offsets[index], preimage.fields[index].data(), sizeof(float)))
            journal.restored |= bit;
        else healthy = false;
    }
    if (!access.synchronize_zoom(camera, gdtpc::zoom_distance_from_snapshot(preimage))) healthy = false;
    // Synchronization is allowed to update its own current/target fields. Reapply the raw preimage
    // so an in-progress native interpolation is preserved exactly after derived state is refreshed.
    for (std::size_t index = 0; index < gdtpc::camera_field_count; ++index)
        if (!access.write(camera, gdtpc::camera_field_offsets[index], preimage.fields[index].data(), sizeof(float))) healthy = false;
    for (std::size_t index = 0; index < gdtpc::camera_field_count; ++index)
    {
        std::array<std::byte, sizeof(float)> readback{};
        const auto bit = static_cast<std::uint16_t>(1U << index);
        if (access.read(camera, gdtpc::camera_field_offsets[index], readback.data(), readback.size()) &&
            same_field(readback, preimage.fields[index])) journal.restore_verified |= bit;
        else healthy = false;
    }
    return healthy;
}
}

bool gdtpc::valid_camera_snapshot(const CameraRawSnapshot& snapshot) noexcept
{
    const CameraProfile profile{{field(snapshot, 2), field(snapshot, 3), field(snapshot, 0)},
        {field(snapshot, 4), field(snapshot, 5), field(snapshot, 1)}};
    const auto blend = field(snapshot, 6);
    const auto target_blend = field(snapshot, 7);
    const auto endpoint_a = field(snapshot, 8);
    const auto endpoint_b = field(snapshot, 9);
    return valid_camera_profile(profile) && valid_camera_fov_radians(field(snapshot, camera_fov_field_index)) &&
        std::isfinite(blend) && blend >= 0.0F && blend <= 1.0F &&
        std::isfinite(target_blend) && target_blend >= 0.0F && target_blend <= 1.0F &&
        std::isfinite(endpoint_a) && std::isfinite(endpoint_b) && endpoint_a <= endpoint_b;
}

bool gdtpc::valid_camera_fov_radians(const float value) noexcept
{
    // Broad capture/restore bounds preserve a pre-existing native or user-modified value while
    // rejecting degenerate projection angles. Configured ThirdPerson values use a narrower range.
    constexpr float five_degrees = 0.0872664626F;
    constexpr float one_hundred_twenty_degrees = 2.09439510F;
    return std::isfinite(value) && value >= five_degrees && value <= one_hundred_twenty_degrees;
}

bool gdtpc::capture_camera_snapshot(
    CameraMemoryAccess& access, const void* camera, CameraRawSnapshot& snapshot) noexcept
{
    if (camera == nullptr) return false;
    CameraRawSnapshot candidate{};
    for (std::size_t index = 0; index < camera_field_count; ++index)
    {
        if (!access.preflight(camera, camera_field_offsets[index], false) ||
            !access.read(camera, camera_field_offsets[index], candidate.fields[index].data(), sizeof(float))) return false;
    }
    if (!valid_camera_snapshot(candidate)) return false;
    snapshot = candidate;
    return true;
}

bool gdtpc::make_profile_snapshot(
    const CameraProfile& profile, const float zoom_distance, const float fov_radians,
    CameraRawSnapshot& snapshot) noexcept
{
    if (!valid_camera_profile(profile) || !std::isfinite(zoom_distance) ||
        !valid_camera_fov_radians(fov_radians)) return false;
    const auto distance = std::clamp(zoom_distance, profile.distance.minimum, profile.distance.maximum);
    const auto span = profile.distance.maximum - profile.distance.minimum;
    const auto blend = span > 0.0F ? (distance - profile.distance.minimum) / span : 0.0F;
    CameraRawSnapshot candidate{};
    set_field(candidate, 0, profile.distance.initial);
    set_field(candidate, 1, profile.pitch.initial);
    set_field(candidate, 2, profile.distance.minimum);
    set_field(candidate, 3, profile.distance.maximum);
    set_field(candidate, 4, profile.pitch.minimum);
    set_field(candidate, 5, profile.pitch.maximum);
    set_field(candidate, 6, blend);
    set_field(candidate, 7, blend);
    set_field(candidate, 8, profile.distance.minimum);
    set_field(candidate, 9, profile.distance.maximum);
    set_field(candidate, camera_fov_field_index, fov_radians);
    if (!valid_camera_snapshot(candidate)) return false;
    snapshot = candidate;
    return true;
}

float gdtpc::zoom_distance_from_snapshot(const CameraRawSnapshot& snapshot) noexcept
{
    if (!valid_camera_snapshot(snapshot)) return std::numeric_limits<float>::quiet_NaN();
    return field(snapshot, 8) + (field(snapshot, 9) - field(snapshot, 8)) * field(snapshot, 6);
}

namespace
{
// Shared preamble: both entry points refuse a distance the endpoints cannot express, because a blend
// outside [0, 1] is an invalid camera to capture_camera_snapshot and would make every restore refuse,
// the F8 exit and the logical stop included.
gdtpc::CollisionZoomResult move_camera_zoom(gdtpc::CameraMemoryAccess& access, void* camera,
    const float distance) noexcept
{
    if (camera == nullptr || !std::isfinite(distance)) return gdtpc::CollisionZoomResult::invalid_argument;

    float endpoint_a = 0.0F, endpoint_b = 0.0F;
    if (!read_camera_field(access, camera, gdtpc::camera_zoom_endpoint_a_field_index, endpoint_a) ||
        !read_camera_field(access, camera, gdtpc::camera_zoom_endpoint_b_field_index, endpoint_b) ||
        endpoint_a >= endpoint_b)
        return gdtpc::CollisionZoomResult::unreadable;
    if (distance < endpoint_a || distance > endpoint_b) return gdtpc::CollisionZoomResult::out_of_range;
    if (!access.synchronize_zoom(camera, distance))
        return gdtpc::CollisionZoomResult::synchronization_failed;
    return gdtpc::CollisionZoomResult::success;
}
}

gdtpc::CollisionZoomResult gdtpc::write_collision_zoom(CameraMemoryAccess& access, void* camera,
    const float distance) noexcept
{
    // The setter has just overwritten both blend fields with this distance, and leaving the target
    // there is the entire point: the engine has nothing left to animate toward.
    return move_camera_zoom(access, camera, distance);
}

gdtpc::CollisionZoomResult gdtpc::restore_player_zoom(CameraMemoryAccess& access, void* camera,
    const float distance, const float player_target_blend) noexcept
{
    if (!std::isfinite(player_target_blend) || player_target_blend < 0.0F || player_target_blend > 1.0F)
        return CollisionZoomResult::invalid_argument;
    const auto moved = move_camera_zoom(access, camera, distance);
    if (moved != CollisionZoomResult::success) return moved;

    float observed = 0.0F;
    if (!access.preflight(camera, camera_field_offsets[camera_zoom_target_field_index], true) ||
        !access.write(camera, camera_field_offsets[camera_zoom_target_field_index], &player_target_blend,
            sizeof(player_target_blend)) ||
        !read_camera_field(access, camera, camera_zoom_target_field_index, observed) ||
        observed != player_target_blend)
        return CollisionZoomResult::target_repair_failed;
    return CollisionZoomResult::success;
}

gdtpc::CameraTransactionResult gdtpc::write_camera_snapshot(
    CameraMemoryAccess& access, void* camera, const CameraRawSnapshot& desired,
    CameraRawSnapshot& preimage, CameraWriteJournal& journal) noexcept
{
    journal = {};
    if (camera == nullptr || !valid_camera_snapshot(desired)) return CameraTransactionResult::invalid_argument;
    CameraRawSnapshot captured{};
    if (!capture_camera_snapshot(access, camera, captured)) return CameraTransactionResult::capture_failed;
    for (std::size_t index = 0; index < camera_field_count; ++index)
        if (!access.preflight(camera, camera_field_offsets[index], true)) return CameraTransactionResult::preflight_failed;
    preimage = captured;

    for (std::size_t index = 0; index < camera_field_count; ++index)
    {
        const auto bit = static_cast<std::uint16_t>(1U << index);
        journal.attempted |= bit;
        if (!access.write(camera, camera_field_offsets[index], desired.fields[index].data(), sizeof(float)))
            return rollback(access, camera, captured, journal) ? CameraTransactionResult::write_failed_rolled_back
                                                               : CameraTransactionResult::restore_failed;
        journal.written |= bit;
        std::array<std::byte, sizeof(float)> readback{};
        if (!access.read(camera, camera_field_offsets[index], readback.data(), readback.size()) ||
            !same_field(readback, desired.fields[index]))
            return rollback(access, camera, captured, journal) ? CameraTransactionResult::verification_failed_rolled_back
                                                               : CameraTransactionResult::restore_failed;
        journal.verified |= bit;
    }
    journal.synchronization_attempted = true;
    if (!access.synchronize_zoom(camera, zoom_distance_from_snapshot(desired)))
        return rollback(access, camera, captured, journal) ? CameraTransactionResult::synchronization_failed_rolled_back
                                                           : CameraTransactionResult::restore_failed;
    journal.synchronization_completed = true;
    // SetZoom refreshes derived distance/pitch but also owns current/target zoom fields. Restore the
    // requested raw state afterward and verify the complete snapshot byte-for-byte.
    for (std::size_t index = 0; index < camera_field_count; ++index)
    {
        std::array<std::byte, sizeof(float)> readback{};
        if (!access.read(camera, camera_field_offsets[index], readback.data(), readback.size()))
            return rollback(access, camera, captured, journal) ? CameraTransactionResult::verification_failed_rolled_back
                                                               : CameraTransactionResult::restore_failed;
        if (!same_field(readback, desired.fields[index]) &&
            (!access.write(camera, camera_field_offsets[index], desired.fields[index].data(), sizeof(float)) ||
             !access.read(camera, camera_field_offsets[index], readback.data(), readback.size()) ||
             !same_field(readback, desired.fields[index])))
            return rollback(access, camera, captured, journal) ? CameraTransactionResult::verification_failed_rolled_back
                                                               : CameraTransactionResult::restore_failed;
    }
    return CameraTransactionResult::success;
}
