/**
 * @file hooks/archery_hook.cpp
 * @brief Third-person arrows and distraction stones aimed at the crosshair, the arrow flight trail and the aim preview.
 *
 * @details The player's shot runs C_ActorActionShootingFiring's "firearrow" event -> the fire routine -> the bow
 *          joint's position and axis (wh_w_AimByProjectile, on by default) -> FireProjectile(actor, dir, pos, speed)
 *          -> CProjectile::Launch(pos, dir, velocity = dir * speed, 1). The shot never reads the camera, so with the
 *          over-the-shoulder camera the arrow misses the screen center by the shoulder parallax, and by more the
 *          further the target sits from the camera's convergence depth.
 *
 *          Three threads meet here. Each third-person frame with a missile weapon or a distraction stone in hand, the
 *          main-thread frame step casts the crosshair ray from the player's depth with the arrow's own collision
 *          filter and publishes the point it hits through a seqlock. The shot runs on the thread that updates its
 *          fire action (a job worker, as Mannequin actions run there). The FireProjectile detour marks that thread
 *          when the shooter is the player, and the Launch detour on the same thread re-aims the shot at the point
 *          from the exact launch point Launch receives. The Launch detour hands the main thread a shot record through
 *          a lock-free ring, and the arrow collision handler reports the impact the same way. The main thread follows
 *          each shot's projectile, logs the measured miss and draws the trail.
 *
 *          The arrow is a physics particle: Launch orients the entity from dir and flies it on velocity *
 *          speedScale, with the ammo's gravity and air resistance (Arrow.xml: 9.0 and 0). The re-aimed shot keeps the
 *          game's launch point and speed, as a bow shot in first person does, and only its direction changes. The
 *          draw sets how far it flies, and gravity sets how far it drops below the point. The game applies no spread
 *          to the player's shot, and the bow sway lives in the look the camera follows, so the crosshair carries the
 *          sway and the re-aimed shot keeps it. The bow sits beside the camera, so a shot that comes down short of
 *          the point (or falls from the open sky onto something nearer) is turned about the vertical until its traced
 *          arc lands in the camera's vertical plane: straight below the crosshair on screen, never beside it. First
 *          person, free-look, shotgun pellets and corrections past MaxAimCorrection keep the game's own shot. The
 *          player's distraction stone is re-aimed the same way from where it leaves the hand: the game throws it
 *          along the look from beside the eye, which in third person lands it beside the crosshair.
 *
 *          The game gives every ammo the same gravity, and only the launch speed differs. The player's shots drop more
 *          or less per weapon: FireProjectile's ammo item names the ammo class (arrow, bolt, ball or scatter shot), and
 *          after Launch the particle takes the scaled gravity through the same SetParams call Launch sets its velocity
 *          with. Every arrow, bolt and firearm shot, the player's and the NPCs', draws from one projectile pool, and
 *          neither the pool re-init nor Launch sets gravity. Once a scaled gravity applied, each Launch therefore
 *          writes the launched projectile's own gravity back after the original: the scaled one for the player's
 *          shot, the ammo template's for every other.
 *
 *          While a bow is drawn or a stone is held ready, the aim preview draws where the shot comes down: a ring on
 *          the spot and an optional dashed arc. It predicts the shot the Launch detour flies, re-aimed or the game's
 *          own, from where the earlier shots with the weapon family left the bow and how fast they flew.
 */

