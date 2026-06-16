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
    static DMK::Memory::ModuleRange s_game_module{};

    bool initialize_physics_raycast(uintptr_t module_base, size_t module_size, uintptr_t g_env)
    {
        DMK::Logger &logger = DMK::Logger::get_instance();

        // The module-scoped cascade resolved the helper inside the game image (or 0 on a miss); read here
        // from the anchor registry resolved at startup by resolve_all_anchors().
        const uintptr_t ray_fn = anchor_address(AnchorId::RayWorldIntersection);
        if (ray_fn == 0)
        {
            logger.warning(
                "PhysicsRaycast: RayWorldIntersection not found (game patched?); collision/aim raycast unavailable");
            return false;
        }

        s_ray_world_intersection = reinterpret_cast<RayWorldIntersectionFn>(ray_fn);
        // p_physical_world is a member of the g_env struct (see PHYSICAL_WORLD_OFFSET); deriving its
        // slot from the patch-resiliently resolved g_env base avoids a second hardcoded address.
        s_physical_world_global_addr = g_env + Constants::PHYSICAL_WORLD_OFFSET;
        s_game_module = {module_base, module_base + module_size};

        logger.info("PhysicsRaycast: RayWorldIntersection at {}, p_physical_world slot at {}",
                    DMK::Format::format_address(ray_fn), DMK::Format::format_address(s_physical_world_global_addr));
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

    std::optional<RayHit> ray_world_intersection(const Vector3 &origin, const Vector3 &direction, int objtypes,
                                                 unsigned int flags, const uintptr_t *skip_ents, int n_skip_ents)
    {
        if (s_ray_world_intersection == nullptr || s_physical_world_global_addr == 0)
        {
            return std::nullopt;
        }

        // Resolve the physical world fresh; bail cleanly while it is null (no level / loading).
        const auto world_value = DMK::Memory::seh_read<uintptr_t>(s_physical_world_global_addr);
        if (!world_value || *world_value == 0 || !DMK::Memory::plausible_userspace_ptr(*world_value))
        {
            return std::nullopt;
        }

        alignas(16) std::byte hit_buffer[Constants::RAY_HIT_SIZE];
        std::memset(hit_buffer, 0, sizeof(hit_buffer));

        const int hit_count = ray_world_intersection_guarded(
            reinterpret_cast<void *>(*world_value), &origin, &direction, objtypes, flags, hit_buffer,
            const_cast<uintptr_t *>(skip_ents), (skip_ents != nullptr) ? n_skip_ents : 0);
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
     *          engine-written params and screened with plausible_userspace_ptr, but a stale-but-plausible
     *          prw (the world counter concurrently invalidated) would fault on the atomic store, so the
     *          store runs under __try. POD-only body so the SEH frame shares no C++ unwinding.
     */
    static void release_spwi_write_lock_guarded(std::byte *params) noexcept
    {
        __try
        {
            auto *prw = *reinterpret_cast<volatile long **>(params + Constants::SPWI_OFF_LOCK_PRW);
            const long active = *reinterpret_cast<volatile long *>(params + Constants::SPWI_OFF_LOCK_IACTIVE);
            if (prw && DMK::Memory::plausible_userspace_ptr(reinterpret_cast<uintptr_t>(prw)) && active != 0)
            {
                _InterlockedExchangeAdd(prw, -active);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    std::optional<RayHit> sphere_world_sweep(const Vector3 &origin, float radius, const Vector3 &sweep, int objtypes,
                                             const uintptr_t *p_skip_ents, int n_skip_ents)
    {
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
                DMK::Logger::get_instance().debug("Sphere sweep unavailable: {}", reason);
            }
        };

        if (s_physical_world_global_addr == 0)
        {
            log_fail_once("physics raycast not initialized");
            return std::nullopt;
        }

        // Resolve the physical world fresh; bail cleanly while it is null (no level / loading).
        const auto world_value = DMK::Memory::seh_read<uintptr_t>(s_physical_world_global_addr);
        if (!world_value || *world_value == 0 || !DMK::Memory::plausible_userspace_ptr(*world_value))
        {
            log_fail_once("physical world null (no level / loading)");
            return std::nullopt;
        }
        const uintptr_t world = *world_value;

        // PWI is resolved from the LIVE world vtable (slot PHYS_WORLD_VTABLE_PWI_OFFSET), not a static
        // address: the static pPhysicalWorld global was found to hold non-pointer data in the running
        // process, and the vtable slot is the patch-stable anchor. The slot is a lock wrapper that takes
        // the world mutex and forwards to the real impl, so calling it from the render thread is safe
        // (it waits for the mutex; it never re-enters our code).
        const auto vtable = DMK::Memory::seh_read<uintptr_t>(world);
        if (!vtable || !DMK::Memory::plausible_userspace_ptr(*vtable) || !DMK::Memory::contains(s_game_module, *vtable))
        {
            log_fail_once("world vtable unreadable or outside the game image");
            return std::nullopt;
        }
        const auto fn_slot = DMK::Memory::seh_read<uintptr_t>(*vtable + Constants::PHYS_WORLD_VTABLE_PWI_OFFSET);
        if (!fn_slot || !DMK::Memory::plausible_userspace_ptr(*fn_slot) ||
            !DMK::Memory::contains(s_game_module, *fn_slot))
        {
            log_fail_once("PWI vtable slot unresolved or outside the game image");
            return std::nullopt;
        }
        const auto fn = reinterpret_cast<PrimitiveWorldIntersectionFn>(*fn_slot);
        if (!s_logged_ok)
        {
            s_logged_ok = true;
            DMK::Logger::get_instance().debug("Sphere sweep: PWI RESOLVED (world={}, fn={})",
                                              DMK::Format::format_address(world),
                                              DMK::Format::format_address(reinterpret_cast<uintptr_t>(fn)));
        }

        // primitives::sphere { Vec3 center; float r; } in WORLD space (CryEngine PWI primitives are world).
        alignas(16) std::byte sphere[Constants::PRIMITIVE_SPHERE_SIZE];
        std::memset(sphere, 0, sizeof(sphere));
        *reinterpret_cast<Vector3 *>(sphere + 0x0) = origin;
        *reinterpret_cast<float *>(sphere + 0xC) = radius;

        // geom_contact*; the engine writes through ppcontact, but we read only the float return (distance),
        // so we never dereference the contact (no shared-data lifetime concern).
        void *contact = nullptr;

        alignas(16) std::byte params[Constants::SPWI_PARAMS_SIZE];
        std::memset(params, 0, sizeof(params));
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_ITYPE) = Constants::PRIMITIVE_TYPE_SPHERE;
        *reinterpret_cast<const void **>(params + Constants::SPWI_OFF_PPRIM) = sphere;
        *reinterpret_cast<Vector3 *>(params + Constants::SPWI_OFF_SWEEPDIR) = sweep;
        // Flags field at +0x98 (the impl tests &0x800 = rwi_queue; do NOT set that -- keep the call synchronous).
        // 0x101 is the empirically FULL-RANGE value: the swept sphere registers both close AND far
        // contacts. This field is NOT standard rwi pierceability -- 0x40F
        // (rwi_stop_at_pierceable|colltype_any) hit only FAR geometry (missed close walls), and 0 registered NO
        // contacts at all.
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_FLAGS) = 0x101;
        // entTypes @+0x9C is DEAD in this fork: the impl (sub_1808182A0) has ZERO reads of +0x9C, so the sphere
        // ALWAYS queries ent_all and CANNOT be type-filtered here -- this write is a defensive no-op (harmless,
        // kept in case a future build wires the field up). Actors (player body/gear, NPCs) are NOT excluded here;
        // they are dropped by the FAN-AUTHORITY gate in camera_hook, which accepts the sphere only when it agrees
        // with the RWI fan's world hit (the fan's objtypes=0x101 is a real arg that provably excludes all actors).
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_ENTTYPES) = objtypes;
        *reinterpret_cast<void **>(params + Constants::SPWI_OFF_PPCONTACT) = &contact;
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_GEOMFLAGSALL) = 0;
        // Broad collision-type mask so the sphere stops on any solid surface (mirrors the RWI colltype_any
        // intent). A wrong value here only changes which parts block (functional, not a safety issue).
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_GEOMFLAGSANY) = 0x0FFF;
        // Skip the player's own physics entities (resolved by the caller via resolve_player_physics_skip), so
        // the sweep ignores the body/shield/gear and reports a clean WORLD distance. nSkipEnts is impl-clamped
        // to <=4; pSkipEnts is read as pSkipEnts[i] (an array of IPhysicalEntity*). Both offsets impl-confirmed.
        const bool have_skip = p_skip_ents != nullptr && n_skip_ents > 0;
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_NSKIPENTS) = have_skip ? n_skip_ents : 0;
        *reinterpret_cast<const void **>(params + Constants::SPWI_OFF_PSKIPENTS) = have_skip ? p_skip_ents : nullptr;
        // WriteLockCond: prw self-pointer == thread-safe mode (no global lock held), matching the engine
        // ctor. iActive starts 0.
        *reinterpret_cast<int *>(params + Constants::SPWI_OFF_LOCK_IACTIVE) = 0;
        *reinterpret_cast<void **>(params + Constants::SPWI_OFF_LOCK_PRW) = params + Constants::SPWI_OFF_LOCK_IACTIVE;

        const float distance = primitive_world_intersection_guarded(fn, reinterpret_cast<void *>(world), params);

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

    std::optional<RayHit> ray_fan_sweep(const Vector3 &origin, const Vector3 &sweep, float radius, int objtypes,
                                        unsigned int flags, const uintptr_t *skip_ents, int n_skip_ents)
    {
        const float len = sweep.magnitude();
        if (len < 1e-4f)
        {
            return std::nullopt;
        }
        const Vector3 dir = sweep / len;

        // Two axes perpendicular to the sweep. Pick a seed not parallel to dir, then Gram-Schmidt.
        const Vector3 seed = (std::fabs(dir.z) < 0.9f) ? Vector3{0.0f, 0.0f, 1.0f} : Vector3{1.0f, 0.0f, 0.0f};
        Vector3 right = dir.cross(seed);
        const float rlen = right.magnitude();
        if (rlen < 1e-4f)
        {
            // Degenerate: fall back to the single centre ray.
            return ray_world_intersection(origin, sweep, objtypes, flags, skip_ents, n_skip_ents);
        }
        right = right / rlen;
        const Vector3 up = right.cross(dir); // already unit (right and dir are orthonormal)

        // Centre + four parallel rays offset by the radius => a square tube approximating the swept sphere.
        const Vector3 offsets[5] = {Vector3{0.0f, 0.0f, 0.0f}, right * radius, right * (-radius), up * radius,
                                    up * (-radius)};

        std::optional<RayHit> best;
        for (const Vector3 &off : offsets)
        {
            const auto h = ray_world_intersection(origin + off, sweep, objtypes, flags, skip_ents, n_skip_ents);
            if (h.has_value() && (!best.has_value() || h->m_distance < best->m_distance))
            {
                best = h;
            }
        }
        return best;
    }

    float collider_horizontal_footprint(uintptr_t collider) noexcept
    {
        if (collider == 0 || !DMK::Memory::plausible_userspace_ptr(collider))
        {
            return -1.0f;
        }
        // A collider with a foreign OWNER carries a sane world m_BBox at this offset, whether it is a static brush
        // (fType == PHYS_FOREIGN_ID_STATIC == 1) OR a placed ENTITY (live: the gate post's fType is 0x200002 = an
        // entity id with flags, NOT static -- but its bbox 0.30x0.30x3.69 is valid). Do NOT require STATIC, or
        // entity posts are missed. Only fType == 0 (terrain heightmap / unowned geom) reads a 0x0 bbox (live), so
        // exclude it; the degenerate-AABB check below catches it too.
        const auto ftype = DMK::Memory::seh_read<int>(collider + Constants::PHYS_ENTITY_FOREIGN_TYPE_OFFSET);
        if (!ftype || *ftype == 0)
        {
            return -1.0f;
        }
        const auto min_x = DMK::Memory::seh_read<float>(collider + Constants::PHYS_ENTITY_BBOX_MIN_OFFSET + 0);
        const auto min_y = DMK::Memory::seh_read<float>(collider + Constants::PHYS_ENTITY_BBOX_MIN_OFFSET + 4);
        const auto max_x = DMK::Memory::seh_read<float>(collider + Constants::PHYS_ENTITY_BBOX_MAX_OFFSET + 0);
        const auto max_y = DMK::Memory::seh_read<float>(collider + Constants::PHYS_ENTITY_BBOX_MAX_OFFSET + 4);
        if (!min_x || !min_y || !max_x || !max_y)
        {
            return -1.0f;
        }
        const float sx = *max_x - *min_x;
        const float sy = *max_y - *min_y;
        if (!std::isfinite(sx) || !std::isfinite(sy) || sx < 1.0e-3f || sy < 1.0e-3f || sx > 1.0e5f || sy > 1.0e5f)
        {
            return -1.0f; // degenerate / implausible AABB -> unknown
        }
        // The LARGER horizontal extent. A post is small in BOTH axes (footprint small); a wall is large in at
        // least one (footprint large) even when thin in the other -- so this separates a thin POST from a thin
        // WALL, which the minimum-extent metric (smallest of all three axes) cannot: a thin wall is also small on
        // its depth axis.
        return (sx > sy) ? sx : sy;
    }

    uintptr_t static_brush_render_node(uintptr_t collider) noexcept
    {
        if (collider == 0 || !DMK::Memory::plausible_userspace_ptr(collider))
        {
            return 0;
        }
        const auto ftype = DMK::Memory::seh_read<int>(collider + Constants::PHYS_ENTITY_FOREIGN_TYPE_OFFSET);
        if (!ftype || *ftype != Constants::PHYS_FOREIGN_ID_STATIC)
        {
            return 0; // not a static brush owner (entity-attached / pure-physics): no usable render node
        }
        const auto node = DMK::Memory::seh_read<uintptr_t>(collider + Constants::PHYS_ENTITY_FOREIGN_DATA_OFFSET);
        if (!node || !DMK::Memory::plausible_userspace_ptr(*node))
        {
            return 0;
        }
        return *node;
    }

    float character_occluded_fraction(const Vector3 &camera, const Vector3 &pivot, int objtypes, unsigned int flags)
    {
        // Screen-horizontal axis at the character (perpendicular to the view, kept world-horizontal).
        const Vector3 view = pivot - camera; // camera -> character
        const Vector3 world_up{0.0f, 0.0f, 1.0f};
        Vector3 right = view.cross(world_up);
        const float rl = right.magnitude();
        right = (rl > 1e-4f) ? (right / rl) : Vector3{1.0f, 0.0f, 0.0f};

        // Sample the character silhouette relative to the pivot (~eye/head height): four vertical levels from
        // just above the head down to the shins, three columns across the shoulders. A ray from the camera to
        // each sample is "occluded" when WORLD geometry lies before it (the player body is ent_living, excluded
        // by the world-only objtypes, so the character never occludes itself). Like a real TPV camera (which
        // protects the head/look-at socket, not the feet) each level is WEIGHTED by importance head->feet, so an
        // obstruction over the upper body counts far more than one over the legs: occluding the upper half
        // exceeds 0.5, occluding only the legs stays well under it. (k_char_level_weight sums to 2.45.)
        static const float v_levels[4] = {0.10f, -0.45f, -1.00f, -1.55f};
        static const float k_char_level_weight[4] = {1.0f, 0.8f, 0.45f, 0.2f}; // head, chest, hips, shins
        static const float h_cols[3] = {-0.25f, 0.0f, 0.25f};
        float total = 0.0f;
        float blocked = 0.0f;
        for (int vi = 0; vi < 4; ++vi)
        {
            const float v = v_levels[vi];
            const float w = k_char_level_weight[vi];
            for (const float h : h_cols)
            {
                const Vector3 target{pivot.x + right.x * h, pivot.y + right.y * h, pivot.z + v};
                const Vector3 to_target = target - camera;
                const float dist = to_target.magnitude();
                if (dist < 1e-3f)
                {
                    continue;
                }
                total += w;
                const auto hit = ray_world_intersection(camera, to_target, objtypes, flags, nullptr, 0);
                if (hit.has_value() && hit->m_distance < dist - 0.10f)
                {
                    blocked += w;
                }
            }
        }
        return (total > 0.0f) ? (blocked / total) : 0.0f;
    }

    int resolve_player_physics_skip(const Vector3 &origin, const Vector3 &sweep, float radius, int objtypes_all,
                                    int objtypes_world, unsigned int flags, uintptr_t *out_skip, int max_out)
    {
        if (out_skip == nullptr || max_out <= 0)
        {
            return 0;
        }
        const float len = sweep.magnitude();
        if (len < 1e-4f)
        {
            return 0;
        }
        const Vector3 dir = sweep / len;
        const Vector3 seed = (std::fabs(dir.z) < 0.9f) ? Vector3{0.0f, 0.0f, 1.0f} : Vector3{1.0f, 0.0f, 0.0f};
        Vector3 right = dir.cross(seed);
        const float rlen = right.magnitude();
        Vector3 up{0.0f, 0.0f, 0.0f};
        if (rlen >= 1e-4f)
        {
            right = right / rlen;
            up = right.cross(dir);
        }
        else
        {
            right = Vector3{0.0f, 0.0f, 0.0f};
        }
        // REVERSE probe: cast from the camera end back toward the pivot. The pivot is inside the body, so a
        // forward ray exits through a back-face and misses the body; a reverse ray hits it from outside. Same
        // square-tube fan offsets as the sweep so coverage matches the sphere's footprint.
        const Vector3 camera = origin + sweep;
        const Vector3 back = sweep * -1.0f; // camera -> pivot, length = |sweep|
        const Vector3 offsets[5] = {Vector3{0.0f, 0.0f, 0.0f}, right * radius, right * (-radius), up * radius,
                                    up * (-radius)};
        int n = 0;
        for (const Vector3 &off : offsets)
        {
            const Vector3 o = camera + off;
            const auto h_all = ray_world_intersection(o, back, objtypes_all, flags);
            if (!h_all.has_value() || h_all->m_collider == 0)
            {
                continue;
            }
            // A non-world entity (player body, worn gear, NPC) is invisible to the world-only mask: collect it
            // only when the all-types hit is nearer than the world hit (or the world ray misses). World geometry
            // appears in BOTH casts at the same range, so it is never collected -- we must not skip the world.
            const auto h_world = ray_world_intersection(o, back, objtypes_world, flags);
            const bool is_non_world = !h_world.has_value() || h_all->m_distance < h_world->m_distance - 0.02f;
            if (!is_non_world)
            {
                continue;
            }
            bool seen = false;
            for (int i = 0; i < n; ++i)
            {
                if (out_skip[i] == h_all->m_collider)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen && n < max_out)
            {
                out_skip[n++] = h_all->m_collider;
            }
        }
        return n;
    }

} // namespace TPVCamera
