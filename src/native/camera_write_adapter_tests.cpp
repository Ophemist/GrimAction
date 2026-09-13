#include "camera_write_adapter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
constexpr std::size_t camera_size = 0x600;
constexpr std::uint16_t all_fields = (1U << gdtpc::camera_field_count) - 1;
constexpr float native_fov = 0.523598776F;
constexpr float third_fov = 0.785398163F;

void require(const bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class FakeAccess final : public gdtpc::CameraMemoryAccess
{
public:
    std::array<std::byte, camera_size> bytes{};
    int deny_read_field{-1};
    int deny_write_field{-1};
    int fail_write_operation{-1};
    int corrupt_readback_operation{-1};
    bool fail_all_rollback_writes{false};
    int fail_synchronize_operation{-1};
    int synchronize_calls{};
    float synchronized_distance{};

    [[nodiscard]] bool preflight(const void*, const std::size_t offset, const bool writable) noexcept override
    {
        const auto index = index_of(offset);
        return index >= 0 && index != (writable ? deny_write_field : deny_read_field);
    }
    [[nodiscard]] bool read(const void*, const std::size_t offset, void* destination, const std::size_t size) noexcept override
    {
        if (size != sizeof(float) || offset + size > bytes.size()) return false;
        std::memcpy(destination, bytes.data() + offset, size);
        if (writes_seen_ > 0 && corrupt_readback_operation == writes_seen_ - 1)
            static_cast<std::byte*>(destination)[0] ^= std::byte{1};
        return true;
    }
    [[nodiscard]] bool write(void*, const std::size_t offset, const void* source, const std::size_t size) noexcept override
    {
        if (size != sizeof(float) || offset + size > bytes.size()) return false;
        const auto operation = writes_seen_++;
        if (operation == fail_write_operation || (fail_all_rollback_writes && operation > fail_write_operation)) return false;
        std::memcpy(bytes.data() + offset, source, size);
        return true;
    }
    // Models the real GameCamera::SetZoom, which does not merely refresh derived state: it OWNS the
    // current and target blend fields and overwrites both, writing the current blend unclamped and
    // the target clamped into [0, 1]. The fake used to only record the distance, which is why it
    // never caught the collision path resetting the player's zoom target.
    [[nodiscard]] bool synchronize_zoom(void* camera, const float distance) noexcept override
    {
        const auto operation = synchronize_calls++;
        if (operation == fail_synchronize_operation) return false;
        synchronized_distance = distance;
        float endpoint_a = 0.0F, endpoint_b = 0.0F;
        std::memcpy(&endpoint_a, bytes.data() + gdtpc::camera_field_offsets[8], sizeof(endpoint_a));
        std::memcpy(&endpoint_b, bytes.data() + gdtpc::camera_field_offsets[9], sizeof(endpoint_b));
        if (endpoint_b > endpoint_a)
        {
            const auto raw = (distance - endpoint_a) / (endpoint_b - endpoint_a);
            const auto clamped = std::clamp(raw, 0.0F, 1.0F);
            std::memcpy(bytes.data() + gdtpc::camera_field_offsets[6], &raw, sizeof(raw));
            std::memcpy(bytes.data() + gdtpc::camera_field_offsets[7], &clamped, sizeof(clamped));
        }
        static_cast<void>(camera);
        return true;
    }

private:
    int writes_seen_{};
    static int index_of(const std::size_t offset) noexcept
    {
        for (std::size_t index = 0; index < gdtpc::camera_field_count; ++index)
            if (gdtpc::camera_field_offsets[index] == offset) return static_cast<int>(index);
        return -1;
    }
};

gdtpc::CameraRawSnapshot profile(const gdtpc::CameraProfile& value, const float zoom, const float fov)
{
    gdtpc::CameraRawSnapshot result{};
    require(gdtpc::make_profile_snapshot(value, zoom, fov, result), "could not make profile snapshot");
    return result;
}

void seed(FakeAccess& access, const gdtpc::CameraRawSnapshot& snapshot)
{
    access.bytes.fill(std::byte{0x5a});
    for (std::size_t index = 0; index < gdtpc::camera_field_count; ++index)
        std::memcpy(access.bytes.data() + gdtpc::camera_field_offsets[index], snapshot.fields[index].data(), sizeof(float));
}
}