#include "archery_hook.hpp"
#include "aob_resolver.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "game_state.hpp"
#include "game_structures.hpp"
#include "global_state.hpp"
#include "offset_heal.hpp"
#include "physics_raycast.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace TPVCamera
{
    namespace
    {
        // FireProjectile(utils, actor, id, dir, nock desc, pos, weapon, ammo, speed, single_shot). single_shot is 0
        // for each pellet of a shotgun-type weapon and 1 for an arrow or bolt.
        using FireProjectileFunc = std::uintptr_t(__fastcall *)(std::uintptr_t utils, std::uintptr_t actor, int id,
                                                                const float *dir, std::uintptr_t desc, float *pos,
                                                                std::uintptr_t weapon, std::uintptr_t ammo, float speed,
                                                                std::uint8_t single_shot);
        // CProjectile::Launch.
        using LaunchFunc = void(__fastcall *)(std::uintptr_t projectile, const float *pos, const float *dir,
                                              const float *velocity, float speed_scale);
        // CArrow's collision handler. Returns true when it handed the arrow back to the pool.
        using ArrowCollisionFunc = std::uint8_t(__fastcall *)(std::uintptr_t arrow, std::uintptr_t collision);
        // IEntity::GetPhysics.
        using GetPhysicsFunc = std::uintptr_t(__fastcall *)(std::uintptr_t entity);
        // IPhysicalEntity::SetParams(params, bThreadSafe). Returns 0 when the entity rejects the parameters.
        using SetParamsFunc = int(__fastcall *)(std::uintptr_t entity, const void *params, int thread_safe);

        /**
         * @brief The engine's plain 3-float Vec3. Aux-geometry vertex arrays, physics parameter blocks and the Launch
         *        arguments take it as is, so its layout must stay the engine's.
         */
        struct Vec3f
        {
            float x;
            float y;
            float z;
        };
        static_assert(std::is_standard_layout_v<Vec3f> && sizeof(Vec3f) == 3 * sizeof(float));

        /** @brief Engine ColorB. */
        struct AuxColor
        {
            std::uint8_t r;
            std::uint8_t g;
            std::uint8_t b;
            std::uint8_t a;
        };

        /** @brief Engine SAuxGeomRenderFlags (a packed u32). */
        struct AuxFlags
        {
            std::uint32_t value;
        };

        using GetAuxFunc = std::uintptr_t(__fastcall *)(std::uintptr_t renderer);
        using SetFlagsFunc = AuxFlags *(__fastcall *)(std::uintptr_t aux, AuxFlags *previous, const AuxFlags *flags);
        using DrawLinesFunc = void(__fastcall *)(std::uintptr_t aux, const Vec3f *vertices, std::uint32_t count,
                                                 const AuxColor *color, float thickness);

        /** @brief How a predicted flight is drawn: its line thickness multiple and its least ring radius (meters). */
        struct PreviewStyle
        {
            float line_scale{1.0f};
            float min_ring{0.0f};
        };

        /**
         * @brief pe_params_particle (physinterface.h layout), what IPhysicalEntity::SetParams takes for a particle.
         * @details The physics applies only the fields without its "unused" marker: a float or Vec3 by the bit
         *          pattern of its (first) float, an int by its value, the collider to ignore by an all-ones pointer.
         */
        struct ParticleParams
        {
            int type;
            std::uint32_t flags;
            float mass;
            float size;
            float thickness;
            Vec3f heading;
            float velocity;
            float air_resistance;
            float water_resistance;
            float accel_thrust;
            float accel_lift;
            int surface_index;
            Vec3f spin;
            Vec3f gravity;
            Vec3f water_gravity;
            Vec3f normal;
            Vec3f roll_axis;
            // The quaternion v.xyz, w. The w component carries the marker.
            float orientation[4];
            float min_bounce_speed;
            float min_speed;
            std::uintptr_t collider_to_ignore;
            int pierceability;
            int collide_types;
            int area_check_period;
            int no_hit_effect;
        };
        static_assert(sizeof(ParticleParams) == 0xA8);
        static_assert(offsetof(ParticleParams, air_resistance) == Constants::PARTICLE_PARAMS_AIR_RESISTANCE_OFFSET);
        static_assert(offsetof(ParticleParams, gravity) == Constants::PARTICLE_PARAMS_GRAVITY_OFFSET);
        static_assert(offsetof(ParticleParams, pierceability) == Constants::PARTICLE_PARAMS_PIERCEABILITY_OFFSET);

        /** @brief The weapon family a shot belongs to, by its ammo class. */
        enum class AmmoKind : std::uint8_t
        {
            Unknown,
            Arrow,
            Bolt,
            Firearm,
        };

        /** @brief The log name of @p kind. */
        [[nodiscard]] const char *ammo_text(AmmoKind kind) noexcept
        {
            switch (kind)
            {
            case AmmoKind::Arrow:
                return "arrow";
            case AmmoKind::Bolt:
                return "bolt";
            case AmmoKind::Firearm:
                return "firearm shot";
            case AmmoKind::Unknown:
            default:
                return "unknown ammo";
            }
        }

        /** @brief What became of a shot's gravity scale. */
        enum class GravityChange : std::uint8_t
        {
            // Scale 1, or ammo of no known weapon: the game's gravity.
            None,
            // To be set once Launch has run.
            Pending,
            Applied,
            // The particle did not take it: the game's gravity.
            Refused,
        };

        /** @brief The log phrase for @p change. */
        [[nodiscard]] const char *gravity_text(GravityChange change) noexcept
        {
            switch (change)
            {
            case GravityChange::Applied:
                return "scaled";
            case GravityChange::Refused:
                return "scale not applied, the arrow's physics refused it";
            case GravityChange::Pending:
            case GravityChange::None:
            default:
                return "the game's";
            }
        }

        // A crosshair target older than this is not used for a shot (the shot then keeps the game's aim).
        constexpr std::uint64_t k_target_max_age_ms = 500;
        // The most a weapon's gravity is scaled by.
        constexpr float k_max_gravity_scale = 4.0f;
        // MaxAimCorrection is held to this many degrees: a re-aim past a right angle turns the shot backward.
        constexpr float k_max_aim_correction_cap = 90.0f;
        // A Launch velocity (m/s) at or below this has no direction to re-aim from.
        constexpr float k_min_launch_velocity = 1e-3f;
        // The bounds an ammo template's gravity (m/s2), air resistance and pierceability must fall within, else the
        // engine default stands in. Pierceability is the 4-bit pierceability field of the ray flags.
        constexpr float k_max_gravity = 100.0f;
        constexpr float k_max_drag = 10.0f;
        constexpr int k_max_pierceability = 15;
        // The foreign ids (EPhysicsForeignIds) the impact log names besides Constants::PHYS_FOREIGN_ID_STATIC.
        constexpr int PHYS_FOREIGN_ID_TERRAIN = 0;
        constexpr int PHYS_FOREIGN_ID_FOLIAGE = 3;
        // A seqlock read retries this often while a write is under way, then fails closed.
        constexpr int k_seqlock_attempts = 4;
        // The landing trace: chord length (meters), how long and over how many chords it follows the arc, how close to
        // the camera's vertical plane (meters) the landing has to come, how many sideways turns it can take, and the
        // slowest launch (m/s) it traces.
        constexpr float k_trace_chord = 4.0f;
        constexpr float k_trace_max_seconds = 8.0f;
        constexpr int k_trace_max_chords = 160;
        constexpr float k_landing_tolerance = 0.01f;
        constexpr int k_landing_turns = 3;
        constexpr float k_min_trace_speed = 1.0f;
        // A target closer to the launch point than this keeps the game's aim, and a landing turn stops when the level
        // reach to its goal is shorter than this: the direction to either is ill-defined.
        constexpr float k_min_target_distance = 1.5f;
        // A flight is followed at most this long (Arrow.xml lifetime is 15 s).
        constexpr std::uint64_t k_max_flight_ms = 20000;
        // A new trail sample is kept only after the arrow moved this far (meters).
        constexpr float k_sample_spacing = 0.05f;
        constexpr std::size_t k_ring = 8;
        constexpr std::size_t k_max_samples = 600;
        // ArrowTrailSeconds is held to this, which keeps the trail's keep time far inside the millisecond clock.
        constexpr float k_max_trail_seconds = 3600.0f;
        // The crosshair target log: one line when the target changes (another object, or this share nearer or
        // farther), at most once per interval.
        constexpr float k_target_log_change_share = 0.05f;
        constexpr std::uint64_t k_target_log_interval_ms = 1000;
        // The trail: solid lines that fade out over the last part of ArrowTrailSeconds, a plus at the crosshair point,
        // a ring where the arrow stopped and a small plus where it left the bow. A marker's half size is its distance
        // from the release camera times the marker scale, held to the bounds (meters), times the marker's share.
        constexpr float k_trail_thickness = 2.0f;
        constexpr float k_marker_thickness = 2.0f;
        constexpr float k_trail_fade_share = 0.4f;
        constexpr float k_marker_scale = 0.012f;
        constexpr float k_marker_min_half = 0.05f;
        constexpr float k_marker_max_half = 1.5f;
        constexpr float k_target_marker_share = 0.7f;
        constexpr float k_impact_marker_share = 0.6f;
        constexpr float k_launch_marker_share = 0.35f;
        // Orange: the shot the mod re-aimed.
        constexpr AuxColor k_color_redirected{
            .r = 255,
            .g = 150,
            .b = 0,
            .a = 220,
        };
        // Cyan: a shot the game aimed on its own.
        constexpr AuxColor k_color_native{
            .r = 0,
            .g = 190,
            .b = 255,
            .a = 220,
        };
        // Green: the crosshair point at release.
        constexpr AuxColor k_color_target{
            .r = 40,
            .g = 255,
            .b = 60,
            .a = 230,
        };
        // Red: where the arrow stopped.
        constexpr AuxColor k_color_impact{
            .r = 255,
            .g = 60,
            .b = 60,
            .a = 240,
        };
        // Yellow: where the arrow left the bow.
        constexpr AuxColor k_color_launch{
            .r = 255,
            .g = 240,
            .b = 60,
            .a = 200,
        };
        // A ring is built from two axes across its normal: from world up, or from world x for a normal whose vertical
        // component reaches this cosine, where the cross product with up degenerates.
        constexpr float k_ring_vertical_cosine = 0.9f;
        constexpr int k_ring_segments = 32;
        // The aim preview: a thin dashed arc, faded in over the first meters from the bow so the character stays
        // clear, and a ring that lies on the spot the arrow comes down. The dash period and the ring size follow the
        // distance from the camera, so they read the same near and far. A sky shot fades out over the last share of
        // the sky length, and a dash below the least share is not drawn.
        constexpr float k_preview_thickness = 1.5f;
        constexpr float k_preview_ring_thickness = 2.0f;
        constexpr float k_preview_fade_in = 2.0f;
        constexpr float k_preview_dash_scale = 0.012f;
        // Per meter from the camera.
        constexpr float k_preview_dash_period = 2.5f * k_preview_dash_scale;
        constexpr float k_preview_min_dash_period = 0.3f;
        // The share of each period drawn.
        constexpr float k_preview_dash_on = 0.6f;
        constexpr float k_preview_sample = 0.25f;
        constexpr int k_preview_max_samples = 2400;
        constexpr float k_preview_sky_length = 150.0f;
        constexpr float k_preview_sky_fade_share = 0.4f;
        constexpr float k_preview_min_share = 0.02f;
        constexpr int k_preview_bands = 4;
        constexpr std::size_t k_preview_band_vertices = 256;
        // The landing ring: at most this radius (meters) unless the style's least ring is larger, an inner ring at
        // this share of it, and a lift off the surface by this share of the radius, at least the least lift.
        constexpr float k_preview_max_ring = 0.6f;
        constexpr float k_preview_inner_ring_share = 0.25f;
        constexpr float k_preview_ring_lift_share = 0.1f;
        constexpr float k_preview_min_ring_lift = 0.02f;
        // An arrow's preview at the base thickness with a small ring. A thrown stone comes down near and on the
        // ground, where the arrow preview's thin lines and small ring read as nothing, so its lines are thicker and
        // its ring is wider.
        constexpr PreviewStyle k_arrow_preview_style{
            .line_scale = 1.0f,
            .min_ring = 0.06f,
        };
        constexpr PreviewStyle k_decoy_preview_style{
            .line_scale = 1.6f,
            .min_ring = 0.2f,
        };
        // Before the first shot with a weapon teaches the preview, where the bow sits from the first-person eye
        // (right, up, forward, meters), the full-draw speed per weapon family (m/s, indexed by AmmoKind), and the
        // ammo's own gravity.
        constexpr Vec3f k_default_bow_offset{-0.2f, -0.15f, 0.55f};
        constexpr std::array<float, 4> k_default_speed = {
            72.0f,
            72.0f,
            118.0f,
            118.0f,
        };
        constexpr float k_default_gravity = 9.0f;
        // Before the first throw teaches it, where a distraction stone leaves the hand from where it was held ready, in
        // the look's level frame (right, forward, up, meters). A larger jump is not a release.
        constexpr Vec3f k_default_decoy_release{0.0f, 0.15f, 0.1f};
        constexpr float k_max_decoy_release = 0.6f;
        // Each throw moves the learned release this share of the way to what it showed, so one odd throw (a turn or a
        // step in the swing) does not throw the preview off.
        constexpr float k_decoy_release_blend = 0.35f;
        // The slowest and fastest launch speed (m/s) a distraction stone's ammo can carry.
        constexpr float k_min_decoy_speed = 1.0f;
        constexpr float k_max_decoy_speed = 200.0f;

        /** @brief Why a player shot was or was not re-aimed. */
        enum class ShotOutcome : std::uint8_t
        {
            Redirected,
            AimOff,
            NoTarget,
            FreeLook,
            Pellets,
            TooClose,
            TooFar,
            Faulted,
        };

        /** @brief The log phrase for @p outcome. */
        [[nodiscard]] const char *outcome_text(ShotOutcome outcome) noexcept
        {
            switch (outcome)
            {
            case ShotOutcome::Redirected:
                return "re-aimed at the crosshair";
            case ShotOutcome::AimOff:
                return "kept: AimAtCrosshair is off";
            case ShotOutcome::NoTarget:
                return "kept: no crosshair target (first person or no fresh target)";
            case ShotOutcome::FreeLook:
                return "kept: free-look orbit is active";
            case ShotOutcome::Pellets:
                return "kept: pellet of a spread weapon";
            case ShotOutcome::TooClose:
                return "kept: crosshair point too near the bow";
            case ShotOutcome::TooFar:
                return "kept: crosshair point beyond MaxAimCorrection from the bow's aim";
            case ShotOutcome::Faulted:
            default:
                return "kept: launch arguments could not be read or are not finite";
            }
        }

        /** @brief The crosshair target of one frame: the camera ray and the point it hits. */
        struct AimTarget
        {
            Vec3f camera{};
            Vec3f forward{};
            Vec3f point{};
            // The first-person eye the ray started beside. Where the bow sits does not move with the camera's zoom or
            // its collision, so the aim preview places the bow from it.
            Vec3f eye{};
            float distance{0.0f};
            bool hit{false};
            bool eye_valid{false};
            std::uint64_t stamp_ms{0};
        };

        /**
         * @brief Single-producer seqlock for the AimTarget.
         * @details The main-thread frame step writes it and the Launch detour reads it on the shot's thread, so a read
         *          must see one frame's target, never a camera from one frame and a point from the next. Same protocol
         *          as InteractionAimPose: an odd sequence marks a write in progress, and a read retries or fails
         *          closed.
         */
        class AimTargetChannel
        {
        public:
            /** @brief Publishes @p target as valid. Main thread only. */
            void store(const AimTarget &target) noexcept
            {
                const std::uint32_t seq = m_seq.load(std::memory_order_relaxed);
                m_seq.store(seq + 1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                const std::array<float, k_floats> values = {
                    target.camera.x,
                    target.camera.y,
                    target.camera.z,
                    target.forward.x,
                    target.forward.y,
                    target.forward.z,
                    target.point.x,
                    target.point.y,
                    target.point.z,
                    target.eye.x,
                    target.eye.y,
                    target.eye.z,
                    // The distance comes last, at index 12: load() decodes the floats by their index.
                    target.distance,
                };
                for (std::size_t i = 0; i < k_floats; ++i)
                {
                    m_values[i].store(values[i], std::memory_order_relaxed);
                }
                m_hit.store(target.hit, std::memory_order_relaxed);
                m_eye_valid.store(target.eye_valid, std::memory_order_relaxed);
                m_stamp.store(target.stamp_ms, std::memory_order_relaxed);
                m_valid.store(true, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                m_seq.store(seq + 2, std::memory_order_relaxed);
            }

            /**
             * @brief Copies the published target into @p out.
             * @return False when no target is published, or when no tear-free copy was read within the retry bound.
             *         @p out is then untouched.
             */
            [[nodiscard]] bool load(AimTarget &out) const noexcept
            {
                for (int attempt = 0; attempt < k_seqlock_attempts; ++attempt)
                {
                    const std::uint32_t before = m_seq.load(std::memory_order_acquire);
                    if (before & 1u)
                    {
                        continue;
                    }
                    std::array<float, k_floats> values{};
                    for (std::size_t i = 0; i < k_floats; ++i)
                    {
                        values[i] = m_values[i].load(std::memory_order_relaxed);
                    }
                    const bool hit = m_hit.load(std::memory_order_relaxed);
                    const bool eye_valid = m_eye_valid.load(std::memory_order_relaxed);
                    const std::uint64_t stamp = m_stamp.load(std::memory_order_relaxed);
                    const bool valid = m_valid.load(std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (m_seq.load(std::memory_order_relaxed) != before)
                    {
                        continue;
                    }
                    if (!valid)
                    {
                        return false;
                    }
                    out.camera = Vec3f{values[0], values[1], values[2]};
                    out.forward = Vec3f{values[3], values[4], values[5]};
                    out.point = Vec3f{values[6], values[7], values[8]};
                    out.eye = Vec3f{values[9], values[10], values[11]};
                    out.distance = values[12];
                    out.hit = hit;
                    out.eye_valid = eye_valid;
                    out.stamp_ms = stamp;
                    return true;
                }
                return false;
            }

            /** @brief Withdraws the published target. Main thread only. */
            void invalidate() noexcept
            {
                const std::uint32_t seq = m_seq.load(std::memory_order_relaxed);
                m_seq.store(seq + 1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                m_valid.store(false, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                m_seq.store(seq + 2, std::memory_order_relaxed);
            }

        private:
            static constexpr std::size_t k_floats = 13;
            std::atomic<std::uint32_t> m_seq{0};
            std::array<std::atomic<float>, k_floats> m_values{};
            std::atomic<bool> m_hit{false};
            std::atomic<bool> m_eye_valid{false};
            std::atomic<std::uint64_t> m_stamp{0};
            std::atomic<bool> m_valid{false};
        };

        /** @brief A distraction stone held ready on one previewed frame and the throw the preview predicted from it. */
        struct DecoyPose
        {
            std::uintptr_t decoy{0};
            // Where the stone sat, and the look yaw then (engine convention: atan2(-x, y)).
            Vec3f held{};
            float yaw{0.0f};
            // Where the preview had the throw start, the way it had it go, and where it had it land when it landed.
            Vec3f predicted_from{};
            Vec3f predicted_dir{};
            Vec3f predicted_landing{};
            bool landed{false};
        };

        /**
         * @brief Single-producer seqlock for the DecoyPose, with the AimTargetChannel protocol.
         * @details The frame step writes it and the stone's Launch reads it on the throw's thread, so a read sees one
         *          frame's held spot and prediction, never the held spot of one frame with the prediction of the next.
         */
        class DecoyChannel
        {
        public:
            /** @brief Publishes @p pose. Main thread only. */
            void store(const DecoyPose &pose) noexcept
            {
                const std::uint32_t seq = m_seq.load(std::memory_order_relaxed);
                m_seq.store(seq + 1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                const std::array<float, k_floats> values = {
                    pose.held.x,
                    pose.held.y,
                    pose.held.z,
                    pose.yaw,
                    pose.predicted_from.x,
                    pose.predicted_from.y,
                    pose.predicted_from.z,
                    pose.predicted_dir.x,
                    pose.predicted_dir.y,
                    pose.predicted_dir.z,
                    pose.predicted_landing.x,
                    pose.predicted_landing.y,
                    pose.predicted_landing.z,
                };
                for (std::size_t i = 0; i < k_floats; ++i)
                {
                    m_values[i].store(values[i], std::memory_order_relaxed);
                }
                m_decoy.store(pose.decoy, std::memory_order_relaxed);
                m_landed.store(pose.landed, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_release);
                m_seq.store(seq + 2, std::memory_order_relaxed);
            }

            /**
             * @brief Copies the published pose into @p out.
             * @return False when no tear-free copy was read within the retry bound. @p out is then untouched. Before
             *         the first store, the copy is a pose of no stone (decoy 0).
             */
            [[nodiscard]] bool load(DecoyPose &out) const noexcept
            {
                for (int attempt = 0; attempt < k_seqlock_attempts; ++attempt)
                {
                    const std::uint32_t before = m_seq.load(std::memory_order_acquire);
                    if (before & 1u)
                    {
                        continue;
                    }
                    std::array<float, k_floats> values{};
                    for (std::size_t i = 0; i < k_floats; ++i)
                    {
                        values[i] = m_values[i].load(std::memory_order_relaxed);
                    }
                    const std::uintptr_t decoy = m_decoy.load(std::memory_order_relaxed);
                    const bool landed = m_landed.load(std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (m_seq.load(std::memory_order_relaxed) != before)
                    {
                        continue;
                    }
                    out.decoy = decoy;
                    out.held = Vec3f{values[0], values[1], values[2]};
                    out.yaw = values[3];
                    out.predicted_from = Vec3f{values[4], values[5], values[6]};
                    out.predicted_dir = Vec3f{values[7], values[8], values[9]};
                    out.predicted_landing = Vec3f{values[10], values[11], values[12]};
                    out.landed = landed;
                    return true;
                }
                return false;
            }

        private:
            static constexpr std::size_t k_floats = 13;
            std::atomic<std::uint32_t> m_seq{0};
            std::array<std::atomic<float>, k_floats> m_values{};
            std::atomic<std::uintptr_t> m_decoy{0};
            std::atomic<bool> m_landed{false};
        };

        /** @brief One player shot as Launch saw it. */
        struct ShotRecord
        {
            std::uintptr_t projectile{0};
            std::uintptr_t vtable{0};
            Vec3f launch{};
            Vec3f native_dir{};
            Vec3f flown_dir{};
            float speed{0.0f};
            // The gravity the shot flies with, and the ammo's own.
            float gravity{0.0f};
            float native_gravity{0.0f};
            float gravity_scale{1.0f};
            GravityChange gravity_change{GravityChange::None};
            AmmoKind ammo{AmmoKind::Unknown};
            // The weapon C_Item that loosed it (FireProjectile's weapon).
            std::uintptr_t weapon{0};
            float air_resistance{0.0f};
            int pierceability{0};
            // Free-look was active, so the release camera did not look along the bow's aim.
            bool free_look{false};
            AimTarget target{};
            bool target_valid{false};
            float correction_deg{0.0f};
            // Where the shot's traced arc lands, how far that was off the camera's vertical plane before and after the
            // sideways turn (meters, + to the camera's right), the turn, and whether MaxAimCorrection refused it.
            bool landing_traced{false};
            Vec3f landing{};
            float landing_lateral_unturned{0.0f};
            float landing_lateral{0.0f};
            float landing_turn_deg{0.0f};
            bool landing_turn_refused{false};
            ShotOutcome outcome{ShotOutcome::Faulted};
            std::uint32_t thread_id{0};
            std::uint64_t launch_ms{0};
            // The s_tracked slot this shot holds, so the end of its flight never clears a newer shot of the same
            // pooled arrow.
            std::uint32_t track_slot{0};
        };

        /** @brief What an arrow hit: the other side of its collision. */
        struct HitObject
        {
            // iForeignData (EPhysicsForeignIds: terrain 0, static 1, entity 2, foliage 3).
            int foreign_id{-1};
            // pForeignData is a CEntity.
            bool entity{false};
            // The entity's name when it is one.
            char name[48]{};
        };

        /**
         * @brief One arrow impact as the collision handler saw it.
         * @details Arrows are pooled, so the projectile address alone can name two shots. The arrow's launch point
         *          (m_initial_pos, written by every Launch) tells which shot the impact belongs to.
         */
        struct ImpactRecord
        {
            std::uintptr_t projectile{0};
            Vec3f launch{};
            Vec3f point{};
            float flight_time{0.0f};
            HitObject object{};
        };

        /** @brief A followed shot (main thread only). */
        struct Flight
        {
            bool used{false};
            bool active{false};
            // The projectile showed its end flags. The flight ends on the next frame, which gives the impact the
            // collision hook publishes after the original handler returns one frame to arrive.
            bool end_seen{false};
            std::uint32_t serial{0};
            ShotRecord shot{};
            std::array<Vec3f, k_max_samples> points{};
            std::array<float, k_max_samples> times{};
            std::size_t count{0};
            bool impact{false};
            Vec3f impact_point{};
            HitObject impact_object{};
            float end_time{0.0f};
            std::uint64_t end_ms{0};
        };

        /**
         * @brief Lock-free ring that hands records from detours on any thread to the main thread.
         * @details A writer claims an index with fetch_add on the count, zeroes the slot's sequence, copies the record
         *          into the slot, then publishes index + 1 as the slot's sequence with release. The main thread copies
         *          a slot between two reads of its sequence and keeps the copy only when both read index + 1, so a
         *          copy torn by a writer that lapped the ring is dropped. A slot claimed but not yet written holds the
         *          main thread back until a later frame.
         */
        template <typename Record> class RecordRing
        {
        public:
            /** @brief Claims the next slot and publishes @p record in it. Callable from any thread, lock-free. */
            void push(const Record &record) noexcept
            {
                const std::uint32_t index = m_count.fetch_add(1, std::memory_order_relaxed);
                std::atomic<std::uint32_t> &sequence = m_sequence[index % k_ring];
                sequence.store(0, std::memory_order_relaxed);
                // Orders the zeroed sequence before the record bytes, so a copy that reads any of them reads a changed
                // sequence after it.
                std::atomic_thread_fence(std::memory_order_release);
                m_slots[index % k_ring] = record;
                sequence.store(index + 1, std::memory_order_release);
            }

            /** @brief True when a record was claimed since the last drain. Main thread only. */
            [[nodiscard]] bool pending() const noexcept { return m_count.load(std::memory_order_relaxed) != m_seen; }

            /**
             * @brief Hands each record published since the last drain to @p consume, oldest first. Main thread only.
             * @details A record that a writer lapped before or during its copy is skipped. A record whose write is
             *          under way ends the drain, and the next drain resumes at it.
             */
            template <typename Consume> void drain(Consume &&consume) noexcept
            {
                // Relaxed: the count only bounds the walk, and each slot's own sequence publishes its record.
                const std::uint32_t count = m_count.load(std::memory_order_relaxed);
                if (count - m_seen > k_ring)
                {
                    m_seen = count - static_cast<std::uint32_t>(k_ring);
                }
                while (m_seen != count)
                {
                    const std::uint32_t wanted = m_seen + 1;
                    const std::atomic<std::uint32_t> &sequence = m_sequence[m_seen % k_ring];
                    const std::uint32_t before = sequence.load(std::memory_order_acquire);
                    if (before != wanted)
                    {
                        // Zero or an older index: claimed but not yet written. A newer index: lapped.
                        if (before == 0 || static_cast<std::int32_t>(before - wanted) < 0)
                        {
                            return;
                        }
                        ++m_seen;
                        continue;
                    }
                    const Record record = m_slots[m_seen % k_ring];
                    std::atomic_thread_fence(std::memory_order_acquire);
                    const bool intact = sequence.load(std::memory_order_relaxed) == before;
                    ++m_seen;
                    if (intact)
                    {
                        consume(record);
                    }
                }
            }

        private:
            std::array<Record, k_ring> m_slots{};
            // The index + 1 of the record each slot holds, or 0 while it holds none.
            std::array<std::atomic<std::uint32_t>, k_ring> m_sequence{};
            std::atomic<std::uint32_t> m_count{0};
            // Main thread only.
            std::uint32_t m_seen{0};
        };

        /** @brief The direction and velocity a re-aimed Launch receives in place of the game's. */
        struct FlownLaunch
        {
            Vec3f dir{};
            Vec3f vel{};
        };

        /** @brief An ammo's flight physics: launch speed, gravity (m/s2, down), air resistance and pierceability. */
        struct FlightPhysics
        {
            float speed{0.0f};
            float gravity{Constants::PARTICLE_DEFAULT_GRAVITY};
            float drag{0.0f};
            int pierceability{Constants::ARROW_DEFAULT_PIERCEABILITY};
        };

        std::atomic<FireProjectileFunc> s_fire_original{nullptr};
        std::atomic<LaunchFunc> s_launch_original{nullptr};
        std::atomic<ArrowCollisionFunc> s_collision_original{nullptr};

        // Written by init before s_aim_ready (s_genv, s_get_physics_offset) or s_trail_ready (the aux functions) is
        // released. Every reader tests that flag with acquire first.
        std::uintptr_t s_genv = 0;
        std::ptrdiff_t s_get_physics_offset = 0;
        std::uintptr_t s_get_aux_fn = 0;
        std::uintptr_t s_set_flags_fn = 0;
        std::uintptr_t s_draw_lines_fn = 0;
        std::atomic<bool> s_aim_ready{false};
        std::atomic<bool> s_trail_ready{false};

        // The C_Player the frame step saw, whose physics the stone's re-aim skips, and its entity id, by which the
        // FireProjectile detour knows the player. Written by the frame step.
        std::atomic<std::uintptr_t> s_player{0};
        std::atomic<std::uint32_t> s_player_id{0};
        // The thread inside the player's FireProjectile (0 when none). Launch runs synchronously under it. Relaxed:
        // only the thread that stored its own id can match it, and that thread reads its own earlier stores of the
        // s_fire_* values below in program order.
        std::atomic<DWORD> s_fire_thread{0};
        std::atomic<bool> s_fire_single_shot{true};
        std::atomic<AmmoKind> s_fire_ammo{AmmoKind::Unknown};
        std::atomic<std::uintptr_t> s_fire_weapon{0};
        // The arrow pierceability of the last player shot, mirrored into the crosshair target ray.
        std::atomic<int> s_pierceability{Constants::ARROW_DEFAULT_PIERCEABILITY};
        // The shooter's own physics, read from the live shooter inside its FireProjectile. The landing trace skips it,
        // as the game's own fire ray does and the arrow's collision handler rejects the owner. The rider's horse is not
        // skipped: an arrow loosed into it hits it.
        std::atomic<std::uintptr_t> s_fire_skip{0};
        // Set by the first scaled gravity a shot took, and never cleared. While it is set, each Launch writes the
        // launched projectile's own gravity back. Relaxed: the flag orders no other data, and a pooled projectile
        // reaches a later Launch only after its flight ends in a later physics step, long after the store.
        std::atomic<bool> s_gravity_dirty{false};
        // The distraction stone held ready on the last previewed frame, armed for one learn by its own Launch. Relaxed:
        // s_decoy_pose carries the data, and this only arms the learn.
        std::atomic<std::uintptr_t> s_decoy_held{0};
        // The distraction stone in the player's hand on the last frame (held ready or mid-throw), which the Launch
        // detour re-aims at the crosshair. Written by the frame step. Relaxed: Launch only compares the address.
        std::atomic<std::uintptr_t> s_decoy_in_hand{0};
        // Where a throw leaves the hand from where the stone was held ready, in the look's level frame, as the throws
        // so far showed it. The stone's Launch writes it and the preview reads it. Each component is blended on its
        // own, so a read that mixes two throws stays within one blend step of either.
        std::atomic<float> s_decoy_release_right{k_default_decoy_release.x};
        std::atomic<float> s_decoy_release_forward{k_default_decoy_release.y};
        std::atomic<float> s_decoy_release_up{k_default_decoy_release.z};
        DecoyChannel s_decoy_pose;

        AimTargetChannel s_target;
        RecordRing<ShotRecord> s_shots;
        RecordRing<ImpactRecord> s_impacts;
        // The projectiles whose impact is wanted, so the collision detour ignores every other arrow. publish_shot
        // claims the slots round-robin and untrack clears one. Relaxed: the collision detour only compares the address.
        std::array<std::atomic<std::uintptr_t>, k_ring> s_tracked{};
        std::atomic<std::uint32_t> s_track_next{0};

        // Main thread only.
        std::array<Flight, k_ring> s_flights{};
        std::uint32_t s_next_serial = 1;
        std::array<Vec3f, 2 * k_max_samples + 8> s_vertices{};
        bool s_aux_faulted = false;

        /** @brief What the shots with one weapon family taught the aim preview (main thread only). */
        struct AimProfile
        {
            bool learned{false};
            // The launch point from the first-person eye: right, up, forward.
            Vec3f offset{};
            // The fastest launch seen with this weapon C_Item: a full draw. A partial draw never lowers it, and
            // another weapon starts it over.
            std::uintptr_t weapon{0};
            float speed{0.0f};
            float native_gravity{0.0f};
            float air_resistance{0.0f};
        };
        // Indexed by AmmoKind.
        std::array<AimProfile, 4> s_aim_profiles{};

        /** @brief @p v as a Vector3. */
        [[nodiscard]] Vector3 to_vector(const Vec3f &v) noexcept
        {
            return Vector3{v.x, v.y, v.z};
        }

        /** @brief @p v as the engine's plain Vec3. */
        [[nodiscard]] Vec3f to_vec3f(const Vector3 &v) noexcept
        {
            return Vec3f{v.x, v.y, v.z};
        }

        /** @brief Three floats read from engine memory as the engine's plain Vec3. */
        [[nodiscard]] Vec3f to_vec3f(const std::array<float, 3> &v) noexcept
        {
            return Vec3f{v[0], v[1], v[2]};
        }

        /** @brief The address of @p v as the float pointer an engine Vec3 parameter takes. */
        [[nodiscard]] const float *engine_ptr(const Vec3f &v) noexcept
        {
            return reinterpret_cast<const float *>(&v);
        }

        /** @brief The dot product of @p a and @p b. */
        [[nodiscard]] float dot(const Vector3 &a, const Vector3 &b) noexcept
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        /** @brief The angle between @p a and @p b in degrees, or 0 when either has no length. */
        [[nodiscard]] float angle_deg(const Vector3 &a, const Vector3 &b) noexcept
        {
            const float denom = a.magnitude() * b.magnitude();
            if (denom <= 1e-9f)
            {
                return 0.0f;
            }
            const float cosine = std::clamp(dot(a, b) / denom, -1.0f, 1.0f);
            return DMK::math::radians_to_degrees(std::acos(cosine));
        }

        /** @brief True when every component of @p v is finite. */
        [[nodiscard]] bool is_finite(const Vector3 &v) noexcept
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        /** @brief The camera's right and up axes for a view along @p forward (world up stays up). */
        [[nodiscard]] std::pair<Vector3, Vector3> view_axes(const Vector3 &forward) noexcept
        {
            Vector3 right = forward.cross(Vector3{0.0f, 0.0f, 1.0f});
            if (right.magnitude() < 1e-3f)
            {
                right = Vector3{1.0f, 0.0f, 0.0f};
            }
            right = right.normalized();
            return {right, right.cross(forward).normalized()};
        }

        /** @brief An ammo particle template's flight fields as stored, the physics "unused" NaN marker included. */
        struct AmmoTemplate
        {
            float gravity_z{0.0f};
            float air_resistance{0.0f};
            int pierceability{0};
        };

        /** @brief The SAmmoParams a projectile flies with (its ammo template), or 0 when it cannot be read. */
        [[nodiscard]] std::uintptr_t ammo_params_of(std::uintptr_t projectile) noexcept
        {
            const auto ammo =
                DMK::memory::read<std::uintptr_t>(DMK::Address{projectile + Constants::PROJECTILE_AMMO_PARAMS_OFFSET});
            return ammo && DMK::memory::is_plausible_ptr(DMK::Address{*ammo}) ? *ammo : 0;
        }

        /** @brief Reads the particle template of the SAmmoParams at @p ammo, or nothing when it cannot be read. */
        [[nodiscard]] std::optional<AmmoTemplate> read_ammo_template(std::uintptr_t ammo) noexcept
        {
            constexpr std::array<std::ptrdiff_t, 2> chain = {
                Constants::AMMO_PARTICLE_PARAMS_OFFSET,
                0,
            };
            const auto particle = DMK::memory::walk(DMK::Address{ammo}, chain);
            if (!particle)
            {
                return std::nullopt;
            }
            const auto gravity =
                DMK::memory::read<std::array<float, 3>>(particle->offset(Constants::PARTICLE_PARAMS_GRAVITY_OFFSET));
            const auto air_resistance =
                DMK::memory::read<float>(particle->offset(Constants::PARTICLE_PARAMS_AIR_RESISTANCE_OFFSET));
            const auto pierceability =
                DMK::memory::read<int>(particle->offset(Constants::PARTICLE_PARAMS_PIERCEABILITY_OFFSET));
            if (!gravity || !air_resistance || !pierceability)
            {
                return std::nullopt;
            }
            return AmmoTemplate{(*gravity)[2], *air_resistance, *pierceability};
        }

        /**
         * @brief The flight physics an ammo template gives, each field checked on its own.
         * @details The engine default stands in for an unreadable template, for a field that carries the physics
         *          "unused" NaN marker (the template leaves it to the engine), and for a value out of bounds. The
         *          comparisons reject the NaN marker. The returned speed is 0.
         */
        [[nodiscard]] FlightPhysics checked_physics(const std::optional<AmmoTemplate> &raw) noexcept
        {
            FlightPhysics physics{};
            if (!raw)
            {
                return physics;
            }
            const float gravity = -raw->gravity_z;
            if (gravity > 0.0f && gravity < k_max_gravity)
            {
                physics.gravity = gravity;
            }
            if (raw->air_resistance >= 0.0f && raw->air_resistance < k_max_drag)
            {
                physics.drag = raw->air_resistance;
            }
            if (raw->pierceability >= 0 && raw->pierceability <= k_max_pierceability)
            {
                physics.pierceability = raw->pierceability;
            }
            return physics;
        }

        /** @brief What Launch was handed, read from the shot's own memory. */
        struct LaunchInputs
        {
            std::array<float, 3> pos{};
            std::array<float, 3> vel{};
            std::uintptr_t vtable{0};
            // The ammo template, or nothing when it cannot be read: the engine defaults then stand in.
            std::optional<AmmoTemplate> ammo{};
        };

        /**
         * @brief Reads Launch's position and velocity, the projectile's vtable and its ammo template with guarded
         *        reads.
         * @return The inputs, or nothing when an argument or the vtable cannot be read. An unreadable ammo template
         *         leaves LaunchInputs::ammo empty instead.
         */
        [[nodiscard]] std::optional<LaunchInputs>
        read_launch_inputs_guarded(std::uintptr_t projectile, const float *pos, const float *velocity) noexcept
        {
            using Floats = std::array<float, 3>;
            const auto launch_pos = DMK::memory::read<Floats>(DMK::Address{reinterpret_cast<std::uintptr_t>(pos)});
            const auto launch_vel = DMK::memory::read<Floats>(DMK::Address{reinterpret_cast<std::uintptr_t>(velocity)});
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{projectile});
            if (!launch_pos || !launch_vel || !vtable)
            {
                return std::nullopt;
            }
            LaunchInputs in{};
            in.pos = *launch_pos;
            in.vel = *launch_vel;
            in.vtable = *vtable;
            if (const std::uintptr_t ammo = ammo_params_of(projectile); ammo != 0)
            {
                in.ammo = read_ammo_template(ammo);
            }
            return in;
        }

        /** @brief True when @p address lies in the game image, which holds every engine function the hooks call. */
        [[nodiscard]] bool in_game_image(std::uintptr_t address) noexcept
        {
            const ModuleInfo &mod = module_info();
            return mod.base != 0 && DMK::Region{DMK::Address{mod.base}, mod.size}.contains(DMK::Address{address});
        }

        /** @brief Calls IEntity::GetPhysics through the anchored slot under SEH. */
        [[nodiscard]] std::uintptr_t call_get_physics_guarded(std::uintptr_t entity, std::uintptr_t fn) noexcept
        {
            __try
            {
                return reinterpret_cast<GetPhysicsFunc>(fn)(entity);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /** @brief The player's own physics entity, which the crosshair target ray skips. 0 when unreadable. */
        [[nodiscard]] std::uintptr_t player_physics(std::uintptr_t c_player) noexcept
        {
            const std::ptrdiff_t entity_offset = runtime_offsets().c_player_entity.load().value;
            const auto entity = DMK::memory::read<std::uintptr_t>(DMK::Address{c_player + entity_offset});
            if (!entity || !DMK::memory::is_plausible_ptr(DMK::Address{*entity}))
            {
                return 0;
            }
            // A virtual is called here, so the object must be a CEntity: an unhealed entity offset on a drifted build
            // then turns the player skip off rather than call an unrelated slot.
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*entity});
            if (!vtable || !vtable_is(GameClass::Entity, *vtable))
            {
                return 0;
            }
            const auto fn = DMK::memory::read<std::uintptr_t>(DMK::Address{*vtable + s_get_physics_offset});
            if (!fn || !in_game_image(*fn))
            {
                return 0;
            }
            const std::uintptr_t physics = call_get_physics_guarded(*entity, *fn);
            return DMK::memory::is_plausible_ptr(DMK::Address{physics}) ? physics : 0;
        }

        /** @brief An ammo item's ammo class (the item table's Ammo Type), or empty when it is not one. */
        [[nodiscard]] std::optional<std::uint32_t> ammo_class_of(std::uintptr_t ammo_item) noexcept
        {
            if (ammo_item == 0)
            {
                return std::nullopt;
            }
            const auto item_class =
                DMK::memory::read<std::uintptr_t>(DMK::Address{ammo_item + Constants::ITEM_CLASS_OFFSET});
            if (!item_class || !DMK::memory::is_plausible_ptr(DMK::Address{*item_class}))
            {
                return std::nullopt;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*item_class});
            if (!vtable || !vtable_is(GameClass::AmmoItemClass, *vtable))
            {
                return std::nullopt;
            }
            const auto ammo_class =
                DMK::memory::read<std::uint32_t>(DMK::Address{*item_class + Constants::AMMO_ITEM_CLASS_TYPE_OFFSET});
            if (!ammo_class)
            {
                return std::nullopt;
            }
            return *ammo_class;
        }

        /** @brief The weapon family of an ammo class (Unknown for none). */
        [[nodiscard]] AmmoKind ammo_kind(std::optional<std::uint32_t> ammo_class) noexcept
        {
            if (!ammo_class)
            {
                return AmmoKind::Unknown;
            }
            switch (*ammo_class)
            {
            case Constants::AMMO_CLASS_ARROW:
                return AmmoKind::Arrow;
            case Constants::AMMO_CLASS_BOLT:
                return AmmoKind::Bolt;
            case Constants::AMMO_CLASS_BALL:
            case Constants::AMMO_CLASS_SCATTER_SHOT:
                return AmmoKind::Firearm;
            default:
                return AmmoKind::Unknown;
            }
        }

        /** @brief The weapon family of a shot, from FireProjectile's ammo item. */
        [[nodiscard]] AmmoKind classify_ammo(std::uintptr_t ammo_item) noexcept
        {
            return ammo_kind(ammo_class_of(ammo_item));
        }

        /** @brief The configured gravity multiple for @p kind (1 for ammo of no known weapon). */
        [[nodiscard]] float gravity_scale_for(AmmoKind kind) noexcept
        {
            const LiveSettings &cfg = settings();
            float scale = 1.0f;
            switch (kind)
            {
            case AmmoKind::Arrow:
                scale = cfg.archery_gravity_bow.load(std::memory_order_relaxed);
                break;
            case AmmoKind::Bolt:
                scale = cfg.archery_gravity_crossbow.load(std::memory_order_relaxed);
                break;
            case AmmoKind::Firearm:
                scale = cfg.archery_gravity_firearm.load(std::memory_order_relaxed);
                break;
            case AmmoKind::Unknown:
            default:
                break;
            }
            return std::isfinite(scale) ? std::clamp(scale, 0.0f, k_max_gravity_scale) : 1.0f;
        }

        /** @brief The projectile's particle physics and its SetParams, or {0, 0} when either is not as expected. */
        [[nodiscard]] std::pair<std::uintptr_t, std::uintptr_t> particle_of(std::uintptr_t projectile) noexcept
        {
            const auto particle =
                DMK::memory::read<std::uintptr_t>(DMK::Address{projectile + Constants::PROJECTILE_PHYSICS_OFFSET});
            if (!particle || !DMK::memory::is_plausible_ptr(DMK::Address{*particle}))
            {
                return {0, 0};
            }
            // A virtual is called on it, so it must be the particle Launch flies.
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*particle});
            if (!vtable || !vtable_is(GameClass::ParticleEntity, *vtable))
            {
                return {0, 0};
            }
            const auto fn = DMK::memory::read<std::uintptr_t>(
                DMK::Address{*vtable + Constants::PHYS_ENTITY_VTABLE_SET_PARAMS_OFFSET});
            if (!fn || !in_game_image(*fn))
            {
                return {0, 0};
            }
            return {*particle, *fn};
        }

        /** @brief Particle parameters that set the gravity alone (straight down, @p g m/s2). */
        [[nodiscard]] ParticleParams gravity_only_params(float g) noexcept
        {
            constexpr float unused = std::bit_cast<float>(Constants::PHYS_UNUSED_FLOAT_BITS);
            constexpr int unused_int = std::bit_cast<int>(Constants::PHYS_UNUSED_INT);
            ParticleParams params{};
            params.type = Constants::PE_PARAMS_PARTICLE_TYPE;
            params.flags = Constants::PHYS_UNUSED_INT;
            params.mass = unused;
            params.size = unused;
            params.thickness = unused;
            params.heading.x = unused;
            params.velocity = unused;
            params.air_resistance = unused;
            params.water_resistance = unused;
            params.accel_thrust = unused;
            params.accel_lift = unused;
            params.surface_index = unused_int;
            params.spin.x = unused;
            params.gravity = Vec3f{0.0f, 0.0f, -g};
            params.water_gravity.x = unused;
            params.normal.x = unused;
            params.roll_axis.x = unused;
            params.orientation[3] = unused;
            params.min_bounce_speed = unused;
            params.min_speed = unused;
            params.collider_to_ignore = ~std::uintptr_t{0};
            params.pierceability = unused_int;
            params.collide_types = unused_int;
            params.area_check_period = unused_int;
            params.no_hit_effect = unused_int;
            return params;
        }

        /**
         * @brief Calls IPhysicalEntity::SetParams through @p fn on @p particle under SEH.
         * @return What SetParams returned: 0 when the entity rejected the parameters, and 0 on a fault.
         */
        [[nodiscard]] int call_set_params_guarded(std::uintptr_t fn, std::uintptr_t particle,
                                                  const ParticleParams &params) noexcept
        {
            __try
            {
                // Not thread safe, as Launch passes it: the physics queues the change while a step runs.
                return reinterpret_cast<SetParamsFunc>(fn)(particle, &params, 0);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /** @brief Sets the launched projectile's gravity to @p g. False when its physics did not take it. */
        [[nodiscard]] bool set_shot_gravity(std::uintptr_t projectile, float g) noexcept
        {
            const auto [particle, set_params] = particle_of(projectile);
            if (particle == 0)
            {
                return false;
            }
            const ParticleParams params = gravity_only_params(g);
            return call_set_params_guarded(set_params, particle, params) != 0;
        }

        /**
         * @brief After a Launch that takes no scaled gravity, writes the ammo template's own gravity to the projectile
         *        while any shot gravity was scaled, so a pooled projectile never flies a scale a player shot left.
         */
        void reset_template_gravity(std::uintptr_t projectile) noexcept
        {
            if (projectile == 0 || !s_gravity_dirty.load(std::memory_order_relaxed))
            {
                return;
            }
            const std::uintptr_t ammo = ammo_params_of(projectile);
            // An unreadable template leaves the gravity alone rather than write a guess.
            const std::optional<AmmoTemplate> raw = ammo != 0 ? read_ammo_template(ammo) : std::nullopt;
            if (!raw)
            {
                return;
            }
            // A projectile whose physics refuses the reset keeps what it had: nothing else can restore it.
            (void)set_shot_gravity(projectile, checked_physics(raw).gravity);
        }

        /**
         * @brief After the player's Launch, writes the gravity the shot flies: the scaled one when a scale is pending,
         *        else the ammo template's own (reset_template_gravity), so a pooled projectile never keeps a scale.
         * @details The record then states the gravity the shot really flies. An unscaled shot takes the template's
         *          gravity, not the record's, which is 0 for a shot that faulted before its physics was read.
         */
        void apply_shot_gravity(std::uintptr_t projectile, ShotRecord &shot) noexcept
        {
            if (shot.gravity_change != GravityChange::Pending)
            {
                reset_template_gravity(projectile);
                return;
            }
            if (set_shot_gravity(projectile, shot.gravity))
            {
                shot.gravity_change = GravityChange::Applied;
                s_gravity_dirty.store(true, std::memory_order_relaxed);
            }
            else
            {
                shot.gravity_change = GravityChange::Refused;
                shot.gravity = shot.native_gravity;
            }
        }

        /** @brief Where a particle launched from @p from at @p velocity is after @p t seconds (a = g - k v). */
        [[nodiscard]] Vector3 ballistic_position(const Vector3 &from, const Vector3 &velocity, float g, float k,
                                                 float t) noexcept
        {
            const Vector3 gravity{0.0f, 0.0f, -g};
            if (k <= 1e-4f)
            {
                return from + velocity * t + gravity * (0.5f * t * t);
            }
            // (v - g/k) D + g t/k rearranged to v D + g (t - D)/k, with D = (1 - e^-kt)/k: under a light drag both
            // forms cancel in float, so D takes expm1 and (t - D)/k its series.
            const float kt = k * t;
            const float decayed = -std::expm1(-kt) / k;
            const float sag = kt < 1e-2f ? t * t * (0.5f - kt / 6.0f) : (t - decayed) / k;
            return from + velocity * decayed + gravity * sag;
        }

        /**
         * @brief Where a shot from @p from at @p velocity first meets the world, traced along its arc.
         * @details The trace casts chord by chord with @p flags and passes through @p skip alone (the shooter's
         *          physics, current at the call), as the arrow's own collision does. The point is where the arrow
         *          itself stops, within the sag of one chord below the arc: a few millimeters at arrow speeds, about
         *          4.5 cm for a 20 m/s stone. Empty when the arc meets nothing within the target range and the trace
         *          time.
         * @param normal When given, receives the surface normal at the landing.
         */
        [[nodiscard]] std::optional<Vector3> trace_landing(const Vector3 &from, const Vector3 &velocity, float g,
                                                           float k, unsigned int flags, std::uintptr_t skip,
                                                           Vector3 *normal = nullptr) noexcept
        {
            const float speed = velocity.magnitude();
            // The comparison also rejects a NaN speed.
            if (!(speed >= k_min_trace_speed))
            {
                return std::nullopt;
            }
            const float dt = k_trace_chord / speed;
            Vector3 previous = from;
            for (int i = 1; i <= k_trace_max_chords; ++i)
            {
                const float t = dt * static_cast<float>(i);
                const Vector3 next = ballistic_position(from, velocity, g, k, t);
                if (const std::optional<RayHit> hit =
                        ray_world_intersection(previous, next - previous, Constants::RWI_OBJTYPES_ARROW, flags,
                                               skip != 0 ? &skip : nullptr, skip != 0 ? 1 : 0);
                    hit)
                {
                    if (normal != nullptr)
                    {
                        *normal = hit->m_normal;
                    }
                    return hit->m_point;
                }
                if (t >= k_trace_max_seconds || (next - from).magnitude() > Constants::ARCHERY_TARGET_RANGE)
                {
                    break;
                }
                previous = next;
            }
            return std::nullopt;
        }

        /**
         * @brief Turns a launch about the vertical through @p from so the arrow comes down in the camera's vertical
         *        plane through the crosshair: on screen straight below the crosshair, wherever the drop takes it.
         * @details The bow sits beside the camera, so a shot aimed at the crosshair point meets the camera's plane only
         *          there. One that drops short of it, or falls from the open sky onto something nearer, lands to the
         *          side. The traced landing is moved into the plane and the launch turned toward it, the elevation
         *          kept, until the traced landing is in the plane. The launch closest to the plane is returned, and
         *          @p dir when no landing traces.
         * @param skip The shooter's own physics, which the trace passes through.
         */
        [[nodiscard]] Vector3 land_below_crosshair(const Vector3 &from, const Vector3 &dir, ShotRecord &shot,
                                                   unsigned int flags, std::uintptr_t skip) noexcept
        {
            const Vector3 camera = to_vector(shot.target.camera);
            const Vector3 normal = view_axes(to_vector(shot.target.forward)).first;
            const float level = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (level < 1e-3f)
            {
                return dir;
            }
            Vector3 aimed = dir;
            Vector3 best = dir;
            float best_lateral = 0.0f;
            for (int turn = 0; turn <= k_landing_turns; ++turn)
            {
                const std::optional<Vector3> landing =
                    trace_landing(from, aimed * shot.speed, shot.gravity, shot.air_resistance, flags, skip);
                if (!landing)
                {
                    break;
                }
                const float lateral = dot(*landing - camera, normal);
                if (!shot.landing_traced || std::fabs(lateral) < std::fabs(best_lateral))
                {
                    if (!shot.landing_traced)
                    {
                        shot.landing_lateral_unturned = lateral;
                    }
                    shot.landing_traced = true;
                    shot.landing = to_vec3f(*landing);
                    best = aimed;
                    best_lateral = lateral;
                }
                if (std::fabs(lateral) < k_landing_tolerance || turn == k_landing_turns)
                {
                    break;
                }
                const Vector3 goal = *landing - normal * lateral;
                const float gx = goal.x - from.x;
                const float gy = goal.y - from.y;
                const float reach = std::sqrt(gx * gx + gy * gy);
                if (reach < k_min_target_distance)
                {
                    break;
                }
                aimed = Vector3{gx / reach * level, gy / reach * level, dir.z};
            }
            shot.landing_lateral = best_lateral;
            return best;
        }

        /**
         * @brief True when @p target is recent enough at @p now_ms to aim a shot by.
         * @details Signed: the main thread can publish a target stamped after the shot read the clock.
         */
        [[nodiscard]] bool target_fresh(const AimTarget &target, std::uint64_t now_ms) noexcept
        {
            return static_cast<std::int64_t>(now_ms) - static_cast<std::int64_t>(target.stamp_ms) <=
                   static_cast<std::int64_t>(k_target_max_age_ms);
        }

        /** @brief MaxAimCorrection in degrees, held to [0, k_max_aim_correction_cap]. A non-finite value reads 0. */
        [[nodiscard]] float max_aim_correction() noexcept
        {
            const float limit = settings().archery_max_correction.load(std::memory_order_relaxed);
            return std::isfinite(limit) ? std::clamp(limit, 0.0f, k_max_aim_correction_cap) : 0.0f;
        }

        /** @brief The ray flags of an arrow's own collision: the arrow base flags with @p pierceability. */
        [[nodiscard]] unsigned int arrow_ray_flags(int pierceability) noexcept
        {
            return Constants::RWI_FLAGS_ARROW_BASE |
                   static_cast<unsigned int>(std::clamp(pierceability, 0, k_max_pierceability));
        }

        /**
         * @brief Turns the re-aimed launch @p straight about the vertical so it lands below the crosshair, and writes
         *        the launch Launch receives.
         * @details The turn is held to @p max_correction from the game's @p native aim. A shot that needs more flies
         *          unturned and can land a little beside the crosshair, and its traced landing then does not apply.
         *          @p shot receives the landing trace, the turn and the flown direction.
         * @param vel_length The length of the game's velocity, which the flown velocity keeps.
         * @return False when the launch direction is not finite: the game's arguments then stand.
         */
        [[nodiscard]] bool aim_launch(const Vector3 &start, const Vector3 &native, const Vector3 &straight,
                                      float vel_length, float max_correction, unsigned int flags, std::uintptr_t skip,
                                      ShotRecord &shot, FlownLaunch &flown) noexcept
        {
            Vector3 launch_dir = straight;
            const Vector3 turned = land_below_crosshair(start, straight, shot, flags, skip);
            if (angle_deg(native, turned) <= max_correction)
            {
                shot.landing_turn_deg = angle_deg(straight, turned);
                launch_dir = turned;
            }
            else
            {
                shot.landing_traced = false;
                shot.landing_turn_refused = true;
            }
            // A non-finite direction reaches the physics as the projectile's velocity.
            if (!is_finite(launch_dir))
            {
                return false;
            }
            shot.flown_dir = to_vec3f(launch_dir);
            flown.dir = shot.flown_dir;
            flown.vel = to_vec3f(launch_dir * vel_length);
            return true;
        }

        /**
         * @brief Builds the shot record and, when the shot is to be re-aimed, the launch to fly in place of the game's.
         * @return True when @p flown replaces the game's direction and velocity.
         */
        [[nodiscard]] bool prepare_shot(std::uintptr_t projectile, const float *pos, const float *velocity,
                                        float speed_scale, ShotRecord &shot, FlownLaunch &flown) noexcept
        {
            shot.projectile = projectile;
            shot.thread_id = GetCurrentThreadId();
            shot.launch_ms = GetTickCount64();
            shot.ammo = s_fire_ammo.load(std::memory_order_relaxed);
            shot.weapon = s_fire_weapon.load(std::memory_order_relaxed);
            shot.free_look = camera_state().orbit_active.load(std::memory_order_relaxed);

            const std::optional<LaunchInputs> in = read_launch_inputs_guarded(projectile, pos, velocity);
            if (!in)
            {
                shot.outcome = ShotOutcome::Faulted;
                return false;
            }
            shot.vtable = in->vtable;
            const Vector3 start{in->pos[0], in->pos[1], in->pos[2]};
            const Vector3 vel{in->vel[0], in->vel[1], in->vel[2]};
            const float vel_length = vel.magnitude();
            if (!is_finite(start) || !std::isfinite(vel_length) || vel_length <= k_min_launch_velocity ||
                !std::isfinite(speed_scale))
            {
                shot.outcome = ShotOutcome::Faulted;
                return false;
            }
            const Vector3 native = vel / vel_length;
            shot.launch = to_vec3f(start);
            shot.native_dir = to_vec3f(native);
            shot.flown_dir = shot.native_dir;
            shot.speed = vel_length * speed_scale;

            const FlightPhysics physics = checked_physics(in->ammo);
            shot.native_gravity = physics.gravity;
            shot.gravity = shot.native_gravity;
            shot.gravity_scale = gravity_scale_for(shot.ammo);
            if (shot.gravity_scale != 1.0f)
            {
                // Scaled only when the particle can take it after Launch, so the landing turn never traces an arc the
                // arrow does not fly.
                if (particle_of(projectile).first != 0)
                {
                    shot.gravity = shot.native_gravity * shot.gravity_scale;
                    shot.gravity_change = GravityChange::Pending;
                }
                else
                {
                    shot.gravity_change = GravityChange::Refused;
                }
            }
            shot.air_resistance = physics.drag;
            shot.pierceability = physics.pierceability;
            s_pierceability.store(shot.pierceability, std::memory_order_relaxed);

            AimTarget target{};
            shot.target_valid = s_target.load(target) && target_fresh(target, shot.launch_ms);
            if (shot.target_valid)
            {
                shot.target = target;
            }

            if (!settings().archery_aim_at_crosshair.load(std::memory_order_relaxed))
            {
                shot.outcome = ShotOutcome::AimOff;
                return false;
            }
            if (!s_fire_single_shot.load(std::memory_order_relaxed))
            {
                shot.outcome = ShotOutcome::Pellets;
                return false;
            }
            if (shot.free_look)
            {
                shot.outcome = ShotOutcome::FreeLook;
                return false;
            }
            if (!shot.target_valid)
            {
                shot.outcome = ShotOutcome::NoTarget;
                return false;
            }
            const Vector3 point = to_vector(target.point);
            if (!is_finite(point))
            {
                shot.outcome = ShotOutcome::Faulted;
                return false;
            }
            const Vector3 to_point = point - start;
            if (to_point.magnitude() < k_min_target_distance)
            {
                shot.outcome = ShotOutcome::TooClose;
                return false;
            }
            const Vector3 straight = to_point.normalized();
            shot.correction_deg = angle_deg(native, straight);
            const float max_correction = max_aim_correction();
            if (shot.correction_deg > max_correction)
            {
                shot.outcome = ShotOutcome::TooFar;
                return false;
            }
            if (!aim_launch(start, native, straight, vel_length, max_correction, arrow_ray_flags(shot.pierceability),
                            s_fire_skip.load(std::memory_order_relaxed), shot, flown))
            {
                shot.outcome = ShotOutcome::Faulted;
                return false;
            }
            shot.outcome = ShotOutcome::Redirected;
            return true;
        }

        /**
         * @brief Hands a shot record to the main thread and marks its projectile for the impact hook.
         * @note Call after the original Launch: the main thread checks the projectile's launch point against the
         *       record at once, and only Launch writes it. The pool re-init before Launch zeroes the flag word the
         *       main thread follows, and Launch leaves the collided bit as it finds it.
         */
        void publish_shot(const ShotRecord &shot) noexcept
        {
            ShotRecord record = shot;
            record.track_slot =
                static_cast<std::uint32_t>(s_track_next.fetch_add(1, std::memory_order_relaxed) % k_ring);
            s_tracked[record.track_slot].store(shot.projectile, std::memory_order_relaxed);
            s_shots.push(record);
        }

        /** @brief True when @p projectile is a published shot whose impact the trail still wants. */
        [[nodiscard]] bool is_tracked(std::uintptr_t projectile) noexcept
        {
            for (const std::atomic<std::uintptr_t> &slot : s_tracked)
            {
                if (slot.load(std::memory_order_relaxed) == projectile)
                {
                    return true;
                }
            }
            return false;
        }

        /** @brief Clears a finished shot's s_tracked slot, unless a newer shot already took it over. */
        void untrack(const ShotRecord &shot) noexcept
        {
            std::uintptr_t expected = shot.projectile;
            s_tracked[shot.track_slot].compare_exchange_strong(expected, 0, std::memory_order_relaxed);
        }

        /** @brief Whether FireProjectile's shooter is the local player, and whether it is the C_Player itself. */
        struct Shooter
        {
            bool player{false};
            bool c_player{false};
        };

        /**
         * @brief Classifies FireProjectile's shooter: the local player when it is a C_Player, so a first-person shot
         *        counts before any frame step has run, or when it carries the entity id the frame step saw.
         * @details The shooter's address alone does not count: after a load, another actor can reuse a freed
         *          C_Player's address.
         */
        [[nodiscard]] Shooter classify_shooter(std::uintptr_t actor) noexcept
        {
            Shooter shooter{};
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{actor});
            shooter.c_player = vtable.has_value() && vtable_is(GameClass::Player, *vtable);
            shooter.player = shooter.c_player;
            const std::uint32_t player_id = s_player_id.load(std::memory_order_relaxed);
            if (!shooter.player && player_id != 0)
            {
                const auto id =
                    DMK::memory::read<std::uint32_t>(DMK::Address{actor + Constants::ACTOR_ENTITY_ID_OFFSET});
                shooter.player = id.has_value() && *id == player_id;
            }
            return shooter;
        }

        /**
         * @brief FireProjectile detour: when the shooter is the local player, marks the caller's thread and the shot's
         *        pellet flag, ammo, weapon and shooter physics for the Launch that runs under it.
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        std::uintptr_t __fastcall fire_projectile_detour(std::uintptr_t utils, std::uintptr_t actor, int id,
                                                         const float *dir, std::uintptr_t desc, float *pos,
                                                         std::uintptr_t weapon, std::uintptr_t ammo, float speed,
                                                         std::uint8_t single_shot) noexcept
        {
            const DetourScope in_flight;
            const FireProjectileFunc original = s_fire_original.load(std::memory_order_acquire);
            const Shooter shooter =
                s_aim_ready.load(std::memory_order_acquire) && actor != 0 ? classify_shooter(actor) : Shooter{};
            if (shooter.player)
            {
                s_fire_single_shot.store(single_shot != 0, std::memory_order_relaxed);
                s_fire_ammo.store(classify_ammo(ammo), std::memory_order_relaxed);
                s_fire_weapon.store(weapon, std::memory_order_relaxed);
                // Read from the shooter here, while the fire routine holds it: never a body from an earlier frame.
                s_fire_skip.store(shooter.c_player ? player_physics(actor) : 0, std::memory_order_relaxed);
                s_fire_thread.store(GetCurrentThreadId(), std::memory_order_relaxed);
            }
            const std::uintptr_t result = original(utils, actor, id, dir, desc, pos, weapon, ammo, speed, single_shot);
            if (shooter.player)
            {
                s_fire_thread.store(0, std::memory_order_relaxed);
            }
            return result;
        }

        /**
         * @brief Learns where a held distraction stone leaves the hand when it is thrown.
         * @details The throw animation swings the stone from where it was held ready to where it lets go, so the
         *          preview adds the release's offset from the held spot, averaged over the throws and kept in the
         *          look's level frame so it turns with the look. One learn per hold: the preview arms it again.
         */
        void note_decoy_release(std::uintptr_t projectile, const float *pos, const float *dir) noexcept
        {
            if (projectile == 0 || pos == nullptr || dir == nullptr)
            {
                return;
            }
            std::uintptr_t armed = projectile;
            if (!s_decoy_held.compare_exchange_strong(armed, 0, std::memory_order_relaxed))
            {
                return;
            }
            DecoyPose pose{};
            const auto launch =
                DMK::memory::read<std::array<float, 3>>(DMK::Address{reinterpret_cast<std::uintptr_t>(pos)});
            const auto heading =
                DMK::memory::read<std::array<float, 3>>(DMK::Address{reinterpret_cast<std::uintptr_t>(dir)});
            if (!launch || !heading || !s_decoy_pose.load(pose) || pose.decoy != projectile)
            {
                return;
            }
            const Vector3 released = to_vector(to_vec3f(*launch));
            // How far the preview's start and direction were from the real throw, for the log.
            const float start_error = (released - to_vector(pose.predicted_from)).magnitude();
            const float dir_error = angle_deg(to_vector(to_vec3f(*heading)), to_vector(pose.predicted_dir));
            const Vector3 moved = released - to_vector(pose.held);
            if (!is_finite(moved) || moved.magnitude() > k_max_decoy_release)
            {
                return;
            }
            const float right = moved.x * std::cos(pose.yaw) + moved.y * std::sin(pose.yaw);
            const float forward = moved.y * std::cos(pose.yaw) - moved.x * std::sin(pose.yaw);
            const auto blend = [](std::atomic<float> &learned, float seen) -> void
            {
                const float was = learned.load(std::memory_order_relaxed);
                learned.store(was + (seen - was) * k_decoy_release_blend, std::memory_order_relaxed);
            };
            blend(s_decoy_release_right, right);
            blend(s_decoy_release_forward, forward);
            blend(s_decoy_release_up, moved.z);
            if (pose.landed)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Archery[preview]: stone released {:+.2f} m right, {:+.2f} m forward, {:+.2f} m up of where it was "
                    "held; the preview had it start {:.3f} m off and go {:.2f} deg off, landing at ({:.2f}, {:.2f}, "
                    "{:.2f})",
                    right, forward, moved.z, start_error, dir_error, pose.predicted_landing.x, pose.predicted_landing.y,
                    pose.predicted_landing.z);
            }
            else
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Archery[preview]: stone released {:+.2f} m right, {:+.2f} m forward, {:+.2f} m up of where it was "
                    "held; the preview had it start {:.3f} m off and go {:.2f} deg off, landing nowhere in reach",
                    right, forward, moved.z, start_error, dir_error);
            }
        }

        /**
         * @brief Re-aims the player's thrown distraction stone at the crosshair, as an arrow is: from where it leaves
         *        the hand toward the crosshair point at the throw's speed, turned so it comes down straight below the
         *        crosshair.
         * @details The game throws the stone along the look from the hand beside the eye, so in third person it lands
         *          beside the crosshair. Kept as thrown with AimAtCrosshair off, in free-look, without a fresh
         *          crosshair target (first person), for a point too near the hand, and beyond MaxAimCorrection.
         * @return True when @p flown replaces the game's direction and velocity.
         */
        [[nodiscard]] bool redirect_decoy(std::uintptr_t projectile, const float *pos, const float *velocity,
                                          float speed_scale, FlownLaunch &flown) noexcept
        {
            if (!settings().archery_aim_at_crosshair.load(std::memory_order_relaxed) ||
                camera_state().orbit_active.load(std::memory_order_relaxed))
            {
                return false;
            }
            AimTarget target{};
            const std::optional<LaunchInputs> in = read_launch_inputs_guarded(projectile, pos, velocity);
            if (!in || !s_target.load(target) || !target_fresh(target, GetTickCount64()))
            {
                return false;
            }
            const Vector3 start{in->pos[0], in->pos[1], in->pos[2]};
            const Vector3 vel{in->vel[0], in->vel[1], in->vel[2]};
            const float vel_length = vel.magnitude();
            const Vector3 point = to_vector(target.point);
            if (!is_finite(start) || !is_finite(point) || !std::isfinite(vel_length) ||
                vel_length <= k_min_launch_velocity || !std::isfinite(speed_scale))
            {
                return false;
            }
            const Vector3 native = vel / vel_length;
            if ((point - start).magnitude() < k_min_target_distance)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Archery: stone kept as thrown: crosshair point too near the hand");
                return false;
            }
            const Vector3 straight = (point - start).normalized();
            const float correction = angle_deg(native, straight);
            const float max_correction = max_aim_correction();
            if (correction > max_correction)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Archery: stone kept as thrown: crosshair point {:.1f} deg from the throw, "
                                         "beyond MaxAimCorrection",
                                         correction);
                return false;
            }
            const FlightPhysics physics = checked_physics(in->ammo);
            ShotRecord shot{};
            shot.target = target;
            shot.speed = vel_length * speed_scale;
            shot.gravity = physics.gravity;
            shot.air_resistance = physics.drag;
            if (!aim_launch(start, native, straight, vel_length, max_correction, arrow_ray_flags(physics.pierceability),
                            player_physics(s_player.load(std::memory_order_relaxed)), shot, flown))
            {
                return false;
            }
            if (shot.landing_traced)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Archery: stone re-aimed at the crosshair point {:.2f} m away, {:.1f} deg from "
                    "the throw, landing {:+.2f} m beside the crosshair after a {:.2f} deg turn",
                    target.distance, correction, shot.landing_lateral, shot.landing_turn_deg);
            }
            else if (shot.landing_turn_refused)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Archery: stone re-aimed at the crosshair point {:.2f} m away, {:.1f} deg from "
                    "the throw, landing {:+.2f} m beside the crosshair: the turn to bring it below "
                    "is beyond MaxAimCorrection",
                    target.distance, correction, shot.landing_lateral_unturned);
            }
            else
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Archery: stone re-aimed at the crosshair point {:.2f} m away, {:.1f} deg from "
                    "the throw, no landing traced",
                    target.distance, correction);
            }
            return true;
        }

        /**
         * @brief CProjectile::Launch detour: re-aims the player's shot or thrown stone at the crosshair, sets the
         *        shot's gravity, and publishes the player's shot record.
         * @details Every other Launch passes through. While any shot gravity was scaled, each Launch writes the
         *          launched projectile's own gravity back after the original (apply_shot_gravity for the player's
         *          shot, reset_template_gravity for every other).
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        void __fastcall launch_detour(std::uintptr_t projectile, const float *pos, const float *dir,
                                      const float *velocity, float speed_scale) noexcept
        {
            const DetourScope in_flight;
            const LaunchFunc original = s_launch_original.load(std::memory_order_acquire);
            const bool usable = s_aim_ready.load(std::memory_order_acquire) && projectile != 0 && pos != nullptr &&
                                dir != nullptr && velocity != nullptr;
            if (usable && s_fire_thread.load(std::memory_order_relaxed) == GetCurrentThreadId())
            {
                ShotRecord shot{};
                FlownLaunch flown{};
                if (prepare_shot(projectile, pos, velocity, speed_scale, shot, flown))
                {
                    original(projectile, pos, engine_ptr(flown.dir), engine_ptr(flown.vel), speed_scale);
                }
                else
                {
                    original(projectile, pos, dir, velocity, speed_scale);
                }
                // After Launch, so nothing Launch sets on the particle overrides it.
                apply_shot_gravity(projectile, shot);
                // The arrow can collide only in a later physics step, so the record still reaches the impact hook in
                // time.
                publish_shot(shot);
                return;
            }
            if (usable && projectile == s_decoy_in_hand.load(std::memory_order_relaxed))
            {
                FlownLaunch flown{};
                const bool redirect = redirect_decoy(projectile, pos, velocity, speed_scale, flown);
                const float *launch_dir = redirect ? engine_ptr(flown.dir) : dir;
                note_decoy_release(projectile, pos, launch_dir);
                original(projectile, pos, launch_dir, redirect ? engine_ptr(flown.vel) : velocity, speed_scale);
            }
            else
            {
                original(projectile, pos, dir, velocity, speed_scale);
            }
            reset_template_gravity(projectile);
        }

        /**
         * @brief Reads the arrow's flags and launch point, the contact point, and the other side of the contact with
         *        guarded reads. False when any of them cannot be read.
         * @details A collision names two sides. The arrow's own side carries the arrow's entity as its foreign data,
         *          so the hit object is the side that does not.
         */
        [[nodiscard]] bool read_collision_guarded(std::uintptr_t arrow, std::uintptr_t collision, std::uint32_t &flags,
                                                  Vec3f &launch, Vec3f &point, std::uintptr_t &other_foreign,
                                                  int &other_foreign_id) noexcept
        {
            const auto arrow_flags =
                DMK::memory::read<std::uint32_t>(DMK::Address{arrow + Constants::PROJECTILE_FLAGS_OFFSET});
            const auto initial =
                DMK::memory::read<std::array<float, 3>>(DMK::Address{arrow + Constants::PROJECTILE_INITIAL_POS_OFFSET});
            const auto contact = DMK::memory::read<std::array<float, 3>>(
                DMK::Address{collision + Constants::PHYS_COLLISION_POINT_OFFSET});
            const auto arrow_entity =
                DMK::memory::read<std::uintptr_t>(DMK::Address{arrow + Constants::PROJECTILE_ENTITY_OFFSET});
            const auto foreign = DMK::memory::read<std::array<std::uintptr_t, 2>>(
                DMK::Address{collision + Constants::PHYS_COLLISION_FOREIGN_DATA_OFFSET});
            const auto foreign_id = DMK::memory::read<std::array<int, 2>>(
                DMK::Address{collision + Constants::PHYS_COLLISION_FOREIGN_ID_OFFSET});
            if (!arrow_flags || !initial || !contact || !arrow_entity || !foreign || !foreign_id)
            {
                return false;
            }
            flags = *arrow_flags;
            launch = to_vec3f(*initial);
            point = to_vec3f(*contact);
            const std::size_t other = (*foreign)[0] == *arrow_entity ? 1 : 0;
            other_foreign = (*foreign)[other];
            other_foreign_id = (*foreign_id)[other];
            return true;
        }

        /**
         * @brief Copies a CEntity's name into @p out with guarded reads, cut to fit (empty when unreadable).
         * @details A guarded read fails as a whole, so the name is copied one page at a time: a short name near the end
         *          of its page still reads when the next page is unmapped.
         */
        void read_entity_name_guarded(std::uintptr_t entity, std::span<char> out) noexcept
        {
            constexpr std::uintptr_t k_page_size = 0x1000;
            out[0] = '\0';
            const auto name = DMK::memory::read<std::uintptr_t>(DMK::Address{entity + Constants::ENTITY_NAME_OFFSET});
            if (!name || !DMK::memory::is_plausible_ptr(DMK::Address{*name}))
            {
                return;
            }
            std::size_t copied = 0;
            while (copied + 1 < out.size())
            {
                const std::uintptr_t at = *name + copied;
                const auto page_left = static_cast<std::size_t>(k_page_size - (at & (k_page_size - 1)));
                const std::span<char> part = out.subspan(copied, std::min(out.size() - 1 - copied, page_left));
                if (!DMK::memory::read_into(DMK::Address{at}, std::as_writable_bytes(part)))
                {
                    break;
                }
                if (std::find(part.begin(), part.end(), '\0') != part.end())
                {
                    return;
                }
                copied += part.size();
            }
            out[copied] = '\0';
        }

        /** @brief Identifies the other side of an arrow's contact for the impact log. */
        [[nodiscard]] HitObject identify_hit(std::uintptr_t foreign, int foreign_id) noexcept
        {
            HitObject object{};
            object.foreign_id = foreign_id;
            if (foreign != 0 && DMK::memory::is_plausible_ptr(DMK::Address{foreign}))
            {
                const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{foreign});
                if (vtable && vtable_is(GameClass::Entity, *vtable))
                {
                    object.entity = true;
                    read_entity_name_guarded(foreign, object.name);
                }
            }
            return object;
        }

        /** @brief Reads a projectile's flag word and flight time with guarded reads. False when one is unreadable. */
        [[nodiscard]] bool read_flags_guarded(std::uintptr_t projectile, std::uint32_t &flags,
                                              float &flight_time) noexcept
        {
            const auto projectile_flags =
                DMK::memory::read<std::uint32_t>(DMK::Address{projectile + Constants::PROJECTILE_FLAGS_OFFSET});
            const auto time =
                DMK::memory::read<float>(DMK::Address{projectile + Constants::PROJECTILE_FLIGHT_TIME_OFFSET});
            if (!projectile_flags || !time)
            {
                return false;
            }
            flags = *projectile_flags;
            flight_time = *time;
            return true;
        }

        /**
         * @brief CArrow collision handler detour: publishes the impact of a tracked player arrow.
         * @details The handler also runs for contacts it rejects (the shooter, a pierced surface), which leave the
         *          collided bit clear. The impact is the call that sets it.
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        std::uint8_t __fastcall arrow_collision_detour(std::uintptr_t arrow, std::uintptr_t collision) noexcept
        {
            const DetourScope in_flight;
            const ArrowCollisionFunc original = s_collision_original.load(std::memory_order_acquire);
            if (!s_trail_ready.load(std::memory_order_acquire) || arrow == 0 || collision == 0 || !is_tracked(arrow))
            {
                return original(arrow, collision);
            }
            std::uint32_t before = 0;
            Vec3f launch{};
            Vec3f point{};
            std::uintptr_t other_foreign = 0;
            int other_foreign_id = -1;
            const bool have_contact =
                read_collision_guarded(arrow, collision, before, launch, point, other_foreign, other_foreign_id);
            // Named before the original runs: the handler can hand the hit entity's state on (a stuck arrow, a kill).
            const HitObject object = have_contact ? identify_hit(other_foreign, other_foreign_id) : HitObject{};
            const std::uint8_t result = original(arrow, collision);
            std::uint32_t after = 0;
            float flight_time = 0.0f;
            if (have_contact && (before & Constants::PROJECTILE_FLAG_COLLIDED) == 0 &&
                read_flags_guarded(arrow, after, flight_time) && (after & Constants::PROJECTILE_FLAG_COLLIDED) != 0)
            {
                s_impacts.push(ImpactRecord{arrow, launch, point, flight_time, object});
            }
            return result;
        }

        /**
         * @brief True when a living body the crosshair ray hit is the rider's own horse: close to the eye, with the eye
         *        inside the level footprint (the x-y extent) of its world box.
         * @details A body that stands beside the horse is as close, but the rider sits over the horse only.
         */
        [[nodiscard]] bool is_own_mount(const RayHit &hit, const Vector3 &eye, bool eye_valid) noexcept
        {
            if (!eye_valid || hit.m_collider == 0 ||
                (hit.m_point - eye).magnitude() >= Constants::ARCHERY_MOUNT_CLEARANCE)
            {
                return false;
            }
            const auto box_min =
                DMK::memory::read<Vector3>(DMK::Address{hit.m_collider + Constants::PHYS_ENTITY_BBOX_MIN_OFFSET});
            const auto box_max =
                DMK::memory::read<Vector3>(DMK::Address{hit.m_collider + Constants::PHYS_ENTITY_BBOX_MAX_OFFSET});
            if (!box_min || !box_max)
            {
                return false;
            }
            return eye.x >= box_min->x && eye.x <= box_max->x && eye.y >= box_min->y && eye.y <= box_max->y;
        }

        /** @brief True while the player holds a missile weapon (drawn or aimed). */
        [[nodiscard]] bool missile_weapon_in_hand(std::uintptr_t c_player) noexcept
        {
            const std::ptrdiff_t offset = runtime_offsets().c_player_missile_controller.load().value;
            const std::uintptr_t controller = c_player + offset;
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{controller});
            if (!vtable || !vtable_is(GameClass::MissileController, *vtable))
            {
                return false;
            }
            const auto in_hand = DMK::memory::read<std::uint8_t>(
                DMK::Address{controller + Constants::MISSILE_CONTROLLER_IN_HAND_FLAG_OFFSET});
            const auto aiming = DMK::memory::read<std::uint8_t>(
                DMK::Address{controller + Constants::MISSILE_CONTROLLER_AIM_FLAG_OFFSET});
            return (in_hand && *in_hand != 0) || (aiming && *aiming != 0);
        }

        /**
         * @brief Casts the crosshair ray and publishes where it hits, or withdraws the target.
         * @param armed A missile weapon or a distraction stone is in the player's hand: both aim at the crosshair.
         * @param skip The player's own physics, which the ray passes through, or 0.
         * @param pierceability What the ray passes through: the held stone's, else the last arrow's.
         */
        void update_aim_target(const Vector3 &eye, bool eye_valid, std::uint64_t now_ms, bool armed,
                               std::uintptr_t skip, int pierceability) noexcept
        {
            static bool s_was_valid = false;
            static std::uint64_t s_last_trace_ms = 0;
            static std::uintptr_t s_last_collider = 0;
            static float s_last_distance = 0.0f;

            // Cast even with AimAtCrosshair off: a shot the game aims on its own is still measured against the
            // crosshair, and the aim preview places the bow from the eye the target carries.
            float cx = 0.0f, cy = 0.0f, cz = 0.0f, dx = 0.0f, dy = 1.0f, dz = 0.0f;
            if (!armed || !interaction_aim_pose().load(cx, cy, cz, dx, dy, dz))
            {
                if (s_was_valid)
                {
                    s_target.invalidate();
                    s_was_valid = false;
                }
                return;
            }

            const Vector3 camera{cx, cy, cz};
            const Vector3 forward = Vector3{dx, dy, dz}.normalized();
            // Start at the player's depth along the crosshair ray: nothing between the camera and the bow can stop the
            // arrow, and the ray passes beside the player there (the player's own physics is skipped as well).
            const float advance = eye_valid ? std::max(0.0f, dot(eye - camera, forward)) : 0.0f;
            const Vector3 origin = camera + forward * advance;
            const unsigned int flags = arrow_ray_flags(pierceability);
            const std::uintptr_t *skip_list = skip != 0 ? &skip : nullptr;
            const int skip_count = skip != 0 ? 1 : 0;
            std::optional<RayHit> hit;
            if ((game_state_mask().load(std::memory_order_relaxed) & state_bit(GameState::Mount)) == 0)
            {
                hit = ray_world_intersection(origin, forward * Constants::ARCHERY_TARGET_RANGE,
                                             Constants::RWI_OBJTYPES_ARROW, flags, skip_list, skip_count);
            }
            else
            {
                // In the saddle the ray can graze the rider's own horse when aimed down past its head. The world and
                // living bodies are cast apart so that, when the first living body is the mount, the whole horse is
                // skipped and the search goes on behind it along the same ray.
                const std::optional<RayHit> world = ray_world_intersection(
                    origin, forward * Constants::ARCHERY_TARGET_RANGE,
                    Constants::RWI_OBJTYPES_ARROW & ~Constants::RWI_OBJTYPE_LIVING, flags, skip_list, skip_count);
                const float reach = world ? world->m_distance : Constants::ARCHERY_TARGET_RANGE;
                std::optional<RayHit> living = ray_world_intersection(
                    origin, forward * reach, Constants::RWI_OBJTYPE_LIVING, flags, skip_list, skip_count);
                if (living && is_own_mount(*living, eye, eye_valid))
                {
                    std::uintptr_t mount_skip[2]{};
                    int mount_skip_count = 0;
                    if (skip != 0)
                    {
                        mount_skip[mount_skip_count++] = skip;
                    }
                    mount_skip[mount_skip_count++] = living->m_collider;
                    living = ray_world_intersection(origin, forward * reach, Constants::RWI_OBJTYPE_LIVING, flags,
                                                    mount_skip, mount_skip_count);
                }
                hit = living ? living : world;
            }

            AimTarget target{};
            target.camera = to_vec3f(camera);
            target.forward = to_vec3f(forward);
            target.hit = hit.has_value();
            const Vector3 point = hit ? hit->m_point : origin + forward * Constants::ARCHERY_TARGET_RANGE;
            target.point = to_vec3f(point);
            target.eye = to_vec3f(eye);
            target.eye_valid = eye_valid;
            target.distance = (point - camera).magnitude();
            target.stamp_ms = now_ms;
            s_target.store(target);
            s_was_valid = true;

            const std::uintptr_t collider = hit ? hit->m_collider : 0;
            const bool changed =
                collider != s_last_collider || std::fabs(target.distance - s_last_distance) >
                                                   k_target_log_change_share * std::max(1.0f, s_last_distance);
            if (changed && now_ms - s_last_trace_ms >= k_target_log_interval_ms)
            {
                s_last_trace_ms = now_ms;
                s_last_collider = collider;
                s_last_distance = target.distance;
                if (hit)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "Archery[target]: crosshair hits ({:.2f}, {:.2f}, {:.2f}) {:.2f} m from the "
                        "camera (collider {:#x}, terrain {}, ray from {:.2f} m, skip {:#x})",
                        point.x, point.y, point.z, target.distance, hit->m_collider, hit->m_terrain, advance, skip);
                }
                else
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Trace,
                        "Archery[target]: crosshair hits nothing within {:.0f} m (ray from {:.2f} m, "
                        "skip {:#x})",
                        Constants::ARCHERY_TARGET_RANGE, advance, skip);
                }
            }
        }

        /** @brief The active flight of the shot that launched @p projectile from @p launch, or nullptr. */
        [[nodiscard]] Flight *find_active_flight(std::uintptr_t projectile, const Vec3f &launch) noexcept
        {
            for (Flight &flight : s_flights)
            {
                // Exact float equality is intended: Launch stores the exact launch point the shot record copied.
                if (flight.used && flight.active && flight.shot.projectile == projectile &&
                    flight.shot.launch.x == launch.x && flight.shot.launch.y == launch.y &&
                    flight.shot.launch.z == launch.z)
                {
                    return &flight;
                }
            }
            return nullptr;
        }

        /**
         * @brief Adds a trail sample once the arrow moved k_sample_spacing from the last one.
         * @details A full buffer overwrites its last sample, so the stop point always ends the trail.
         */
        void append_sample(Flight &flight, const Vec3f &point, float time) noexcept
        {
            if (flight.count > 0)
            {
                const Vec3f &last = flight.points[flight.count - 1];
                const float ddx = point.x - last.x, ddy = point.y - last.y, ddz = point.z - last.z;
                if (ddx * ddx + ddy * ddy + ddz * ddz < k_sample_spacing * k_sample_spacing)
                {
                    return;
                }
            }
            const std::size_t slot = std::min(flight.count, k_max_samples - 1);
            flight.points[slot] = point;
            flight.times[slot] = time;
            flight.count = slot + 1;
        }

        /** @brief What kind of object the arrow stopped in, for the impact log. */
        [[nodiscard]] const char *hit_kind(const Flight &flight) noexcept
        {
            if (!flight.impact)
            {
                return "nothing reported";
            }
            if (flight.impact_object.entity)
            {
                return "entity";
            }
            switch (flight.impact_object.foreign_id)
            {
            case PHYS_FOREIGN_ID_TERRAIN:
                return "terrain";
            case Constants::PHYS_FOREIGN_ID_STATIC:
                return "static geometry";
            case PHYS_FOREIGN_ID_FOLIAGE:
                return "foliage";
            default:
                return "object";
            }
        }

        /**
         * @brief Logs where a finished flight landed against the crosshair it was released at.
         * @param reason Why the flight ended.
         */
        void report_flight(const Flight &flight, std::string_view reason) noexcept
        {
            const char *kind = hit_kind(flight);
            const char *name = flight.impact && flight.impact_object.entity ? flight.impact_object.name : "";
            const Vec3f end = flight.impact ? flight.impact_point
                                            : (flight.count > 0 ? flight.points[flight.count - 1] : flight.shot.launch);
            const Vector3 stop = to_vector(end);
            float level_speed = 0.0f;
            if (flight.count >= 2 && flight.times[flight.count - 1] > flight.times[0])
            {
                const Vec3f &first = flight.points[0];
                const Vec3f &last = flight.points[flight.count - 1];
                level_speed =
                    std::sqrt((last.x - first.x) * (last.x - first.x) + (last.y - first.y) * (last.y - first.y)) /
                    (flight.times[flight.count - 1] - flight.times[0]);
            }

            if (!flight.shot.target_valid)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Info,
                                         "Archery {} #{}: at ({:.2f}, {:.2f}, {:.2f}) after {:.2f} s | {} {} {} | no "
                                         "crosshair target to compare | {} samples, level speed {:.1f} m/s",
                                         flight.impact ? "IMPACT" : "END", flight.serial, stop.x, stop.y, stop.z,
                                         flight.end_time, reason, kind, name, flight.count, level_speed);
                return;
            }

            const Vector3 camera = to_vector(flight.shot.target.camera);
            const Vector3 forward = to_vector(flight.shot.target.forward);
            const Vector3 point = to_vector(flight.shot.target.point);
            const Vector3 seen = stop - camera;
            const float along = dot(seen, forward);
            const auto [right, up] = view_axes(forward);
            const float err_total = angle_deg(seen, forward);
            const float err_right = DMK::math::radians_to_degrees(std::atan2(dot(seen, right), along));
            const float err_up = DMK::math::radians_to_degrees(std::atan2(dot(seen, up), along));
            const float miss = (stop - point).magnitude();
            (void)DMK::log().try_log(
                DMK::LogLevel::Info,
                "Archery {} #{}: at ({:.2f}, {:.2f}, {:.2f}) after {:.2f} s | {} {} {} | {:.3f} m from the crosshair "
                "point ({}) | {:.3f} deg off the crosshair from the release camera (right {:+.3f}, up {:+.3f}) | {} "
                "samples, level speed {:.1f} m/s",
                flight.impact ? "IMPACT" : "END", flight.serial, stop.x, stop.y, stop.z, flight.end_time, reason, kind,
                name, miss, flight.shot.target.hit ? "ray hit" : "ray range end", err_total, err_right, err_up,
                flight.count, level_speed);
            if (flight.shot.landing_traced)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "Archery {} #{} landing: {:.3f} m from the traced landing, {:+.3f} m beside the crosshair",
                    flight.impact ? "IMPACT" : "END", flight.serial,
                    (stop - to_vector(flight.shot.landing)).magnitude(), dot(seen, right));
            }
        }

        /**
         * @brief Ends a flight: clears its s_tracked slot and logs where it landed.
         * @param reason Why the flight ended, for the log.
         */
        void end_flight(Flight &flight, std::string_view reason, std::uint64_t now_ms) noexcept
        {
            flight.active = false;
            flight.end_ms = now_ms;
            // An end without its own time (reuse, a dropped flight) reports the last sample's.
            if (flight.end_time <= 0.0f && flight.count > 0)
            {
                flight.end_time = flight.times[flight.count - 1];
            }
            untrack(flight.shot);
            report_flight(flight, reason);
        }

        /**
         * @brief The end reason of a flight still in the air at k_max_flight_ms, with the limit in seconds.
         * @param buffer Holds the text the returned view points into.
         */
        [[nodiscard]] std::string_view timeout_reason(std::array<char, 48> &buffer) noexcept
        {
            constexpr std::string_view lead = "still flying after ";
            constexpr std::string_view unit = " s";
            char *end = std::copy(lead.begin(), lead.end(), buffer.data());
            end = std::to_chars(end, buffer.data() + buffer.size() - unit.size(), k_max_flight_ms / 1000).ptr;
            end = std::copy(unit.begin(), unit.end(), end);
            return std::string_view{buffer.data(), static_cast<std::size_t>(end - buffer.data())};
        }

        /**
         * @brief Starts to follow a published shot in a flight slot and logs the shot.
         * @details A pooled arrow can be launched again while its previous flight is still followed. That flight ends
         *          on its own (its impact matches by launch point, or sample_flights sees the new launch), so the new
         *          shot's s_tracked slot is never touched. With every slot in flight, the oldest flight ends for the
         *          new one.
         */
        void start_flight(const ShotRecord &shot, std::uint64_t now_ms) noexcept
        {
            // A free slot, else the finished flight that ended first, else (all eight still in the air) the oldest.
            Flight *slot = nullptr;
            for (Flight &flight : s_flights)
            {
                if (!flight.used)
                {
                    slot = &flight;
                    break;
                }
                if (!flight.active && (slot == nullptr || flight.end_ms < slot->end_ms))
                {
                    slot = &flight;
                }
            }
            if (slot == nullptr)
            {
                slot = &s_flights[0];
                for (Flight &flight : s_flights)
                {
                    if (flight.serial < slot->serial)
                    {
                        slot = &flight;
                    }
                }
                end_flight(*slot, "dropped for a newer shot", now_ms);
            }
            Flight &flight = *slot;
            flight.used = true;
            flight.active = true;
            flight.end_seen = false;
            flight.serial = s_next_serial++;
            flight.shot = shot;
            flight.count = 0;
            flight.impact = false;
            flight.impact_object = HitObject{};
            flight.end_time = 0.0f;
            flight.end_ms = 0;
            append_sample(flight, shot.launch, 0.0f);

            const Vec3f &launch = shot.launch;
            const Vec3f &native = shot.native_dir;
            const Vec3f &flown = shot.flown_dir;
            if (shot.target_valid)
            {
                const Vec3f &point = shot.target.point;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "Archery SHOT #{}: {} | {} launched ({:.2f}, {:.2f}, {:.2f}) at {:.1f} m/s, gravity {:.2f} ({}, "
                    "x{:.2f} of {:.2f}), drag {:.3f}, pierce {} | crosshair point ({:.2f}, {:.2f}, {:.2f}) {:.2f} m "
                    "from the camera ({}) | correction {:.2f} deg | dir ({:.3f}, {:.3f}, {:.3f}) -> ({:.3f}, {:.3f}, "
                    "{:.3f}) | projectile {:#x}, thread {}",
                    flight.serial, outcome_text(shot.outcome), ammo_text(shot.ammo), launch.x, launch.y, launch.z,
                    shot.speed, shot.gravity, gravity_text(shot.gravity_change), shot.gravity_scale,
                    shot.native_gravity, shot.air_resistance, shot.pierceability, point.x, point.y, point.z,
                    shot.target.distance, shot.target.hit ? "ray hit" : "ray range end", shot.correction_deg, native.x,
                    native.y, native.z, flown.x, flown.y, flown.z, shot.projectile, shot.thread_id);
                if (shot.landing_traced)
                {
                    const Vec3f &landing = shot.landing;
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Info,
                        "Archery SHOT #{} landing: traced at ({:.2f}, {:.2f}, {:.2f}), {:+.3f} m beside the crosshair "
                        "before the turn, {:+.3f} m after a {:.2f} deg turn",
                        flight.serial, landing.x, landing.y, landing.z, shot.landing_lateral_unturned,
                        shot.landing_lateral, shot.landing_turn_deg);
                }
                else if (shot.landing_turn_refused)
                {
                    (void)DMK::log().try_log(DMK::LogLevel::Info,
                                             "Archery SHOT #{} landing: {:+.3f} m beside the crosshair, the turn to "
                                             "bring it below is beyond MaxAimCorrection",
                                             flight.serial, shot.landing_lateral_unturned);
                }
            }
            else
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "Archery SHOT #{}: {} | {} launched ({:.2f}, {:.2f}, {:.2f}) at {:.1f} m/s, gravity {:.2f} ({}, "
                    "x{:.2f} of {:.2f}) | dir ({:.3f}, {:.3f}, {:.3f}) | projectile {:#x}, thread {}",
                    flight.serial, outcome_text(shot.outcome), ammo_text(shot.ammo), launch.x, launch.y, launch.z,
                    shot.speed, shot.gravity, gravity_text(shot.gravity_change), shot.gravity_scale,
                    shot.native_gravity, native.x, native.y, native.z, shot.projectile, shot.thread_id);
            }
        }

        /**
         * @brief Teaches the aim preview where the bow sat and how fast a full draw flew, from a single shot released
         *        outside free-look.
         * @details The bow's offset from the eye is learned in the release camera's frame, which looks along the look
         *          only outside free-look. Pellets, a faulted shot and a shot without a crosshair target teach nothing.
         */
        void learn_aim_profile(const ShotRecord &shot) noexcept
        {
            if (!shot.target_valid || !shot.target.eye_valid || shot.free_look ||
                shot.outcome == ShotOutcome::Pellets || shot.outcome == ShotOutcome::Faulted)
            {
                return;
            }
            AimProfile &profile = s_aim_profiles[static_cast<std::size_t>(shot.ammo) % s_aim_profiles.size()];
            const Vector3 forward = to_vector(shot.target.forward);
            const auto [right, up] = view_axes(forward);
            const Vector3 from = to_vector(shot.launch) - to_vector(shot.target.eye);
            profile.offset = Vec3f{dot(from, right), dot(from, up), dot(from, forward)};
            if (shot.weapon != profile.weapon)
            {
                profile.weapon = shot.weapon;
                profile.speed = 0.0f;
            }
            profile.speed = std::max(profile.speed, shot.speed);
            profile.native_gravity = shot.native_gravity;
            profile.air_resistance = shot.air_resistance;
            profile.learned = true;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Archery[preview]: {} profile, bow at ({:+.2f}, {:+.2f}, {:+.2f}) m from the eye (right, up, forward), "
                "weapon {:#x} at {:.1f} m/s",
                ammo_text(shot.ammo), profile.offset.x, profile.offset.y, profile.offset.z, profile.weapon,
                profile.speed);
        }

        /** @brief Starts to follow each shot published since the last frame, and teaches the preview from it. */
        void consume_shots(std::uint64_t now_ms) noexcept
        {
            s_shots.drain(
                [now_ms](const ShotRecord &shot) -> void
                {
                    // A shot from frames that did not run this step is too old to follow: clear its s_tracked slot
                    // instead. A shot launched after the frame read its clock is not too old.
                    if (shot.launch_ms <= now_ms && now_ms - shot.launch_ms > k_max_flight_ms)
                    {
                        untrack(shot);
                        return;
                    }
                    start_flight(shot, now_ms);
                    learn_aim_profile(shot);
                });
        }

        /** @brief Ends each followed flight whose impact the collision hook published since the last frame. */
        void consume_impacts(std::uint64_t now_ms) noexcept
        {
            s_impacts.drain(
                [now_ms](const ImpactRecord &impact) -> void
                {
                    Flight *flight = find_active_flight(impact.projectile, impact.launch);
                    if (flight == nullptr)
                    {
                        return;
                    }
                    flight->impact = true;
                    flight->impact_point = impact.point;
                    flight->impact_object = impact.object;
                    flight->end_time = impact.flight_time;
                    append_sample(*flight, impact.point, impact.flight_time);
                    end_flight(*flight, "hit", now_ms);
                });
        }

        /** @brief The projectile fields a flight is followed by. */
        struct ProjectileState
        {
            std::uintptr_t vtable{0};
            std::uint32_t flags{0};
            Vec3f initial{};
            float flight_time{0.0f};
            Vec3f position{};
        };

        /** @brief Reads the fields a flight is followed by with guarded reads, or nothing when one is unreadable. */
        [[nodiscard]] std::optional<ProjectileState> read_projectile_guarded(std::uintptr_t projectile) noexcept
        {
            constexpr std::array<std::ptrdiff_t, 2> world_chain = {
                Constants::PROJECTILE_ENTITY_OFFSET,
                Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER,
            };
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{projectile});
            const auto flags =
                DMK::memory::read<std::uint32_t>(DMK::Address{projectile + Constants::PROJECTILE_FLAGS_OFFSET});
            const auto initial = DMK::memory::read<std::array<float, 3>>(
                DMK::Address{projectile + Constants::PROJECTILE_INITIAL_POS_OFFSET});
            const auto flight_time =
                DMK::memory::read<float>(DMK::Address{projectile + Constants::PROJECTILE_FLIGHT_TIME_OFFSET});
            const auto world_at = DMK::memory::walk(DMK::Address{projectile}, world_chain);
            if (!vtable || !flags || !initial || !flight_time || !world_at)
            {
                return std::nullopt;
            }
            const auto world = DMK::memory::read<GameStructures::Matrix34f>(*world_at);
            if (!world)
            {
                return std::nullopt;
            }
            ProjectileState state{};
            state.vtable = *vtable;
            state.flags = *flags;
            state.initial = to_vec3f(*initial);
            state.flight_time = *flight_time;
            state.position = Vec3f{world->m[0][3], world->m[1][3], world->m[2][3]};
            return state;
        }

        /**
         * @brief Follows each active flight one frame: adds a trail sample, and ends the flight when its projectile is
         *        unreadable or reused, ended without a reported impact, or flew past the flight limit.
         */
        void sample_flights(std::uint64_t now_ms) noexcept
        {
            for (Flight &flight : s_flights)
            {
                if (!flight.used || !flight.active)
                {
                    continue;
                }
                if (flight.end_seen)
                {
                    end_flight(flight, "flight ended without a reported impact", now_ms);
                    continue;
                }
                const std::optional<ProjectileState> state = read_projectile_guarded(flight.shot.projectile);
                if (!state)
                {
                    end_flight(flight, "projectile unreadable", now_ms);
                    continue;
                }
                const Vec3f &launch = flight.shot.launch;
                if (state->vtable != flight.shot.vtable || state->initial.x != launch.x ||
                    state->initial.y != launch.y || state->initial.z != launch.z)
                {
                    end_flight(flight, "projectile reused", now_ms);
                    continue;
                }
                if ((state->flags & (Constants::PROJECTILE_FLAG_COLLIDED | Constants::PROJECTILE_FLAG_DESTROYING |
                                     Constants::PROJECTILE_FLAG_HIDDEN)) != 0)
                {
                    // A hidden arrow is hidden where it stopped, so its position is the best stop point there is. A
                    // destroyed one can already be back in the pool.
                    if ((state->flags & Constants::PROJECTILE_FLAG_DESTROYING) == 0)
                    {
                        append_sample(flight, state->position, state->flight_time);
                    }
                    flight.end_time = state->flight_time;
                    // The collision handler sets the collided bit inside the original, and the hook publishes the
                    // impact only after it returns, so the impact can trail the flag by a frame.
                    flight.end_seen = true;
                    continue;
                }
                append_sample(flight, state->position, state->flight_time);
                if (flight.shot.launch_ms <= now_ms && now_ms - flight.shot.launch_ms > k_max_flight_ms)
                {
                    flight.end_time = state->flight_time;
                    std::array<char, 48> reason{};
                    end_flight(flight, timeout_reason(reason), now_ms);
                }
            }
        }

        /**
         * @brief The function in @p object's vtable slot at @p slot_offset when it is @p expected, or a jmp thunk that
         *        lands on it. 0 otherwise.
         */
        [[nodiscard]] std::uintptr_t validated_slot(std::uintptr_t object, std::ptrdiff_t slot_offset,
                                                    std::uintptr_t expected) noexcept
        {
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{object});
            if (!vtable || expected == 0)
            {
                return 0;
            }
            const auto fn = DMK::memory::read<std::uintptr_t>(DMK::Address{*vtable + slot_offset});
            if (!fn)
            {
                return 0;
            }
            if (*fn == expected)
            {
                return *fn;
            }
            // A jmp thunk in the slot (another layer's hook, or an incremental-link stub) that lands on the function.
            const auto opcode = DMK::memory::read<std::uint8_t>(DMK::Address{*fn});
            if (opcode && *opcode == std::to_integer<std::uint8_t>(DMK::scan::PREFIX_JMP_REL32[0]))
            {
                const auto target =
                    DMK::scan::resolve_rip_relative(DMK::Address{*fn}, DMK::scan::PREFIX_JMP_REL32.size(),
                                                    DMK::scan::PREFIX_JMP_REL32.size() + sizeof(std::int32_t));
                if (target && target->raw() == expected)
                {
                    return *fn;
                }
            }
            return 0;
        }

        /** @brief Calls the renderer's aux-geometry getter through @p fn under SEH. 0 on a fault. */
        [[nodiscard]] std::uintptr_t call_get_aux_guarded(std::uintptr_t fn, std::uintptr_t renderer) noexcept
        {
            __try
            {
                return reinterpret_cast<GetAuxFunc>(fn)(renderer);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /** @brief Sets the aux render flags and returns the ones they replaced in @p previous, under SEH. */
        [[nodiscard]] bool swap_flags_guarded(std::uintptr_t set_fn, std::uintptr_t aux, std::uint32_t flags,
                                              std::uint32_t *previous) noexcept
        {
            __try
            {
                // SetRenderFlags hands back the flags it replaced, so the restore value needs no separate read.
                const AuxFlags wanted{flags};
                AuxFlags old{};
                reinterpret_cast<SetFlagsFunc>(set_fn)(aux, &old, &wanted);
                *previous = old.value;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief Draws @p count vertices as a line list under SEH. False on a fault. */
        [[nodiscard]] bool draw_lines_guarded(std::uintptr_t fn, std::uintptr_t aux, const Vec3f *vertices,
                                              std::uint32_t count, const AuxColor *color, float thickness) noexcept
        {
            __try
            {
                reinterpret_cast<DrawLinesFunc>(fn)(aux, vertices, count, color, thickness);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /** @brief Appends a plus along the world axes, of half size @p half at @p at, to the vertex buffer. */
        void push_cross(std::size_t &n, const Vec3f &at, float half) noexcept
        {
            if (n + 6 > s_vertices.size())
            {
                return;
            }
            s_vertices[n++] = Vec3f{at.x - half, at.y, at.z};
            s_vertices[n++] = Vec3f{at.x + half, at.y, at.z};
            s_vertices[n++] = Vec3f{at.x, at.y - half, at.z};
            s_vertices[n++] = Vec3f{at.x, at.y + half, at.z};
            s_vertices[n++] = Vec3f{at.x, at.y, at.z - half};
            s_vertices[n++] = Vec3f{at.x, at.y, at.z + half};
        }

        /** @brief Appends a ring of @p radius around @p at, in the plane across @p axis, as line segments. */
        void push_ring(std::size_t &n, const Vector3 &at, const Vector3 &axis, float radius) noexcept
        {
            if (n + 2 * k_ring_segments > s_vertices.size())
            {
                return;
            }
            const Vector3 normal = axis.magnitude() > 1e-4f ? axis.normalized() : Vector3{0.0f, 0.0f, 1.0f};
            const Vector3 helper =
                std::fabs(normal.z) < k_ring_vertical_cosine ? Vector3{0.0f, 0.0f, 1.0f} : Vector3{1.0f, 0.0f, 0.0f};
            const Vector3 u = normal.cross(helper).normalized();
            const Vector3 v = normal.cross(u).normalized();
            constexpr float k_step = 2.0f * std::numbers::pi_v<float> / static_cast<float>(k_ring_segments);
            for (int i = 0; i < k_ring_segments; ++i)
            {
                const float a0 = k_step * static_cast<float>(i);
                const float a1 = k_step * static_cast<float>(i + 1);
                s_vertices[n++] = to_vec3f(at + (u * std::cos(a0) + v * std::sin(a0)) * radius);
                s_vertices[n++] = to_vec3f(at + (u * std::cos(a1) + v * std::sin(a1)) * radius);
            }
        }

        /** @brief The frame's aux-geometry buffer and the two calls the trail and the aim preview draw with. */
        struct AuxSession
        {
            std::uintptr_t aux{0};
            std::uintptr_t set_flags{0};
            std::uintptr_t draw_lines{0};
        };

        /** @brief The aux-geometry buffer of the caller's thread, each slot validated, or empty while it is off. */
        [[nodiscard]] std::optional<AuxSession> open_aux() noexcept
        {
            if (s_aux_faulted || s_genv == 0)
            {
                return std::nullopt;
            }
            const auto renderer =
                DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_RENDERER_OFFSET});
            if (!renderer || !DMK::memory::is_plausible_ptr(DMK::Address{*renderer}))
            {
                return std::nullopt;
            }
            const auto renderer_vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*renderer});
            if (!renderer_vtable || !vtable_is(GameClass::Renderer, *renderer_vtable))
            {
                return std::nullopt;
            }
            const std::uintptr_t get_aux =
                validated_slot(*renderer, Constants::RENDERER_VTABLE_GET_AUX_GEOM_OFFSET, s_get_aux_fn);
            if (get_aux == 0)
            {
                return std::nullopt;
            }
            AuxSession session{};
            session.aux = call_get_aux_guarded(get_aux, *renderer);
            if (session.aux == 0)
            {
                return std::nullopt;
            }
            const auto aux_vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{session.aux});
            if (!aux_vtable || !vtable_is(GameClass::AuxGeom, *aux_vtable))
            {
                // CAuxGeomCB_Null: aux geometry is off in this session. Not an error.
                return std::nullopt;
            }
            session.set_flags =
                validated_slot(session.aux, Constants::AUX_VTABLE_SET_RENDER_FLAGS_OFFSET, s_set_flags_fn);
            session.draw_lines = validated_slot(session.aux, Constants::AUX_VTABLE_DRAW_LINES_OFFSET, s_draw_lines_fn);
            if (session.set_flags == 0 || session.draw_lines == 0)
            {
                return std::nullopt;
            }
            return session;
        }

        /** @brief The frame's aux session while no aux-geometry call has faulted, else nullptr. */
        [[nodiscard]] const AuxSession *live_aux(const std::optional<AuxSession> &session) noexcept
        {
            return session && !s_aux_faulted ? &*session : nullptr;
        }

        /** @brief Marks aux geometry faulted for the session and says so once. */
        void aux_faulted() noexcept
        {
            if (!s_aux_faulted)
            {
                s_aux_faulted = true;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Error,
                    "Archery: an aux-geometry call faulted; the arrow trail and aim preview are off for this session");
            }
        }

        /** @brief Puts back the render flags a draw replaced, and marks aux geometry faulted when either faulted. */
        void restore_flags(const AuxSession &session, std::uint32_t previous, bool ok) noexcept
        {
            std::uint32_t replaced = 0;
            if (!swap_flags_guarded(session.set_flags, session.aux, previous, &replaced) || !ok)
            {
                aux_faulted();
            }
        }

        /** @brief @p color with its alpha scaled by @p share (0..1). */
        [[nodiscard]] AuxColor faded(const AuxColor &color, float share) noexcept
        {
            AuxColor out = color;
            out.a = static_cast<std::uint8_t>(std::clamp(share, 0.0f, 1.0f) * static_cast<float>(color.a) + 0.5f);
            return out;
        }

        /** @brief A marker's half size, scaled with its distance from the release camera so it reads far away. */
        [[nodiscard]] float marker_half(const Flight &flight, const Vec3f &at) noexcept
        {
            const Vec3f &camera = flight.shot.target_valid ? flight.shot.target.camera : flight.shot.launch;
            const float distance = (to_vector(at) - to_vector(camera)).magnitude();
            return std::clamp(distance * k_marker_scale, k_marker_min_half, k_marker_max_half);
        }

        /** @brief How long a finished trail stays drawn: ArrowTrailSeconds held to [0, k_max_trail_seconds], in ms. */
        [[nodiscard]] std::uint64_t trail_keep_ms() noexcept
        {
            const float seconds = settings().archery_trail_seconds.load(std::memory_order_relaxed);
            // The comparison also turns a NaN into 0.
            if (!(seconds > 0.0f))
            {
                return 0;
            }
            return static_cast<std::uint64_t>(std::min(seconds, k_max_trail_seconds) * 1000.0f);
        }

        /** @brief True while @p flight is drawn: in the air, or ended within the last @p keep_ms. */
        [[nodiscard]] bool flight_visible(const Flight &flight, std::uint64_t now_ms, std::uint64_t keep_ms) noexcept
        {
            return flight.used && (flight.active || now_ms - flight.end_ms <= keep_ms);
        }

        /**
         * @brief Draws each visible flight: the path, faded out over the last part of ArrowTrailSeconds, a plus at the
         *        crosshair point, a ring that faces @p view where the arrow stopped, and a small plus at the bow.
         */
        void draw_flights(const AuxSession &session, std::uint64_t now_ms, std::uint64_t keep_ms,
                          const Vector3 &view) noexcept
        {
            std::uint32_t previous = 0;
            if (!swap_flags_guarded(session.set_flags, session.aux, Constants::AUX_TRAIL_RENDER_FLAGS, &previous))
            {
                aux_faulted();
                return;
            }
            const float fade_ms = std::max(1.0f, static_cast<float>(keep_ms) * k_trail_fade_share);
            bool ok = true;
            for (const Flight &flight : s_flights)
            {
                if (!ok || !flight_visible(flight, now_ms, keep_ms))
                {
                    continue;
                }
                const float left_ms = flight.active ? fade_ms : static_cast<float>(keep_ms - (now_ms - flight.end_ms));
                const float share = std::clamp(left_ms / fade_ms, 0.0f, 1.0f);
                std::size_t n = 0;
                for (std::size_t i = 1; i < flight.count && n + 2 <= s_vertices.size(); ++i)
                {
                    s_vertices[n++] = flight.points[i - 1];
                    s_vertices[n++] = flight.points[i];
                }
                const AuxColor trail =
                    faded(flight.shot.outcome == ShotOutcome::Redirected ? k_color_redirected : k_color_native, share);
                if (n > 0)
                {
                    ok = draw_lines_guarded(session.draw_lines, session.aux, s_vertices.data(),
                                            static_cast<std::uint32_t>(n), &trail, k_trail_thickness);
                }
                if (ok && flight.shot.target_valid && flight.shot.target.hit)
                {
                    const Vec3f &at = flight.shot.target.point;
                    n = 0;
                    push_cross(n, at, marker_half(flight, at) * k_target_marker_share);
                    const AuxColor color = faded(k_color_target, share);
                    ok = draw_lines_guarded(session.draw_lines, session.aux, s_vertices.data(),
                                            static_cast<std::uint32_t>(n), &color, k_marker_thickness);
                }
                if (ok && flight.impact)
                {
                    const Vector3 at = to_vector(flight.impact_point);
                    n = 0;
                    push_ring(n, at, view - at, marker_half(flight, flight.impact_point) * k_impact_marker_share);
                    const AuxColor color = faded(k_color_impact, share);
                    ok = draw_lines_guarded(session.draw_lines, session.aux, s_vertices.data(),
                                            static_cast<std::uint32_t>(n), &color, k_marker_thickness);
                }
                if (ok)
                {
                    n = 0;
                    push_cross(n, flight.shot.launch, marker_half(flight, flight.shot.launch) * k_launch_marker_share);
                    const AuxColor color = faded(k_color_launch, share);
                    ok = draw_lines_guarded(session.draw_lines, session.aux, s_vertices.data(),
                                            static_cast<std::uint32_t>(n), &color, k_marker_thickness);
                }
            }
            restore_flags(session, previous, ok);
        }

        /** @brief The preview color from the INI (0xRRGGBB) at @p opacity. */
        [[nodiscard]] AuxColor preview_color(float opacity) noexcept
        {
            const std::uint32_t rgb = settings().archery_aim_preview_color.load(std::memory_order_relaxed);
            return AuxColor{
                .r = static_cast<std::uint8_t>((rgb >> 16) & 0xFFu),
                .g = static_cast<std::uint8_t>((rgb >> 8) & 0xFFu),
                .b = static_cast<std::uint8_t>(rgb & 0xFFu),
                .a = static_cast<std::uint8_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f + 0.5f),
            };
        }

        /**
         * @brief Draws a predicted flight in the preview style: a dashed arc from @p from at @p velocity (with
         *        AimPreviewArc) and a ring on @p landing when it has one.
         * @param session The open aux-geometry session.
         * @param camera Where the view is rendered from, which sizes the dashes and the ring.
         * @param physics The gravity and drag the arc falls by.
         * @param normal The surface normal at @p landing, which the ring lies on.
         * @param style The line thickness multiple and the least ring radius.
         */
        void draw_predicted_flight(const AuxSession &session, const Vector3 &camera, const Vector3 &from,
                                   const Vector3 &velocity, const FlightPhysics &physics,
                                   const std::optional<Vector3> &landing, const Vector3 &normal,
                                   const PreviewStyle &style) noexcept
        {
            const LiveSettings &cfg = settings();
            const bool arc = cfg.archery_aim_preview_arc.load(std::memory_order_relaxed);
            const float speed = velocity.magnitude();
            if ((!landing && !arc) || speed < 1e-3f)
            {
                return;
            }
            const std::uint32_t flags_wanted = cfg.archery_aim_preview_through_walls.load(std::memory_order_relaxed)
                                                   ? Constants::AUX_TRAIL_RENDER_FLAGS
                                                   : Constants::AUX_PREVIEW_RENDER_FLAGS;
            std::uint32_t previous = 0;
            if (!swap_flags_guarded(session.set_flags, session.aux, flags_wanted, &previous))
            {
                aux_faulted();
                return;
            }
            const AuxColor base = preview_color(cfg.archery_aim_preview_opacity.load(std::memory_order_relaxed));
            bool ok = true;

            if (arc)
            {
                // Walk the arc in short samples to the landing's level distance (or the sky length) and lay dashes
                // along it, cut at the dash edges, in a few alpha bands: faded in near the start, and out at the end
                // of a sky shot. The dash phase is carried in periods, so a period that grows with the distance from
                // the camera stretches the dashes evenly. A full band is drawn at once and refilled.
                std::array<std::size_t, k_preview_bands> counts{};
                std::array<std::array<Vec3f, k_preview_band_vertices>, k_preview_bands> bands{};
                const auto flush = [&](std::size_t band) -> void
                {
                    if (counts[band] > 0)
                    {
                        const AuxColor color =
                            faded(base, (static_cast<float>(band) + 1.0f) / static_cast<float>(k_preview_bands));
                        ok = ok && draw_lines_guarded(session.draw_lines, session.aux, bands[band].data(),
                                                      static_cast<std::uint32_t>(counts[band]), &color,
                                                      k_preview_thickness * style.line_scale);
                        counts[band] = 0;
                    }
                };
                const auto share_at = [&](float travelled) -> float
                {
                    float share = std::clamp(travelled / k_preview_fade_in, 0.0f, 1.0f);
                    if (!landing)
                    {
                        share = std::min(share, std::clamp((k_preview_sky_length - travelled) /
                                                               (k_preview_sky_length * k_preview_sky_fade_share),
                                                           0.0f, 1.0f));
                    }
                    return share;
                };
                const float land_reach = landing ? std::sqrt((landing->x - from.x) * (landing->x - from.x) +
                                                             (landing->y - from.y) * (landing->y - from.y))
                                                 : 0.0f;
                const float dt = k_preview_sample / speed;
                Vector3 previous_point = from;
                float travelled = 0.0f;
                float phase = 0.0f;
                for (int i = 1; ok && i <= k_preview_max_samples; ++i)
                {
                    const Vector3 next =
                        ballistic_position(from, velocity, physics.gravity, physics.drag, dt * static_cast<float>(i));
                    const float nx = next.x - from.x;
                    const float ny = next.y - from.y;
                    const bool last =
                        landing ? std::sqrt(nx * nx + ny * ny) >= land_reach : travelled >= k_preview_sky_length;
                    const Vector3 end = last && landing ? *landing : next;
                    const Vector3 step = end - previous_point;
                    const float length = step.magnitude();
                    const float period = std::max(k_preview_min_dash_period,
                                                  (previous_point - camera).magnitude() * k_preview_dash_period);
                    float done = 0.0f;
                    Vector3 piece_start = previous_point;
                    while (ok && done < length)
                    {
                        const float frac = phase - std::floor(phase);
                        const bool on = frac < k_preview_dash_on;
                        const float to_edge = ((on ? k_preview_dash_on : 1.0f) - frac) * period;
                        const float piece = std::max(1e-4f, std::min(to_edge, length - done));
                        const Vector3 piece_end = previous_point + step * ((done + piece) / length);
                        const float share = share_at(travelled + done);
                        if (on && share > k_preview_min_share)
                        {
                            const auto band = static_cast<std::size_t>(
                                std::min(static_cast<float>(k_preview_bands - 1), share * k_preview_bands));
                            if (counts[band] + 2 > bands[band].size())
                            {
                                flush(band);
                            }
                            bands[band][counts[band]++] = to_vec3f(piece_start);
                            bands[band][counts[band]++] = to_vec3f(piece_end);
                        }
                        phase += piece / period;
                        done += piece;
                        piece_start = piece_end;
                    }
                    travelled += length;
                    previous_point = end;
                    if (last)
                    {
                        break;
                    }
                }
                for (std::size_t band = 0; band < bands.size(); ++band)
                {
                    flush(band);
                }
            }

            if (ok && landing)
            {
                // The ring lies on the surface, lifted off it so the depth test does not swallow it.
                const float distance = (*landing - camera).magnitude();
                const float radius = std::clamp(distance * k_preview_dash_scale, style.min_ring,
                                                std::max(style.min_ring, k_preview_max_ring));
                const Vector3 surface = normal.magnitude() > 1e-4f ? normal.normalized() : Vector3{0.0f, 0.0f, 1.0f};
                const Vector3 at =
                    *landing + surface * std::max(k_preview_min_ring_lift, radius * k_preview_ring_lift_share);
                std::size_t n = 0;
                push_ring(n, at, surface, radius);
                push_ring(n, at, surface, radius * k_preview_inner_ring_share);
                ok = draw_lines_guarded(session.draw_lines, session.aux, s_vertices.data(),
                                        static_cast<std::uint32_t>(n), &base,
                                        k_preview_ring_thickness * style.line_scale);
            }
            restore_flags(session, previous, ok);
        }

        /**
         * @brief Predicts a shot from @p from re-aimed at the crosshair point as the Launch detour flies it: its launch
         *        direction and its landing.
         * @details The landing is traced from @p from moved into the camera's vertical plane, where the Launch detour
         *          turns the real shot to land, and the launch direction is the elevation of the line to the point,
         *          turned toward that landing. A turn past @p max_correction from the game's @p native aim is refused
         *          as the Launch detour refuses it: the straight line to the point then flies, traced from @p from.
         * @param landing Receives the landing, or nothing when the arc meets nothing in reach.
         * @param normal Receives the surface normal at the landing.
         */
        [[nodiscard]] Vector3 predict_below_crosshair(const AimTarget &target, const Vector3 &from,
                                                      const Vector3 &native, float max_correction,
                                                      const FlightPhysics &physics, std::uintptr_t skip,
                                                      std::optional<Vector3> &landing, Vector3 &normal) noexcept
        {
            const unsigned int flags = arrow_ray_flags(physics.pierceability);
            const Vector3 camera = to_vector(target.camera);
            const Vector3 point = to_vector(target.point);
            const Vector3 right = view_axes(to_vector(target.forward)).first;
            const Vector3 in_plane = from - right * dot(from - camera, right);
            landing = trace_landing(in_plane, (point - in_plane).normalized() * physics.speed, physics.gravity,
                                    physics.drag, flags, skip, &normal);
            const Vector3 straight = (point - from).normalized();
            Vector3 dir = straight;
            if (landing)
            {
                const float gx = landing->x - from.x;
                const float gy = landing->y - from.y;
                const float reach = std::sqrt(gx * gx + gy * gy);
                const float level = std::sqrt(straight.x * straight.x + straight.y * straight.y);
                if (reach > 1e-3f)
                {
                    dir = Vector3{gx / reach * level, gy / reach * level, straight.z};
                }
            }
            if (angle_deg(native, dir) > max_correction)
            {
                dir = straight;
                landing =
                    trace_landing(from, straight * physics.speed, physics.gravity, physics.drag, flags, skip, &normal);
            }
            return dir;
        }

        /** @brief The player's first-person look direction (the look controller quaternion's forward), or nothing. */
        [[nodiscard]] std::optional<Vector3> player_look(std::uintptr_t c_player) noexcept
        {
            const std::array<std::ptrdiff_t, 2> chain = {
                runtime_offsets().c_player_look_controller.load().value,
                Constants::LOOK_CONTROLLER_QUAT_OFFSET,
            };
            const auto quat_at = DMK::memory::walk(DMK::Address{c_player}, chain);
            if (!quat_at)
            {
                return std::nullopt;
            }
            const auto quat = DMK::memory::read<std::array<float, 4>>(*quat_at);
            if (!quat)
            {
                return std::nullopt;
            }
            const auto [qx, qy, qz, qw] = *quat;
            // The quaternion's rotated local forward (+y) axis.
            const Vector3 look{2.0f * (qx * qy - qw * qz), 1.0f - 2.0f * (qx * qx + qz * qz),
                               2.0f * (qy * qz + qw * qx)};
            if (!is_finite(look) || look.magnitude() < 1e-3f)
            {
                return std::nullopt;
            }
            return look.normalized();
        }

        /**
         * @brief While the bow is held drawn in third person, draws where the shot comes down: a ring on the spot and,
         *        with AimPreviewArc, a dashed arc from the bow.
         * @details The shot is predicted as the Launch detour flies it: from the bow (placed from the first-person eye
         *          where the earlier shots with this weapon family sat), at the fastest speed seen with this weapon (or
         *          defaults), by the weapon's gravity. With AimAtCrosshair on, outside free-look, and the crosshair
         *          point clear of the bow and within MaxAimCorrection of its aim, the shot is re-aimed at the point and
         *          turned to land below the crosshair. Otherwise the game's own shot flies along the look, which the
         *          bow's axis follows, and the preview traces that. Scatter shot has no preview: its pellets keep the
         *          game's spread, which no single arc shows.
         * @param skip The player's own physics, which the trace passes through.
         */
        void draw_aim_preview(const AuxSession &session, std::uintptr_t c_player, const AimTarget &target,
                              std::uintptr_t skip) noexcept
        {
            if (!target.eye_valid || !player_missile_drawn(c_player))
            {
                return;
            }
            const std::optional<std::uint32_t> ammo_class = ammo_class_of(player_missile_ammo(c_player));
            if (ammo_class == Constants::AMMO_CLASS_SCATTER_SHOT)
            {
                return;
            }
            const std::optional<Vector3> look = player_look(c_player);
            if (!look)
            {
                return;
            }
            const AmmoKind kind = ammo_kind(ammo_class);
            const auto kind_index = static_cast<std::size_t>(kind);
            const AimProfile &profile = s_aim_profiles[kind_index % s_aim_profiles.size()];
            const bool same_weapon = profile.learned && profile.weapon == player_missile_weapon(c_player);
            const Vec3f offset = profile.learned ? profile.offset : k_default_bow_offset;
            FlightPhysics physics{};
            physics.speed = same_weapon && profile.speed > k_min_trace_speed
                                ? profile.speed
                                : k_default_speed[kind_index % k_default_speed.size()];
            const float native_gravity =
                profile.learned && profile.native_gravity > 0.0f ? profile.native_gravity : k_default_gravity;
            physics.gravity = native_gravity * gravity_scale_for(kind);
            physics.drag = profile.learned ? profile.air_resistance : 0.0f;
            physics.pierceability = s_pierceability.load(std::memory_order_relaxed);

            // The profile placed the bow in the release camera's frame, which looks along the look outside free-look.
            // In free-look the camera turns away from the look, so the bow is placed in the look's own frame.
            const bool free_look = camera_state().orbit_active.load(std::memory_order_relaxed);
            const Vector3 frame = free_look ? *look : to_vector(target.forward);
            const auto [right, up] = view_axes(frame);
            const Vector3 bow = to_vector(target.eye) + right * offset.x + up * offset.y + frame * offset.z;
            const Vector3 point = to_vector(target.point);
            const float max_correction = max_aim_correction();
            Vector3 normal{0.0f, 0.0f, 1.0f};
            std::optional<Vector3> landing;
            Vector3 dir = *look;
            // The re-aim prepare_shot applies, under its conditions: every other shot flies the game's own aim.
            if (settings().archery_aim_at_crosshair.load(std::memory_order_relaxed) && !free_look && is_finite(point) &&
                (point - bow).magnitude() >= k_min_target_distance &&
                angle_deg(*look, (point - bow).normalized()) <= max_correction)
            {
                dir = predict_below_crosshair(target, bow, *look, max_correction, physics, skip, landing, normal);
            }
            else
            {
                landing = trace_landing(bow, dir * physics.speed, physics.gravity, physics.drag,
                                        arrow_ray_flags(physics.pierceability), skip, &normal);
            }
            draw_predicted_flight(session, to_vector(target.camera), bow, dir * physics.speed, physics, landing, normal,
                                  k_arrow_preview_style);
        }

        /** @brief Reads a distraction stone's flight from its ammo, or nothing when the ammo cannot be read. */
        [[nodiscard]] std::optional<FlightPhysics> read_decoy_flight(std::uintptr_t decoy) noexcept
        {
            const std::uintptr_t ammo = ammo_params_of(decoy);
            if (ammo == 0)
            {
                return std::nullopt;
            }
            const auto speed = DMK::memory::read<float>(DMK::Address{ammo + Constants::AMMO_SPEED_OFFSET});
            const std::optional<AmmoTemplate> raw = read_ammo_template(ammo);
            // The comparisons also reject a NaN speed.
            if (!speed || !(*speed >= k_min_decoy_speed && *speed <= k_max_decoy_speed) || !raw)
            {
                return std::nullopt;
            }
            FlightPhysics flight = checked_physics(raw);
            flight.speed = *speed;
            return flight;
        }

        /**
         * @brief While a distraction stone is held ready, draws where the throw comes down, as the game throws it: a
         *        ring on the spot and, with AimPreviewArc, a dashed arc from the hand.
         * @details The game launches the stone from where it leaves the hand, along the look turned about the vertical
         *          by atan(DISTRACT_HAND_OFFSET / range), at the ammo's speed under its gravity. The range is the level
         *          distance the throw covers back down to the feet, the game's own formula. The stone leaves the hand
         *          a little ahead of where it is held, by the offset the throws so far showed. Everything else is read
         *          live, so in first person the preview is the game's own throw. In third person, with AimAtCrosshair
         *          on and the crosshair point within MaxAimCorrection, it is the throw re-aimed at the crosshair as
         *          the Launch detour flies it.
         * @param session The frame's aux session, or nullptr. The held spot and the prediction reach the stone's
         *        Launch either way.
         * @param target This frame's crosshair target, used when @p fresh.
         * @param camera Where the view is rendered from.
         * @param skip The player's own physics, which the trace passes through.
         */
        void draw_decoy_preview(const AuxSession *session, std::uintptr_t c_player, const HeldDecoy &stone,
                                const FlightPhysics &flight, const AimTarget &target, bool fresh, const Vector3 &camera,
                                std::uintptr_t skip) noexcept
        {
            if (stone.decoy == 0 || !stone.ready)
            {
                return;
            }
            const auto held = DMK::memory::read<std::array<float, 3>>(
                DMK::Address{stone.decoy + Constants::PROJECTILE_POSITION_OFFSET});
            if (!held || !is_finite(to_vector(to_vec3f(*held))))
            {
                return;
            }
            const Vec3f held_at = to_vec3f(*held);

            // The look the throw follows, and the feet the range is measured down to.
            const std::optional<Vector3> look = player_look(c_player);
            const std::array<std::ptrdiff_t, 2> world_chain = {
                runtime_offsets().c_player_entity.load().value,
                Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER,
            };
            const auto world_at = DMK::memory::walk(DMK::Address{c_player}, world_chain);
            if (!look || !world_at)
            {
                return;
            }
            const auto world = DMK::memory::read<GameStructures::Matrix34f>(*world_at);
            if (!world)
            {
                return;
            }
            const float yaw = std::atan2(-look->x, look->y);
            const float right = s_decoy_release_right.load(std::memory_order_relaxed);
            const float ahead = s_decoy_release_forward.load(std::memory_order_relaxed);
            const Vector3 from{held_at.x + right * std::cos(yaw) - ahead * std::sin(yaw),
                               held_at.y + right * std::sin(yaw) + ahead * std::cos(yaw),
                               held_at.z + s_decoy_release_up.load(std::memory_order_relaxed)};
            const float pitch = std::asin(std::clamp(look->z, -1.0f, 1.0f));
            const float rise = flight.speed * std::sin(pitch);
            const float reach = rise * rise + 2.0f * flight.gravity * (from.z - world->m[2][3]);
            float turn = 0.0f;
            if (reach >= 0.0f)
            {
                const float range = flight.speed * std::cos(pitch) / flight.gravity * (rise + std::sqrt(reach));
                if (range > 1e-3f)
                {
                    turn = std::atan(Constants::DISTRACT_HAND_OFFSET / range);
                }
            }
            const Vector3 thrown{std::cos(turn) * look->x - std::sin(turn) * look->y,
                                 std::sin(turn) * look->x + std::cos(turn) * look->y, look->z};
            Vector3 normal{0.0f, 0.0f, 1.0f};
            std::optional<Vector3> landing;
            Vector3 dir = thrown;
            bool aimed = false;
            // In third person the Launch detour re-aims the throw at the crosshair, within MaxAimCorrection.
            if (fresh && settings().archery_aim_at_crosshair.load(std::memory_order_relaxed) &&
                !camera_state().orbit_active.load(std::memory_order_relaxed))
            {
                const Vector3 point = to_vector(target.point);
                const float max_correction = max_aim_correction();
                if (is_finite(point) && (point - from).magnitude() >= k_min_target_distance &&
                    angle_deg(thrown, (point - from).normalized()) <= max_correction)
                {
                    dir = predict_below_crosshair(target, from, thrown, max_correction, flight, skip, landing, normal);
                    aimed = true;
                }
            }
            if (!aimed)
            {
                landing = trace_landing(from, dir * flight.speed, flight.gravity, flight.drag,
                                        arrow_ray_flags(flight.pierceability), skip, &normal);
            }

            // One record after the trace, so the stone's Launch learns from and logs a single frame's prediction.
            DecoyPose pose{};
            pose.decoy = stone.decoy;
            pose.held = held_at;
            pose.yaw = yaw;
            pose.predicted_from = to_vec3f(from);
            pose.predicted_dir = to_vec3f(dir);
            pose.predicted_landing = landing ? to_vec3f(*landing) : Vec3f{};
            pose.landed = landing.has_value();
            s_decoy_pose.store(pose);
            s_decoy_held.store(stone.decoy, std::memory_order_relaxed);

            if (session != nullptr)
            {
                draw_predicted_flight(*session, camera, from, dir * flight.speed, flight, landing, normal,
                                      k_decoy_preview_style);
            }
        }

        /**
         * @brief Installs one inline hook, stores it in @p hooks, and publishes its trampoline before it is armed.
         * @throws std::bad_alloc If the hook name or the hook set cannot allocate.
         */
        template <typename Func, typename Detour>
        [[nodiscard]] DMK::Result<void> install(HookSet &hooks, std::string_view name, std::uintptr_t target,
                                                std::atomic<Func> &original, Detour detour)
        {
            auto result = DMK::hook::inline_at(
                DMK::hook::InlineRequest{
                    .name = std::string{name},
                    .target = DMK::Address{target},
                },
                detour);
            if (!result.has_value())
            {
                return std::unexpected(result.error());
            }
            original.store(result->template original<Func>(), std::memory_order_release);
            if (auto armed = hooks.push(std::move(*result)).enable(); !armed.has_value())
            {
                return std::unexpected(armed.error());
            }
            return {};
        }

    } // namespace

    DMK::Result<void> initialize_archery_hook(std::uintptr_t g_env, HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();
        s_genv = g_env;

        const std::uintptr_t fire = gated_anchor_address(Feature::ArcheryAim, AnchorId::FireProjectile);
        const std::uintptr_t launch = gated_anchor_address(Feature::ArcheryAim, AnchorId::ProjectileLaunch);
        const std::optional<std::int64_t> physics_slot =
            gated_anchor_value(Feature::ArcheryAim, AnchorId::GetPhysicsSlot);
        if (fire == 0 || launch == 0 || !physics_slot)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "archery_hook/aim_anchors"});
        }
        s_get_physics_offset = static_cast<std::ptrdiff_t>(*physics_slot);

        // Launch first: a shot can only be marked by FireProjectile once its Launch detour is live.
        if (auto armed = install(hooks, "ProjectileLaunch", launch, s_launch_original, launch_detour); !armed)
        {
            return armed;
        }
        if (auto armed = install(hooks, "FireProjectile", fire, s_fire_original, fire_projectile_detour); !armed)
        {
            return armed;
        }
        s_aim_ready.store(true, std::memory_order_release);

        // The trail is a second, independent feature: a failure here only warns.
        const std::uintptr_t collision = gated_anchor_address(Feature::ArcheryTrail, AnchorId::ArrowCollision);
        s_get_aux_fn = gated_anchor_address(Feature::ArcheryTrail, AnchorId::GetAuxGeom);
        s_set_flags_fn = gated_anchor_address(Feature::ArcheryTrail, AnchorId::AuxSetFlags);
        s_draw_lines_fn = gated_anchor_address(Feature::ArcheryTrail, AnchorId::AuxDrawLines);
        if (collision == 0 || g_env == 0)
        {
            logger.warning("Archery: the trail gate failed; arrows are still aimed at the crosshair, without a trail");
        }
        else if (auto armed = install(hooks, "ArrowCollision", collision, s_collision_original, arrow_collision_detour);
                 !armed)
        {
            logger.warning("Archery: the arrow collision hook failed ({}); arrows are still aimed at the crosshair, "
                           "without a trail",
                           armed.error().message());
        }
        else
        {
            s_trail_ready.store(true, std::memory_order_release);
        }

        logger.info("Archery: ready (FireProjectile {}, Launch {}, GetPhysics slot {:#x}, trail {})",
                    DMK::format::format_address(fire), DMK::format::format_address(launch), s_get_physics_offset,
                    s_trail_ready.load(std::memory_order_relaxed) ? "available" : "off");
        return {};
    }

    void archery_frame(std::uintptr_t c_player, const Vector3 &eye, bool eye_valid) noexcept
    {
        if (!s_aim_ready.load(std::memory_order_acquire))
        {
            return;
        }
        s_player.store(c_player, std::memory_order_relaxed);
        std::uint32_t player_id = 0;
        if (c_player != 0)
        {
            const auto id =
                DMK::memory::read<std::uint32_t>(DMK::Address{c_player + Constants::ACTOR_ENTITY_ID_OFFSET});
            player_id = id ? *id : 0u;
        }
        s_player_id.store(player_id, std::memory_order_relaxed);
        const std::uint64_t now_ms = GetTickCount64();
        // A distraction stone in hand is aimed at the crosshair like a missile weapon (see redirect_decoy).
        const HeldDecoy stone = player_held_decoy(c_player);
        const std::optional<FlightPhysics> stone_flight =
            stone.decoy != 0 ? read_decoy_flight(stone.decoy) : std::optional<FlightPhysics>{};
        s_decoy_in_hand.store(stone_flight ? stone.decoy : 0, std::memory_order_relaxed);
        const bool armed = c_player != 0 && (stone_flight.has_value() || missile_weapon_in_hand(c_player));
        // The player's own physics, which every ray of this frame passes through.
        const std::uintptr_t skip = armed ? player_physics(c_player) : 0;
        update_aim_target(eye, eye_valid, now_ms, armed, skip,
                          stone_flight ? stone_flight->pierceability : s_pierceability.load(std::memory_order_relaxed));
        consume_shots(now_ms);
        consume_impacts(now_ms);
        sample_flights(now_ms);
        // This frame's crosshair target, when there is one (third person, a missile weapon or a distraction stone in
        // hand). update_aim_target either stores this frame's target or withdraws it, so a loaded target is fresh.
        AimTarget target{};
        const bool fresh = s_target.load(target);
        // The trail's impact ring and the stone preview face the rendered camera: the published pose, or in first
        // person the eye itself.
        Vector3 view = eye;
        float cx = 0.0f, cy = 0.0f, cz = 0.0f, dx = 0.0f, dy = 0.0f, dz = 0.0f;
        if (fresh)
        {
            view = to_vector(target.camera);
        }
        else if (interaction_aim_pose().load(cx, cy, cz, dx, dy, dz))
        {
            view = Vector3{cx, cy, cz};
        }

        const LiveSettings &cfg = settings();
        const bool trail_ready = s_trail_ready.load(std::memory_order_acquire);
        const std::uint64_t keep_ms = trail_keep_ms();
        const bool trail = trail_ready && cfg.archery_show_trail.load(std::memory_order_relaxed) &&
                           std::any_of(s_flights.begin(), s_flights.end(),
                                       [&](const Flight &flight) { return flight_visible(flight, now_ms, keep_ms); });
        const bool preview = trail_ready && cfg.archery_show_aim_preview.load(std::memory_order_relaxed);
        const bool aim_preview = preview && fresh;
        const bool decoy_preview = preview && stone_flight.has_value();
        if (!trail && !aim_preview && !decoy_preview)
        {
            return;
        }
        // One aux session serves every draw of the frame. A draw that faults ends it for the rest (live_aux).
        const std::optional<AuxSession> session = open_aux();
        if (const AuxSession *aux = live_aux(session); trail && aux != nullptr)
        {
            draw_flights(*aux, now_ms, keep_ms, view);
        }
        if (const AuxSession *aux = live_aux(session); aim_preview && aux != nullptr)
        {
            draw_aim_preview(*aux, c_player, target, skip);
        }
        if (decoy_preview)
        {
            draw_decoy_preview(live_aux(session), c_player, stone, *stone_flight, target, fresh, view, skip);
        }
    }

    bool archery_wants_frame(std::uintptr_t c_player) noexcept
    {
        if (!s_aim_ready.load(std::memory_order_acquire))
        {
            return false;
        }
        if (s_shots.pending())
        {
            return true;
        }
        const LiveSettings &cfg = settings();
        const bool trail_ready = s_trail_ready.load(std::memory_order_acquire);
        if (c_player != 0 && trail_ready && cfg.archery_show_aim_preview.load(std::memory_order_relaxed) &&
            player_held_decoy(c_player).decoy != 0)
        {
            return true;
        }
        const bool trail = trail_ready && cfg.archery_show_trail.load(std::memory_order_relaxed);
        const std::uint64_t keep_ms = trail_keep_ms();
        const std::uint64_t now_ms = GetTickCount64();
        for (const Flight &flight : s_flights)
        {
            if ((flight.used && flight.active) || (trail && flight_visible(flight, now_ms, keep_ms)))
            {
                return true;
            }
        }
        return false;
    }

    void release_archery() noexcept
    {
        // The flags alone stop the re-aim, the impact hook and the frame step. The target channel and the tracking
        // slots stay as they are: the frame step writes the channel, publish_shot and untrack write the slots, and a
        // writer can still be inside a detour. A Launch under a marked s_fire_thread re-checks s_aim_ready first.
        s_aim_ready.store(false, std::memory_order_release);
        s_trail_ready.store(false, std::memory_order_release);
    }

} // namespace TPVCamera
