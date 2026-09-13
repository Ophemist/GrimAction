#pragma once

#include "camera_write_adapter.h"

#include <cstdint>

namespace gdtpc
{
enum class ProfileMode : std::uint32_t { native, third_person };
enum class ProfileRestoreState : std::uint32_t { clean, pending, verified, impossible, abandoned };
enum class ProfileTransition : std::uint32_t { none, entered_third_person, returned_native, rejected_fault };

struct CameraSessionIdentity
{
    std::uintptr_t camera{};
    std::uintptr_t engine{};
    std::uintptr_t player{};
    std::uint64_t generation{};

    [[nodiscard]] bool valid() const noexcept
    {
        return camera != 0 && engine != 0 && player != 0 && generation != 0;
    }
    friend bool operator==(const CameraSessionIdentity&, const CameraSessionIdentity&) = default;
};

struct ProfileFrame
{
    CameraSessionIdentity session{};
    void* camera{};
    bool foreground{};
    bool toggle_down{};
    bool stop_requested{};
};

struct ProfileStepResult
{
    ProfileMode mode{};
    ProfileRestoreState restore_state{};
    ProfileTransition transition{};
    CameraTransactionResult transaction{};
    bool control_enabled{};
    bool dirty{};
    std::uint64_t control_write_count{};
    std::uint64_t restore_write_count{};
};

class ProfileSwitchModel final
{
public:
    explicit ProfileSwitchModel(CameraProfile third_person_profile, float third_person_fov_radians) noexcept;
    [[nodiscard]] ProfileStepResult step(CameraMemoryAccess& access, const ProfileFrame& frame) noexcept;
    [[nodiscard]] ProfileMode mode() const noexcept { return mode_; }
    [[nodiscard]] ProfileRestoreState restore_state() const noexcept { return restore_state_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] std::uint64_t control_write_count() const noexcept { return control_write_count_; }
    [[nodiscard]] std::uint64_t restore_write_count() const noexcept { return restore_write_count_; }
    // Excursions whose session disappeared before they could be returned to Native. Monotonic, and
    // reported in telemetry, so an abandoned excursion is never silently forgotten.
    [[nodiscard]] std::uint64_t abandoned_excursion_count() const noexcept { return abandoned_excursion_count_; }

private:
    enum class AppliedProfilePresence { absent, present, indeterminate };

    [[nodiscard]] ProfileStepResult result(ProfileTransition transition, CameraTransactionResult transaction) const noexcept;
    [[nodiscard]] ProfileStepResult restore(CameraMemoryAccess& access, const ProfileFrame& frame) noexcept;
    void account_control(const CameraWriteJournal& journal) noexcept;
    void account_restore(const CameraWriteJournal& journal) noexcept;
    void clear_session_memories() noexcept;

    [[nodiscard]] AppliedProfilePresence applied_profile_presence(CameraMemoryAccess& access, void* camera) noexcept;

    CameraProfile third_person_profile_{};
    float third_person_fov_radians_{};
    CameraSessionIdentity session_{};
    CameraRawSnapshot native_snapshot_{};
    CameraRawSnapshot applied_snapshot_{}; // exactly what was written on the last accepted entry
    bool has_native_snapshot_{};
    float third_person_zoom_{};
    bool has_third_person_zoom_{};
    bool key_armed_{};
    bool dirty_{};
    bool faulted_{};
    bool stop_latched_{};
    ProfileMode mode_{ProfileMode::native};
    ProfileRestoreState restore_state_{ProfileRestoreState::clean};
    std::uint64_t control_write_count_{};
    std::uint64_t restore_write_count_{};
    std::uint64_t abandoned_excursion_count_{};
};
}
