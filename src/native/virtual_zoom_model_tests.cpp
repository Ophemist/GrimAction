// Offline coverage for virtual zoom (IMPLEMENTATION_PLAN.md section 8): click detection into the visual
// distance and its bounds, the engine target held and put back, the pull P = D - A and its vector signs,
// pitch_for_distance against the disassembled UpdatePitch formula at both branches and the default-distance
// boundary, tolerant eye-offset ownership with exact release, V remembered across F8, and collision
// testing the visual distance rather than the engine arm.

#include "camera_collision_model.h"
#include "mouse_look_model.h"
#include "virtual_zoom_model.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void near(const float actual, const float expected, const char* message, const float tolerance = 0.0001F)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) throw std::runtime_error(message);
}

constexpr float nan_value = std::numeric_limits<float>::quiet_NaN();
constexpr float degrees = 0.01745329238474369F;
// Accepted third-person profile: distance 4/90 default 42, pitch 15/44 default 30.
constexpr float endpoint_a = 4.0F;
constexpr float endpoint_b = 90.0F;

gdtpc::VirtualZoomSettings settings()
{
    gdtpc::VirtualZoomSettings s{};
    s.enabled = true;
    s.engine_distance = 85.0F;
    s.visual_minimum = 4.0F;
    s.visual_default = 42.0F;
    s.ratio = 1.14F;
    return s;
}

gdtpc::PitchProfileFields profile()
{
    return {42.0F, 30.0F, 4.0F, 90.0F, 15.0F, 44.0F};
}

float hold_blend() { return (85.0F - endpoint_a) / (endpoint_b - endpoint_a); }

// Brings a fresh model to holding at E, as the runtime does on the first applying frame.
void acquire(gdtpc::VirtualZoomModel& model)
{
    const auto d = model.step(true, 0.44F, endpoint_a, endpoint_b);
    require(d.acquire && d.state == gdtpc::VirtualZoomState::acquiring, "the first applying frame did not acquire");
    near(d.target_blend, hold_blend(), "acquire did not ask for the blend of E");
    model.record_acquire_result(true, d.target_blend);
    require(model.holding(), "a verified acquire did not hold");
}

// Records the visual length collision was asked to test.
class RecordingQuery final : public gdtpc::CameraLevelQuery
{
public:
    float hit{std::numeric_limits<float>::infinity()};
    float last_max{};
    [[nodiscard]] bool raycast_from_camera(const float max_distance, float& distance, gdtpc::CollisionRay& ray) noexcept override
    {
        last_max = max_distance;
        ray = {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
        distance = hit;
        return true;
    }
};
}

