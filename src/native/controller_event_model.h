#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace gdtpc
{
// Exact 16-byte element produced by SteamControllerDevice::Update in Engine.dll build 24825149.
// kind is zero for analog events and one for digital events. The runtime only observes these values;
// it never changes the game's vector or suppresses an event.
struct SteamControllerEvent
{
    std::int32_t action_id{};
    float x{};
    float y{};
    std::uint8_t kind{};
    std::uint8_t padding[3]{};
};
static_assert(sizeof(SteamControllerEvent) == 16);

struct AnalogEventSample
{
    bool valid{};
    std::int32_t action_id{-1};
    float x{};
    float y{};
};

// Select the strongest finite analog event in a single native update. This is deliberately a probe
// policy, not gameplay policy: the marked live sequence identifies which action id represents camera
// aim before any pitch write is designed.
inline AnalogEventSample select_strongest_analog_event(
    const SteamControllerEvent* events, const std::size_t count) noexcept
{
    AnalogEventSample result{};
    if (events == nullptr) return result;
    float strongest = -1.0F;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto& event = events[index];
        if (event.kind != 0 || !std::isfinite(event.x) || !std::isfinite(event.y)) continue;
        const auto magnitude = std::max(std::fabs(event.x), std::fabs(event.y));
        if (magnitude <= strongest) continue;
        strongest = magnitude;
        result = {true, event.action_id, event.x, event.y};
    }
    return result;
}
}
