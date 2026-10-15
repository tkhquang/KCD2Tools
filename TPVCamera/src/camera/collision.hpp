/**
 * @file camera/collision.hpp
 * @brief Camera collision along the arm from the pivot to the camera, and the occluder fade set it feeds.
 *
 * @details The arm runs from the pivot (a safe point inside the player) to where the rig puts the camera. Each object
 *          the arm meets is classed by its camera rule (see Rules::Behaviour). Terrain, buildings and their wall, door and
 *          window pieces, walls, rocks, and anything that matches no scanned render node stop the camera: it pulls in
 *          at once to just short of the surface and eases back out once the obstruction clears, so the camera stays
 *          inside the room the player stands in. A prop (a fence, a cart, a lamp), a bush or a tree is stepped past
 *          instead when UseOccluderFade is on, and a prop dithers out, so the camera keeps its distance rather than
 *          jumping in front of each prop and back. RWI_OBJTYPES_CAMERA excludes living entities, so the player and
 *          NPCs never stop the camera. The follow distance is carried in CameraState::collision_distance. With nothing
 *          in the way it follows the desired distance on the same frame, so a zoom stays on the view axis.
 *
 *          The fade keeps the head, the chest and the hips visible: the props whose mesh crosses a sightline from the
 *          camera to those points fade, along with the ones the arm steps past. A tree, a bush or a grass patch never
 *          fades whole for hiding the character (the tree see-through opens a window in it instead); only a prop or a
 *          bush the camera is buried in fades, and it starts at once. A building corner that hides part of the
 *          character is left as it is.
 */
#ifndef TPVCAMERA_CAMERA_COLLISION_HPP
#define TPVCAMERA_CAMERA_COLLISION_HPP

#include "camera/collision_trace.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "math_utils.hpp"
#include "physics_raycast.hpp"
#include "query_ledger.hpp"
#include "render_occlusion.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace TPVCamera
{
    // The least gap kept between the camera and a surface (meters): the game's own near clip distance. A pull-in that
    // lags its target never puts the lens on or past the surface that stopped it, and the lens near plane (see
    // k_lens_near_max) is held this far short of the ground or a building.
    constexpr float k_lens_near_margin = 0.05f;

    /** @brief What a camera probe does with a physics hit on its way along a line. */
    enum class ProbeVerdict : std::uint8_t
    {
        Stop,        // terrain, a structure where it draws something, or anything that matches no scanned node
        Undrawn,     // a structure's collision with nothing drawn there (a tent's canvas): walked past
        PassThrough, // a prop, a frame or a plant: the arm steps past it, the lens near plane cuts it
    };

    /**
     * @brief Classes a physics hit for a camera probe: the arm walk and the lens clearance share this rule.
     * @details The hit's collider is matched to its scanned render node (through its foreign link or, for baked level
     *          physics, its world box). A man-made structure stops a probe only where it draws something: one short
     *          line per probe line, COLLISION_DRAWN_REACH either side of the hit, is tested against its drawn mesh,
     *          and with none of it there the hit is on collision that draws nothing (the canvas walls and roof of a
     *          tent model that draws only its frame). Anything drawn there that one of the lines meets, a beam, a rail
     *          or a plank, keeps the stop. Main thread only (uses the occluder scan).
     * @param along Distance of the hit along each line from its start.
     * @param line_starts The start of each probe line (at most 9): the arm's five, or one lens ray.
     * @param dir The lines' common unit direction.
     * @param node Receives the matched render node, or nullptr.
     * @param cls Receives the matched node's class (Collide when there is none).
     */
    [[nodiscard]] ProbeVerdict classify_probe_hit(QueryLedger::Tag tag, const RayHit &hit, float along,
                                                  std::span<const Vector3> line_starts, const Vector3 &dir, void **node,
                                                  Rules::Behaviour *cls);

    /**
     * @brief The collision and occluder fade carry-over between camera frames.
     * @note Main thread only (the camera detour). One instance, owned by the camera frame.
     */
    struct CollisionState
    {
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

    /**
     * @brief Runs the camera collision along the arm and refreshes the fade set.
     * @details With EnableCollision on, the arm's result is reused while the pivot and the desired camera stay within
     *          k_collision_recompute_dist of where it was computed, for at most k_collision_max_age_ms of real time; a
     *          recompute walks the arm with the ray fan, stops at a drawn
     *          structure mesh the physics misses, holds a pull-in for k_collision_hold_seconds, eases the distance and
     *          casts the fade's sightlines. With it off, the distance eases out to the desired one like after any
     *          obstruction (no jump), and the objects in front of the character still fade. Main thread only.
     * @return The camera position after collision.
     */
    [[nodiscard]] Vector3 resolve_camera_collision(CameraState &cam, CollisionState &state, const CollisionInput &in);

    /**
     * @brief Hands the fade set to the dissolve hook and logs it when it changes.
     * @details With the fade off the set is empty, so whatever is still faded fades back in. Each object fades until
     *          only OccluderFadeMinOpacity of it stays, a ghost of where it stands. Main thread only, once per frame
     *          (the frame's second build changes nothing).
     */
    void submit_fade_set(const CollisionState &state, float delta_time);

    /**
     * @brief Drops the collision ease's carry-over, so the next collided frame starts afresh instead of easing across a
     *        gap.
     */
    void reset_collision_carry(CameraState &cam) noexcept;
} // namespace TPVCamera

#endif // TPVCAMERA_CAMERA_COLLISION_HPP
