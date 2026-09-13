#include "controller_model.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace gdtpc;

ControllerSettings settings()
{
    return {{{8.0F, 40.0F, 25.0F}, {38.0F, 52.0F, 46.0F}},
        {{2.0F, 12.0F, 6.0F}, {18.0F, 30.0F, 24.0F}}, 0.5F, 0.3F};
}
FrameInput input(bool toggle = false, bool menu = false, Vec2 movement = {}, Vec2 look = {}, float heading = 0.0F,
    float yaw = 0.0F, float delta = 0.1F) { return {delta, movement, look, toggle, menu, heading, yaw}; }
void require(const bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void near(const float actual, const float expected, const float tolerance = 0.0001F)
{ if (std::abs(actual - expected) > tolerance) throw std::runtime_error("Values differ."); }
void near(const Vec2 actual, const Vec2 expected) { near(actual.x, expected.x); near(actual.y, expected.y); }

void isometric_pass_through()
{
    const auto output = ControllerModel(settings()).step(input(false, false, {0.2F, 0.8F}, {}, 0.0F, 1.0F));
    require(!output.override_movement && !output.override_camera_yaw, "Isometric override occurred.");
    near(output.world_movement, {0.2F, 0.8F});
}
void toggle_is_edge_triggered()
{
    ControllerModel controller(settings());
    require(controller.step(input(true)).mode == CameraMode::third_person, "First edge failed.");
    require(!controller.step(input(true)).apply_camera_profile, "Held toggle reapplied the profile.");
    static_cast<void>(controller.step(input(false)));
    require(controller.step(input(true)).mode == CameraMode::isometric, "Second edge failed.");
}
void menu_blocks_control()
{
    ControllerModel controller(settings());
    const auto output = controller.step(input(true, true));
    require(output.mode == CameraMode::isometric && !output.override_movement, "Menu input was intercepted.");
    require(!output.apply_camera_profile, "Menu-owned toggle applied a camera profile.");
}
void third_person_movement_remains_native()
{
    ControllerModel controller(settings()); static_cast<void>(controller.step(input(true)));
    const Vec2 movement{0.3F, -0.8F};
    const auto output = controller.step(input(false, false, movement));
    require(!output.override_movement, "Third-person movement was intercepted.");
    near(output.world_movement, movement);
}
void movement_magnitude_remains_native()
{
    ControllerModel controller(settings()); static_cast<void>(controller.step(input(true)));
    const Vec2 movement{1.0F, 1.0F};
    const auto output = controller.step(input(false, false, movement));
    require(!output.override_movement, "Native diagonal movement was intercepted.");
    near(output.world_movement, movement);
}
void manual_look_delays_follow()
{
    ControllerModel controller(settings()); static_cast<void>(controller.step(input(true)));
    const auto manual = controller.step(input(false, false, {}, {1.0F, 0.0F}, 2.0F));
    require(manual.camera_yaw_radians > 0.0F, "Manual look failed.");
    const auto delayed = controller.step(input(false, false, {}, {}, 2.0F, manual.camera_yaw_radians, 0.2F));
    near(delayed.camera_yaw_radians, manual.camera_yaw_radians);
}
void follow_converges()
{
    ControllerModel controller(settings()); auto yaw = controller.step(input(true)).camera_yaw_radians;
    for (int index = 0; index < 180; ++index)
        yaw = controller.step(input(false, false, {}, {}, 1.2F, yaw, 1.0F / 60.0F)).camera_yaw_radians;
    near(yaw, 1.2F, 0.001F);
}
void follow_uses_shortest_path()
{
    ControllerModel controller(settings());
    static_cast<void>(controller.step(input(true, false, {}, {}, 0.0F, 3.1F)));
    for (int index = 0; index < 6; ++index)
        static_cast<void>(controller.step(input(false, false, {}, {}, -3.1F, 3.1F)));
    const auto output = controller.step(input(false, false, {}, {}, -3.1F, 3.1F));
    require(output.camera_yaw_radians > 3.1F || output.camera_yaw_radians < -3.1F, "Follow chose the long path.");
}
void aim_drives_follow_independently_of_movement()
{
    for (const auto movement : {Vec2{0.0F, -1.0F}, Vec2{1.0F, 0.0F}})
    {
        ControllerModel controller(settings()); static_cast<void>(controller.step(input(true)));
        for (int index = 0; index < 6; ++index)
            static_cast<void>(controller.step(input(false, false, movement, {}, std::numbers::pi_v<float>)));
        const auto output = controller.step(input(false, false, movement, {}, std::numbers::pi_v<float>));
        require(std::abs(output.camera_yaw_radians) > 0.01F, "Aim heading did not drive follow while moving.");
        require(!output.override_movement, "Follow attempted to replace native movement.");
    }
}
void zoom_is_independent()
{
    ControllerModel controller(settings()); controller.set_zoom(33.0F);
    static_cast<void>(controller.step(input(true))); static_cast<void>(controller.step(input(false)));
    controller.set_zoom(4.0F); const auto menu = controller.step(input(false, true)); near(menu.zoom_distance, 4.0F);
    const auto isometric = controller.step(input(true)); near(isometric.zoom_distance, 33.0F);
    require(isometric.apply_camera_profile, "Isometric profile was not requested.");
    near(isometric.active_camera_profile.pitch.initial, 46.0F);
    static_cast<void>(controller.step(input(false)));
    const auto third_person = controller.step(input(true)); near(third_person.zoom_distance, 4.0F);
    require(third_person.apply_camera_profile, "Third-person profile was not requested.");
    near(third_person.active_camera_profile.pitch.initial, 24.0F);
}
}

int main()
{
    using Test = std::pair<std::string_view, std::function<void()>>;
    const Test tests[]{{"isometric pass-through", isometric_pass_through}, {"toggle edge", toggle_is_edge_triggered},
        {"menu gate", menu_blocks_control}, {"third-person movement remains native", third_person_movement_remains_native},
        {"movement magnitude remains native", movement_magnitude_remains_native}, {"manual-look delay", manual_look_delays_follow},
        {"shortest-path follow", follow_uses_shortest_path}, {"follow convergence", follow_converges},
        {"aim drives follow independently of movement", aim_drives_follow_independently_of_movement},
        {"independent zoom", zoom_is_independent}};
    int failures = 0;
    for (const auto& [name, body] : tests)
    {
        try { body(); std::cout << "PASS  " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL  " << name << ": " << error.what() << '\n'; }
    }
    std::cout << std::size(tests) - failures << '/' << std::size(tests) << " native controller tests passed.\n";
    return failures == 0 ? 0 : 1;
}
