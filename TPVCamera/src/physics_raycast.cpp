/**
 * @file physics_raycast.cpp
 * @brief Implementation of the IPhysicalWorld::RayWorldIntersection wrapper.
 */

#include "physics_raycast.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <cmath>
#include <cstring>
#include <intrin.h>

namespace TPVCamera
{

    // IPhysicalWorld::RayWorldIntersection inline helper (see constants.hpp for the ABI). org
    // and dir are passed by pointer (Vector3 is three packed floats, matching the engine Vec3).
    using RayWorldIntersectionFn = int(__fastcall *)(void *physical_world, const Vector3 *org, const Vector3 *dir,
                                                     int objtypes, unsigned int flags, void *hits, int n_max_hits,
                                                     void *skip_ents, int num_skip_ents, void *foreign_data,
                                                     int foreign_index, const char *name_tag);

    static RayWorldIntersectionFn s_ray_world_intersection = nullptr;

    // Address of the IPhysicalWorld* global (read fresh per ray, not cached: it is null until a
    // level loads and is replaced across level transitions).
    static uintptr_t s_physical_world_global_addr = 0;

    // Mapped range of the scanned game module (WHGame.dll), captured once at init. The sphere-sweep path
    // confirms a freshly resolved world vtable / PWI function pointer lives inside the game image with a
    // branch-only contains() test (no syscall): a stale or reallocated world pointer yields a vtable slot
    // that does not point into the image, and calling through it must be rejected before the indirect call.
    static DMK::Region s_game_module{};

    bool initialize_physics_raycast(uintptr_t module_base, size_t module_size, uintptr_t g_env)
    {
        DMK::Logger &logger = DMK::log();

        // The module-scoped cascade resolved the helper inside the game image (or 0 on a miss); read here
        // from the anchor registry resolved at startup by resolve_all_anchors().
        const uintptr_t ray_fn = gated_anchor_address(Feature::Collision, AnchorId::RayWorldIntersection);
        if (ray_fn == 0 || g_env == 0)
        {
            logger.warning("PhysicsRaycast: RayWorldIntersection or g_env not found (game patched?); collision/aim "
                           "raycast unavailable");
            return false;
        }

        s_ray_world_intersection = reinterpret_cast<RayWorldIntersectionFn>(ray_fn);
        // p_physical_world is a member of the g_env struct (see PHYSICAL_WORLD_OFFSET); deriving its
        // slot from the patch-resiliently resolved g_env base avoids a second hardcoded address.
        s_physical_world_global_addr = g_env + Constants::PHYSICAL_WORLD_OFFSET;
        s_game_module = DMK::Region{DMK::Address{module_base}, module_size};

        logger.info("PhysicsRaycast: RayWorldIntersection at {}, p_physical_world slot at {}",
                    DMK::format::format_address(ray_fn), DMK::format::format_address(s_physical_world_global_addr));
        return true;
    }

    /**
     * @brief SEH-isolated engine call. Held apart from the C++ caller so the structured
     *        handler shares no frame with object unwinding; a fault becomes "no hit".
     */
    static int ray_world_intersection_guarded(void *physical_world, const Vector3 *org, const Vector3 *dir,
                                              int objtypes, unsigned int flags, void *hits, void *skip_ents,
                                              int n_skip_ents)
    {
        __try
        {
            return s_ray_world_intersection(physical_world, org, dir, objtypes, flags, hits, 1, skip_ents, n_skip_ents,
                                            nullptr, 0, "TPVCameraRay");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }

    std::optional<RayHit> ray_world_intersection(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &direction,
                                                 int objtypes, unsigned int flags, const uintptr_t *skip_ents,
                                                 int n_skip_ents)
    {
        if (s_ray_world_intersection == nullptr || s_physical_world_global_addr == 0)
        {
            return std::nullopt;
        }

        // Resolve the physical world fresh; bail cleanly while it is null (no level / loading).
        const auto world_value = DMK::memory::read<uintptr_t>(DMK::Address{s_physical_world_global_addr});
        if (!world_value || *world_value == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{*world_value}))
        {
            return std::nullopt;
        }

        alignas(16) std::byte hit_buffer[Constants::RAY_HIT_SIZE];
        std::memset(hit_buffer, 0, sizeof(hit_buffer));

        const std::int64_t ledger_started = QueryLedger::begin();
        const int hit_count = ray_world_intersection_guarded(
            reinterpret_cast<void *>(*world_value), &origin, &direction, objtypes, flags, hit_buffer,
            const_cast<uintptr_t *>(skip_ents), (skip_ents != nullptr) ? n_skip_ents : 0);
        QueryLedger::end(tag, QueryLedger::Kind::Physics, ledger_started);
        if (hit_count < 1)
        {
            return std::nullopt;
        }

        RayHit hit{};
        hit.m_distance = *reinterpret_cast<const float *>(hit_buffer + Constants::RAY_HIT_OFFSET_DISTANCE);
        hit.m_point = *reinterpret_cast<const Vector3 *>(hit_buffer + Constants::RAY_HIT_OFFSET_POINT);
        hit.m_normal = *reinterpret_cast<const Vector3 *>(hit_buffer + Constants::RAY_HIT_OFFSET_NORMAL);
        hit.m_collider = *reinterpret_cast<const uintptr_t *>(hit_buffer + Constants::RAY_HIT_OFFSET_COLLIDER);
        hit.m_terrain = *reinterpret_cast<const int *>(hit_buffer + Constants::RAY_HIT_OFFSET_TERRAIN);
        hit.m_part = *reinterpret_cast<const int *>(hit_buffer + Constants::RAY_HIT_OFFSET_PART);
        hit.m_surface = *reinterpret_cast<const std::int16_t *>(hit_buffer + Constants::RAY_HIT_OFFSET_SURFACE);
        return hit;
    }

