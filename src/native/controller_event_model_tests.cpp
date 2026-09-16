#include "controller_event_model.h"

#include <array>
#include <bit>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    try
    {
        require(!gdtpc::select_strongest_analog_event(nullptr, 0).valid, "null event list must be empty");

        std::array events{
            gdtpc::SteamControllerEvent{10, 0.0F, 0.0F, 1, {}},
            gdtpc::SteamControllerEvent{20, 0.2F, -0.6F, 0, {}},
            gdtpc::SteamControllerEvent{30, -0.9F, 0.1F, 0, {}}};
        auto sample = gdtpc::select_strongest_analog_event(events.data(), events.size());
        require(sample.valid && sample.action_id == 30 && sample.x == -0.9F && sample.y == 0.1F,
            "strongest finite analog event must win");

        events[2].x = std::numeric_limits<float>::quiet_NaN();
        sample = gdtpc::select_strongest_analog_event(events.data(), events.size());
        require(sample.valid && sample.action_id == 20 && sample.y == -0.6F,
            "nonfinite analog event must be ignored");

        events[1].kind = 1;
        sample = gdtpc::select_strongest_analog_event(events.data(), events.size());
        require(!sample.valid && sample.action_id == -1, "digital and nonfinite events must produce no sample");

        std::cout << "PASS: Steam controller event layout and observation-only analog selection.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
