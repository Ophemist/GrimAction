#include "profile_switch_model.h"

#include <array>
#include <bit>
#include <cmath>

namespace
{
std::uint64_t count(const std::uint16_t mask) noexcept
{
    return static_cast<std::uint64_t>(std::popcount(mask));
}
}

gdtpc::ProfileSwitchModel::ProfileSwitchModel(
    const CameraProfile third_person_profile, const float third_person_fov_radians) noexcept
    : third_person_profile_{third_person_profile}, third_person_fov_radians_{third_person_fov_radians}
{
    if (!valid_camera_profile(third_person_profile_) || !valid_camera_fov_radians(third_person_fov_radians_))
        faulted_ = true;
}

void gdtpc::ProfileSwitchModel::account_control(const CameraWriteJournal& journal) noexcept
{
    control_write_count_ += count(journal.written);
    restore_write_count_ += count(journal.restored);
}

void gdtpc::ProfileSwitchModel::account_restore(const CameraWriteJournal& journal) noexcept
{
    restore_write_count_ += count(journal.written) + count(journal.restored);
}

void gdtpc::ProfileSwitchModel::clear_session_memories() noexcept
{
    native_snapshot_ = {};
    applied_snapshot_ = {};
    has_native_snapshot_ = false;
    third_person_zoom_ = 0.0F;
    has_third_person_zoom_ = false;
}

gdtpc::ProfileStepResult gdtpc::ProfileSwitchModel::result(
    const ProfileTransition transition, const CameraTransactionResult transaction) const noexcept
{
    return {mode_, restore_state_, transition, transaction, !faulted_ && !stop_latched_, dirty_,
        control_write_count_, restore_write_count_};
}

gdtpc::ProfileStepResult gdtpc::ProfileSwitchModel::restore(
    CameraMemoryAccess& access, const ProfileFrame& frame) noexcept
{
    if (!dirty_)
    {
        mode_ = ProfileMode::native;
        restore_state_ = ProfileRestoreState::clean;
        return result(ProfileTransition::none, CameraTransactionResult::success);
    }
    if (!has_native_snapshot_)
    {
        faulted_ = true;
        restore_state_ = ProfileRestoreState::impossible;
        return result(ProfileTransition::rejected_fault, CameraTransactionResult::restore_failed);
    }
    if (!frame.session.valid() || frame.session != session_ || frame.camera == nullptr ||
        reinterpret_cast<std::uintptr_t>(frame.camera) != frame.session.camera)
    {
        faulted_ = true;
        restore_state_ = ProfileRestoreState::impossible;
        return result(ProfileTransition::rejected_fault, CameraTransactionResult::restore_failed);
    }

    restore_state_ = ProfileRestoreState::pending;
    CameraRawSnapshot current{};
    CameraWriteJournal journal{};
    const auto transaction = write_camera_snapshot(access, frame.camera, native_snapshot_, current, journal);
    account_restore(journal);
    if (transaction == CameraTransactionResult::success)
    {
        const auto outgoing_zoom = zoom_distance_from_snapshot(current);
        if (std::isfinite(outgoing_zoom))
        {
            third_person_zoom_ = outgoing_zoom;
            has_third_person_zoom_ = true;
        }
        dirty_ = false;
        mode_ = ProfileMode::native;
        has_native_snapshot_ = false;
        restore_state_ = ProfileRestoreState::verified;
        return result(ProfileTransition::returned_native, transaction);
    }
    faulted_ = true;
    if (transaction == CameraTransactionResult::write_failed_rolled_back ||
        transaction == CameraTransactionResult::verification_failed_rolled_back ||
        transaction == CameraTransactionResult::preflight_failed ||
        transaction == CameraTransactionResult::capture_failed)
        restore_state_ = ProfileRestoreState::pending;
    else restore_state_ = ProfileRestoreState::impossible;
    return result(ProfileTransition::rejected_fault, transaction);
}

// Blend fields 0x580/0x584 are deliberately excluded: native scrolling changes them between
// transitions. Absence is proven only when every invariant byte matches the captured native
// preimage. A partial game reset (including FOV alone) is indeterminate and latches safely.
gdtpc::ProfileSwitchModel::AppliedProfilePresence gdtpc::ProfileSwitchModel::applied_profile_presence(
    CameraMemoryAccess& access, void* camera) noexcept
{
    if (camera == nullptr) return AppliedProfilePresence::indeterminate;
    constexpr std::array<std::size_t, 9> invariant_fields{0, 1, 2, 3, 4, 5, 8, 9, camera_fov_field_index};
    auto all_applied = true;
    auto all_native = has_native_snapshot_;
    for (const auto index : invariant_fields)
    {
        std::array<std::byte, sizeof(float)> current{};
        if (!access.preflight(camera, camera_field_offsets[index], false) ||
            !access.read(camera, camera_field_offsets[index], current.data(), current.size()))
            return AppliedProfilePresence::indeterminate;
        if (current != applied_snapshot_.fields[index]) all_applied = false;
        if (!has_native_snapshot_ || current != native_snapshot_.fields[index]) all_native = false;
    }
    if (all_applied) return AppliedProfilePresence::present;
    if (all_native) return AppliedProfilePresence::absent;
    return AppliedProfilePresence::indeterminate;
}

