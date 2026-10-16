/**
 * @file core/collision/probe_types.hpp
 * @brief The data the camera probes pass around: a physics hit, a sightline, what a fan of rays met, the objects the
 *        walk met and the camera fades, and what a probe does with a hit.
 *
 * @details Engine-free (no Windows or DetourModKit header), so the local tests compile it as is. Engine pointers are
 *          carried as opaque addresses and are never read here.
 */
#ifndef TPVCAMERA_CORE_COLLISION_PROBE_TYPES_HPP
#define TPVCAMERA_CORE_COLLISION_PROBE_TYPES_HPP

#include "core/math/vec.hpp"
#include "core/rules/camera_rules.hpp"

#include <cstdint>
#include <optional>

namespace TPVCamera
{
    /**
     * @struct RayHit
     * @brief Decoded subset of the engine ray_hit for a single nearest hit.
     */
    struct RayHit
    {
        /// World distance from the ray origin to the hit.
        float m_distance{0.0f};
        /// World-space hit position.
        Vector3 m_point{};
        /// Surface normal at the hit.
        Vector3 m_normal{};
        /// IPhysicalEntity* of the entity hit.
        uintptr_t m_collider{0};
        /**
         * @brief Non-zero when the hit is the global TERRAIN heightmap (ray_hit.bTerrain).
         * @details Zero for a brush. This is the engine's own ground flag, used
         *          to always block the terrain so the camera cannot clip under the world.
         */
        int m_terrain{0};
        /// The geometry part of the collider hit (ray_hit.ipart).
        int m_part{0};
        /// The mapped surface type of the hit (ray_hit.surface_idx).
        int m_surface{0};
    };

    /// The rays of ray_fan_sweep(): its centre and four offsets by the radius.
    inline constexpr int k_fan_rays = 5;

    /**
     * @brief What each ray of a fan met, kept across the casts of one walk along one sweep (see ray_fan_sweep()).
     * @details A walk casts the fan again after it steps past a hit, from the same start with one more collider skipped
     *          or from a little past the hit. A ray that met nothing meets nothing on any later part of its line, and a
     *          ray whose first hit lies past the new start on a collider still not skipped meets that same hit, so only
     *          the other rays are cast again. Distances are measured from the walk's start.
     */
    struct FanMemory
    {
        bool cast[k_fan_rays]{};               // the ray has been cast in this walk
        std::optional<RayHit> hit[k_fan_rays]; // its first hit then, or none along the rest of the sweep
    };

    /** @brief A world-space sightline from the camera to a point on the character. */
    struct Sightline
    {
        Vector3 from{};
        Vector3 to{};
    };

    /** @brief A node whose render mesh hides the character from the camera. */
    struct SightlineOccluder
    {
        void *node = nullptr;
        // The camera is buried in it (a prop or a bush): it fills the view and starts fading at once.
        bool buried = false;
    };

    /** @brief A render node the camera fades this frame, how it was found, and whether the camera is in it. */
    struct FadeEntry
    {
        void *node = nullptr;
        char source = '?';   // 'a' a prop the arm stepped past, 's' on a sightline
        bool buried = false; // the sightline test found the camera buried in it (a prop or a bush)
        bool inside = false; // the arm walk found the camera inside its physics
    };

    /** @brief One hit the camera walk met along the arm, for the walk trace. */
    struct WalkStep
    {
        uintptr_t collider = 0;
        void *node = nullptr; // the scanned render node it matched, or nullptr
        float distance = 0.0f;
        Rules::Behaviour cls = Rules::Behaviour::Collide;
        // 'p' stepped past, 'c' the collision, 't' terrain, 'r' re-hit and walked past, 'x' re-hit too often, 'u' a
        // structure's collision with nothing drawn there, walked past, 'g' a collider that matches nothing drawn
        // while part of the fan is clear, walked past (UseBeamGate)
        char what = '?';
    };

    /** @brief What a camera probe does with a physics hit on its way along a line. */
    enum class ProbeVerdict : std::uint8_t
    {
        Stop,        // terrain, a structure where it draws something, or anything that matches no scanned node
        Undrawn,     // a structure's collision with nothing drawn there (a tent's canvas): walked past
        PassThrough, // a prop, a frame or a plant: the arm steps past it, the lens near plane cuts it
    };
} // namespace TPVCamera

#endif // TPVCAMERA_CORE_COLLISION_PROBE_TYPES_HPP
