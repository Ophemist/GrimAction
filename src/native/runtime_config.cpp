#include "runtime_config.h"

#include "camera_collision_model.h"
#include "camera_memory_model.h"
#include "mouse_look_model.h"
#include "virtual_zoom_model.h"

#include <charconv>
#include <cmath>

namespace
{
std::string_view trim(std::string_view value) noexcept
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.remove_suffix(1);
    return value;
}

bool parse_float(const std::string_view text, float& value) noexcept
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
}

bool parse_bool(const std::string_view text, bool& value) noexcept
{
    if (text == "true") { value = true; return true; }
    if (text == "false") { value = false; return true; }
    return false;
}

bool parse_u32(const std::string_view text, std::uint32_t& value) noexcept
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

bool parse_toggle_key(const std::string_view text, std::uint32_t& value) noexcept
{
    if (text.size() < 2 || text.front() != 'F') return false;
    int number{};
    const auto digits = text.substr(1);
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
    if (error != std::errc{} || end != digits.data() + digits.size() || number < 1 || number > 12) return false;
    value = 0x70U + static_cast<std::uint32_t>(number - 1);
    return true;
}
}

gdtpc::ZoomStepSettings gdtpc::zoom_step_settings(const RuntimeConfig& config) noexcept
{
    ZoomStepSettings settings{};
    settings.ratio = 1.0F + static_cast<float>(config.zoom_step_percent) / 100.0F;
    return settings;
}

gdtpc::VirtualZoomSettings gdtpc::virtual_zoom_settings(const RuntimeConfig& config) noexcept
{
    VirtualZoomSettings settings{};
    const auto steps = zoom_step_settings(config);
    settings.enabled = config.virtual_zoom_enabled;
    settings.engine_distance = config.virtual_zoom_engine_distance;
    settings.visual_minimum = config.third_person_camera.distance.minimum;
    settings.visual_default = config.third_person_camera.distance.initial;
    settings.ratio = steps.ratio;
    settings.engine_step = steps.engine_step;
    settings.step_tolerance = steps.step_tolerance;
    return settings;
}

