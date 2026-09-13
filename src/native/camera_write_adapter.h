#pragma once

#include "camera_memory_model.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace gdtpc
{
constexpr std::size_t camera_field_count = 11;
constexpr std::size_t camera_fov_field_index = 10;
constexpr std::array<std::size_t, camera_field_count> camera_field_offsets{
    0x10c, 0x110, 0x570, 0x574, 0x578, 0x57c, 0x580, 0x584, 0x590, 0x594, 0x14};

struct CameraRawSnapshot
{
    std::array<std::array<std::byte, sizeof(float)>, camera_field_count> fields{};
};

class CameraMemoryAccess
{
public:
    virtual ~CameraMemoryAccess() = default;
    [[nodiscard]] virtual bool preflight(const void* camera, std::size_t offset, bool writable) noexcept = 0;
    [[nodiscard]] virtual bool read(const void* camera, std::size_t offset, void* destination, std::size_t size) noexcept = 0;
    [[nodiscard]] virtual bool write(void* camera, std::size_t offset, const void* source, std::size_t size) noexcept = 0;
    [[nodiscard]] virtual bool synchronize_zoom(void*, float) noexcept { return true; }
};

enum class CameraTransactionResult : std::uint32_t
{
    success,
    invalid_argument,
    capture_failed,
    preflight_failed,
    write_failed_rolled_back,
    verification_failed_rolled_back,
    synchronization_failed_rolled_back,
    restore_failed
};

struct CameraWriteJournal
{
    std::uint16_t attempted{};
    std::uint16_t written{};
    std::uint16_t verified{};
    std::uint16_t restore_attempted{};
    std::uint16_t restored{};
    std::uint16_t restore_verified{};
    bool synchronization_attempted{};
    bool synchronization_completed{};
};

// Indices into camera_field_offsets for the zoom state. The engine stores the camera distance as a
// blend between two endpoints: the CURRENT blend is where the camera is now and the engine animates
// it, while the TARGET blend is what the player asked for by scrolling. Only these two are absent
// from the profile residence check, which is what makes the current blend safe for collision to own
// and the endpoints unsafe to touch outside a profile transition.
constexpr std::size_t camera_zoom_blend_field_index = 6;
constexpr std::size_t camera_zoom_target_field_index = 7;
constexpr std::size_t camera_zoom_endpoint_a_field_index = 8;
constexpr std::size_t camera_zoom_endpoint_b_field_index = 9;

// How a collision zoom write ended.
enum class CollisionZoomResult : std::uint32_t
{
    success,
    invalid_argument,     // no camera, or a nonfinite distance
    unreadable,           // the camera's zoom state could not be read, or is not sane
    out_of_range,         // the distance is not expressible as a blend within [0, 1]
    synchronization_failed,
    target_repair_failed  // the player's zoom target could not be given back
};

// Moves the camera to `distance` for collision, leaving the engine's zoom TARGET at that distance so
// the engine has nothing to animate toward and the camera stays exactly where it is put. Live, that
// took the shimmer from a mean of 0.0162 units to 0.0004.
//
// This is only safe because the player's own zoom is held outside the engine, by ZoomStepModel, and
// written back by the caller on release, on ineligibility and before a mode toggle. The native setter
// is used because it refreshes the engine's derived distance and pitch state, but it also OWNS both
// blend fields and overwrites them; a caller that needs the player's zoom left in place must put it
// back itself, which is what `restore_player_zoom` is for.
[[nodiscard]] CollisionZoomResult write_collision_zoom(CameraMemoryAccess& access, void* camera,
    float distance) noexcept;

// Puts the camera at the player's own zoom and leaves the target there, for handing the camera back.
[[nodiscard]] CollisionZoomResult restore_player_zoom(CameraMemoryAccess& access, void* camera,
    float distance, float player_target_blend) noexcept;

[[nodiscard]] bool valid_camera_snapshot(const CameraRawSnapshot& snapshot) noexcept;
[[nodiscard]] bool valid_camera_fov_radians(float value) noexcept;
[[nodiscard]] bool capture_camera_snapshot(
    CameraMemoryAccess& access, const void* camera, CameraRawSnapshot& snapshot) noexcept;
[[nodiscard]] bool make_profile_snapshot(
    const CameraProfile& profile, float zoom_distance, float fov_radians, CameraRawSnapshot& snapshot) noexcept;
[[nodiscard]] float zoom_distance_from_snapshot(const CameraRawSnapshot& snapshot) noexcept;

// Captures the complete preimage before the first write, preflights the complete allowlist,
// writes and verifies one field at a time, and restores/verifies the full preimage on any failure.
[[nodiscard]] CameraTransactionResult write_camera_snapshot(
    CameraMemoryAccess& access, void* camera, const CameraRawSnapshot& desired,
    CameraRawSnapshot& preimage, CameraWriteJournal& journal) noexcept;
}
