#include "runtime_config.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void near(const float actual, const float expected)
{
    if (!std::isfinite(actual) || !std::isfinite(expected) || std::abs(actual - expected) > 0.0001F)
        throw std::runtime_error("Values differ or are nonfinite.");
}
}

int main(const int argc, char** argv)
{
    try
    {
        constexpr std::string_view valid = R"ini(
# comment
[general]
schema_version=1
toggle_key=F8
start_in_third_person=false
persist_mode=false

[third_person]
distance_min=8
distance_max=32
distance_default=18
pitch_min=18
pitch_max=34
pitch_default=26
fov_degrees=45
follow_delay_seconds=0.4
follow_smooth_seconds=0.3
follow_max_radians_per_second=6
aim_yaw_offset_degrees=-90
collision_enabled=false
collision_skin=0.35
collision_min_distance=8
collision_extend_per_second=18
collision_query_interval=3
collision_fault_limit=4
collision_release_queries=3
collision_release_step=1
zoom_step_enabled=false
zoom_step_percent=14
shoulder_offset_enabled=false
shoulder_offset_units=1.5
shoulder_height_units=2
ui_probe_enabled=false
mouse_look_enabled=false
mouse_look_yaw_radians_per_pixel=0.003
mouse_look_invert_x=false
aim_start=0.5
aim_band_top=0.25
aim_band_bottom=0.75
aim_vertical_scale=1
menu_release_frames=3
mouse_look_yaw_catchup_per_second=6
mouse_look_pitch_degrees_per_pixel=0.1
mouse_look_pitch_offset_min=-15
mouse_look_pitch_offset_max=25
mouse_look_pitch_floor_degrees=1
virtual_zoom_enabled=false
virtual_zoom_engine_distance=28
third_person_far_plane_percent=100
npc_dialog_releases_cursor=false
mouse_look_dot_cursor=false
right_stick_pitch_enabled=false
right_stick_pitch_degrees_per_second=90
right_stick_pitch_invert=false
)ini";
        gdtpc::RuntimeConfig config;
        std::string error;
        require(gdtpc::parse_runtime_config(valid, config, error), "Valid configuration was rejected.");
        require(config.toggle_virtual_key == 0x77, "F8 was parsed incorrectly.");
        require(!config.start_in_third_person && !config.persist_mode, "Gate 0 booleans were parsed incorrectly.");
        near(config.third_person_camera.distance.minimum, 8.0F);
        near(config.third_person_camera.pitch.initial, 26.0F);
        near(config.third_person_fov_degrees, 45.0F);
        near(config.aim_yaw_offset_degrees, -90.0F);

        auto before = config;
        require(!gdtpc::parse_runtime_config("[third_person]\ndistance_min=40\ndistance_max=20\n", config, error),
            "Inverted range was accepted.");
        require(config.third_person_camera.distance.minimum == before.third_person_camera.distance.minimum,
            "Failed parse mutated the destination.");
        require(!gdtpc::parse_runtime_config("[general]\ntoggle_key=F13\n", config, error),
            "Unsupported toggle key was accepted.");
        require(!gdtpc::parse_runtime_config("[mystery]\nvalue=1\n", config, error),
            "Unknown section was accepted.");
        require(!gdtpc::parse_runtime_config("persist_mode=true\n", config, error),
            "Setting outside a section was accepted.");
        require(!gdtpc::parse_runtime_config("", config, error), "Empty configuration was accepted.");
        require(!gdtpc::parse_runtime_config(std::string(65537, 'x'), config, error), "Oversized configuration was accepted.");
        auto duplicate = std::string(valid) + "\n[general]\nschema_version=1\n";
        require(!gdtpc::parse_runtime_config(duplicate, config, error), "Duplicate section was accepted.");
        duplicate = std::string(valid);
        const auto insert_at = duplicate.find("toggle_key=F8") + std::string("toggle_key=F8").size();
        duplicate.insert(insert_at, "\ntoggle_key=F8");
        require(!gdtpc::parse_runtime_config(duplicate, config, error), "Duplicate key was accepted.");
        auto unsupported = std::string(valid);
        unsupported.replace(unsupported.find("toggle_key=F8"), 13, "toggle_key=F9");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "Unsupported Gate 0 toggle was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("persist_mode=false"), 18, "persist_mode=true");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "Persistence was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("distance_max=32"), 15, "distance_max=101");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "Unsafe distance was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("follow_smooth_seconds=0.3"), 25, "follow_smooth_seconds=nan");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "NaN was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("fov_degrees=45"), 14, "fov_degrees=61");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "Unsafe FOV was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("fov_degrees=45"), 14, "fov_degrees=nan");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "Nonfinite FOV was accepted.");

        // Optional second mode: prove that a specific configuration file, such as the copy staged
        // into an immutable release, is accepted by this exact parser.
        if (argc == 2)
        {
            std::ifstream file(argv[1], std::ios::binary);
            if (!file) throw std::runtime_error("The supplied configuration file could not be opened.");
            const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            gdtpc::RuntimeConfig staged;
            std::string staged_error;
            if (!gdtpc::parse_runtime_config(contents, staged, staged_error))
                throw std::runtime_error("The supplied configuration file was rejected: " + staged_error);
            require(staged.toggle_virtual_key == 0x77 && !staged.start_in_third_person && !staged.persist_mode,
                "The supplied configuration file is not a Gate 0 configuration.");
            // Collision settings are validated by the same predicate the model uses, so a configuration
        // the model would refuse can never reach it.
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("collision_query_interval=3"), 26, "collision_query_interval=0");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "A zero collision query interval was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("collision_fault_limit=4"), 23, "collision_fault_limit=0");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "A zero collision fault limit was accepted.");
        // Collision without zoom stepping has nowhere to keep the player's zoom, because collision
        // leaves the engine's own target parked at the arm.
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("collision_enabled=false"), 23, "collision_enabled=true");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "Collision without zoom stepping was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("zoom_step_percent=14"), 20, "zoom_step_percent=0");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A zero zoom step percent was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("collision_release_step=1"), 24, "collision_release_step=99");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "An oversized immediate release step was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("collision_release_queries=3"), 27, "collision_release_queries=0");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A zero collision release confirmation count was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("collision_skin=0.35"), 19, "collision_skin=nan");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "A nonfinite collision skin was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("collision_min_distance=8"), 24, "collision_min_distance=0.1");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "An unsafe collision minimum was accepted.");
        // The collision floor must sit at or ABOVE the profile's own minimum distance. Zoom is
        // stored as a blend between the profile's distance endpoints and a blend outside [0, 1] is
        // an invalid camera to the write adapter, so an arm shorter than the profile minimum makes
        // every restore refuse. Live, that blocked the logical stop for 283 frames.
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("collision_min_distance=8"), 24, "collision_min_distance=4");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A collision minimum below the profile minimum was accepted.");
        // Collision defaults to off, so a capable build behaves exactly like the accepted runtime
        // until it is deliberately enabled.
        require(!config.collision_enabled, "Collision assistance defaulted to enabled.");
        require(config.collision.release_confirm_queries == 3,
            "The collision release confirmation count was parsed incorrectly.");
        near(config.collision.release_immediate_units, 1.0F);
        require(!config.zoom_step_enabled, "Finer zoom stepping defaulted to enabled.");
        require(config.zoom_step_percent == 14, "The zoom step percent was parsed incorrectly.");
        near(gdtpc::zoom_step_settings(config).ratio, 1.14F);
        require(!config.shoulder_offset_enabled, "Shoulder offset defaulted to enabled.");
        near(config.shoulder_offset_units, 1.5F);
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("shoulder_offset_units=1.5"), 25, "shoulder_offset_units=9");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "An oversized shoulder offset was accepted.");
        near(config.shoulder_height_units, 2.0F);
        for (const auto* const bad_height : {"shoulder_height_units=-1", "shoulder_height_units=11",
                 "shoulder_height_units=nan"})
        {
            unsupported = std::string(valid);
            unsupported.replace(unsupported.find("shoulder_height_units=2"), 23, bad_height);
            require(!gdtpc::parse_runtime_config(unsupported, config, error),
                "An out-of-range shoulder height was accepted.");
        }
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("shoulder_height_units=2\n"), 24, "");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A configuration missing the shoulder height was accepted.");
        require(!config.ui_probe_enabled, "The UI probe defaulted to enabled.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("ui_probe_enabled=false\n"), 23, "");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A configuration missing the UI probe key was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("ui_probe_enabled=false"), 22, "ui_probe_enabled=maybe");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "An invalid UI probe value was accepted.");
        require(!config.mouse_look.enabled, "Mouse look defaulted to enabled.");
        near(config.mouse_look.yaw_radians_per_pixel, 0.003F);
        require(config.mouse_look.menu_release_frames == 3, "The menu release debounce was parsed incorrectly.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("mouse_look_enabled=false"), 24, "mouse_look_enabled=true");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "Mouse look without collision was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("aim_band_top=0.25"), 17, "aim_band_top=0.9");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "An inverted aim band was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("menu_release_frames=3\n"), 22, "");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A configuration missing a key above bit 31 was accepted.");
        near(config.mouse_look.yaw_catchup_per_second, 6.0F);
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("mouse_look_pitch_floor_degrees=1\n"), 33, "");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A configuration missing the pitch floor (bit 41) was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("mouse_look_pitch_offset_min=-15"), 31, "mouse_look_pitch_offset_min=5");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A positive pitch offset minimum was accepted.");
        // Virtual zoom: off by default, held distance inside the profile with room for an outward click
        // (profile 8..32 allows at most blend 0.95, 30.8 units), and only with mouse look.
        require(!config.virtual_zoom_enabled, "Virtual zoom defaulted to enabled.");
        near(config.virtual_zoom_engine_distance, 28.0F);
        near(gdtpc::virtual_zoom_settings(config).visual_minimum, 8.0F);
        near(gdtpc::virtual_zoom_settings(config).visual_default, 18.0F);
        near(gdtpc::virtual_zoom_settings(config).ratio, 1.14F);
        for (const auto* const bad_distance : {"virtual_zoom_engine_distance=32", "virtual_zoom_engine_distance=30.9",
                 "virtual_zoom_engine_distance=8", "virtual_zoom_engine_distance=4", "virtual_zoom_engine_distance=nan"})
        {
            unsupported = std::string(valid);
            unsupported.replace(unsupported.find("virtual_zoom_engine_distance=28"), 31, bad_distance);
            require(!gdtpc::parse_runtime_config(unsupported, config, error),
                "An unusable virtual zoom engine distance was accepted.");
        }
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("virtual_zoom_engine_distance=28"), 31, "virtual_zoom_engine_distance=30.5");
        require(gdtpc::parse_runtime_config(unsupported, config, error),
            "An engine distance with room for an outward click was rejected.");
        require(config.third_person_far_plane_percent == 100, "The far-plane cap did not parse as off.");
        for (const auto* const bad_percent : {"third_person_far_plane_percent=95", "third_person_far_plane_percent=49",
                 "third_person_far_plane_percent=101", "third_person_far_plane_percent=-5", "third_person_far_plane_percent=9.5"})
        {
            // 95 is refused here only because this configuration has mouse look off.
            unsupported = std::string(valid);
            unsupported.replace(unsupported.find("third_person_far_plane_percent=100"), 34, bad_percent);
            require(!gdtpc::parse_runtime_config(unsupported, config, error),
                "An out-of-range far-plane percent, or a cap without mouse look, was accepted.");
        }
        require(!config.npc_dialog_releases_cursor, "The NPC dialog release defaulted to enabled.");
        require(!config.mouse_look_dot_cursor, "The dot cursor defaulted to enabled.");
        require(!config.right_stick_pitch_enabled && !config.right_stick_pitch_invert, "Right-stick pitch defaulted to enabled.");
        near(config.right_stick_pitch_degrees_per_second, 90.0F);
        for (const auto* const bad_speed : {"right_stick_pitch_degrees_per_second=5", "right_stick_pitch_degrees_per_second=400",
                 "right_stick_pitch_degrees_per_second=nan"})
        {
            unsupported = std::string(valid);
            unsupported.replace(unsupported.find("right_stick_pitch_degrees_per_second=90"), 39, bad_speed);
            require(!gdtpc::parse_runtime_config(unsupported, config, error), "An out-of-range right-stick speed was accepted.");
        }
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("right_stick_pitch_enabled=false"), 31, "right_stick_pitch_enabled=true");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "Right-stick pitch without mouse look was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("right_stick_pitch_invert=false\n"), 31, "");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A configuration missing the right-stick invert key (bit 49) was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("mouse_look_dot_cursor=false"), 27, "mouse_look_dot_cursor=true");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "The dot cursor without mouse look was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("mouse_look_dot_cursor=false\n"), 28, "");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A configuration missing the dot cursor key (bit 46) was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("npc_dialog_releases_cursor=false"), 32, "npc_dialog_releases_cursor=true");
        require(!gdtpc::parse_runtime_config(unsupported, config, error), "The NPC dialog release without mouse look was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("npc_dialog_releases_cursor=false\n"), 33, "");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A configuration missing the NPC dialog key (bit 45) was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("third_person_far_plane_percent=100\n"), 35, "");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A configuration missing the far-plane percent (bit 44) was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("virtual_zoom_engine_distance=28\n"), 32, "");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "A configuration missing the virtual zoom engine distance (bit 43) was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("virtual_zoom_enabled=false"), 26, "virtual_zoom_enabled=true");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "Virtual zoom without mouse look was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("virtual_zoom_enabled=false"), 26, "virtual_zoom_fraction=0.8");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "The removed virtual zoom experiment key was accepted.");
        unsupported = std::string(valid);
        unsupported.replace(unsupported.find("shoulder_offset_enabled=false"), 29,
            "shoulder_offset_enabled=true");
        require(!gdtpc::parse_runtime_config(unsupported, config, error),
            "Shoulder offset without collision was accepted.");

        std::cout << "PASS: strict runtime configuration parsing, validation, and acceptance of "
                      << argv[1] << ".\n";
            return 0;
        }
        if (argc != 1) throw std::runtime_error("Supply at most one configuration file path.");

        std::cout << "PASS: strict runtime configuration parsing and validation.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
