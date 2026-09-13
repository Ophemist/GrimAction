#pragma once

namespace gdtpc
{
struct AimSample
{
    float facing_x;
    float facing_y;
    float facing_z;
    float heading_radians;
};

[[nodiscard]] bool capture_aim_heading(const void* game_camera, AimSample& sample) noexcept;
}