    // IPhysicalWorld::PrimitiveWorldIntersection (consolidated SPWIParams form). The float return value
    // is the distance to first contact for a sweep; resolved from the live world vtable (see constants).
    using PrimitiveWorldIntersectionFn = float(__fastcall *)(void *physical_world, void *pp, void *p_lock_contacts,
                                                             const char *name_tag);

    /**
     * @brief SEH-isolated PWI engine call, held apart from the C++ caller so a fault becomes "no hit".
     */
    static float primitive_world_intersection_guarded(PrimitiveWorldIntersectionFn fn, void *world, void *pp)
    {
        __try
        {
            return fn(world, pp, nullptr, "TPVCameraSweep");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0.0f;
        }
    }

    /**
     * @brief Releases the SPWIParams WriteLockCond (InterlockedAdd(prw, -iActive)) under an SEH frame.
     * @details The engine repoints prw and sets iActive only when it took the real global world lock;
     *          otherwise prw stays the self-pointer and iActive is 0 (a no-op). prw is read back from the
     *          engine-written params and screened with memory::is_plausible_ptr, but a stale-but-plausible
     *          prw (the world counter concurrently invalidated) would fault on the atomic store, so the
     *          store runs under __try. POD-only body so the SEH frame shares no C++ unwinding.
     */
    static void release_spwi_write_lock_guarded(std::byte *params) noexcept
    {
        __try
        {
            auto *prw = *reinterpret_cast<volatile long **>(params + Constants::SPWI_OFF_LOCK_PRW);
            const long active = *reinterpret_cast<volatile long *>(params + Constants::SPWI_OFF_LOCK_IACTIVE);
            if (prw && DMK::memory::is_plausible_ptr(DMK::Address{reinterpret_cast<uintptr_t>(prw)}) && active != 0)
            {
                _InterlockedExchangeAdd(prw, -active);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    std::optional<RayHit> sphere_world_sweep(QueryLedger::Tag tag, const Vector3 &origin, float radius,
                                             const Vector3 &sweep, int objtypes, const uintptr_t *p_skip_ents,
                                             int n_skip_ents)
    {
        DMK_PROFILE_SCOPE("phys.pwi_sphere");
        // One-time resolution diagnostics so the verify log can tell "PWI unavailable" (call path broken)
        // apart from "PWI ran but missed". The first failure reason and the first success are each logged
        // once (separate flags, so a transient loading-time failure does not hide the eventual success).
        static bool s_logged_ok = false;
        static bool s_logged_fail = false;
        auto log_fail_once = [](const char *reason)
        {
            if (!s_logged_fail)
            {
                s_logged_fail = true;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "Sphere sweep unavailable: {}", reason);
            }
        };

        if (s_physical_world_global_addr == 0)
        {
            log_fail_once("physics raycast not initialized");
            return std::nullopt;
        }

        // Resolve the physical world fresh; bail cleanly while it is null (no level / loading).
        const auto world_value = DMK::memory::read<uintptr_t>(DMK::Address{s_physical_world_global_addr});
        if (!world_value || *world_value == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{*world_value}))
        {
            log_fail_once("physical world null (no level / loading)");
            return std::nullopt;
        }
        const uintptr_t world = *world_value;

        // PWI is resolved from the world's vtable (slot PHYS_WORLD_VTABLE_PWI_OFFSET) rather than an AOB of its
        // own; the vtable slot is the patch-stable anchor. The slot is a lock wrapper that takes the world mutex
        // and forwards to the real impl, so calling it from the camera's main-thread detour is safe (it waits for
        // the mutex; it never re-enters our code).
        const auto vtable = DMK::memory::read<uintptr_t>(DMK::Address{world});
        if (!vtable || !DMK::memory::is_plausible_ptr(DMK::Address{*vtable}) ||
            !s_game_module.contains(DMK::Address{*vtable}))
        {
            log_fail_once("world vtable unreadable or outside the game image");
            return std::nullopt;
        }
        const auto fn_slot =
            DMK::memory::read<uintptr_t>(DMK::Address{*vtable + Constants::PHYS_WORLD_VTABLE_PWI_OFFSET});
        if (!fn_slot || !DMK::memory::is_plausible_ptr(DMK::Address{*fn_slot}) ||
            !s_game_module.contains(DMK::Address{*fn_slot}))
        {
            log_fail_once("PWI vtable slot unresolved or outside the game image");
            return std::nullopt;
        }
        const auto fn = reinterpret_cast<PrimitiveWorldIntersectionFn>(*fn_slot);
        if (!s_logged_ok)
        {
            s_logged_ok = true;
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "Sphere sweep: PWI RESOLVED (world={}, fn={})",
                                     DMK::format::format_address(world),
                                     DMK::format::format_address(reinterpret_cast<uintptr_t>(fn)));
        }

        // primitives::sphere { Vec3 center; float r; } in WORLD space (CryEngine PWI primitives are world).
        alignas(16) std::byte sphere[Constants::PRIMITIVE_SPHERE_SIZE];
        std::memset(sphere, 0, sizeof(sphere));
        *reinterpret_cast<Vector3 *>(sphere + 0x0) = origin;
        *reinterpret_cast<float *>(sphere + Constants::PRIMITIVE_SPHERE_RADIUS_OFFSET) = radius;

        // geom_contact*; the engine writes through ppcontact, but we read only the float return (distance),
        // so we never dereference the contact (no shared-data lifetime concern).
        void *contact = nullptr;

        alignas(16) std::byte params[Constants::SPWI_PARAMS_SIZE];
        std::memset(params, 0, sizeof(params));
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_ITYPE) = Constants::PRIMITIVE_TYPE_SPHERE;
        *reinterpret_cast<const void **>(params + Constants::SPWI_OFF_PPRIM) = sphere;
        *reinterpret_cast<Vector3 *>(params + Constants::SPWI_OFF_SWEEPDIR) = sweep;
        // entTypes @+0x98 is the objtypes mask. The real PWI (1.5.6 sub_180811680) queues the call when 0x800
        // (rwi_queue) is set and otherwise passes this field straight to GetEntitiesAround (sub_1806EE310), which
        // keeps only the entities whose sim-class bit is in the mask. 0x800 is cleared so the call always runs
        // synchronously. With the camera's 0x101 (static | terrain) the player's living entity (class 3) and its
        // skeleton / worn-gear articulated entity (class 4) are filtered out here; 0 would query no class at all.
        // The impl never reads +0x9C, so nothing is written there.
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_ENTTYPES) = objtypes & ~0x800;
        *reinterpret_cast<void **>(params + Constants::SPWI_OFF_PPCONTACT) = &contact;
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_GEOMFLAGSALL) = 0;
        // The camera's part collide types, the ones its rays request (CAMERA_GEOM_COLLTYPES): the sphere stops on
        // the parts the fan stops on, so only the sweep's continuous contact separates the two probes. A wrong value
        // here only changes which parts block (functional, not a safety issue).
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_GEOMFLAGSANY) =
            static_cast<int>(Constants::CAMERA_GEOM_COLLTYPES);
        // Skip the entities the caller lists (the faded occluders the camera steps past), so the sweep reports a
        // clean WORLD distance. The synchronous PWI
        // walks every nSkipEnts entry (only its queued path clamps the count to 4); a negative count would make
        // pSkipEnts the candidate list and skip the world, so only a positive count is written. The PWI implementation
        // reads both offsets, and pSkipEnts as pSkipEnts[i] (an array of IPhysicalEntity*).
        const bool have_skip = p_skip_ents != nullptr && n_skip_ents > 0;
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_NSKIPENTS) = have_skip ? n_skip_ents : 0;
        *reinterpret_cast<const void **>(params + Constants::SPWI_OFF_PSKIPENTS) = have_skip ? p_skip_ents : nullptr;
        // WriteLockCond: prw self-pointer == thread-safe mode (no global lock held), matching the engine
        // ctor. iActive starts 0.
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_LOCK_IACTIVE) = 0;
        *reinterpret_cast<void **>(params + Constants::SPWI_OFF_LOCK_PRW) = params + Constants::SPWI_OFF_LOCK_IACTIVE;

        const std::int64_t ledger_started = QueryLedger::begin();
        const float distance = primitive_world_intersection_guarded(fn, reinterpret_cast<void *>(world), params);
        QueryLedger::end(tag, QueryLedger::Kind::Physics, ledger_started);

        // Release the WriteLockCond exactly as the engine's own caller does: InterlockedAdd(prw, -iActive),
        // reading both back from the struct AFTER the call. If the engine took a real (global) lock it
        // repointed prw and set iActive, so this frees it (preventing a physics-thread stall); if it stayed
        // in thread-safe self-pointer mode, iActive is 0 and this is a no-op. Runs even after a faulted PWI
        // call (so a real lock is never left held), under its own SEH frame so a stale prw cannot crash.
        release_spwi_write_lock_guarded(params);

        if (!std::isfinite(distance) || distance <= 0.0f)
        {
            return std::nullopt;
        }

        // Only the distance is used by the camera; approximate the point/normal along the sweep so the
        // RayHit is fully populated for any caller that inspects them.
        RayHit hit{};
        hit.m_distance = distance;
        const float len = sweep.magnitude();
        const Vector3 dir = (len > 1e-6f) ? sweep / len : Vector3{0.0f, 0.0f, 0.0f};
        hit.m_point = origin + dir * distance;
        hit.m_normal = dir * -1.0f;
        return hit;
    }

    std::optional<RayHit> ray_fan_sweep(QueryLedger::Tag tag, const Vector3 &origin, const Vector3 &sweep, float radius,
                                        int objtypes, unsigned int flags, const uintptr_t *skip_ents, int n_skip_ents,
                                        int *rays_blocked)
    {
        DMK_PROFILE_SCOPE("phys.ray_fan");
        if (rays_blocked != nullptr)
        {
            *rays_blocked = 0;
        }
        const float len = sweep.magnitude();
        // Written so a NaN length also returns: a NaN ray must never reach the engine.
        if (!(len >= 1e-4f))
        {
            return std::nullopt;
        }
        const Vector3 dir = sweep / len;

        // Two axes perpendicular to the sweep: right is level (across world up), up is right x dir. One continuous
        // basis at every pitch, so the ray pattern never turns as the camera passes a pitch (world X is the seed only
        // straight up or down, which the camera never reaches).
        Vector3 right = dir.cross(Vector3{0.0f, 0.0f, 1.0f});
        if (right.magnitude() < 1e-3f)
        {
            right = dir.cross(Vector3{1.0f, 0.0f, 0.0f});
        }
        const float rlen = right.magnitude();
        if (rlen < 1e-4f)
        {
            // Degenerate: fall back to the single centre ray, which stands for the whole fan.
            std::optional<RayHit> centre =
                ray_world_intersection(tag, origin, sweep, objtypes, flags, skip_ents, n_skip_ents);
            if (rays_blocked != nullptr && centre.has_value())
            {
                *rays_blocked = k_fan_rays;
            }
            return centre;
        }
        right = right / rlen;
        const Vector3 up = right.cross(dir); // already unit (right and dir are orthonormal)

        // Centre + four parallel rays offset by the radius => a square tube approximating the swept sphere.
        const Vector3 offsets[k_fan_rays] = {Vector3{0.0f, 0.0f, 0.0f}, right * radius, right * (-radius), up * radius,
                                             up * (-radius)};

        std::optional<RayHit> best;
        int blocked = 0;
        for (const Vector3 &off : offsets)
        {
            const auto h = ray_world_intersection(tag, origin + off, sweep, objtypes, flags, skip_ents, n_skip_ents);
            if (!h.has_value())
            {
                continue;
            }
            ++blocked;
            if (!best.has_value() || h->m_distance < best->m_distance)
            {
                best = h;
            }
        }
        if (rays_blocked != nullptr)
        {
            *rays_blocked = blocked;
        }
        return best;
    }

    uintptr_t static_brush_render_node(uintptr_t collider) noexcept
    {
        if (collider == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{collider}))
        {
            return 0;
        }
        // +0x28 packs m_iForeignData in the low 16 bits and m_iForeignFlags in the high 16 bits. The engine's
        // GetiForeignData reads only the signed low word, so compare that and ignore the flags.
        const auto ftype =
            DMK::memory::read<std::int16_t>(DMK::Address{collider + Constants::PHYS_ENTITY_FOREIGN_TYPE_OFFSET});
        if (!ftype || *ftype != Constants::PHYS_FOREIGN_ID_STATIC)
        {
            return 0; // not a static brush owner (entity-attached / pure-physics): no usable render node
        }
        const auto node =
            DMK::memory::read<uintptr_t>(DMK::Address{collider + Constants::PHYS_ENTITY_FOREIGN_DATA_OFFSET});
        if (!node || !DMK::memory::is_plausible_ptr(DMK::Address{*node}))
        {
            return 0;
        }
        return *node;
    }

} // namespace TPVCamera
