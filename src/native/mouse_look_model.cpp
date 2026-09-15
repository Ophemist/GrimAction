#include "mouse_look_model.h"

#include <cmath>

namespace
{
bool usable_rect(const gdtpc::MouseLookRect& rect) noexcept
{
    // A minimised or zero-sized client area has nothing to aim into.
    return rect.right - rect.left >= 16 && rect.bottom - rect.top >= 16;
}

float clamp_value(const float value, const float low, const float high) noexcept
{
    return value < low ? low : value > high ? high : value;
}

bool same_rect(const gdtpc::MouseLookRect& a, const gdtpc::MouseLookRect& b) noexcept
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
}

bool gdtpc::valid_mouse_look_settings(const MouseLookSettings& s) noexcept
{
    return std::isfinite(s.yaw_radians_per_pixel) && s.yaw_radians_per_pixel > 0.0F &&
        s.yaw_radians_per_pixel <= 0.05F &&
        std::isfinite(s.aim_band_top) && std::isfinite(s.aim_band_bottom) && std::isfinite(s.aim_start) &&
        s.aim_band_top >= 0.05F && s.aim_band_bottom <= 0.95F && s.aim_band_top < s.aim_band_bottom &&
        s.aim_start >= s.aim_band_top && s.aim_start <= s.aim_band_bottom &&
        std::isfinite(s.aim_vertical_scale) && s.aim_vertical_scale > 0.0F && s.aim_vertical_scale <= 10.0F &&
        s.menu_release_frames >= 1 && s.menu_release_frames <= 60 &&
        std::isfinite(s.yaw_catchup_per_second) && s.yaw_catchup_per_second >= 0.0F && s.yaw_catchup_per_second <= 60.0F &&
        std::isfinite(s.pitch_degrees_per_pixel) && s.pitch_degrees_per_pixel > 0.0F && s.pitch_degrees_per_pixel <= 2.0F &&
        std::isfinite(s.pitch_offset_min) && std::isfinite(s.pitch_offset_max) &&
        s.pitch_offset_min >= -90.0F && s.pitch_offset_max <= 60.0F && s.pitch_offset_min <= 0.0F && s.pitch_offset_max >= 0.0F &&
        std::isfinite(s.pitch_floor_degrees) && s.pitch_floor_degrees >= -80.0F && s.pitch_floor_degrees <= 45.0F;
}

std::uint32_t gdtpc::classify_panel_open_flags(const std::uint8_t inventory, const std::uint8_t quest,
    const std::uint8_t skills, const std::uint8_t map, const std::uint8_t escape_primary,
    const std::uint8_t escape_confirm, const std::uint8_t factions, const std::uint8_t loot_filter) noexcept
{
    std::uint32_t flags = 0;
    if (inventory == 1) flags |= 1U << 0;
    if (quest == 1) flags |= 1U << 1;
    if (skills == 1) flags |= 1U << 2;
    if (map == 1) flags |= 1U << 3;
    if (escape_primary == 1 && escape_confirm == 1) flags |= 1U << 4;
    if (factions == 1) flags |= 1U << 5;
    if (loot_filter == 1) flags |= 1U << 6;
    return flags;
}

float gdtpc::yaw_catchup_fraction(const float per_second, const float delta_seconds) noexcept
{
    if (!std::isfinite(per_second) || !std::isfinite(delta_seconds) || per_second <= 0.0F ||
        delta_seconds <= 0.0F || delta_seconds > 0.1F) return 0.0F;
    return 1.0F - std::exp(-per_second * delta_seconds);
}

float gdtpc::stick_pitch_degrees(const std::int16_t thumb_y, const float degrees_per_second, const bool invert,
    const float delta_seconds) noexcept
{
    constexpr float deadzone = 8689.0F; // XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE
    if (!std::isfinite(degrees_per_second) || degrees_per_second <= 0.0F || !std::isfinite(delta_seconds) ||
        delta_seconds <= 0.0F || delta_seconds > 0.1F) return 0.0F;
    const auto value = static_cast<float>(thumb_y);
    const auto magnitude = std::abs(value);
    if (magnitude <= deadzone) return 0.0F;
    const auto scaled = (magnitude > 32767.0F ? 1.0F : (magnitude - deadzone) / (32767.0F - deadzone)) * (value > 0.0F ? 1.0F : -1.0F);
    const auto degrees = -scaled * degrees_per_second * delta_seconds;
    return invert ? -degrees : degrees;
}