int main()
{
    try
    {
        const gdtpc::CameraProfile native_profile{{20, 48, 36}, {38, 52, 46}};
        const gdtpc::CameraProfile third_profile{{12, 90, 42}, {20, 44, 30}};
        const auto native = profile(native_profile, 36, native_fov);
        const auto third = profile(third_profile, 42, third_fov);

        // A successful transaction may touch only the eleven allowlisted fields and must synchronize
        // the engine's derived distance/pitch state through the native zoom path.
        {
            FakeAccess access; seed(access, native); const auto before = access.bytes;
            gdtpc::CameraRawSnapshot preimage{}; gdtpc::CameraWriteJournal journal{};
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), third, preimage, journal) ==
                gdtpc::CameraTransactionResult::success, "valid transaction failed");
            require(std::memcmp(&preimage, &native, sizeof(native)) == 0, "transaction did not retain the exact preimage");
            require(journal.attempted == all_fields && journal.written == all_fields && journal.verified == all_fields,
                "successful transaction journal was incomplete");
            require(journal.synchronization_attempted && journal.synchronization_completed &&
                access.synchronize_calls == 1 && std::abs(access.synchronized_distance - 42.0F) < 0.0001F,
                "successful transaction did not synchronize derived zoom state");
            for (std::size_t offset = 0; offset < access.bytes.size(); ++offset)
            {
                auto allowed = false;
                for (const auto field_offset : gdtpc::camera_field_offsets)
                    if (offset >= field_offset && offset < field_offset + sizeof(float)) allowed = true;
                if (!allowed) require(access.bytes[offset] == before[offset], "non-allowlisted sentinel changed");
            }
        }

        // A synchronization failure uses the same full-preimage rollback path.
        {
            FakeAccess access; seed(access, native); const auto before = access.bytes;
            access.fail_synchronize_operation = 0;
            gdtpc::CameraRawSnapshot preimage{}; gdtpc::CameraWriteJournal journal{};
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), third, preimage, journal) ==
                gdtpc::CameraTransactionResult::synchronization_failed_rolled_back,
                "derived-state synchronization failure did not report verified rollback");
            require(access.bytes == before && access.synchronize_calls == 2 && journal.restore_verified == all_fields,
                "synchronization failure did not restore the exact preimage and derived state");
        }

        // Preserve a distinct native target blend byte after synchronization so native scrolling
        // already in progress can resume on the next callback.
        {
            FakeAccess access; auto scrolling = native;
            scrolling.fields[7] = std::bit_cast<std::array<std::byte, sizeof(float)>>(0.75F);
            seed(access, scrolling); const auto before = access.bytes;
            gdtpc::CameraRawSnapshot preimage{}; gdtpc::CameraWriteJournal journal{};
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), third, preimage, journal) ==
                gdtpc::CameraTransactionResult::success, "entry from scrolling native state failed");
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), preimage, scrolling, journal) ==
                gdtpc::CameraTransactionResult::success && access.bytes == before,
                "native current/target blend was not restored exactly after synchronization");
        }

        // Null, unreadable, read-only/cross-region preflight, and corrupt snapshots fail before writes.
        {
            FakeAccess access; seed(access, native); const auto before = access.bytes;
            gdtpc::CameraRawSnapshot preimage{}; gdtpc::CameraWriteJournal journal{};
            require(gdtpc::write_camera_snapshot(access, nullptr, third, preimage, journal) ==
                gdtpc::CameraTransactionResult::invalid_argument, "null camera was accepted");
            access.deny_read_field = 4;
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), third, preimage, journal) ==
                gdtpc::CameraTransactionResult::capture_failed, "unreadable field was accepted");
            access.deny_read_field = -1; access.deny_write_field = 7;
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), third, preimage, journal) ==
                gdtpc::CameraTransactionResult::preflight_failed, "read-only/cross-region field was accepted");
            require(access.bytes == before && journal.attempted == 0, "preflight failure wrote camera memory");
            auto invalid = third;
            invalid.fields[6] = std::bit_cast<std::array<std::byte, sizeof(float)>>(1.01F);
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), invalid, preimage, journal) ==
                gdtpc::CameraTransactionResult::invalid_argument, "corrupt desired snapshot was accepted");
            invalid = third;
            invalid.fields[gdtpc::camera_fov_field_index] =
                std::bit_cast<std::array<std::byte, sizeof(float)>>(std::numeric_limits<float>::infinity());
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), invalid, preimage, journal) ==
                gdtpc::CameraTransactionResult::invalid_argument, "nonfinite desired FOV was accepted");
        }

        // Every possible write failure and every post-write verification failure restores all bytes.
        for (int failure = 0; failure < static_cast<int>(gdtpc::camera_field_count); ++failure)
        {
            FakeAccess access; seed(access, native); const auto before = access.bytes;
            access.fail_write_operation = failure;
            gdtpc::CameraRawSnapshot preimage{}; gdtpc::CameraWriteJournal journal{};
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), third, preimage, journal) ==
                gdtpc::CameraTransactionResult::write_failed_rolled_back, "write failure did not report verified rollback");
            require(access.bytes == before && journal.restore_verified == all_fields, "write failure did not restore exact bytes");
        }
        for (int failure = 0; failure < static_cast<int>(gdtpc::camera_field_count); ++failure)
        {
            FakeAccess access; seed(access, native); const auto before = access.bytes;
            access.corrupt_readback_operation = failure;
            gdtpc::CameraRawSnapshot preimage{}; gdtpc::CameraWriteJournal journal{};
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), third, preimage, journal) ==
                gdtpc::CameraTransactionResult::verification_failed_rolled_back,
                "readback failure did not report verified rollback");
            require(access.bytes == before && journal.restore_verified == all_fields,
                "readback failure did not restore exact bytes");
        }

        // If rollback itself cannot be verified, the result must remain explicitly unrecovered.
        {
            FakeAccess access; seed(access, native); access.fail_write_operation = 3; access.fail_all_rollback_writes = true;
            gdtpc::CameraRawSnapshot preimage{}; gdtpc::CameraWriteJournal journal{};
            require(gdtpc::write_camera_snapshot(access, access.bytes.data(), third, preimage, journal) ==
                gdtpc::CameraTransactionResult::restore_failed, "failed rollback was reported as recovered");
            require(journal.restore_verified != all_fields, "failed rollback journal claimed complete verification");
        }

        // Blend endpoints are valid; nonfinite and out-of-range captured blends are rejected.
        for (const auto blend : {0.0F, 1.0F})
        {
            FakeAccess access; auto candidate = native;
            candidate.fields[6] = std::bit_cast<std::array<std::byte, sizeof(float)>>(blend); seed(access, candidate);
            gdtpc::CameraRawSnapshot captured{};
            require(gdtpc::capture_camera_snapshot(access, access.bytes.data(), captured), "valid blend endpoint was rejected");
        }
        for (const auto blend : {-0.01F, 1.01F, std::numeric_limits<float>::infinity()})
        {
            FakeAccess access; auto candidate = native;
            candidate.fields[6] = std::bit_cast<std::array<std::byte, sizeof(float)>>(blend); seed(access, candidate);
            gdtpc::CameraRawSnapshot captured{};
            require(!gdtpc::capture_camera_snapshot(access, access.bytes.data(), captured), "invalid captured blend was accepted");
        }

        // Collision zoom writes must move the camera and leave the player's zoom target where the
        // caller says it belongs. The native setter owns both blend fields, so the target has to be
        // written back afterwards; skipping that is what reset the player's scroll to the arm within
        // the same frame they raised it, and stopped zooming out working live.
        {
            const gdtpc::CameraProfile collision_profile{{4, 90, 42}, {15, 44, 30}};
            FakeAccess access;
            seed(access, profile(collision_profile, 42.0F, third_fov));
            float player_target = 0.0F;
            std::memcpy(&player_target, access.bytes.data() + gdtpc::camera_field_offsets[7], sizeof(player_target));

            require(gdtpc::restore_player_zoom(access, access.bytes.data(), 12.0F, player_target) ==
                gdtpc::CollisionZoomResult::success, "a valid collision zoom write was refused");
            float blend_after = 0.0F, target_after = 0.0F;
            std::memcpy(&blend_after, access.bytes.data() + gdtpc::camera_field_offsets[6], sizeof(blend_after));
            std::memcpy(&target_after, access.bytes.data() + gdtpc::camera_field_offsets[7], sizeof(target_after));
            require(std::abs(blend_after - (12.0F - 4.0F) / 86.0F) < 0.0001F,
                "the collision write did not move the camera");
            require(target_after == player_target, "the collision write reset the player's zoom target");
            require(access.synchronize_calls == 1, "the collision write skipped derived-state refresh");

            // The player scrolls further out while the arm is still held in. The caller passes their
            // new zoom and it must be what ends up in the field.
            const auto scrolled = 0.9F;
            require(gdtpc::restore_player_zoom(access, access.bytes.data(), 12.0F, scrolled) ==
                gdtpc::CollisionZoomResult::success, "a second collision zoom write was refused");
            std::memcpy(&target_after, access.bytes.data() + gdtpc::camera_field_offsets[7], sizeof(target_after));
            require(target_after == scrolled, "a scroll made while obstructed was not honoured");
        }

        // Holding the target at the arm leaves the engine nothing to animate toward, which is what
        // removes the residual shimmer. The player's zoom is then held outside the engine entirely,
        // so this mode must NOT write it anywhere.
        {
            const gdtpc::CameraProfile collision_profile{{4, 90, 42}, {15, 44, 30}};
            FakeAccess access;
            seed(access, profile(collision_profile, 42.0F, third_fov));
            const auto arm_blend = (8.0F - 4.0F) / 86.0F;

            require(gdtpc::write_collision_zoom(access, access.bytes.data(), 8.0F) ==
                gdtpc::CollisionZoomResult::success, "a collision zoom write was refused");
            float blend_after = 0.0F, target_after = 0.0F;
            std::memcpy(&blend_after, access.bytes.data() + gdtpc::camera_field_offsets[6], sizeof(blend_after));
            std::memcpy(&target_after, access.bytes.data() + gdtpc::camera_field_offsets[7], sizeof(target_after));
            require(std::abs(blend_after - arm_blend) < 0.0001F, "the held write did not move the camera");
            require(std::abs(target_after - arm_blend) < 0.0001F,
                "the held write did not leave the target at the arm");
            require(std::abs(blend_after - target_after) < 0.0001F,
                "the engine was left something to animate toward");
        }

        // A distance the endpoints cannot express is refused outright rather than producing a blend
        // outside [0, 1], which capture_camera_snapshot treats as an invalid camera and which would
        // make every restore, the F8 exit and the logical stop included, refuse.
        {
            const gdtpc::CameraProfile collision_profile{{12, 90, 42}, {15, 44, 30}};
            FakeAccess access;
            seed(access, profile(collision_profile, 42.0F, third_fov));
            require(gdtpc::restore_player_zoom(access, access.bytes.data(), 4.0F, 0.5F) ==
                gdtpc::CollisionZoomResult::out_of_range, "an unexpressible collision distance was accepted");
            require(access.synchronize_calls == 0, "a refused collision write still touched the camera");
            require(gdtpc::restore_player_zoom(access, access.bytes.data(), 200.0F, 0.5F) ==
                gdtpc::CollisionZoomResult::out_of_range,
                "a collision distance beyond the far endpoint was accepted");

            require(gdtpc::restore_player_zoom(access, nullptr, 20.0F, 0.5F) ==
                gdtpc::CollisionZoomResult::invalid_argument, "a null camera was accepted");
            require(gdtpc::write_collision_zoom(access, access.bytes.data(),
                std::numeric_limits<float>::quiet_NaN()) ==
                gdtpc::CollisionZoomResult::invalid_argument,
                "a nonfinite collision distance was accepted");
            require(gdtpc::restore_player_zoom(access, access.bytes.data(), 20.0F, 1.5F) ==
                gdtpc::CollisionZoomResult::invalid_argument, "an out-of-range player target was accepted");
        }

        // A refused refresh and a refused repair are both reported rather than silently lost.
        {
            const gdtpc::CameraProfile collision_profile{{4, 90, 42}, {15, 44, 30}};
            FakeAccess denied;
            seed(denied, profile(collision_profile, 42.0F, third_fov));
            denied.fail_synchronize_operation = 0;
            require(gdtpc::restore_player_zoom(denied, denied.bytes.data(), 12.0F, 0.5F) ==
                gdtpc::CollisionZoomResult::synchronization_failed, "a failed refresh was not reported");

            FakeAccess unwritable;
            seed(unwritable, profile(collision_profile, 42.0F, third_fov));
            unwritable.deny_write_field = 7;
            require(gdtpc::restore_player_zoom(unwritable, unwritable.bytes.data(), 12.0F, 0.5F) ==
                gdtpc::CollisionZoomResult::target_repair_failed,
                "an unrepairable zoom target was not reported");
            // Holding at the arm writes no target at all, so an unwritable target field cannot fail it.
            FakeAccess held;
            seed(held, profile(collision_profile, 42.0F, third_fov));
            held.deny_write_field = 7;
            require(gdtpc::write_collision_zoom(held, held.bytes.data(), 12.0F) ==
                gdtpc::CollisionZoomResult::success,
                "a collision write depended on writing the target field");
        }

        std::cout << "PASS: eleven-field profile/FOV allowlist, native derived-state synchronization, sentinels, preflight, raw snapshots, per-write/readback/sync faults, exact rollback, truthful restore failure, and collision zoom writes that move the camera, refuse an unexpressible distance, preserve the player's own zoom target, and hold the target at the arm when asked.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
