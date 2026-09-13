#pragma once

// Offline spring-arm model for camera collision. Deliberately free of Win32 and of any engine call,
// exactly like camera_write_adapter: the live adapter supplies a LevelQuery, and every decision the
// arm makes is provable against a fake one.
//
// Engine facts this model encodes, all established by research and live observation and recorded in
// IMPLEMENTATION_PLAN.md:
//  - GAME::Ray is { float origin[3]; float direction[3]; } with a NORMALIZED direction.
//  - A miss writes +infinity into Intersection.distance, so a miss reads as "no obstruction".
//  - The engine's own hit test treats a NaN distance as a hit. This model must not inherit that.

#include <cstdint>

namespace gdtpc
{
struct CollisionVec3
{
    float x{}, y{}, z{};
    friend bool operator==(const CollisionVec3&, const CollisionVec3&) = default;
};

enum class ShoulderSide : std::uint32_t { center, right, left };

struct ShoulderOffsetDecision
{
    CollisionVec3 value{};       // complete value to store at GameCamera+0x55c
    CollisionVec3 translation{}; // change from the offset used by the current camera transform
    ShoulderSide side{ShoulderSide::center};
    bool write{};
    bool side_changed{};
    // The camera jumped rather than drifted with yaw: a side change, or the overlay being acquired or
    // released. The collision ray must be re-queried from the new origin on this frame.
    bool force_query{};
};

// Pure ownership model for GameCamera's target offset. A value exactly equal to our last write still
// contains our overlay and is recomposed from the saved pre-overlay base; a different value is
// adopted as fresh native state. Saving the base is load-bearing: subtracting floats did not restore
// exact zero in the first live candidate.
//
// `units` is the lateral shoulder displacement, applied only on the right/left sides. `height` lifts
// the framing target on every side, center included, so the character sits lower on screen and more
// of the ground ahead is visible. With center and zero height the model owns nothing.
class ShoulderOffsetModel final
{
public:
    [[nodiscard]] ShoulderOffsetDecision step(CollisionVec3 current, float yaw, float units, float height,
        bool eligible, bool cycle_key_down) noexcept;
    [[nodiscard]] ShoulderOffsetDecision relinquish(CollisionVec3 current) noexcept;
    void reset() noexcept;
    [[nodiscard]] ShoulderSide side() const noexcept { return side_; }
    [[nodiscard]] std::uint64_t edge_count() const noexcept { return edge_count_; }

private:
    ShoulderSide side_{ShoulderSide::center};
    CollisionVec3 last_base_{};
    CollisionVec3 last_written_{};
    bool owns_{};
    bool key_down_{};
    std::uint64_t edge_count_{};
};

// Mirrors GAME::Ray field for field so the live adapter can copy it straight through.
struct CollisionRay
{
    CollisionVec3 origin{};
    CollisionVec3 direction{}; // unit length
};

// The only thing the live adapter must implement. Returning false means the query could not be
// performed at all, which is a fault; returning true with +infinity means a clean miss.
class LevelQuery
{
public:
    virtual ~LevelQuery() = default;
    [[nodiscard]] virtual bool raycast(const CollisionRay& ray, float max_distance, float& distance) noexcept = 0;
};

// Live camera queries derive their own engine-convention ray at the instant the throttled query is
// due. This keeps CalculateViewPosition, GetRegion, GetLevelPtr and GetIntersection in one coherent
// operation and avoids caching any world object between callbacks.
class CameraLevelQuery
{
public:
    virtual ~CameraLevelQuery() = default;
    [[nodiscard]] virtual bool raycast_from_camera(float max_distance, float& distance,
        CollisionRay& ray) noexcept = 0;
};

struct CollisionSettings
{
    float skin{0.35F};                 // stop short of the surface by this much
    float minimum_distance{2.0F};      // never pull closer than this, even against a wall
    float extend_units_per_second{18.0F}; // easing back out; pulling in is immediate
    std::uint32_t query_interval{3};   // run the ray every Nth eligible frame
    // Outward moves must be confirmed by this many consecutive queries before the arm is allowed to
    // follow them. At an obstruction boundary the ray chatters between hit and miss from frame to
    // frame; releasing on the first clear sample turns that chatter into a visible bounce.
    std::uint32_t release_confirm_queries{3};
    // An outward move no larger than this is applied at once, without waiting to be confirmed.
    //
    // Confirmation exists to reject hit/miss CHATTER, which is a large alternation: a wall at 5 units
    // one query and nothing at 42 the next. Walking away from a wall is not that. The obstruction
    // recedes smoothly, so every query is a small outward move, and confirming each one advanced the
    // target only once every release_confirm_queries queries. Live, that stalled the arm for a mean of
    // 8.6 frames at a time while extending, which is the chop the player reported. A spurious small
    // step is harmless: it moves the camera a fraction of a unit and the next hit pulls it back.
    float release_immediate_units{1.0F};
    std::uint32_t fault_limit{4};      // latch off after this many failed or invalid queries
};

[[nodiscard]] bool valid_collision_settings(const CollisionSettings& settings) noexcept;

enum class CollisionState : std::uint32_t
{
    idle,        // eligible, nothing obstructing
    shortened,   // arm is being held in by geometry
    ineligible,  // not third person, or identity not validated this frame
    latched_off  // too many faults; assistance is permanently disabled for the session
};

// What the camera callback should do this frame. `write` is the only instruction the runtime acts
// on: when false it must leave the camera entirely alone, so an unobstructed camera is governed by
// the game's own zoom exactly as it is today.
struct CollisionDecision
{
    float arm{};         // distance to apply through the native zoom setter
    float desired{};     // the player's own chosen zoom, tracked while unobstructed
    bool write{};        // call SetZoom(arm) this frame
    CollisionState state{CollisionState::ineligible};
};

// Consumes the desired arm length and returns the length to actually use. The result is never longer
// than desired and never shorter than minimum_distance, so a caller that ignores collision entirely
// still gets a usable value.
class CameraCollisionModel final
{
public:
    explicit CameraCollisionModel(CollisionSettings settings) noexcept;

