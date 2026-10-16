/**
 * @file core/collision/arm.hpp
 * @brief Camera collision along the arm from the pivot to the camera, the shoulder collision ahead of it, and the
 *        occluder fade set they feed.
 *
 * @details The arm runs from the pivot (a safe point inside the player) to where the rig puts the camera. Each object
 *          the arm meets is classed by its camera rule (see Rules::Behaviour). Terrain, buildings and their wall, door
 *          and window pieces, walls, rocks, and anything that matches no scanned render node stop the camera: it pulls
 *          in at once to just short of the surface and eases back out once the obstruction clears, so the camera stays
 *          inside the room the player stands in. A prop (a fence, a cart, a lamp), a bush or a tree is stepped past
 *          instead when UseOccluderFade is on, and a prop dithers out, so the camera keeps its distance rather than
 *          jumping in front of each prop and back. RWI_OBJTYPES_CAMERA excludes living entities, so the player and
 *          NPCs never stop the camera. The follow distance is carried in CollisionState::arm. With nothing in the way
 *          it follows the desired distance on the same frame, so a zoom stays on the view axis.
 *
 *          The fade keeps the head, the chest and the hips visible: the props whose mesh crosses a sightline from the
 *          camera to those points fade, along with the ones the arm steps past. A tree, a bush or a grass patch never
 *          fades whole for hiding the character (the tree see-through opens a window in it instead); only a prop or a
 *          bush the camera is buried in fades, and it starts at once. A building corner that hides part of the
 *          character is left as it is.
 *
 *          Engine-free (no Windows or DetourModKit header): the world is asked through a WorldProbe, the trace lines
 *          go through a CollisionTrace, and the INI and preset values come in as CollisionParams, so a recorded frame
 *          replays exactly (tests/replay). camera/collision.hpp runs it on the live probe.
 */
#ifndef TPVCAMERA_CORE_COLLISION_ARM_HPP
#define TPVCAMERA_CORE_COLLISION_ARM_HPP

