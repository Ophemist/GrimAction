#include "camera_memory_model.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace
{
constexpr std::size_t camera_size = 0x600;

void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void near(const float actual, const float expected, const float tolerance = 0.0001F)
{
    if (std::abs(actual - expected) > tolerance) throw std::runtime_error("Values differ.");
}
}

int main()
{
    try
    {
        std::array<std::byte, camera_size> camera{};
        camera.fill(std::byte{0x5a});
        const gdtpc::CameraProfile isometric{{20.0F, 48.0F, 36.0F}, {38.0F, 52.0F, 46.0F}};
        require(gdtpc::apply_camera_profile(camera.data(), isometric, 48.0F), "Could not seed fake camera.");

        gdtpc::CameraMemorySnapshot original{};
        require(gdtpc::capture_camera_memory(camera.data(), original), "Could not capture fake camera.");
        near(gdtpc::zoom_distance_from_memory(original), 48.0F);
        const auto original_bytes = camera;
        const auto unrelated_before = camera[0x300];

        const gdtpc::CameraProfile third_person{{8.0F, 32.0F, 18.0F}, {18.0F, 32.0F, 24.0F}};
        require(gdtpc::apply_camera_profile(camera.data(), third_person, 14.0F), "Could not apply third-person profile.");
        gdtpc::CameraMemorySnapshot changed{};
        require(gdtpc::capture_camera_memory(camera.data(), changed), "Could not capture changed camera.");
        near(changed.profile.distance.minimum, 8.0F);
        near(changed.profile.distance.maximum, 32.0F);
        near(changed.profile.pitch.initial, 24.0F);
        near(gdtpc::zoom_distance_from_memory(changed), 14.0F);
        require(camera[0x300] == unrelated_before, "An unrelated camera byte changed.");

        require(gdtpc::restore_camera_memory(camera.data(), original), "Could not restore camera.");
        gdtpc::CameraMemorySnapshot restored{};
        require(gdtpc::capture_camera_memory(camera.data(), restored), "Could not recapture restored camera.");
        require(camera == original_bytes, "Original camera state was not restored exactly.");

        const gdtpc::CameraProfile invalid{{20.0F, 10.0F, 15.0F}, {18.0F, 32.0F, 24.0F}};
        require(!gdtpc::apply_camera_profile(camera.data(), invalid, 15.0F), "Invalid profile was accepted.");
        require(!gdtpc::capture_camera_memory(nullptr, restored), "Null camera was accepted.");

        // Captured native state is evidence, not a value to sanitize. A blend outside [0,1] must
        // be rejected without changing the caller's previous valid snapshot.
        const auto preserved = restored;
        const auto bad_low = std::bit_cast<std::array<std::byte, sizeof(float)>>(-0.01F);
        std::copy(bad_low.begin(), bad_low.end(), camera.begin() + 0x580);
        require(!gdtpc::capture_camera_memory(camera.data(), restored), "Negative zoom blend was accepted.");
        require(std::memcmp(&restored, &preserved, sizeof(restored)) == 0, "Rejected capture changed the output snapshot.");
        const auto bad_high = std::bit_cast<std::array<std::byte, sizeof(float)>>(1.01F);
        std::copy(bad_high.begin(), bad_high.end(), camera.begin() + 0x580);
        require(!gdtpc::capture_camera_memory(camera.data(), restored), "Zoom blend above one was accepted.");
        require(std::memcmp(&restored, &preserved, sizeof(restored)) == 0, "Rejected capture changed the output snapshot.");

        // Finer zoom stepping. The engine steps its zoom target by a fixed 0.1 of the blend, which
        // on a 4 to 90 range is 8.6 units per click: a threefold jump at the near end and a tenth of
        // the distance at the far end. The model replaces that with a proportional step.
        {
            constexpr auto endpoint_a = 4.0F, endpoint_b = 90.0F, span = endpoint_b - endpoint_a;
            const auto blend_of = [](const float distance) { return (distance - endpoint_a) / span; };
            const auto distance_of = [](const float blend) { return endpoint_a + span * blend; };

            gdtpc::ZoomStepSettings settings{};
            settings.ratio = 1.14F;
            require(gdtpc::valid_zoom_step_settings(settings), "valid zoom step settings were rejected");
            gdtpc::ZoomStepModel model(settings);
            auto written = 0.0F;

            // The first observation is adopted, never reinterpreted: there is no previous value to
            // measure a click against.
            require(!model.step(blend_of(12.6F), endpoint_a, endpoint_b, written),
                "the first target observation was rewritten");

            // One inward click. The engine would have gone to 4.00; the model goes to 12.6 / 1.14.
            const auto engine_inward = blend_of(12.6F) - 0.1F;
            require(model.step(engine_inward, endpoint_a, endpoint_b, written),
                "an inward click was not substituted");
            near(distance_of(written), 12.6F / 1.14F, 0.001F);
            require(distance_of(written) > 4.0F, "an inward click collapsed to the near endpoint");

            // Clicking out from there returns to where it started, so the steps are reversible.
            const auto engine_outward = written + 0.1F;
            require(model.step(engine_outward, endpoint_a, endpoint_b, written),
                "an outward click was not substituted");
            near(distance_of(written), 12.6F, 0.001F);

            // Resolution where the player asked for it: from the closest zoom, an outward click is a
            // fraction of a unit rather than the engine's 8.6.
            gdtpc::ZoomStepModel low(settings);
            require(!low.step(0.0F, endpoint_a, endpoint_b, written), "the first observation was rewritten");
            require(low.step(0.1F, endpoint_a, endpoint_b, written), "an outward click was not substituted");
            near(distance_of(written), 4.0F * 1.14F, 0.001F);
            require(distance_of(written) - 4.0F < 1.0F, "the step near the close endpoint was still coarse");
        }

        // A target change that is not one of the engine's clicks belongs to something else, a profile
        // entry for instance, and must be adopted rather than reinterpreted as a click.
        {
            constexpr auto endpoint_a = 4.0F, endpoint_b = 90.0F;
            gdtpc::ZoomStepSettings settings{};
            gdtpc::ZoomStepModel model(settings);
            auto written = 0.0F;
            require(!model.step(0.2F, endpoint_a, endpoint_b, written), "the first observation was rewritten");
            require(!model.step(0.7F, endpoint_a, endpoint_b, written),
                "a large external target change was reinterpreted as a click");
            require(model.adopted_count() == 2, "the external change was not adopted");

            // Stepping continues from the adopted value.
            require(model.step(0.8F, endpoint_a, endpoint_b, written), "a click after adoption was ignored");
            require(model.click_count() == 1, "the click was not counted");

            // An unchanged target is left completely alone.
            require(!model.step(written, endpoint_a, endpoint_b, written),
                "an unchanged target was rewritten");
        }

        // Both endpoints hold under sustained clicking, and invalid settings disable substitution
        // entirely rather than producing erratic zooming.
        {
            constexpr auto endpoint_a = 4.0F, endpoint_b = 90.0F;
            gdtpc::ZoomStepSettings settings{};
            auto written = 0.0F;

            // Drives the model the way the engine does: the engine always steps from whatever value
            // the target currently holds, which is the value we last wrote.
            const auto click = [&](gdtpc::ZoomStepModel& model, float& blend, const float direction) {
                const auto engine = std::clamp(blend + direction * 0.1F, 0.0F, 1.0F);
                auto substituted = 0.0F;
                if (model.step(engine, endpoint_a, endpoint_b, substituted)) blend = substituted;
                else blend = engine;
            };

            gdtpc::ZoomStepModel inward(settings);
            auto blend = 0.5F;
            require(!inward.step(blend, endpoint_a, endpoint_b, written), "the first observation was rewritten");
            for (auto index = 0; index < 60; ++index)
            {
                click(inward, blend, -1.0F);
                require(blend >= 0.0F && blend <= 1.0F, "the blend left the range while clicking in");
            }
            near(blend, 0.0F, 0.0001F); // pinned at the near endpoint, which is the profile minimum

            gdtpc::ZoomStepModel outward(settings);
            blend = 0.5F;
            require(!outward.step(blend, endpoint_a, endpoint_b, written), "the first observation was rewritten");
            for (auto index = 0; index < 60; ++index)
            {
                click(outward, blend, 1.0F);
                require(blend >= 0.0F && blend <= 1.0F, "the blend left the range while clicking out");
            }
            near(blend, 1.0F, 0.0001F); // pinned at the far endpoint

            auto bad = settings; bad.ratio = 1.0F;
            require(!gdtpc::valid_zoom_step_settings(bad), "a ratio of one was accepted");
            bad = settings; bad.ratio = std::numeric_limits<float>::quiet_NaN();
            require(!gdtpc::valid_zoom_step_settings(bad), "a nonfinite ratio was accepted");
            bad = settings; bad.step_tolerance = settings.engine_step;
            require(!gdtpc::valid_zoom_step_settings(bad), "a tolerance as wide as the step was accepted");

            bad = settings; bad.ratio = 0.5F;
            gdtpc::ZoomStepModel disabled(bad);
            require(!disabled.step(0.2F, endpoint_a, endpoint_b, written), "a disabled model substituted");
            require(!disabled.step(0.3F, endpoint_a, endpoint_b, written), "a disabled model substituted");

            // A new session forgets what was commanded, so the next observation is adopted afresh.
            gdtpc::ZoomStepModel session(settings);
            require(!session.step(0.4F, endpoint_a, endpoint_b, written), "the first observation was rewritten");
            session.reset();
            require(!session.step(0.9F, endpoint_a, endpoint_b, written),
                "the first observation after a session reset was rewritten");
        }

        // While collision holds the target field at the arm, a scroll must still be recognised, and it
        // must step the PLAYER's zoom rather than the held value. This is what lets the camera be
        // pinned perfectly still against geometry without the player's zoom being destroyed.
        {
            constexpr auto endpoint_a = 4.0F, endpoint_b = 90.0F, span = endpoint_b - endpoint_a;
            const auto blend_of = [](const float distance) { return (distance - endpoint_a) / span; };
            const auto distance_of = [](const float blend) { return endpoint_a + span * blend; };

            gdtpc::ZoomStepSettings settings{};
            settings.ratio = 1.14F;
            gdtpc::ZoomStepModel model(settings);
            auto written = 0.0F;

            require(!model.step(blend_of(42.0F), endpoint_a, endpoint_b, written),
                "the first observation was rewritten");
            near(distance_of(model.player_blend()), 42.0F);

            // Collision pins the field at the arm. The player's zoom is untouched by that.
            const auto arm_blend = blend_of(8.0F);
            model.note_field_write(arm_blend);
            near(distance_of(model.player_blend()), 42.0F);

            // The engine applies one outward click to the HELD value, because that is what is in the
            // field. The model must read that as a click and step the player's 42 outward, not the 8.
            require(model.step(arm_blend + 0.1F, endpoint_a, endpoint_b, written),
                "a scroll made while the field was held was not recognised");
            near(distance_of(written), 42.0F * 1.14F, 0.001F);
            near(distance_of(model.player_blend()), 42.0F * 1.14F, 0.001F);

            // And an inward click from a held field steps the player's zoom inward by one step. The
            // engine clamps its own target into [0, 1], so from a field held close in it writes zero
            // rather than a negative blend; that clipped move is still one click.
            model.note_field_write(arm_blend);
            require(model.step(std::max(0.0F, arm_blend - 0.1F), endpoint_a, endpoint_b, written),
                "an inward scroll made while the field was held was not recognised");
            near(distance_of(written), 42.0F, 0.01F);

            // A field value the model itself last wrote is not a click and changes nothing.
            model.note_field_write(written);
            require(!model.step(written, endpoint_a, endpoint_b, written),
                "an unchanged held field was treated as a scroll");
            near(distance_of(model.player_blend()), 42.0F, 0.01F);
            require(model.click_count() == 2, "the clicks made while held were miscounted");
        }

        std::cout << "PASS: camera profile capture, normalized-blend validation, apply, zoom conversion, exact restore, and proportional zoom stepping that substitutes the engine fixed blend click, adopts external changes, holds both endpoints, and keeps the player zoom separate from a collision-held target field.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
