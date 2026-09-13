// Offline fault coverage for the spring-arm model. No Win32, no engine, no game. The fake level
// query lets every failure mode the live adapter could encounter be exercised deliberately,
// including the two the engine itself would get wrong: a NaN distance, which the game's own hit test
// treats as a hit, and a miss, which the game reports as +infinity.

#include "camera_collision_model.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
void require(const bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void near(const float actual, const float expected, const char* message, const float tolerance = 0.001F)
{
    if (!std::isfinite(actual) || !std::isfinite(expected) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

constexpr float infinity = std::numeric_limits<float>::infinity();
constexpr float quiet_nan = std::numeric_limits<float>::quiet_NaN();

// Replays a scripted sequence of results and records exactly what it was asked.
class FakeQuery final : public gdtpc::LevelQuery
{
public:
    std::vector<float> distances;   // value to report; +inf means a clean miss
    std::vector<bool> succeeds;     // false means the query itself could not run
    std::size_t calls{};
    float last_max{};
    gdtpc::CollisionRay last_ray{};

    [[nodiscard]] bool raycast(const gdtpc::CollisionRay& ray, const float max_distance, float& distance) noexcept override
    {
        last_ray = ray;
        last_max = max_distance;
        const auto index = calls < distances.size() ? calls : distances.size() - 1;
        const auto ok = succeeds.empty() ? true : succeeds[calls < succeeds.size() ? calls : succeeds.size() - 1];
        ++calls;
        if (!ok) return false;
        distance = distances.empty() ? infinity : distances[index];
        return true;
    }
};

class FakeCameraQuery final : public gdtpc::CameraLevelQuery
{
public:
    float scripted_distance{infinity};
    bool succeeds{true};
    bool normalized{true};
    std::size_t calls{};

    [[nodiscard]] bool raycast_from_camera(const float, float& distance,
        gdtpc::CollisionRay& ray) noexcept override
    {
        ++calls;
        if (!succeeds) return false;
        ray = {{100.0F, 10.0F, 200.0F}, normalized ? gdtpc::CollisionVec3{0.0F, 0.0F, 1.0F}
                                 : gdtpc::CollisionVec3{0.0F, 0.0F, 2.0F}};
        distance = scripted_distance;
        return true;
    }
};

// Drives the model the way the live runtime does. `camera_distance` is where the camera actually is,
// which trails or escapes what it is told because the engine animates its own zoom; `player_zoom` is
// the engine's zoom target, which nothing in the runtime writes and which therefore never moves
// unless the player scrolls. `escape_per_frame` models the engine pulling the camera back toward the
// player's zoom; `lag` models it following a value we command.
gdtpc::CollisionDecision run_camera(gdtpc::CameraCollisionModel& model, gdtpc::CameraLevelQuery& query,
    float& camera_distance, const float player_zoom, const int frames,
    const float escape_per_frame = 0.0F, const float lag = 1.0F)
{
    gdtpc::CollisionDecision decision{};
    for (auto frame = 0; frame < frames; ++frame)
    {
        decision = model.decide_camera(query, player_zoom, camera_distance, 0.016F, true);
        if (decision.write) camera_distance += (decision.arm - camera_distance) * lag;
        else camera_distance += (player_zoom - camera_distance) * 0.05F; // released: the engine goes home
        camera_distance += escape_per_frame;
    }
    return decision;
}

gdtpc::CollisionSettings settings()
{
    gdtpc::CollisionSettings value{};
    value.skin = 0.5F;
    value.minimum_distance = 2.0F;
    value.extend_units_per_second = 10.0F;
    value.query_interval = 1;
    // The existing cases prove the extension rate itself, so they release on the first clear query.
    // The release-confirmation cases below tune this up deliberately.
    value.release_confirm_queries = 1;
    value.release_immediate_units = 1.0F;
    value.fault_limit = 3;
    return value;
}

constexpr gdtpc::CollisionVec3 focus{100.0F, 10.0F, 200.0F};
constexpr gdtpc::CollisionVec3 behind{0.0F, 1.0F, 0.0F}; // deliberately not unit length
}

int main()
{
    try
    {
        // Settings validation, and an invalid configuration latches off instead of half working.
        {
            require(gdtpc::valid_collision_settings(settings()), "valid settings were rejected");
            auto bad = settings(); bad.minimum_distance = 0.1F;
            require(!gdtpc::valid_collision_settings(bad), "an out-of-range minimum distance was accepted");
            bad = settings(); bad.skin = quiet_nan;
            require(!gdtpc::valid_collision_settings(bad), "a nonfinite skin was accepted");
            bad = settings(); bad.query_interval = 0;
            require(!gdtpc::valid_collision_settings(bad), "a zero query interval was accepted");
            bad = settings(); bad.release_confirm_queries = 0;
            require(!gdtpc::valid_collision_settings(bad), "a zero release confirmation count was accepted");
            bad = settings(); bad.query_interval = 0;

            gdtpc::CameraCollisionModel model(bad);
            FakeQuery query;
            require(model.state() == gdtpc::CollisionState::latched_off, "invalid settings did not latch off");
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 42.0F, "latched-off model altered the arm");
            require(query.calls == 0, "latched-off model issued a query");
        }

        // A clean miss is +infinity and means no obstruction: the arm reaches the desired length and
        // it is not counted as a fault.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {infinity};
            const auto arm = model.step(query, focus, behind, 42.0F, 0.016F, true);
            near(arm, 42.0F, "a miss did not allow the full desired distance");
            require(model.fault_count() == 0, "a miss was counted as a fault");
            require(model.hit_count() == 0, "a miss was counted as a hit");
            require(model.state() == gdtpc::CollisionState::idle, "a miss did not leave the arm idle");
        }

        // The ray handed to the engine must be normalized and bounded by the desired distance,
        // because GAME::Ray documents a unit direction and the max distance bounds the search.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {infinity};
            static_cast<void>(model.step(query, focus, behind, 42.0F, 0.016F, true));
            require(query.calls == 1, "no query was issued");
            near(query.last_max, 42.0F, "the query was not bounded by the desired distance");
            require(query.last_ray.origin == focus, "the ray did not start at the focus point");
            const auto& d = query.last_ray.direction;
            near(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), 1.0F, "the ray direction was not normalized");
        }

        // A hit shortens the arm to the hit distance less the skin, and reports it as shortened.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {10.0F};
            const auto arm = model.step(query, focus, behind, 42.0F, 0.016F, true);
            near(arm, 9.5F, "a hit did not shorten the arm by the skin");
            require(model.hit_count() == 1 && model.fault_count() == 0, "hit accounting was wrong");
            require(model.state() == gdtpc::CollisionState::shortened, "a hit did not report a shortened arm");
        }

        // A hit closer than the minimum is clamped, so the camera never ends up inside the character.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {0.25F};
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 2.0F,
                "a very close hit was not clamped to the minimum distance");
        }

        // NaN must never be treated as a hit. The engine's own test would, and that would drive the
        // camera into the character.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {quiet_nan};
            const auto arm = model.step(query, focus, behind, 42.0F, 0.016F, true);
            near(arm, 42.0F, "a NaN distance moved the arm");
            require(model.fault_count() == 1, "a NaN distance was not counted as a fault");
            require(model.hit_count() == 0, "a NaN distance was counted as a hit");
        }

        // A negative distance is equally invalid.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {-5.0F};
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 42.0F, "a negative distance moved the arm");
            require(model.fault_count() == 1, "a negative distance was not counted as a fault");
        }

        // Repeated faults latch assistance off for good, and a latched model never queries again.
        {
            auto tuned = settings(); tuned.fault_limit = 3;
            gdtpc::CameraCollisionModel model(tuned);
            FakeQuery query; query.succeeds = {false}; query.distances = {infinity};
            for (int attempt = 0; attempt < 3; ++attempt)
                static_cast<void>(model.step(query, focus, behind, 42.0F, 0.016F, true));
            require(model.state() == gdtpc::CollisionState::latched_off, "repeated faults did not latch off");
            const auto calls_at_latch = query.calls;
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 42.0F, "latched-off model altered the arm");
            require(query.calls == calls_at_latch, "latched-off model kept querying");
            // A latch must survive a session change: one bad world does not earn a fresh chance.
            model.reset_session();
            require(model.state() == gdtpc::CollisionState::latched_off, "a session reset cleared the fault latch");
            require(query.calls == calls_at_latch, "a session reset resumed querying after a latch");
        }

        // Pulling in is immediate, because clipping is worse than a jump. Easing back out is bounded
        // by the configured rate.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {10.0F, 10.0F, infinity, infinity};
            static_cast<void>(model.step(query, focus, behind, 42.0F, 0.016F, true));
            near(model.arm(), 9.5F, "arm did not pull in on the first hit");
            static_cast<void>(model.step(query, focus, behind, 42.0F, 0.016F, true));
            near(model.arm(), 9.5F, "arm drifted while still obstructed");
            // Obstruction clears. A 100 ms frame at 10 units per second extends by exactly 1.0, not
            // straight back to 42. Note the timestep is deliberately at the plausibility limit: a
            // larger one is rejected outright by the guard tested below, so it could not be used
            // here to demonstrate extension.
            const auto extended = model.step(query, focus, behind, 42.0F, 0.1F, true);
            near(extended, 10.5F, "arm extended faster than the configured rate");
            require(extended < 42.0F, "arm snapped straight back to the desired distance");
        }

        // An implausible or nonpositive timestep leaves the arm alone rather than inventing a step.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {10.0F, infinity, infinity, infinity};
            static_cast<void>(model.step(query, focus, behind, 42.0F, 0.016F, true));
            const auto held = model.arm();
            near(model.step(query, focus, behind, 42.0F, 0.0F, true), held, "a zero timestep moved the arm");
            near(model.step(query, focus, behind, 42.0F, 5.0F, true), held, "a huge timestep moved the arm");
            near(model.step(query, focus, behind, 42.0F, quiet_nan, true), held, "a NaN timestep moved the arm");
        }

        // Ineligible frames issue no query at all and return the desired distance untouched.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {5.0F};
            near(model.step(query, focus, behind, 42.0F, 0.016F, false), 42.0F, "an ineligible frame moved the arm");
            require(query.calls == 0, "an ineligible frame issued a query");
            require(model.state() == gdtpc::CollisionState::ineligible, "an ineligible frame was not reported");
        }

        // Nonfinite geometry must never reach the engine. This is the failure that a +0x28 camera
        // position would have produced: a NaN origin handed to a physics raycast.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {5.0F};
            const gdtpc::CollisionVec3 poisoned{quiet_nan, 0.0F, 0.0F};
            near(model.step(query, poisoned, behind, 42.0F, 0.016F, true), 42.0F, "a NaN focus moved the arm");
            near(model.step(query, focus, poisoned, 42.0F, 0.016F, true), 42.0F, "a NaN direction moved the arm");
            // A nonfinite desired distance is returned exactly as given. The model never invents a
            // value; it only ever shortens a good one, so a bad input stays visibly bad.
            require(std::isnan(model.step(query, focus, behind, quiet_nan, 0.016F, true)),
                "a NaN desired distance was silently replaced");
            require(query.calls == 0, "nonfinite geometry reached the level query");
            require(model.fault_count() == 0, "rejecting nonfinite geometry was counted as a fault");
        }

        // A degenerate direction cannot make a unit ray; no query, no fault, no movement.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {5.0F};
            near(model.step(query, focus, {0.0F, 0.0F, 0.0F}, 42.0F, 0.016F, true), 42.0F,
                "a zero-length direction moved the arm");
            require(query.calls == 0 && model.fault_count() == 0, "a zero-length direction queried or faulted");
        }

        // Throttling: with an interval of 3 the ray runs on the first frame and every third after,
        // and the arm holds its value in between.
        {
            auto tuned = settings(); tuned.query_interval = 3;
            gdtpc::CameraCollisionModel model(tuned);
            FakeQuery query; query.distances = {12.0F};
            for (int frame = 0; frame < 7; ++frame)
                static_cast<void>(model.step(query, focus, behind, 42.0F, 0.016F, true));
            require(query.calls == 3, "throttling did not limit the query rate");
            near(model.arm(), 11.5F, "the arm did not hold its value between throttled queries");
        }

        // A session reset forgets the arm but not the counters, so evidence is never lost.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {10.0F};
            static_cast<void>(model.step(query, focus, behind, 42.0F, 0.016F, true));
            const auto queries = model.query_count();
            model.reset_session();
            require(model.query_count() == queries, "a session reset discarded the query count");
            require(model.state() == gdtpc::CollisionState::ineligible, "a session reset left the arm active");
        }

        // Arbitration between the player's zoom and a collision override. The player's zoom is an
        // independent input now, read from the engine's own zoom target, so the camera's distance is
        // fed back separately and cannot be mistaken for a choice.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {infinity};
            auto camera = 42.0F;
            auto decision = model.decide(query, focus, behind, 42.0F, camera, 0.016F, true);
            require(!decision.write, "an unobstructed camera was written to");
            near(decision.desired, 42.0F, "the player zoom was not read while unobstructed");

            // The player scrolls. The engine's target moves and the camera animates after it.
            decision = model.decide(query, focus, behind, 30.0F, camera, 0.016F, true);
            near(decision.desired, 30.0F, "a player zoom change was not read");
            require(!decision.write, "reading the player zoom caused a write");
            camera = 30.0F;

            // Obstruction appears: collision takes over and writes.
            query.distances = {8.0F};
            decision = model.decide(query, focus, behind, 30.0F, camera, 0.016F, true);
            require(decision.write, "an obstruction did not produce a write");
            near(decision.arm, 7.5F, "the override did not shorten to the hit less the skin");
            near(decision.desired, 30.0F, "the player zoom was lost when the override began");
            camera = decision.arm;

            // The player's zoom is untouched however long the override holds, and the arm is steady.
            for (int frame = 0; frame < 10; ++frame)
                decision = model.decide(query, focus, behind, 30.0F, camera, 0.016F, true);
            near(decision.desired, 30.0F, "the player zoom moved while overriding");
            near(decision.arm, 7.5F, "the arm drifted while the obstruction was unchanged");

            // A static obstruction the camera does not escape from costs no repeated writes.
            auto writes = 0;
            for (int frame = 0; frame < 10; ++frame)
                if (model.decide(query, focus, behind, 30.0F, camera, 0.016F, true).write) ++writes;
            require(writes == 0, "a static obstruction wrote every frame");

            // Obstruction clears: the arm eases back out and the model then simply stops writing,
            // leaving the engine to hold the camera at the player's own target.
            query.distances = {infinity};
            auto released = false;
            for (int frame = 0; frame < 400 && !released; ++frame)
            {
                decision = model.decide(query, focus, behind, 30.0F, camera, 0.05F, true);
                if (decision.write) camera = decision.arm;
                if (!decision.write && decision.state != gdtpc::CollisionState::shortened) released = true;
            }
            require(released, "the arm never returned to the player zoom after the obstruction cleared");
            near(decision.desired, 30.0F, "the player zoom changed while easing back out");
            // The last fraction of a unit is the engine's to cover: the model stops writing as soon
            // as the arm is back at the player's zoom rather than chasing the final hundredths.
            near(camera, 30.0F, "the camera was not eased back to the player zoom", 0.5F);

            // Released: a new player zoom is simply read, with no write of our own.
            decision = model.decide(query, focus, behind, 25.0F, camera, 0.016F, true);
            require(!decision.write, "the model kept writing after releasing the camera");
            near(decision.desired, 25.0F, "a new player zoom was not read after release");
        }

        // Ineligible and latched-off frames hand the camera back and never write.
        {
            gdtpc::CameraCollisionModel model(settings());
            FakeQuery query; query.distances = {5.0F};
            auto decision = model.decide(query, focus, behind, 42.0F, 42.0F, 0.016F, false);
            require(!decision.write && query.calls == 0, "an ineligible frame queried or wrote");
            require(decision.state == gdtpc::CollisionState::ineligible, "ineligibility was not reported");

            auto bad = settings(); bad.fault_limit = 1;
            gdtpc::CameraCollisionModel latched(bad);
            FakeQuery failing; failing.succeeds = {false}; failing.distances = {infinity};
            static_cast<void>(latched.decide(failing, focus, behind, 42.0F, 42.0F, 0.016F, true));
            const auto after = latched.decide(failing, focus, behind, 42.0F, 42.0F, 0.016F, true);
            require(latched.state() == gdtpc::CollisionState::latched_off, "the fault did not latch");
            require(!after.write, "a latched-off model wrote");
            near(after.arm, 42.0F, "a latched-off model altered the camera");
        }


        // The runtime path asks the camera-aware adapter only on due frames, validates the unit ray
        // it reports, and accounts a refused native SetZoom through the same fault latch.
        {
            auto tuned = settings(); tuned.query_interval = 3; tuned.fault_limit = 2;
            gdtpc::CameraCollisionModel model(tuned);
            FakeCameraQuery query; query.scripted_distance = 8.0F;
            auto decision = model.decide_camera(query, 30.0F, 30.0F, 0.016F, true);
            require(decision.write && query.calls == 1, "camera-aware path did not query and shorten");
            model.record_write_result(false);
            decision = model.decide_camera(query, 30.0F, 8.0F, 0.016F, true);
            require(decision.write, "a refused SetZoom was not made retryable");
            model.record_write_result(false);
            require(model.state() == gdtpc::CollisionState::latched_off,
                "repeated SetZoom faults did not latch collision off");
        }

        {
            gdtpc::CameraCollisionModel model(settings());
            FakeCameraQuery query; query.normalized = false; query.scripted_distance = 8.0F;
            const auto decision = model.decide_camera(query, 30.0F, 30.0F, 0.016F, true);
            require(!decision.write && model.fault_count() == 1,
                "a non-unit camera-derived ray was not refused and counted");
        }

        // Release confirmation. At an obstruction boundary the ray chatters between a hit and a
        // miss from one query to the next; releasing the arm on the first clear sample and pulling
        // it back in on the next hit is the bounce observed in the live session.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 3;
            gdtpc::CameraCollisionModel model(tuned);
            FakeQuery query; query.distances = {8.0F, infinity, 8.0F, infinity, 8.0F, infinity};
            auto arm = model.step(query, focus, behind, 42.0F, 0.016F, true);
            near(arm, 7.5F, "the first obstruction did not shorten the arm");
            for (auto index = 0; index < 5; ++index)
            {
                arm = model.step(query, focus, behind, 42.0F, 0.016F, true);
                near(arm, 7.5F, "hit/miss chatter moved the arm");
            }
            require(model.state() == gdtpc::CollisionState::shortened,
                "chatter released the arm back to the game");
        }

        // Genuine clearance still releases, once the configured run of queries agrees, and then
        // extends at the configured rate rather than snapping.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 3;
            gdtpc::CameraCollisionModel model(tuned);
            FakeQuery query; query.distances = {8.0F, infinity};
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 7.5F, "the arm did not shorten");
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 7.5F, "one clear query released the arm");
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 7.5F, "two clear queries released the arm");
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 7.66F,
                "three clear queries did not begin extending the arm");
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 7.82F,
                "the arm did not keep extending at the configured rate");
        }

        // A confirming run releases only as far as its nearest sample, so a run that contains one
        // close reading hands back that reading and not the full arm.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 3;
            gdtpc::CameraCollisionModel model(tuned);
            FakeQuery query; query.distances = {8.0F, infinity, 20.0F, infinity, 20.0F};
            auto arm = 0.0F;
            for (auto index = 0; index < 200; ++index)
            {
                arm = model.step(query, focus, behind, 42.0F, 0.016F, true);
                require(arm <= 19.5F + 0.001F, "the arm released past the nearest confirming sample");
            }
            near(arm, 19.5F, "the arm did not settle at the nearest confirming sample");
        }

        // A query that could not be qualified is not evidence of clearance, so it breaks the run.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 3; tuned.fault_limit = 10;
            gdtpc::CameraCollisionModel model(tuned);
            FakeQuery query;
            query.distances = {8.0F, infinity};
            query.succeeds = {true, true, false, true};
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 7.5F, "the arm did not shorten");
            for (auto index = 0; index < 4; ++index)
                near(model.step(query, focus, behind, 42.0F, 0.016F, true), 7.5F,
                    "a failed query counted toward the release confirmation");
            require(model.fault_count() == 1, "the failed query was not counted once");
            near(model.step(query, focus, behind, 42.0F, 0.016F, true), 7.66F,
                "the run did not complete after the fault broke it");
        }

        // The shipping runtime path shares the confirmation, and because the arm never moves during
        // chatter the native setter is never called: no write, no bounce.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 3;
            gdtpc::CameraCollisionModel model(tuned);
            FakeCameraQuery query; query.scripted_distance = 8.0F;
            auto decision = model.decide_camera(query, 30.0F, 30.0F, 0.016F, true);
            require(decision.write, "the first obstruction did not write");
            near(decision.arm, 7.5F, "the runtime path did not shorten the arm");
            auto writes = 0;
            for (auto index = 0; index < 6; ++index)
            {
                query.scripted_distance = index % 2 == 0 ? infinity : 8.0F;
                decision = model.decide_camera(query, 30.0F, decision.arm, 0.016F, true);
                near(decision.arm, 7.5F, "chatter moved the arm on the runtime path");
                if (decision.write) ++writes;
            }
            require(writes == 0, "chatter produced camera writes");
        }

        // The player's zoom is read from the engine's zoom target, which nothing in this runtime
        // writes, so it cannot be contaminated by our own collision writes. These cases pin that
        // property against the three ways the previous design lost it.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 1;
            gdtpc::CameraCollisionModel model(tuned);
            FakeCameraQuery query; query.scripted_distance = 8.0F;

            // Obstructed, then ineligible for a long time with the camera frozen on our value, then
            // eligible again. Live, this replaced a zoom of 12 with the shortened 4.83 for good.
            auto decision = model.decide_camera(query, 42.0F, 42.0F, 0.016F, true);
            near(decision.arm, 7.5F, "the arm did not shorten");
            near(decision.desired, 42.0F, "the player's zoom was not taken from the engine target");
            for (auto frame = 0; frame < 60; ++frame)
                decision = model.decide_camera(query, 42.0F, 7.5F, 0.016F, false);
            near(decision.desired, 42.0F, "ineligibility disturbed the player's zoom");
            for (auto frame = 0; frame < 60; ++frame)
                decision = model.decide_camera(query, 42.0F, 7.5F, 0.016F, true);
            near(decision.desired, 42.0F, "our own held distance displaced the player's zoom");
            near(decision.arm, 7.5F, "the arm did not stay against the obstruction");

            // An abandoned override cannot disturb it either.
            model.abandon_override();
            decision = model.decide_camera(query, 42.0F, 7.5F, 0.016F, true);
            near(decision.desired, 42.0F, "abandoning the override disturbed the player's zoom");
        }

        // A full obstruct-and-release cycle with the camera trailing every commanded value. The
        // previous design dragged the player's zoom inward a fraction on each such cycle.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 1;
            gdtpc::CameraCollisionModel model(tuned);
            FakeCameraQuery query; query.scripted_distance = 8.0F;
            auto camera_distance = 42.0F;
            auto decision = run_camera(model, query, camera_distance, 42.0F, 40, 0.0F, 0.3F);
            near(decision.desired, 42.0F, "the player's zoom moved while obstructed");
            near(decision.arm, 7.5F, "the arm did not shorten");

            query.scripted_distance = infinity;
            decision = run_camera(model, query, camera_distance, 42.0F, 400, 0.0F, 0.3F);
            near(decision.desired, 42.0F, "the player's zoom moved during release");
            near(decision.arm, 42.0F, "the arm did not return to the player's zoom");
            require(decision.state == gdtpc::CollisionState::idle,
                "the model did not hand the camera back to the game");
            require(!decision.write, "the model kept writing after releasing the camera");
        }

        // The player scrolling IS respected, immediately, because the target they write is the very
        // value the model reads.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 1;
            gdtpc::CameraCollisionModel model(tuned);
            FakeCameraQuery query; query.scripted_distance = 8.0F;
            auto decision = model.decide_camera(query, 42.0F, 42.0F, 0.016F, true);
            near(decision.desired, 42.0F, "the player's zoom was not read");
            decision = model.decide_camera(query, 20.0F, 7.5F, 0.016F, true);
            near(decision.desired, 20.0F, "a scroll while obstructed was ignored");
        }

        // Holding the camera in against geometry takes continuous force. The engine animates its own
        // zoom back toward the player's target every frame, so a camera written once escapes: in the
        // third live session the arm was held at 4.52 while the camera slid out to 8.42 over 48
        // frames, was snapped back by a single write, and repeated. That sawtooth is the bounce.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 1;
            gdtpc::CameraCollisionModel model(tuned);
            FakeCameraQuery query; query.scripted_distance = 8.0F;
            auto camera_distance = 12.0F;
            auto decision = model.decide_camera(query, 12.0F, camera_distance, 0.016F, true);
            near(decision.arm, 7.5F, "the arm did not shorten");
            require(decision.write, "the obstruction did not write");
            camera_distance = decision.arm;

            auto worst_escape = 0.0F;
            for (auto frame = 0; frame < 120; ++frame)
            {
                camera_distance += 0.08F; // the engine pulling back toward the player's zoom
                decision = model.decide_camera(query, 12.0F, camera_distance, 0.016F, true);
                near(decision.arm, 7.5F, "the held arm moved");
                if (decision.write) camera_distance = decision.arm;
                worst_escape = std::max(worst_escape, camera_distance - decision.arm);
            }
            // Without re-assertion the camera reaches the far side of the wall before anything
            // corrects it. The whole point is that it is never allowed to get far.
            require(worst_escape <= 0.2F, "the camera was allowed to slide away from the held arm");
        }

        // The decision reports being shortened and reports needing a write as two INDEPENDENT facts.
        // A caller that infers "the obstruction cleared" from "no write this frame" is wrong, because
        // writes are suppressed while an unchanged arm holds an unchanged obstruction, which is most
        // frames at a wall. The runtime made exactly that mistake and it silently disabled the held
        // target method, so the distinction is pinned here.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 1;
            gdtpc::CameraCollisionModel model(tuned);
            FakeCameraQuery query; query.scripted_distance = 8.0F;
            auto decision = model.decide_camera(query, 42.0F, 42.0F, 0.016F, true);
            require(decision.write, "the first obstruction did not write");
            require(decision.state == gdtpc::CollisionState::shortened, "the arm was not reported shortened");

            // The camera is exactly where we put it and the obstruction has not moved, so there is
            // nothing to write, yet the model is still very much holding the camera in.
            auto suppressed = 0;
            for (auto frame = 0; frame < 30; ++frame)
            {
                decision = model.decide_camera(query, 42.0F, decision.arm, 0.016F, true);
                require(decision.state == gdtpc::CollisionState::shortened,
                    "a suppressed write was reported as no longer shortened");
                if (!decision.write) ++suppressed;
            }
            require(suppressed > 0, "no writes were suppressed, so the distinction was never exercised");

            // Only a genuine clearance changes the state, and that is the signal a caller must use.
            query.scripted_distance = infinity;
            for (auto frame = 0; frame < 400 && decision.state == gdtpc::CollisionState::shortened; ++frame)
                decision = model.decide_camera(query, 42.0F, decision.arm, 0.016F, true);
            require(decision.state == gdtpc::CollisionState::idle,
                "clearing the obstruction did not change the reported state");
        }

        // An obstruction that RECEDES, which is what walking away from a wall looks like, must let the
        // arm follow it smoothly. Confirming every small outward step advanced the target only once per
        // confirmation run and stalled the arm for a mean of 8.6 frames at a time live.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 3; tuned.query_interval = 1;
            tuned.release_immediate_units = 1.0F;
            gdtpc::CameraCollisionModel model(tuned);
            FakeCameraQuery query; query.scripted_distance = 8.0F;
            auto decision = model.decide_camera(query, 42.0F, 42.0F, 0.016F, true);
            near(decision.arm, 7.5F, "the arm did not shorten");

            // The wall recedes a fifth of a unit per query. Every query is a small outward move.
            auto advanced = 0;
            auto previous = decision.arm;
            for (auto step = 1; step <= 20; ++step)
            {
                query.scripted_distance = 8.0F + 0.2F * static_cast<float>(step);
                decision = model.decide_camera(query, 42.0F, decision.arm, 0.016F, true);
                if (decision.arm > previous + 0.0001F) ++advanced;
                previous = decision.arm;
            }
            // Without the immediate step this advances on roughly one query in three.
            require(advanced >= 18, "a receding obstruction still stalled the arm");
        }

        // A large outward jump is still chatter and must still be confirmed, or the bounce comes back.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 3; tuned.query_interval = 1;
            tuned.release_immediate_units = 1.0F;
            gdtpc::CameraCollisionModel model(tuned);
            FakeCameraQuery query; query.scripted_distance = 8.0F;
            auto decision = model.decide_camera(query, 42.0F, 42.0F, 0.016F, true);
            near(decision.arm, 7.5F, "the arm did not shorten");

            // Alternating near hit and clean miss: the arm must not move at all.
            for (auto index = 0; index < 8; ++index)
            {
                query.scripted_distance = index % 2 == 0 ? infinity : 8.0F;
                decision = model.decide_camera(query, 42.0F, decision.arm, 0.016F, true);
                near(decision.arm, 7.5F, "hit/miss chatter moved the arm");
            }
            require(decision.state == gdtpc::CollisionState::shortened,
                "chatter released the arm back to the game");
        }

        // Moving to another shoulder forces an immediate ray even between normal query slots, but
        // it must preserve the shortened arm and the accepted outward-release confirmation.
        {
            auto tuned = settings(); tuned.release_confirm_queries = 3; tuned.query_interval = 30;
            tuned.release_immediate_units = 1.0F;
            gdtpc::CameraCollisionModel model(tuned);
            FakeCameraQuery query; query.scripted_distance = 8.0F;
            auto decision = model.decide_camera(query, 42.0F, 42.0F, 0.016F, true);
            near(decision.arm, 7.5F, "the force-query setup did not shorten the arm");
            require(query.calls == 1, "the setup ray count was wrong");
            query.scripted_distance = infinity;
            model.force_query_next();
            decision = model.decide_camera(query, 42.0F, decision.arm, 0.016F, true);
            require(query.calls == 2, "a shoulder change did not force the next collision query");
            near(decision.arm, 7.5F, "a forced clear query discarded the shortened arm");
        }

        // The threshold is validated by the same predicate the model uses.
        {
            auto bad = settings(); bad.release_immediate_units = -1.0F;
            require(!gdtpc::valid_collision_settings(bad), "a negative immediate release step was accepted");
            bad = settings(); bad.release_immediate_units = quiet_nan;
            require(!gdtpc::valid_collision_settings(bad), "a nonfinite immediate release step was accepted");
            bad = settings(); bad.release_immediate_units = 50.0F;
            require(!gdtpc::valid_collision_settings(bad), "an oversized immediate release step was accepted");
            // Zero is legal and restores the previous behaviour: everything outward is confirmed.
            bad = settings(); bad.release_immediate_units = 0.0F;
            require(gdtpc::valid_collision_settings(bad), "a zero immediate release step was rejected");
        }

        // The shoulder overlay has explicit custody: it cycles only on rising edges, follows yaw,
        // composes a fresh native value, and removes only a value it can prove it still owns.
        {
            gdtpc::ShoulderOffsetModel model;
            const gdtpc::CollisionVec3 zero{};
            auto decision = model.step(zero, 0.0F, 1.5F, 0.0F, true, false);
            require(!decision.write && decision.side == gdtpc::ShoulderSide::center,
                "the shoulder model did not start centered");

            decision = model.step(zero, 0.0F, 1.5F, 0.0F, true, true);
            require(decision.write && decision.side_changed &&
                decision.side == gdtpc::ShoulderSide::right,
                "the first F9 edge did not select the right shoulder");
            near(decision.value.x, 1.5F, "the right shoulder did not move laterally");
            near(decision.translation.x, 1.5F, "the first ray translation was wrong");
            const auto right = decision.value;

            decision = model.step(right, 0.0F, 1.5F, 0.0F, true, true);
            require(!decision.write && !decision.side_changed && model.edge_count() == 1,
                "a held F9 key generated another shoulder edge");
            static_cast<void>(model.step(right, 0.0F, 1.5F, 0.0F, true, false));
            decision = model.step(right, 0.0F, 1.5F, 0.0F, true, true);
            require(decision.write && decision.side == gdtpc::ShoulderSide::left,
                "the second F9 edge did not select the left shoulder");
            near(decision.value.x, -1.5F, "the left shoulder displacement was wrong");
            near(decision.translation.x, -3.0F, "the right-to-left ray translation was wrong");

            // A changed field is native state, not our stale write. Preserve it and add the active
            // overlay; do not subtract an overlay the engine has already replaced.
            const gdtpc::CollisionVec3 native{0.2F, 0.1F, 0.3F};
            decision = model.step(native, 0.0F, 1.5F, 0.0F, true, false);
            near(decision.value.x, -1.3F, "a native offset overwrite was not composed");
            near(decision.value.y, 0.1F, "the native vertical offset was replaced");
            near(decision.value.z, 0.3F, "the native depth offset was replaced");
            const auto composed_native = decision.value;
            decision = model.relinquish(composed_native);
            require(decision.write && decision.value == native,
                "relinquishment did not restore the exact saved native base");

            // Yaw rotates the lateral vector in the horizontal plane.
            model.reset();
            decision = model.step(zero, 1.57079632679F, 1.5F, 0.0F, true, true);
            near(decision.value.x, 0.0F, "yaw did not rotate shoulder X", 0.0001F);
            near(decision.value.z, -1.5F, "yaw did not rotate shoulder Z", 0.0001F);

            // Relinquishment peels our overlay from a value we still own. If native code already
            // replaced the field, it leaves that replacement untouched.
            decision = model.relinquish(decision.value);
            require(decision.write, "owned shoulder displacement was not removed");
            near(decision.value.x, 0.0F, "owned shoulder X was not restored");
            near(decision.value.z, 0.0F, "owned shoulder Z was not restored");
            model.reset();
            decision = model.step(zero, 0.0F, 1.5F, 0.0F, true, true);
            decision = model.relinquish(native);
            require(!decision.write, "a native overwrite was clobbered during relinquishment");
        }

        // Height lifts the framing target on every side, center included, and composes with the
        // lateral shoulder vector. It must leave the side cycle, collision forcing and exact restore
        // behaving exactly as they do for the lateral overlay alone.
        {
            gdtpc::ShoulderOffsetModel model;
            const gdtpc::CollisionVec3 zero{};
            auto decision = model.step(zero, 0.0F, 3.0F, 2.0F, true, false);
            require(decision.write && decision.side == gdtpc::ShoulderSide::center && !decision.side_changed,
                "center with a height did not lift the target");
            require(decision.force_query, "acquiring the height overlay did not force a collision query");
            near(decision.value.x, 0.0F, "center height moved the target laterally");
            near(decision.value.y, 2.0F, "center height was not applied");
            near(decision.translation.y, 2.0F, "the height ray translation was wrong");
            auto held = decision.value;

            decision = model.step(held, 0.7F, 3.0F, 2.0F, true, false);
            require(!decision.write && !decision.force_query,
                "a steady center height rewrote or forced a query while rotating");

            decision = model.step(held, 0.0F, 3.0F, 2.0F, true, true);
            require(decision.write && decision.force_query && decision.side == gdtpc::ShoulderSide::right,
                "the first F9 edge with a height did not select the right shoulder");
            near(decision.value.x, 3.0F, "the right shoulder with height was wrong");
            near(decision.value.y, 2.0F, "the side change dropped the height");
            near(decision.translation.x, 3.0F, "the side change translation was wrong");
            near(decision.translation.y, 0.0F, "the side change translated vertically");
            held = decision.value;

            // Yaw drift while on a shoulder is a small translation, not a jump: no forced query, so
            // the collision release streak is not reset on every rotating frame.
            decision = model.step(held, 0.01F, 3.0F, 2.0F, true, false);
            require(decision.write && !decision.force_query, "rotating on a shoulder forced a query");
            held = decision.value;

            // Back to center (right -> left -> center) keeps the height and only removes the side.
            static_cast<void>(model.step(held, 0.01F, 3.0F, 2.0F, true, false));
            decision = model.step(held, 0.01F, 3.0F, 2.0F, true, true);
            held = decision.value;
            static_cast<void>(model.step(held, 0.01F, 3.0F, 2.0F, true, false));
            decision = model.step(held, 0.01F, 3.0F, 2.0F, true, true);
            require(decision.side == gdtpc::ShoulderSide::center && decision.write && decision.force_query,
                "returning to center did not recompose");
            near(decision.value.x, 0.0F, "center kept a lateral offset", 0.0001F);
            near(decision.value.z, 0.0F, "center kept a lateral offset", 0.0001F);
            near(decision.value.y, 2.0F, "center dropped the height");
            held = decision.value;

            // Losing eligibility restores the exact native base, bit for bit, and forces a query.
            decision = model.step(held, 0.01F, 3.0F, 2.0F, false, false);
            require(decision.write && decision.force_query && decision.value == zero,
                "ineligibility did not restore the exact base under a height overlay");

            // Height composes with a nonzero native vertical base and restores it exactly.
            model.reset();
            const gdtpc::CollisionVec3 native{0.125F, 0.3F, -0.2F};
            decision = model.step(native, 0.0F, 3.0F, 2.0F, true, false);
            near(decision.value.y, 2.3F, "height did not compose with the native vertical base");
            decision = model.relinquish(decision.value);
            require(decision.write && decision.value == native,
                "height relinquishment did not restore the exact native base");

            // Zero height at center owns nothing, preserving the accepted lateral-only behaviour.
            model.reset();
            decision = model.step(native, 0.0F, 3.0F, 0.0F, true, false);
            require(!decision.write && !decision.force_query, "zero height at center wrote the target");
            decision = model.step(native, 0.0F, 3.0F, quiet_nan, true, false);
            require(!decision.write, "a NaN height was written");
            decision = model.step(native, 0.0F, 3.0F, -1.0F, true, false);
            require(!decision.write, "a negative height was written");
        }

        std::cout << "PASS: spring-arm settings validation, miss sentinel handling, normalized bounded ray, "
                     "skin and minimum clamping, NaN and negative rejection, fault latching across sessions, "
                     "immediate pull-in with rate-limited extension, timestep guards, ineligibility, "
                     "nonfinite-geometry refusal, query throttling, an uncontaminated player zoom taken "
                     "from the engine's own zoom target across overrides, ineligibility and abandonment, "
                     "release confirmation against hit/miss chatter with smooth following of a receding obstruction, write re-assertion against the "
                     "engine's zoom animation, write suppression on a static obstruction, release without "
                     "a parting write, a shortened state independent of write suppression, lazy camera-derived queries, native-setter fault latching, "
                     "and owned left/center/right shoulder-offset composition and restoration.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
