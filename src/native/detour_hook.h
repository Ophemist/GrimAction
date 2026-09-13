#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace gdtpc
{
class DetourHook final
{
public:
    DetourHook() = default;
    DetourHook(const DetourHook&) = delete;
    DetourHook& operator=(const DetourHook&) = delete;
    ~DetourHook() = default; // installed hooks require an explicit detach decision

    [[nodiscard]] LONG attach(void** target_pointer, void* replacement) noexcept;
    [[nodiscard]] LONG detach() noexcept;
    [[nodiscard]] bool installed() const noexcept { return installed_; }
    [[nodiscard]] LONG last_error() const noexcept { return last_error_; }

private:
    void** target_pointer_{nullptr};
    void* replacement_{nullptr};
    bool installed_{false};
    LONG last_error_{ERROR_SUCCESS};
};
}