int main()
{
    try
    {
        // Settings.
        require(gdtpc::valid_virtual_zoom_settings(settings()), "valid settings rejected");
        auto bad = settings(); bad.engine_distance = 4.0F;
        require(!gdtpc::valid_virtual_zoom_settings(bad), "an engine distance at the visual minimum was accepted");
        bad = settings(); bad.ratio = 1.0F;
        require(!gdtpc::valid_virtual_zoom_settings(bad), "a click ratio of 1 was accepted");
        bad = settings(); bad.engine_distance = nan_value;
        require(!gdtpc::valid_virtual_zoom_settings(bad), "a NaN engine distance was accepted");
        near(gdtpc::maximum_hold_blend(settings()), 0.95F, "the hold blend limit is not half a click below 1");
        {
            gdtpc::VirtualZoomModel off(gdtpc::VirtualZoomSettings{});
            const auto d = off.step(true, 0.5F, endpoint_a, endpoint_b);
            require(d.state == gdtpc::VirtualZoomState::disabled && !d.acquire && !d.write_target && !d.release,
                "a disabled model acted");
        }

        // pitch_for_distance against UpdatePitch: lower branch, upper branch, the default boundary (jae takes
        // the upper branch, and both give the default there), the clamp and degenerate input.
        {
            auto radians = 0.0F;
            require(gdtpc::pitch_for_distance(profile(), 4.0F, radians), "minimum distance refused");
            near(radians, 15.0F * degrees, "pitch at the minimum distance is not pitch_min");
            require(gdtpc::pitch_for_distance(profile(), 23.0F, radians), "lower branch refused");
            near(radians, 22.5F * degrees, "lower-branch interpolation is wrong");
            require(gdtpc::pitch_for_distance(profile(), 42.0F, radians), "default distance refused");
            near(radians, 30.0F * degrees, "pitch at the default distance is not pitch_default");
            require(gdtpc::pitch_for_distance(profile(), 41.999F, radians), "just below default refused");
            near(radians, 30.0F * degrees, "the lower branch is discontinuous at the default", 0.0001F);
            require(gdtpc::pitch_for_distance(profile(), 66.0F, radians), "upper branch refused");
            near(radians, 37.0F * degrees, "upper-branch interpolation is wrong");
            require(gdtpc::pitch_for_distance(profile(), 90.0F, radians), "maximum distance refused");
            near(radians, 44.0F * degrees, "pitch at the maximum distance is not pitch_max");
            // Extrapolation is clamped exactly as UpdateFromInputImpl clamps: [0, 89] degrees.
            require(gdtpc::pitch_for_distance(profile(), -40.0F, radians), "extrapolated distance refused");
            near(radians, 0.0F, "pitch below 0 degrees was not clamped");
            require(gdtpc::pitch_for_distance(profile(), 500.0F, radians), "far extrapolation refused");
            near(radians, 89.0F * degrees, "pitch above 89 degrees was not clamped");
            require(!gdtpc::pitch_for_distance(profile(), nan_value, radians), "a NaN distance was accepted");
            auto degenerate = profile(); degenerate.distance_minimum = 42.0F;
            require(!gdtpc::pitch_for_distance(degenerate, 10.0F, radians), "a zero lower span was accepted");
            require(gdtpc::pitch_for_distance(degenerate, 50.0F, radians), "the valid upper branch of a degenerate lower span refused");
        }

        // Eye direction (verified against live telemetry) and pull signs: P = D - A moves the eye TOWARD the
        // target, landing exactly at the visual arm along the same direction.
        {
            auto u = gdtpc::eye_direction(0.0F, 0.0F);
            near(u.x, 0.0F, "yaw 0 x"); near(u.y, 0.0F, "yaw 0 y"); near(u.z, 1.0F, "yaw 0 is not +z");
            u = gdtpc::eye_direction(1.5707964F, 0.0F);
            near(u.x, 1.0F, "yaw pi/2 is not +x"); near(u.z, 0.0F, "yaw pi/2 z");
            u = gdtpc::eye_direction(0.7F, 0.5F);
            near(u.y, std::sin(0.5F), "positive pitch does not raise the eye");
            near(u.x * u.x + u.y * u.y + u.z * u.z, 1.0F, "eye direction is not unit length");

            gdtpc::CollisionVec3 pull{};
            require(gdtpc::eye_pull(85.0F, 10.0F, 0.0F, 0.0F, pull), "a valid pull refused");
            near(pull.x, 0.0F, "pull x"); near(pull.y, 0.0F, "pull y");
            near(pull.z, -75.0F, "the pull does not move the eye 75 units toward the target");
            const auto yaw = 2.3F, pitch = 0.35F, engine = 84.2F, arm = 7.6F;
            require(gdtpc::eye_pull(engine, arm, yaw, pitch, pull), "an oblique pull refused");
            const auto direction = gdtpc::eye_direction(yaw, pitch);
            near(engine * direction.x + pull.x, arm * direction.x, "pulled eye x is not at the arm", 0.001F);
            near(engine * direction.y + pull.y, arm * direction.y, "pulled eye y is not at the arm", 0.001F);
            near(engine * direction.z + pull.z, arm * direction.z, "pulled eye z is not at the arm", 0.001F);
            require(gdtpc::eye_pull(10.0F, 12.0F, 0.0F, 0.0F, pull) && pull.z > 0.0F,
                "an engine closer than the arm did not push the eye out to the arm");
            require(!gdtpc::eye_pull(nan_value, 10.0F, 0.0F, 0.0F, pull), "a NaN engine distance was accepted");
            require(!gdtpc::eye_pull(85.0F, 10.0F, nan_value, 0.0F, pull), "a NaN yaw was accepted");
            require(!gdtpc::eye_pull(300.0F, 10.0F, 0.0F, 0.0F, pull), "an implausible pull was accepted");
        }

        // Acquire, then hold: an untouched target writes nothing.
        {
            gdtpc::VirtualZoomModel model(settings());
            near(model.visual_distance(), 42.0F, "V does not start at distance_default");
            acquire(model);
            const auto d = model.step(true, hold_blend(), endpoint_a, endpoint_b);
            require(d.state == gdtpc::VirtualZoomState::holding && !d.write_target && !d.acquire && !d.release,
                "an untouched held target was rewritten");
            near(d.visual_distance, 42.0F, "V moved without a click");
        }

        // A setter whose read-back does not match is a fault, not a hold.
        {
            gdtpc::VirtualZoomModel model(settings());
            const auto d = model.step(true, 0.44F, endpoint_a, endpoint_b);
            model.record_acquire_result(true, d.target_blend + 0.01F);
            require(!model.holding() && model.fault_count() == 1, "a mismatched acquire read-back was accepted");
            require(model.step(true, 0.44F, endpoint_a, endpoint_b).acquire, "a failed acquire was not retried");
        }

        // Clicks become proportional V steps and the engine target is put back to E.
        {
            gdtpc::VirtualZoomModel model(settings());
            acquire(model);
            auto d = model.step(true, hold_blend() - 0.1F, endpoint_a, endpoint_b);
            require(d.write_target && d.clicks == -1, "an inward click was not detected");
            near(d.target_blend, hold_blend(), "an inward click did not put the target back to E");
            near(d.visual_distance, 42.0F / 1.14F, "an inward click is not V / 1.14", 0.001F);
            model.record_target_write_result(true);

            // Outward from 0.9419 the engine clamps at 1: a short, clipped move is still one click.
            d = model.step(true, 1.0F, endpoint_a, endpoint_b);
            require(d.write_target && d.clicks == 1, "a clipped outward click was not detected");
            near(d.visual_distance, 42.0F, "an outward click did not undo the inward one", 0.001F);
            model.record_target_write_result(true);

            // Two clicks inside one callback.
            d = model.step(true, hold_blend() - 0.2F, endpoint_a, endpoint_b);
            require(d.clicks == -2, "two clicks in one frame were not counted");
            near(d.visual_distance, 42.0F / (1.14F * 1.14F), "two inward clicks are not V / 1.14^2", 0.001F);
            model.record_target_write_result(true);

            // A change that is not the engine's step is absorbed without moving V.
            const auto before = model.visual_distance();
            d = model.step(true, hold_blend() - 0.05F, endpoint_a, endpoint_b);
            require(d.write_target && d.clicks == 0, "a foreign target change was not put back, or was taken as a click");
            near(d.visual_distance, before, "a foreign target change moved V");
            model.record_target_write_result(true);
            require(model.click_count() == 4, "click count does not total the clicks consumed");

            // Bounds: V never exceeds E nor drops below the visual minimum.
            for (int i = 0; i < 40; ++i)
            {
                static_cast<void>(model.step(true, 1.0F, endpoint_a, endpoint_b));
                model.record_target_write_result(true);
            }
            near(model.visual_distance(), 85.0F, "V exceeded the engine distance");
            for (int i = 0; i < 80; ++i)
            {
                static_cast<void>(model.step(true, hold_blend() - 0.1F, endpoint_a, endpoint_b));
                model.record_target_write_result(true);
            }
            near(model.visual_distance(), 4.0F, "V dropped below the visual minimum");
        }

        // F8 out: the runtime hands the engine back to V and relinquishes; V survives and is reapplied on entry.
        // A new session resets it.
        {
            gdtpc::VirtualZoomModel model(settings());
            acquire(model);
            static_cast<void>(model.step(true, hold_blend() - 0.1F, endpoint_a, endpoint_b));
            model.record_target_write_result(true);
            const auto remembered = model.visual_distance();
            auto blend = 0.0F;
            require(model.visual_blend(endpoint_a, endpoint_b, blend), "V has no blend to hand back");
            near(blend, (remembered - endpoint_a) / (endpoint_b - endpoint_a), "hand-back blend is not V's");
            model.relinquish();
            require(!model.holding(), "relinquish kept holding");
            auto d = model.step(false, 0.3F, endpoint_a, endpoint_b);
            require(!d.release && !d.write_target && d.state == gdtpc::VirtualZoomState::released,
                "a relinquished model released twice or wrote in native");
            d = model.step(true, 0.3F, endpoint_a, endpoint_b);
            require(d.acquire, "re-entry did not acquire");
            near(d.visual_distance, remembered, "V was not remembered across F8");
            model.reset_session();
            near(model.visual_distance(), 42.0F, "a new session did not reset V to distance_default");
        }

        // Ineligible while holding (alt-tab): one release with V's blend, then nothing.
        {
            gdtpc::VirtualZoomModel model(settings());
            acquire(model);
            auto d = model.step(false, hold_blend(), endpoint_a, endpoint_b);
            require(d.release && !model.holding(), "losing eligibility did not release");
            near(d.release_blend, (42.0F - endpoint_a) / (endpoint_b - endpoint_a), "release blend is not V's");
            d = model.step(false, hold_blend(), endpoint_a, endpoint_b);
            require(!d.release, "release repeated");
        }

        // Endpoints that cannot show an outward click at E are refused rather than held.
        {
            gdtpc::VirtualZoomModel model(settings());
            const auto d = model.step(true, 0.5F, 4.0F, 86.0F);
            require(!d.acquire && d.state == gdtpc::VirtualZoomState::released,
                "a hold blend above the click-visibility limit was acquired");
        }

        // Write failures: reacquire, and once latched hand the engine back exactly once and stay off.
        {
            gdtpc::VirtualZoomModel model(settings());
            acquire(model);
            static_cast<void>(model.step(true, hold_blend() - 0.1F, endpoint_a, endpoint_b));
            model.record_target_write_result(false);
            require(!model.holding() && model.fault_count() == 1 && !model.latched(), "a failed target write kept holding");
            auto d = model.step(true, hold_blend() - 0.1F, endpoint_a, endpoint_b);
            require(d.acquire, "a failed target write was not followed by a fresh acquire");
            model.record_acquire_result(false, nan_value);
            model.record_fault();
            model.record_fault();
            require(model.latched(), "the fault limit did not latch");
            d = model.step(true, 0.5F, endpoint_a, endpoint_b);
            require(d.release && d.state == gdtpc::VirtualZoomState::latched_off && !d.acquire,
                "a latch after failed writes did not hand the engine back");
            d = model.step(true, 0.5F, endpoint_a, endpoint_b);
            require(!d.release && !d.acquire && d.state == gdtpc::VirtualZoomState::latched_off, "a latched model acted again");
            model.reset_session();
            require(model.latched(), "the latch did not survive a session change");
        }

        // Smoothed visual distance: glides to V, holds on a bad frame time, snaps when within a thousandth.
        {
            gdtpc::VirtualZoomModel model(settings());
            acquire(model);
            static_cast<void>(model.step(true, 1.0F, endpoint_a, endpoint_b));
            model.record_target_write_result(true);
            near(model.smoothed_distance(), 42.0F, "the glide started somewhere other than the old V");
            const auto first = model.advance(1.0F / 60.0F);
            require(first > 42.0F && first < 42.0F * 1.14F, "the glide did not move part of the way");
            near(model.advance(0.5F), first, "an implausible frame time moved the glide");
            near(model.advance(nan_value), first, "a NaN frame time moved the glide");
            for (int i = 0; i < 240; ++i) static_cast<void>(model.advance(1.0F / 60.0F));
            near(model.smoothed_distance(), 42.0F * 1.14F, "the glide did not settle on V", 0.0F);
        }

        // Eye offset ownership: base adopted, recomposed while ours (tolerant), fresh native value adopted,
        // exact base on release, nothing claimed after a failed write.
        {
            gdtpc::EyeOffsetOverlay eye;
            gdtpc::CollisionVec3 value{};
            const gdtpc::CollisionVec3 base{0.25F, -0.5F, 1.0F};
            require(eye.step(base, {0.0F, -10.0F, -70.0F}, value), "a valid first pull refused");
            require(value == gdtpc::CollisionVec3{0.25F, -10.5F, -69.0F}, "the first pull is not base + pull");
            require(eye.owned() && eye.base() == base, "the base was not adopted");
            // Read back perturbed in the last bits: still ours, recomposed from the saved base.
            const gdtpc::CollisionVec3 perturbed{value.x + 0.0004F, value.y, value.z - 0.0004F};
            require(eye.step(perturbed, {0.0F, -9.0F, -71.0F}, value), "a recomposed pull refused");
            require(value == gdtpc::CollisionVec3{0.25F, -9.5F, -70.0F}, "a tolerant read-back compounded the pull");
            require(eye.recompose({0.0F, -8.0F, -72.0F}, value), "a suppressed-shake recomposition refused");
            require(eye.base() == base && value == gdtpc::CollisionVec3{0.25F, -8.5F, -71.0F},
                "suppressed shake replaced the proven native base");
            // Something else wrote the field: that is the new base.
            const gdtpc::CollisionVec3 native{3.0F, 0.0F, 0.0F};
            require(eye.step(native, {0.0F, -1.0F, 0.0F}, value) && eye.base() == native, "a native change was not adopted");
            gdtpc::CollisionVec3 restore{};
            require(eye.relinquish(value, restore) && restore == native && !eye.owned(), "release did not return the exact base");
            require(eye.pull() == gdtpc::CollisionVec3{}, "a released overlay still reports a pull");
            require(!eye.relinquish(value, restore), "a second release claimed the field");
            require(eye.step(base, {1.0F, 0.0F, 0.0F}, value), "step after release refused");
            require(!eye.relinquish({9.0F, 9.0F, 9.0F}, restore), "release restored over a field that is no longer ours");
            require(eye.step(base, {1.0F, 0.0F, 0.0F}, value), "step refused");
            eye.forget();
            require(!eye.recompose({0.0F, 0.0F, 0.0F}, value), "recomposition claimed an unowned field");
            require(eye.step(value, {0.0F, 0.0F, 0.0F}, value) && eye.base() == gdtpc::CollisionVec3{1.25F, -0.5F, 1.0F},
                "after a failed write the field was still treated as ours");
            require(!eye.step({nan_value, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, value), "a NaN field was accepted");
            require(!eye.step(base, {0.0F, nan_value, 0.0F}, value), "a NaN pull was accepted");
            require(!gdtpc::camera_shake_active(-1) && !gdtpc::camera_shake_active(0) && gdtpc::camera_shake_active(1),
                "the native camera-shake remaining-tick gate is wrong");
        }

        // Pitch overlay explicit base: final = base + offset clamped to the floor, native() is the base, and
        // switching back to field ownership treats the explicit write as ours.
        {
            gdtpc::PitchOverlay overlay;
            const auto base = 20.0F * degrees;
            const auto written = overlay.step_from_base(base, 5.0F, -6.0F);
            near(written, 25.0F * degrees, "explicit base + offset is wrong");
            near(overlay.native(), base, "native() is not the explicit base");
            near(overlay.step_from_base(base, -40.0F, -6.0F), -6.0F * degrees, "the floor was not applied to the explicit base");
            require(!std::isfinite(overlay.step_from_base(nan_value, 0.0F, -6.0F)), "a NaN base was written");
            near(overlay.native(), base, "a NaN base replaced the native pitch");
            const auto again = overlay.step_from_base(base, 5.0F, -6.0F);
            near(overlay.step(again, 5.0F, -6.0F), again, "field ownership did not recognise the explicit write");
        }

        // Far-plane cap: percent validation, 100 is off, fraction of the native value recomposed while ours, a
        // sector's new value adopted, exact native restore, nothing claimed over a foreign value or after a failed write.
        {
            require(gdtpc::valid_far_plane_percent(50) && gdtpc::valid_far_plane_percent(95) &&
                gdtpc::valid_far_plane_percent(100) && !gdtpc::valid_far_plane_percent(49) &&
                !gdtpc::valid_far_plane_percent(101) && !gdtpc::valid_far_plane_percent(0), "far-plane percent bounds are wrong");
            near(gdtpc::far_plane_fraction(95), 0.95F, "95 percent is not a 0.95 cap");
            near(gdtpc::far_plane_fraction(50), 0.5F, "50 percent is not a 0.5 cap");
            near(gdtpc::far_plane_fraction(100), 0.0F, "100 percent is not off");
            near(gdtpc::far_plane_fraction(20), 0.0F, "an invalid percent is not off");
            near(gdtpc::far_plane_floor(76.0F, 65.4047394F, 42.0F), 99.4047394F,
                "the observed view did not preserve its default-camera scenery depth");
            near(gdtpc::far_plane_floor(76.0F, 85.0F, 42.0F), 119.0F,
                "the maximum virtual arm did not preserve its default-camera scenery depth");
            near(gdtpc::far_plane_floor(114.0F, 42.0F, 42.0F), 114.0F,
                "the default visual distance changed a sufficient native cap");
            near(gdtpc::far_plane_floor(30.0F, 42.0F, 42.0F), 42.0F,
                "a short sector cap was made worse than the target distance");
            near(gdtpc::far_plane_floor(76.0F, nan_value, 42.0F), 0.0F,
                "a nonfinite arm produced a far-plane floor");
            gdtpc::FarPlaneOverlay far;
            auto value = 0.0F, restore = 0.0F;
            require(far.step(400.0F, 0.5F, 0.0F, 42.0F, value), "a valid far plane was refused");
            near(value, 200.0F, "the cap is not half of the native far plane");
            require(far.step(200.004F, 0.7F, 0.0F, 42.0F, value), "a recomposed cap was refused");
            near(value, 280.0F, "a mode change compounded instead of recomposing from native");
            require(far.step(600.0F, 0.7F, 0.0F, 42.0F, value) && far.native() == 600.0F, "a sector's new far plane was not adopted");
            near(value, 420.0F, "the adopted far plane was not capped");
            require(far.relinquish(420.0F, restore) && restore == 600.0F && !far.owned(), "release did not restore native");
            require(far.step(80.0F, 0.95F, 85.0F, 42.0F, value), "the live clipping regression was refused");
            near(value, 119.0F, "an 85-unit camera lost the sector's default-camera scenery depth");
            require(far.relinquish(119.0F, restore) && restore == 80.0F, "the floored cap did not restore the short sector native");
            require(far.step(600.0F, 0.5F, 0.0F, 42.0F, value), "step after release refused");
            require(!far.relinquish(123.0F, restore), "release restored over a foreign far plane");
            require(far.step(600.0F, 0.5F, 0.0F, 42.0F, value), "step refused");
            far.forget();
            require(far.step(300.0F, 0.5F, 0.0F, 42.0F, value) && far.native() == 300.0F, "after a failed write the field was still ours");
            require(!far.step(nan_value, 0.5F, 0.0F, 42.0F, value) && !far.step(-1.0F, 0.5F, 0.0F, 42.0F, value) &&
                !far.step(400.0F, 0.0F, 0.0F, 42.0F, value) && !far.step(400.0F, 1.5F, 0.0F, 42.0F, value) &&
                !far.step(400.0F, 0.5F, -1.0F, 42.0F, value) && !far.step(400.0F, 0.5F, 85.0F, 0.0F, value),
                "an implausible far plane, fraction, arm or default was accepted");
        }

        // Collision tests the VISUAL distance: the ray length is V, the arm never exceeds it, and a hit
        // shortens it, independent of how far the engine is held.
        {
            gdtpc::CollisionSettings collision{};
            collision.minimum_distance = 4.0F;
            gdtpc::CameraCollisionModel arm(collision);
            RecordingQuery query;
            auto decision = arm.decide_camera(query, 12.0F, nan_value, 1.0F / 60.0F, true);
            near(query.last_max, 12.0F, "collision did not query the visual distance");
            near(decision.arm, 12.0F, "an unobstructed arm is not V");
            query.hit = 6.35F;
            for (int i = 0; i < 4; ++i) decision = arm.decide_camera(query, 12.0F, nan_value, 1.0F / 60.0F, true);
            near(decision.arm, 6.0F, "a hit did not shorten the visual arm by the skin");
            require(decision.state == gdtpc::CollisionState::shortened, "a shortened visual arm is not reported");
        }

        std::cout << "PASS: virtual zoom settings, UpdatePitch replication at both branches, the default boundary and "
                     "the engine clamp, verified eye direction and pull signs landing at the visual arm, acquire and "
                     "read-back, click detection (single, clipped, multiple, foreign) into bounded proportional V "
                     "with the target put back to E, V remembered across F8 and reset per session, one-shot release "
                     "on ineligibility, refusal of an unclickable hold, fault reacquire and latch hand-back, glide "
                     "smoothing, tolerant eye-offset ownership, suppressed-shake recomposition from the proven base, exact release, native camera-shake activity classification, explicit-base pitch overlay, "
                     "far-plane cap percent, ownership and restore, and collision on the visual distance.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