    [[nodiscard]] float step(LevelQuery& query, const CollisionVec3& focus, const CollisionVec3& toward_camera,
        float desired_distance, float delta_seconds, bool eligible) noexcept;

    // Full decision including the player-zoom arbitration. `current_distance` is the camera's
    // actual zoom this frame, which is the player's own choice whenever collision is not overriding.
    // `player_distance` is the player's OWN chosen zoom, read from the engine's zoom target, which
    // nothing in this runtime ever writes. `observed_distance` is where the camera actually is this
    // frame. They are separate arguments because conflating them is what let collision writes be
    // mistaken for player intent and ratchet the zoom onto the character.
    [[nodiscard]] CollisionDecision decide(LevelQuery& query, const CollisionVec3& focus,
        const CollisionVec3& toward_camera, float player_distance, float observed_distance,
        float delta_seconds, bool eligible) noexcept;

    // Runtime path: geometry is obtained lazily from the camera only when the throttled query is
    // due, so no engine function is called on intervening frames.
    [[nodiscard]] CollisionDecision decide_camera(CameraLevelQuery& query, float player_distance,
        float observed_distance, float delta_seconds, bool eligible) noexcept;

    // The decision is made before the native setter is called. A refused/faulting SetZoom must be
    // fed back so it is retried only up to the same session fault limit rather than silently lost.
    void record_write_result(bool success) noexcept;

    // Relinquish the camera mid-session, for example when identity or foreground changes between the
    // decision and the write. The engine's own zoom target is still the player's choice, so letting
    // go is all that is needed: the engine animates the camera home by itself.
    void abandon_override() noexcept;

    void reset_session() noexcept; // new session: forget the arm, keep the fault latch
    // A lateral camera move changes the ray origin, so the next callback must query even when the
    // normal throttle is not due. Preserve the current arm: forgetting it would allow a clear ray
    // on the new shoulder to snap immediately outward instead of using the accepted release policy.
    void force_query_next() noexcept { since_query_ = settings_.query_interval; clear_streak_ = 0; }

    [[nodiscard]] CollisionState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t query_count() const noexcept { return query_count_; }
    [[nodiscard]] std::uint64_t fault_count() const noexcept { return fault_count_; }
    [[nodiscard]] std::uint64_t hit_count() const noexcept { return hit_count_; }
    [[nodiscard]] float arm() const noexcept { return arm_; }
    [[nodiscard]] std::uint32_t clear_streak() const noexcept { return clear_streak_; }
    // The player's own zoom as the model currently understands it. Published in telemetry, because a
    // wrong value here is invisible in the arm alone and was the defect in the second live session.
    [[nodiscard]] float desired() const noexcept { return desired_; }

private:
    void record_fault() noexcept;
    // Applies one query result to the target. Inward moves take effect at once; outward moves are
    // held until release_confirm_queries consecutive queries agree, and then take the most
    // conservative (nearest) value seen across that run.
    void apply_query_target(float candidate) noexcept;
    [[nodiscard]] CollisionDecision finish_decision(float arm, float observed, bool observed_valid) noexcept;
    [[nodiscard]] float step_camera(CameraLevelQuery& query, float desired_distance,
        float delta_seconds, bool eligible) noexcept;

    CollisionSettings settings_{};
    CollisionState state_{CollisionState::ineligible};
    float arm_{0.0F};
    float target_{0.0F};
    bool has_arm_{false};
    std::uint32_t since_query_{0};
    std::uint32_t clear_streak_{0};  // consecutive queries agreeing the arm may extend
    float pending_target_{0.0F};     // nearest outward candidate seen during the current streak
    float desired_{0.0F};        // the player's chosen zoom, read from the engine's own zoom target
    bool overriding_{false};
    float last_written_{0.0F};
    bool has_written_{false};
    bool retry_write_{false};
    std::uint64_t query_count_{}, fault_count_{}, hit_count_{};
};
}