float gdtpc::apply_pitch_offset(const float native_radians, const float offset_degrees, const float floor_degrees) noexcept
{
    constexpr float radians_per_degree = 0.01745329251994329577F;
    if (!std::isfinite(native_radians) || !std::isfinite(offset_degrees) || !std::isfinite(floor_degrees)) return native_radians;
    const auto degrees = native_radians / radians_per_degree + offset_degrees;
    // The floor may be negative: looking up past horizontal is allowed by player decision (the camera may
    // clip into the ground). The engine's own clamp is 0..89; only the upper bound is kept.
    const auto low = floor_degrees < -80.0F ? -80.0F : floor_degrees;
    const auto clamped = degrees < low ? low : degrees > 89.0F ? 89.0F : degrees;
    return clamped * radians_per_degree;
}

float gdtpc::PitchOverlay::step(const float current_radians, const float offset_degrees, const float floor_degrees) noexcept
{
    if (!std::isfinite(current_radians)) return current_radians;
    // The engine clamps the field to [0, 89] degrees every frame, so a value we wrote below horizontal comes
    // back as exactly 0. Live, reading that 0 as a new native pitch made the camera alternate between
    // native+offset and 0+offset (1662 jumps once the final pitch crossed 0).
    const auto engine_view = written_ < 0.0F ? 0.0F : written_;
    const auto ours = owned_ && (std::abs(current_radians - written_) <= tolerance_radians ||
                                 std::abs(current_radians - engine_view) <= tolerance_radians);
    if (!ours) native_ = current_radians;
    written_ = apply_pitch_offset(native_, offset_degrees, floor_degrees);
    owned_ = true;
    return written_;
}

float gdtpc::PitchOverlay::step_from_base(const float base_radians, const float offset_degrees, const float floor_degrees) noexcept
{
    if (!std::isfinite(base_radians)) return base_radians;
    native_ = base_radians;
    written_ = apply_pitch_offset(base_radians, offset_degrees, floor_degrees);
    owned_ = true;
    return written_;
}

gdtpc::MouseLookModel::MouseLookModel(const MouseLookSettings settings) noexcept
    : settings_{settings}, valid_{valid_mouse_look_settings(settings)}, aim_y_{settings.aim_start}
{
}

void gdtpc::MouseLookModel::reset() noexcept
{
    captured_ = false;
    menu_ = false;
    menu_clear_frames_ = 0;
    aim_y_ = settings_.aim_start;
    pitch_offset_ = 0.0F;
    last_warp_x_ = last_warp_y_ = 0;
    last_client_ = {};
}

gdtpc::MouseLookDecision gdtpc::MouseLookModel::step(const MouseLookInput& input) noexcept
{
    // The menu debounce runs every frame, captured or not, so a menu closing while Alt is held is
    // already settled when Alt is released.
    if (input.menu_raw)
    {
        menu_ = true;
        menu_clear_frames_ = 0;
    }
    else if (menu_ && ++menu_clear_frames_ >= settings_.menu_release_frames)
    {
        menu_ = false;
        menu_clear_frames_ = 0;
    }

    MouseLookDecision decision{};
    decision.menu = menu_;
    decision.aim_y = aim_y_;
    if (!settings_.enabled || !valid_) decision.state = MouseLookState::disabled;
    else if (!input.eligible || !input.mouse_input_active || !usable_rect(input.client)) decision.state = MouseLookState::ineligible;
    else if (input.alt_down) decision.state = MouseLookState::alt;
    else if (menu_ && !input.combat) decision.state = MouseLookState::menu;
    else decision.state = MouseLookState::captured;

    // The pitch offset keeps being applied while Alt or a menu frees the cursor, so the camera does not
    // snap back; it is only changed by captured movement.
    decision.apply_pitch = decision.state == MouseLookState::captured || decision.state == MouseLookState::alt ||
        decision.state == MouseLookState::menu;
    decision.pitch_offset_degrees = pitch_offset_;
    const auto was_captured = captured_;
    captured_ = decision.state == MouseLookState::captured;
    decision.captured = captured_;
    decision.capture_edge = captured_ && !was_captured;
    decision.release_edge = !captured_ && was_captured;
    if (!captured_) return decision;

    const auto height = static_cast<float>(input.client.bottom - input.client.top);
    // A resized or moved window, a failed cursor read, or a capture edge gives no trustworthy delta.
    decision.reanchor = decision.capture_edge || !input.cursor_valid || !same_rect(input.client, last_client_);
    auto pitch_min = settings_.pitch_offset_min;
    if (input.native_pitch_valid && std::isfinite(input.native_pitch_degrees))
    {
        const auto floor_offset = settings_.pitch_floor_degrees - input.native_pitch_degrees;
        if (floor_offset > pitch_min) pitch_min = floor_offset > 0.0F ? 0.0F : floor_offset;
    }
    if (!decision.reanchor)
    {
        const auto dx = static_cast<float>(input.cursor_x - last_warp_x_);
        const auto dy = static_cast<float>(input.cursor_y - last_warp_y_);
        decision.yaw_delta = dx * settings_.yaw_radians_per_pixel * (settings_.invert_x ? -1.0F : 1.0F);
        slide_vertical(dy, height, pitch_min);
    }
    // The right stick has no cursor delta to lose on a re-anchor, and never spills into the aim band.
    if (std::isfinite(input.pitch_stick_degrees) && input.pitch_stick_degrees != 0.0F)
    {
        const auto low = pitch_offset_ < pitch_min ? pitch_offset_ : pitch_min;
        pitch_offset_ = clamp_value(pitch_offset_ + input.pitch_stick_degrees, low, settings_.pitch_offset_max);
    }
    decision.pitch_offset_degrees = pitch_offset_;
    decision.aim_y = aim_y_;
    decision.warp = true;
    decision.warp_x = input.client.left + (input.client.right - input.client.left) / 2;
    decision.warp_y = input.client.top + static_cast<std::int32_t>(std::lround(aim_y_ * height));
    last_warp_x_ = decision.warp_x;
    last_warp_y_ = decision.warp_y;
    last_client_ = input.client;
    return decision;
}