#include "constants.hpp"
#include "core/blend/ease.hpp"
#include "core/collision/probe_types.hpp"
#include "core/collision/world_probe.hpp"
#include "core/math/vec.hpp"
#include "core/rules/camera_rules.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace TPVCamera
{
    // The least gap kept between the camera and a surface (meters): the game's own near clip distance. A pull-in that
    // lags its target never puts the lens on or past the surface that stopped it, and the lens near plane (see
    // k_lens_near_max) is held this far short of the ground or a building.
    constexpr float k_lens_near_margin = 0.05f;

    /**
     * @brief The arm's ease: how far out the collision allows the camera, eased toward its target, and the hold.
     * @details The four collision_* fields are ease_collision_toward()'s. collision_valid is cleared whenever the
     *          offset is suppressed (menu, overlay, view switch), so the next applied frame snaps instead of easing
     *          across a suppression gap.
     */
    struct CollisionEase
    {
        // The allowed follow distance after the collision raycast, eased so the view pulls in instantly but returns
        // out smoothly. collision_desired is the uncollided distance that frame, so the shortfall left by an
        // obstruction can ease out while the desired distance itself moves (a zoom).
        bool collision_valid = false;
        float collision_distance = 0.0f;
        float collision_desired = 0.0f;
        // How fast the collision distance is easing back out (m/s, outward positive); any pull-in zeroes it, so
        // every release starts from rest.
        float collision_speed = 0.0f;
        // Seconds the pulled-in distance is still held after the last recompute that met a surface (drained on every
        // frame): a probe that grazes an edge and misses it the next frame, or an arm swung fast through clutter,
        // keeps the camera in instead of letting it ease out and snap back in.
        float hold_timer = 0.0f;
    };

    /**
     * @brief The shoulder collision's carry-over: how far the shoulder offset is allowed out (see
     *        resolve_shoulder_collision()).
     * @details The four collision_* fields are the ease's own (ease_collision_toward() runs on them as on the arm's),
     *          measured along the shoulder offset from the pivot instead of along the arm.
     */
    struct ShoulderCollision
    {
        bool collision_valid = false;
        float collision_distance = 0.0f;
        float collision_desired = 0.0f;
        float collision_speed = 0.0f;
        // Seconds a pull-in is still held after the last probe that met a surface, as the arm's hold.
        float hold_timer = 0.0f;
        // The throttle: the last probe's pivot and shoulder point, and the offset it allowed, reused while neither
        // moved by a recompute step, for at most the arm's recompute age.
        bool throttle_valid = false;
        Vector3 throttle_pivot{};
        Vector3 throttle_focal{};
        float cached_allowed = 0.0f;
        bool cached_held_in = false;
        std::uint64_t stamp_ms = 0;
        // What the trace last named: the surface the probe stopped at, or none.
        bool traced_blocked = false;
        void *traced_node = nullptr;
    };

    /**
     * @brief The collision and occluder fade carry-over between camera frames.
     * @note Main thread only (the camera frame). One instance, owned by the camera frame.
     */
    struct CollisionState
    {
        // The arm's ease and hold.
        CollisionEase arm{};
        // The shoulder collision ahead of the arm (Shoulder Collision in the preset).
        ShoulderCollision shoulder{};

        // The render nodes faded this frame. Refreshed on every collision recompute and held in between, since the
        // throttle reuses the whole arm while nothing moved. The arm walk's entries come first (arm_fade_count of
        // them); the sightline entries follow.
        std::array<FadeEntry, Constants::OCCLUDER_FADE_MAX_NODES> fade_nodes{};
        int fade_count = 0;
        int arm_fade_count = 0;
        // Set when the camera leaves third person: the next third-person frame drops the fade lists it kept, so
        // nothing from before the gap is faded again.
        bool fade_stale = false;

        // The props on a sightline from the camera to the character, tested against their render meshes (a prop's
        // cloth carries no physics), and the props and bushes the camera is buried in. The set is refreshed at most
        // every k_fade_sightline_period, or as soon as the camera moved k_fade_sightline_move since the last refresh;
        // in between the last set is reused, which the fade's hold bridges. The mesh test is the costly part of the
        // camera frame, so this keeps it to a few runs a second.
        std::array<SightlineOccluder, Constants::OCCLUDER_FADE_MAX_NODES> sight_nodes{};
        int sight_count = 0;
        float sight_age = 1e9f;
        Vector3 sight_cam{};

        // The throttle: the last recomputed arm (its pivot, its desired camera, the allowed distance it found and
        // whether that held the camera in), reused while nothing moved (see resolve_camera_collision).
        bool throttle_valid = false;
        bool throttle_fade_on = false;
        Vector3 throttle_pivot{};
        Vector3 throttle_cam{};
        float cached_allowed = 0.0f;
        bool cached_held_in = false;
        std::uint64_t stamp_ms = 0;

        // What the trace last named: whether the physics hit blocked and at what distance, and the drawn mesh the
        // arm stopped at, so each line is written only on a change.
        bool traced_blocked = false;
        float traced_distance = -1.0f;
        void *traced_mesh = nullptr;

        // Where the collision put the camera on the last frame: an eased pull-in (CollisionPullInSeconds) may trail
        // its target only along a path through open air from here. What the pull-in trace last named.
        Vector3 last_camera{};
        std::uint8_t pull_in_traced = 0;
    };

    /** @brief The INI and preset values the collision reads, taken once per frame. */
    struct CollisionParams
    {
        bool enable = true;            // the preset's Enable Collision
        float skin = 0.2f;             // the preset's Collision Skin
        float return_speed = 6.0f;     // the preset's Collision Return Speed
        float pull_in_seconds = 0.12f; // CollisionPullInSeconds
        float radius = 0.15f;          // CollisionRadius: the probes' half-width (the fan's ray offset)
        bool beam_gate = false;        // UseBeamGate
    };

    /** @brief One camera frame's input to the collision. */
    struct CollisionInput
    {
        Vector3 pivot{};   // the arm's start: a safe point inside the player
        Vector3 desired{}; // where the rig puts the camera, before collision
        Vector3 world_up{};
        // The fade's sightline targets: the head and the hips. The chest lies halfway between them.
        Vector3 head{};
        Vector3 hip{};
        float delta_time = 0.0f;
        bool fade_on = false;            // UseOccluderFade with the dissolve hook available
        bool wall_class_changed = false; // advance_fence_classification() changed a class this frame
        bool repeat_build = false;       // the frame's second build
    };

    /** @brief One camera frame's input to the shoulder collision. */
    struct ShoulderInput
    {
        Vector3 pivot{}; // the arm's start, on the body's column
        // Unit direction of the shoulder offset: the rig's right, or its left for a negative offset.
        Vector3 direction{};
        float lateral = 0.0f; // the shoulder offset's length, meters
        float delta_time = 0.0f;
        bool enabled = false; // Shoulder Collision and Enable Collision are both on in the preset
        bool fade_on = false; // UseOccluderFade with the dissolve hook available
    };

    /** @brief What the pull-in trace last named. */
    enum class PullInTrace : std::uint8_t
    {
        Caught,   // the camera does not trail its target by more than the immediate pull-in's slack
        Trailing, // an eased pull-in trails its target through open air
        Blocked,  // an eased pull-in's path was not open, so it pulled in at once
    };

    /**
     * @brief Where the collision's trace lines go. Each is called only on a change of what it names (the collision
     *        keeps that memory in CollisionState), and an implementation may drop it below its log level.
     */
    class CollisionTrace
    {
    public:
        CollisionTrace() = default;
        CollisionTrace(const CollisionTrace &) = delete;
        CollisionTrace &operator=(const CollisionTrace &) = delete;
        CollisionTrace(CollisionTrace &&) = delete;
        CollisionTrace &operator=(CollisionTrace &&) = delete;
        virtual ~CollisionTrace() = default;

        /** @brief One step of the arm's ease moved the camera this way (every frame). */
        virtual void motion(EaseMotion motion) noexcept = 0;
        /** @brief What the walk along the arm met (every recompute; the sink logs a change). */
        virtual void walk(std::span<const WalkStep> steps, float desired_distance) noexcept = 0;
        /** @brief The arm's physics hit blocks, newly or @p surface moved by more than 0.1 m. */
        virtual void physics_hit(const RayHit &hit, float surface, float desired_distance, int stepped,
                                 const Vector3 &camera, const Vector3 &pivot) noexcept = 0;
        /** @brief The arm stops at the drawn mesh of @p node at @p at meters, or (nullptr) at none any more. */
        virtual void arm_mesh(void *node, float at, float desired_distance) noexcept = 0;
        /** @brief An eased pull-in trails, caught up, or found its path blocked. */
        virtual void pull_in(PullInTrace what, float behind) noexcept = 0;
        /** @brief The shoulder ray stops at @p distance on @p node (nullptr: terrain or unmatched), or is clear. */
        virtual void shoulder(bool blocked, float distance, void *node, Rules::Behaviour cls,
                              float lateral) noexcept = 0;
    };

    /**
     * @brief Classes a physics hit for a camera probe: the arm walk and the lens clearance share this rule.
     * @details The hit's collider is matched to its scanned render node (through its foreign link or, for baked level
     *          physics, its world box). A man-made structure stops a probe only where it draws something: one short
     *          line per probe line, COLLISION_DRAWN_REACH either side of the hit, is tested against its drawn mesh,
     *          and with none of it there the hit is on collision that draws nothing (the canvas walls and roof of a
     *          tent model that draws only its frame). Anything drawn there that one of the lines meets, a beam, a rail
     *          or a plank, keeps the stop.
     * @param along Distance of the hit along each line from its start.
     * @param line_starts The start of each probe line (at most 9): the arm's five, or one lens ray.
     * @param dir The lines' common unit direction.
     * @param node Receives the matched render node, or nullptr.
     * @param cls Receives the matched node's class (Collide when there is none).
     */
    [[nodiscard]] ProbeVerdict classify_probe_hit(WorldProbe &probe, QueryLedger::Tag tag, const RayHit &hit,
                                                  float along, std::span<const Vector3> line_starts, const Vector3 &dir,
                                                  void **node, Rules::Behaviour *cls);

    /**
     * @brief How far out the shoulder offset is allowed: the shoulder collision of the over-the-shoulder rig.
     * @details One ray runs from the pivot out along the shoulder offset, CollisionSkin past it, and walks what it
     *          meets as the arm does (classify_probe_hit()): terrain, a structure where it draws something and
     *          anything that matches no scanned node stop it, a structure's undrawn collision is walked past, and a
     *          prop or a plant is stepped past while the occluder fade is on. A stop pulls the offset in to
     *          CollisionSkin short of the surface, held for the arm's hold time, and it eases back out at Collision
     *          Return Speed. The arm then runs from the pivot to the camera the narrower offset places, so a wall at the
     *          shoulder moves the camera toward the look line instead of pulling it in along the diagonal arm, and it
     *          keeps its distance. The ray's result is reused while the pivot and the shoulder point move less than a
     *          recompute step. Turned off, a pulled-in offset eases back out, and once out the offset is left exactly
     *          as the rig made it.
     * @return The shoulder offset's allowed length, at most @p in.lateral.
     */
    [[nodiscard]] float resolve_shoulder_collision(CollisionState &state, const ShoulderInput &in,
                                                   const CollisionParams &params, WorldProbe &probe,
                                                   CollisionTrace &trace);

    /**
     * @brief Runs the camera collision along the arm and refreshes the fade set.
     * @details With Enable Collision on, the arm's result is reused while the pivot and the desired camera stay within
     *          k_collision_recompute_dist of where it was computed, for at most k_collision_max_age_ms of real time; a
     *          recompute walks the arm with the ray fan, stops at a drawn structure mesh the physics misses, holds a
     *          pull-in for k_collision_hold_seconds, eases the distance and casts the fade's sightlines. With it off,
     *          the distance eases out to the desired one like after any obstruction (no jump), and the objects in
     *          front of the character still fade.
     * @return The camera position after collision.
     */
    [[nodiscard]] Vector3 resolve_camera_collision(CollisionState &state, const CollisionInput &in,
                                                   const CollisionParams &params, WorldProbe &probe,
                                                   CollisionTrace &trace);

    /**
     * @brief Drops the arm ease's carry-over, so the next collided frame starts afresh instead of easing across a gap.
     */
    void reset_collision_carry(CollisionState &state) noexcept;

    /** @brief Drops the shoulder collision's carry-over, so the next third-person frame probes afresh. */
    void reset_shoulder_carry(CollisionState &state) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_COLLISION_ARM_HPP
