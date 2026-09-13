#include "aim_memory_model.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{
constexpr std::size_t camera_player_offset = 0x118;
constexpr std::size_t entity_facing_offset = 0x108;

bool readable_range(const void* address, const std::size_t size) noexcept
{
    if (address == nullptr || size == 0) return false;
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(address, &information, sizeof(information)) != sizeof(information) ||
        information.State != MEM_COMMIT || (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto region_end = reinterpret_cast<std::uintptr_t>(information.BaseAddress) + information.RegionSize;
    return start <= region_end && size <= region_end - start;
}
}

bool gdtpc::capture_aim_heading(const void* game_camera, AimSample& sample) noexcept
{
    if (game_camera == nullptr) return false;
    const auto player_slot = static_cast<const std::byte*>(game_camera) + camera_player_offset;
    if (!readable_range(player_slot, sizeof(void*))) return false;

    const void* player{};
    std::memcpy(&player, player_slot, sizeof(player));
    if (player == nullptr) return false;
    const auto facing = static_cast<const std::byte*>(player) + entity_facing_offset;
    if (!readable_range(facing, sizeof(float) * 3)) return false;

    AimSample candidate{};
    std::memcpy(&candidate.facing_x, facing, sizeof(float));
    std::memcpy(&candidate.facing_y, facing + sizeof(float), sizeof(float));
    std::memcpy(&candidate.facing_z, facing + sizeof(float) * 2, sizeof(float));
    if (!std::isfinite(candidate.facing_x) || !std::isfinite(candidate.facing_y) ||
        !std::isfinite(candidate.facing_z)) return false;
    const auto horizontal_length_squared = candidate.facing_x * candidate.facing_x +
        candidate.facing_z * candidate.facing_z;
    if (horizontal_length_squared < 0.0001F || horizontal_length_squared > 4.0F) return false;
    candidate.heading_radians = std::atan2(candidate.facing_z, candidate.facing_x);
    sample = candidate;
    return true;
}