// One vertical axis in three stretches. Moving down: an aim point that had spilled above its start
// returns to it first, then the pitch offset rises to its maximum, then the aim point slides down the band.
// Moving up mirrors that. Reversing direction therefore always retraces the same path.
void gdtpc::MouseLookModel::slide_vertical(const float dy_pixels, const float height, const float pitch_min) noexcept
{
    if (!std::isfinite(dy_pixels) || dy_pixels == 0.0F || !(height > 0.0F)) return;
    const auto aim_per_pixel = settings_.aim_vertical_scale / height;
    auto remaining = dy_pixels;
    const auto down = remaining > 0.0F;

    // 1. Bring a spilled aim point back toward its start.
    const auto toward_start = down ? settings_.aim_start - aim_y_ : aim_y_ - settings_.aim_start;
    if (toward_start > 0.0F)
    {
        const auto pixels = toward_start / aim_per_pixel;
        const auto used = std::abs(remaining) < pixels ? std::abs(remaining) : pixels;
        aim_y_ += (down ? used : -used) * aim_per_pixel;
        remaining += down ? -used : used;
        if (std::abs(remaining) <= 0.0F) return;
    }

    // 2. Pitch offset within its limits.
    const auto wanted = pitch_offset_ + remaining * settings_.pitch_degrees_per_pixel;
    // An offset already below a newly raised minimum (zoom changed) is not yanked; it only cannot go lower.
    const auto low = pitch_offset_ < pitch_min ? pitch_offset_ : pitch_min;
    const auto pitched = clamp_value(wanted, low, settings_.pitch_offset_max);
    remaining -= (pitched - pitch_offset_) / settings_.pitch_degrees_per_pixel;
    pitch_offset_ = pitched;
    if (std::abs(remaining) < 0.0001F) return;

    // 3. Spill into the aim band.
    const auto aim = aim_y_ + remaining * aim_per_pixel;
    if (std::isfinite(aim)) aim_y_ = clamp_value(aim, settings_.aim_band_top, settings_.aim_band_bottom);
}

gdtpc::NpcTalkSignal gdtpc::classify_npc_talk(const PlayerControllerSample& sample,
    const std::uintptr_t player_controller_vtable, const std::uintptr_t talk_to_npc_vtable) noexcept
{
    if (player_controller_vtable == 0 || talk_to_npc_vtable == 0 || !sample.readable) return NpcTalkSignal::unknown;
    if (sample.controller_vtable != player_controller_vtable) return NpcTalkSignal::not_player_controller;
    // state_count is the temporary-state count; the executing state may be the regular one with no temporaries.
    if (sample.top_state_vtable == 0) return NpcTalkSignal::idle;
    return sample.top_state_vtable == talk_to_npc_vtable ? NpcTalkSignal::talking : NpcTalkSignal::idle;
}

bool gdtpc::CursorHandleOverlay::step(const std::uintptr_t current, const std::uintptr_t dot, std::uintptr_t& value) noexcept
{
    if (dot == 0) return false;
    // Anything but our dot is the game's own cursor, including a change it made while we owned the field. The dot itself is
    // never adopted: a field left holding it (ownership lost) keeps the last known game cursor as the one to restore.
    if (current != dot) base_ = current;
    owned_ = true;
    dot_ = dot;
    value = dot;
    return true;
}

bool gdtpc::CursorHandleOverlay::relinquish(const std::uintptr_t current, std::uintptr_t& restore) noexcept
{
    const auto restorable = owned_ && current == dot_;
    restore = base_;
    owned_ = false;
    return restorable;
}