bool gdtpc::parse_runtime_config(const std::string_view text, RuntimeConfig& result, std::string& error)
{
    if (text.empty() || text.size() > 65536)
    {
        error = "Configuration must contain 1 through 65536 bytes.";
        return false;
    }
    RuntimeConfig candidate;
    std::string_view section;
    bool general_seen = false;
    bool third_person_seen = false;
    std::uint64_t seen = 0;
    std::size_t line_number = 0;
    for (std::size_t start = 0; start <= text.size();)
    {
        ++line_number;
        const auto newline = text.find('\n', start);
        auto line = trim(text.substr(start, newline == std::string_view::npos ? text.size() - start : newline - start));
        start = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        if (line.empty() || line.front() == ';' || line.front() == '#') continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = trim(line.substr(1, line.size() - 2));
            if (section != "general" && section != "third_person")
            {
                error = "Unknown section on line " + std::to_string(line_number) + '.';
                return false;
            }
            auto& section_seen = section == "general" ? general_seen : third_person_seen;
            if (section_seen)
            {
                error = "Duplicate section on line " + std::to_string(line_number) + '.';
                return false;
            }
            section_seen = true;
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos || section.empty())
        {
            error = "Malformed setting on line " + std::to_string(line_number) + '.';
            return false;
        }
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        auto valid = false;
        std::uint64_t key_bit = 0;
        if (section == "general")
        {
            if (key == "schema_version") { key_bit = 1U << 0; valid = parse_u32(value, candidate.schema_version); }
            else if (key == "toggle_key") { key_bit = 1U << 1; valid = parse_toggle_key(value, candidate.toggle_virtual_key); }
            else if (key == "start_in_third_person") { key_bit = 1U << 2; valid = parse_bool(value, candidate.start_in_third_person); }
            else if (key == "persist_mode") { key_bit = 1U << 3; valid = parse_bool(value, candidate.persist_mode); }
        }
        else
        {
            if (key == "distance_min") { key_bit = 1U << 4; valid = parse_float(value, candidate.third_person_camera.distance.minimum); }
            else if (key == "distance_max") { key_bit = 1U << 5; valid = parse_float(value, candidate.third_person_camera.distance.maximum); }
            else if (key == "distance_default") { key_bit = 1U << 6; valid = parse_float(value, candidate.third_person_camera.distance.initial); }
            else if (key == "pitch_min") { key_bit = 1U << 7; valid = parse_float(value, candidate.third_person_camera.pitch.minimum); }
            else if (key == "pitch_max") { key_bit = 1U << 8; valid = parse_float(value, candidate.third_person_camera.pitch.maximum); }
            else if (key == "pitch_default") { key_bit = 1U << 9; valid = parse_float(value, candidate.third_person_camera.pitch.initial); }
            else if (key == "follow_delay_seconds") { key_bit = 1U << 10; valid = parse_float(value, candidate.follow_delay_seconds); }
            else if (key == "follow_smooth_seconds") { key_bit = 1U << 11; valid = parse_float(value, candidate.follow_smooth_seconds); }
            else if (key == "follow_max_radians_per_second") { key_bit = 1U << 12; valid = parse_float(value, candidate.follow_max_radians_per_second); }
            else if (key == "aim_yaw_offset_degrees") { key_bit = 1U << 13; valid = parse_float(value, candidate.aim_yaw_offset_degrees); }
            else if (key == "fov_degrees") { key_bit = 1U << 14; valid = parse_float(value, candidate.third_person_fov_degrees); }
            else if (key == "collision_enabled") { key_bit = 1U << 15; valid = parse_bool(value, candidate.collision_enabled); }
            else if (key == "collision_skin") { key_bit = 1U << 16; valid = parse_float(value, candidate.collision.skin); }
            else if (key == "collision_min_distance") { key_bit = 1U << 17; valid = parse_float(value, candidate.collision.minimum_distance); }
            else if (key == "collision_extend_per_second") { key_bit = 1U << 18; valid = parse_float(value, candidate.collision.extend_units_per_second); }
            else if (key == "collision_query_interval") { key_bit = 1U << 19; valid = parse_u32(value, candidate.collision.query_interval); }
            else if (key == "collision_fault_limit") { key_bit = 1U << 20; valid = parse_u32(value, candidate.collision.fault_limit); }
            else if (key == "collision_release_queries") { key_bit = 1U << 21; valid = parse_u32(value, candidate.collision.release_confirm_queries); }
            else if (key == "zoom_step_enabled") { key_bit = 1U << 22; valid = parse_bool(value, candidate.zoom_step_enabled); }
            else if (key == "zoom_step_percent") { key_bit = 1U << 23; valid = parse_u32(value, candidate.zoom_step_percent); }
            else if (key == "collision_release_step") { key_bit = 1U << 24; valid = parse_float(value, candidate.collision.release_immediate_units); }
            else if (key == "shoulder_offset_enabled") { key_bit = 1U << 25; valid = parse_bool(value, candidate.shoulder_offset_enabled); }
            else if (key == "shoulder_offset_units") { key_bit = 1U << 26; valid = parse_float(value, candidate.shoulder_offset_units); }
            else if (key == "shoulder_height_units") { key_bit = 1U << 27; valid = parse_float(value, candidate.shoulder_height_units); }
            else if (key == "ui_probe_enabled") { key_bit = 1U << 28; valid = parse_bool(value, candidate.ui_probe_enabled); }
            else if (key == "mouse_look_enabled") { key_bit = 1ULL << 29; valid = parse_bool(value, candidate.mouse_look.enabled); }
            else if (key == "mouse_look_yaw_radians_per_pixel") { key_bit = 1ULL << 30; valid = parse_float(value, candidate.mouse_look.yaw_radians_per_pixel); }
            else if (key == "mouse_look_invert_x") { key_bit = 1ULL << 31; valid = parse_bool(value, candidate.mouse_look.invert_x); }
            else if (key == "aim_start") { key_bit = 1ULL << 32; valid = parse_float(value, candidate.mouse_look.aim_start); }
            else if (key == "aim_band_top") { key_bit = 1ULL << 33; valid = parse_float(value, candidate.mouse_look.aim_band_top); }
            else if (key == "aim_band_bottom") { key_bit = 1ULL << 34; valid = parse_float(value, candidate.mouse_look.aim_band_bottom); }
            else if (key == "aim_vertical_scale") { key_bit = 1ULL << 35; valid = parse_float(value, candidate.mouse_look.aim_vertical_scale); }
            else if (key == "menu_release_frames") { key_bit = 1ULL << 36; valid = parse_u32(value, candidate.mouse_look.menu_release_frames); }
            else if (key == "mouse_look_yaw_catchup_per_second") { key_bit = 1ULL << 37; valid = parse_float(value, candidate.mouse_look.yaw_catchup_per_second); }
            else if (key == "mouse_look_pitch_degrees_per_pixel") { key_bit = 1ULL << 38; valid = parse_float(value, candidate.mouse_look.pitch_degrees_per_pixel); }
            else if (key == "mouse_look_pitch_offset_min") { key_bit = 1ULL << 39; valid = parse_float(value, candidate.mouse_look.pitch_offset_min); }
            else if (key == "mouse_look_pitch_offset_max") { key_bit = 1ULL << 40; valid = parse_float(value, candidate.mouse_look.pitch_offset_max); }
            else if (key == "mouse_look_pitch_floor_degrees") { key_bit = 1ULL << 41; valid = parse_float(value, candidate.mouse_look.pitch_floor_degrees); }
            else if (key == "virtual_zoom_enabled") { key_bit = 1ULL << 42; valid = parse_bool(value, candidate.virtual_zoom_enabled); }
            else if (key == "virtual_zoom_engine_distance") { key_bit = 1ULL << 43; valid = parse_float(value, candidate.virtual_zoom_engine_distance); }
            else if (key == "third_person_far_plane_percent") { key_bit = 1ULL << 44; valid = parse_u32(value, candidate.third_person_far_plane_percent); }
            else if (key == "npc_dialog_releases_cursor") { key_bit = 1ULL << 45; valid = parse_bool(value, candidate.npc_dialog_releases_cursor); }
            else if (key == "mouse_look_dot_cursor") { key_bit = 1ULL << 46; valid = parse_bool(value, candidate.mouse_look_dot_cursor); }
            else if (key == "right_stick_pitch_enabled") { key_bit = 1ULL << 47; valid = parse_bool(value, candidate.right_stick_pitch_enabled); }
            else if (key == "right_stick_pitch_degrees_per_second") { key_bit = 1ULL << 48; valid = parse_float(value, candidate.right_stick_pitch_degrees_per_second); }
            else if (key == "right_stick_pitch_invert") { key_bit = 1ULL << 49; valid = parse_bool(value, candidate.right_stick_pitch_invert); }
        }
        if (!valid)
        {
            error = "Unknown or invalid setting on line " + std::to_string(line_number) + '.';
            return false;
        }
        if ((seen & key_bit) != 0)
        {
            error = "Duplicate setting on line " + std::to_string(line_number) + '.';
            return false;
        }
        seen |= key_bit;
    }

    constexpr std::uint64_t all_keys = (1ULL << 50) - 1;
    const auto& distance = candidate.third_person_camera.distance;
    const auto virtual_settings = virtual_zoom_settings(candidate);
    // The held engine distance must sit inside the profile endpoints with room below the far end for an
    // outward click to move the target at all: the engine clamps its target to [0, 1].
    const auto engine_blend = distance.maximum > distance.minimum
        ? (candidate.virtual_zoom_engine_distance - distance.minimum) / (distance.maximum - distance.minimum)
        : -1.0F;
    const auto virtual_zoom_valid = valid_virtual_zoom_settings(virtual_settings) &&
        candidate.virtual_zoom_engine_distance > distance.minimum &&
        candidate.virtual_zoom_engine_distance <= distance.maximum &&
        engine_blend >= 0.0F && engine_blend <= maximum_hold_blend(virtual_settings);
    if (seen != all_keys || candidate.schema_version != 1 || candidate.toggle_virtual_key != 0x77 ||
        candidate.start_in_third_person || candidate.persist_mode ||
        !valid_camera_profile(candidate.third_person_camera) ||
        candidate.third_person_camera.distance.minimum < 1.0F || candidate.third_person_camera.distance.maximum > 100.0F ||
        candidate.third_person_camera.pitch.minimum < 1.0F || candidate.third_person_camera.pitch.maximum > 89.0F ||
        candidate.third_person_fov_degrees < 30.0F || candidate.third_person_fov_degrees > 60.0F ||
        candidate.follow_delay_seconds < 0.0F || candidate.follow_delay_seconds > 5.0F ||
        candidate.follow_smooth_seconds < 0.05F || candidate.follow_smooth_seconds > 2.0F ||
        candidate.follow_max_radians_per_second < 0.1F || candidate.follow_max_radians_per_second > 6.0F ||
        std::abs(candidate.aim_yaw_offset_degrees) > 180.0F ||
        // The collision settings are validated by the same predicate the model uses, so a
        // configuration the model would refuse can never reach it.
        !valid_collision_settings(candidate.collision) ||
        // The collision floor must sit at or ABOVE the profile's own minimum distance. The engine
        // stores zoom as a blend between the profile's two distance endpoints, and the write adapter
        // treats a blend outside [0, 1] as an invalid camera. A collision arm shorter than the
        // profile minimum therefore drives the blend negative and every restore, the F8 exit and the
        // logical stop included, is refused until the engine's own animation drags the blend back to
        // zero. That was observed live for 283 consecutive frames.
        candidate.collision.minimum_distance < candidate.third_person_camera.distance.minimum ||
        // Validated by the same predicate the stepping model uses, so a configuration the model would
        // refuse can never reach it. One percent per click would take hundreds of clicks to cross the
        // range; anything above three hundred is coarser than the engine's own step.
        candidate.zoom_step_percent < 1 || candidate.zoom_step_percent > 300 ||
        !valid_zoom_step_settings(zoom_step_settings(candidate)) ||
        // Collision leaves the engine's zoom target at the arm, which puts the player's zoom in
        // ZoomStepModel's custody. Without stepping enabled there is nothing holding it, and a
        // collision write would have nothing to hand the camera back to.
        (candidate.collision_enabled && !candidate.zoom_step_enabled) ||
        candidate.shoulder_offset_units < 0.25F || candidate.shoulder_offset_units > 8.0F ||
        !(candidate.shoulder_height_units >= 0.0F && candidate.shoulder_height_units <= 10.0F) ||
        (candidate.shoulder_offset_enabled && !candidate.collision_enabled) ||
        // Validated by the same predicate the model uses. Mouse look lives in the collision runtime's
        // camera callback and relies on its third-person ownership, so it is refused without collision.
        !gdtpc::valid_mouse_look_settings(candidate.mouse_look) ||
        (candidate.mouse_look.enabled && !candidate.collision_enabled) ||
        // Validated whether or not enabled, like mouse look. Virtual zoom composes its pitch with the
        // mouse-look overlay and runs its collision through the collision runtime, so it needs both.
        !virtual_zoom_valid ||
        (candidate.virtual_zoom_enabled && !candidate.mouse_look.enabled) ||
        // The cap applies only while mouse look applies, so a cap without mouse look would never act.
        !valid_far_plane_percent(candidate.third_person_far_plane_percent) ||
        (candidate.third_person_far_plane_percent < 100 && !candidate.mouse_look.enabled) ||
        (candidate.npc_dialog_releases_cursor && !candidate.mouse_look.enabled) ||
        (candidate.mouse_look_dot_cursor && !candidate.mouse_look.enabled) ||
        !(candidate.right_stick_pitch_degrees_per_second >= 10.0F && candidate.right_stick_pitch_degrees_per_second <= 360.0F) ||
        (candidate.right_stick_pitch_enabled && !candidate.mouse_look.enabled))
    {
        error = "Configuration values are outside their safe ranges.";
        return false;
    }
    result = candidate;
    error.clear();
    return true;
}
