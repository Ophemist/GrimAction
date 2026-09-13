#include "aim_memory_model.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace
{
void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void near(const float actual, const float expected)
{
    if (std::abs(actual - expected) > 0.0001F) throw std::runtime_error("Heading differs.");
}

void set_facing(std::array<std::byte, 0x200>& player, const float x, const float y, const float z)
{
    std::memcpy(player.data() + 0x108, &x, sizeof(x));
    std::memcpy(player.data() + 0x10c, &y, sizeof(y));
    std::memcpy(player.data() + 0x110, &z, sizeof(z));
}
}

int main()
{
    try
    {
        std::array<std::byte, 0x200> camera{};
        std::array<std::byte, 0x200> player{};
        auto* player_pointer = player.data();
        std::memcpy(camera.data() + 0x118, &player_pointer, sizeof(player_pointer));

        gdtpc::AimSample sample{};
        set_facing(player, 1.0F, 0.0F, 0.0F);
        require(gdtpc::capture_aim_heading(camera.data(), sample), "+X facing was rejected.");
        near(sample.heading_radians, 0.0F);
        set_facing(player, 0.0F, 0.0F, 1.0F);
        require(gdtpc::capture_aim_heading(camera.data(), sample), "+Z facing was rejected.");
        near(sample.heading_radians, std::numbers::pi_v<float> / 2.0F);
        set_facing(player, -1.0F, 0.0F, 0.0F);
        require(gdtpc::capture_aim_heading(camera.data(), sample), "-X facing was rejected.");
        near(std::abs(sample.heading_radians), std::numbers::pi_v<float>);
        set_facing(player, 0.0F, 0.0F, -1.0F);
        require(gdtpc::capture_aim_heading(camera.data(), sample), "-Z facing was rejected.");
        near(sample.heading_radians, -std::numbers::pi_v<float> / 2.0F);

        set_facing(player, 0.0F, 0.0F, 0.0F);
        require(!gdtpc::capture_aim_heading(camera.data(), sample), "Zero facing was accepted.");
        player_pointer = nullptr;
        std::memcpy(camera.data() + 0x118, &player_pointer, sizeof(player_pointer));
        require(!gdtpc::capture_aim_heading(camera.data(), sample), "Null player was accepted.");
        require(!gdtpc::capture_aim_heading(nullptr, sample), "Null camera was accepted.");

        std::cout << "PASS: guarded X/Z aim-heading capture and invalid-pointer rejection.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