gdtpc::ProfileStepResult gdtpc::ProfileSwitchModel::step(
    CameraMemoryAccess& access, const ProfileFrame& frame) noexcept
{
    if (frame.stop_requested) stop_latched_ = true;

    const auto identity_valid = frame.session.valid() && frame.camera != nullptr &&
        reinterpret_cast<std::uintptr_t>(frame.camera) == frame.session.camera;
    if (session_.valid() && (!identity_valid || frame.session != session_))
    {
        key_armed_ = false;
        if (dirty_)
        {
            // The old snapshot must never be written into a new generation, even at the same
            // address, so restoring is not an option here. The only question is whether anything of
            // ours survives. If the camera no longer holds the profile we wrote, the game has
            // already reinitialized it and the excursion is simply over: record it as abandoned and
            // rebind cleanly so the toggle keeps working. If our profile is still resident, a live
            // object really is modified, and latching the fault remains the honest, safe answer.
            const auto presence = applied_profile_presence(access, frame.camera);
            clear_session_memories();
            if (presence != AppliedProfilePresence::absent)
            {
                faulted_ = true;
                restore_state_ = ProfileRestoreState::impossible;
                return result(ProfileTransition::rejected_fault, CameraTransactionResult::restore_failed);
            }
            ++abandoned_excursion_count_;
            dirty_ = false;
            restore_state_ = ProfileRestoreState::abandoned;
            session_ = {};
            mode_ = ProfileMode::native;
            return result(ProfileTransition::returned_native, CameraTransactionResult::success);
        }
        session_ = {};
        clear_session_memories();
        mode_ = ProfileMode::native;
        restore_state_ = ProfileRestoreState::clean;
    }
    if (!session_.valid() && identity_valid) session_ = frame.session;

    if (stop_latched_)
    {
        key_armed_ = false;
        return restore(access, frame);
    }
    if (!identity_valid)
    {
        key_armed_ = false;
        return result(ProfileTransition::none, CameraTransactionResult::success);
    }
    if (!frame.foreground)
    {
        // Losing focus disarms the key so no toggle can ever be taken in the background, but the
        // chosen mode is retained across alt-tab at the user's request. Nothing is persisted to
        // disk and no new excursion can begin while backgrounded; a session lost while away is
        // handled by the abandonment rule above.
        key_armed_ = false;
        return result(ProfileTransition::none, CameraTransactionResult::success);
    }
    if (faulted_) return result(ProfileTransition::rejected_fault, CameraTransactionResult::restore_failed);

    if (!frame.toggle_down)
    {
        key_armed_ = true;
        return result(ProfileTransition::none, CameraTransactionResult::success);
    }
    if (!key_armed_) return result(ProfileTransition::none, CameraTransactionResult::success);
    key_armed_ = false;

    if (mode_ == ProfileMode::third_person) return restore(access, frame);

    CameraRawSnapshot desired{};
    const auto entry_zoom = has_third_person_zoom_ ? third_person_zoom_ : third_person_profile_.distance.initial;
    if (!make_profile_snapshot(third_person_profile_, entry_zoom, third_person_fov_radians_, desired))
    {
        faulted_ = true;
        return result(ProfileTransition::rejected_fault, CameraTransactionResult::invalid_argument);
    }
    CameraRawSnapshot native{};
    CameraWriteJournal journal{};
    const auto transaction = write_camera_snapshot(access, frame.camera, desired, native, journal);
    account_control(journal);
    if (transaction == CameraTransactionResult::success)
    {
        native_snapshot_ = native;
        applied_snapshot_ = desired;
        has_native_snapshot_ = true;
        dirty_ = true;
        mode_ = ProfileMode::third_person;
        restore_state_ = ProfileRestoreState::clean;
        return result(ProfileTransition::entered_third_person, transaction);
    }

    faulted_ = true;
    if (transaction == CameraTransactionResult::restore_failed)
    {
        native_snapshot_ = native;
        has_native_snapshot_ = true;
        dirty_ = true;
        restore_state_ = ProfileRestoreState::impossible;
    }
    else restore_state_ = ProfileRestoreState::verified;
    return result(ProfileTransition::rejected_fault, transaction);
}
