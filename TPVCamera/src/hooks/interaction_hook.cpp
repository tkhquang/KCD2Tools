/**
 * @file hooks/interaction_hook.cpp
 * @brief Camera-space interaction: redirect the player look-at ray to the render camera + crosshair.
 *
 * @details The interactor casts a look ray each tick to find the "press to use" target. The look-ray builder
 *          (sub_1808333C8) computes the ray origin (interactor+0x3E8) and direction (interactor+0x3F4) from
 *          the gameplay VIEW SUBSYSTEM -- reached via sub_18091C138()+56, the entity subsystem registry, NOT
 *          the render camera -- eye-anchored, which the mod never offsets. So in third person the screen-centre
 *          crosshair and the look-ray target diverge by the shoulder offset (worst at close range).
 *          sub_1808333C8 hands that origin+direction to the generic ray-query builder (sub_180530584, called
 *          at fn+0x259). This hook intercepts that builder and -- ONLY when the caller's return address is
 *          inside sub_1808333C8 -- rewrites the origin to the render camera and the direction to the crosshair
 *          forward (preserving the engine's ray reach), so the look-ray follows the screen centre at all
 *          ranges. After the rewrite interactor+0x3E8/+0x3F4 equal the render-cam pose. The
 *          player is in the cast's skip list, so originating from the (behind-the-player) camera does not
 *          self-hit. Off unless InteractFromCamera is set; a no-op in first person (the published pose is
 *          invalid); SEH-guarded; the caller filter guarantees camera/AI/audio ray queries are never touched.
 *
 *          SCOPE: the look-ray redirect fixes ENTITY interaction (NPCs, dropped loot) that resolves through
 *          the look-ray. Beds/shrines/doors (InteractiveScene usables) are instead picked by a proximity
 *          candidate build whose on-screen reticle projection gate (sub_18093C170) drops a candidate that
 *          projects off-reticle in the gameplay camera. A second hook (on_screen_check_detour) force-passes a
 *          candidate whose world point lies along the published crosshair ray, so the crosshair-pointed usable
 *          survives the offset-camera projection too. Both are gated behind InteractFromCamera and no-op in
 *          first person.
 *
 *          DIAGNOSTICS: while InteractFromCamera is on, a rate-limited trace line reports rayBuilds (all
 *          intercepted builds), interactionCalls (builds whose caller is the look-ray -- if this stays 0, the
 *          look-ray does NOT use this builder / the filter missed), redirects (builds actually rewritten),
 *          onscreenForced (reticle gates force-passed for a crosshair-ray candidate), and the before/after
 *          eye->camera + dir values so the rewrite is visible in the log.
 */

#include "interaction_hook.hpp"
#include "aob_resolver.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "global_state.hpp"

#include <DetourModKit.hpp>

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>

namespace TPVCamera
{
namespace
{

// sub_180530584(out /*rcx*/, origin /*rdx*/, dir /*r8*/, objtypes, flags, skip_ents, n, mode): packs a ray
// query (copies origin+dir into out) and returns out. We forward all args after optionally rewriting the
// origin+dir Vec3s the caller passed.
using RayQueryBuildFunc = uintptr_t(__fastcall *)(uintptr_t out, uintptr_t origin, uintptr_t dir,
                                                  int objtypes, int flags, uintptr_t skip_ents,
                                                  unsigned __int8 n, char mode);
RayQueryBuildFunc s_original = nullptr;

// [lo, hi) of sub_1808333C8 (the interactor look-ray builder). The redirect fires only when the query
// builder's return address lies in this range, so ONLY the interaction look-ray is touched.
uintptr_t s_lookray_lo = 0;
uintptr_t s_lookray_hi = 0;

// --- diagnostics (touched on the game thread; atomic for race-free reads in the trace line) ---
std::atomic<unsigned long long> s_calls_total{0};   // every ray-query build intercepted
std::atomic<unsigned long long> s_calls_lookray{0}; // builds whose caller is the interactor look-ray
std::atomic<unsigned long long> s_redirects{0};     // builds actually rewritten
const char *s_last_reason = "none";                 // why the last interaction build was / was not rewritten
// snapshot of the last rewrite so the log shows the before (eye) vs after (camera/crosshair) values
float s_dbg_eye[3] = {0.0f, 0.0f, 0.0f};
float s_dbg_dir_old[3] = {0.0f, 0.0f, 0.0f};
float s_dbg_cam[3] = {0.0f, 0.0f, 0.0f};
float s_dbg_dir_new[3] = {0.0f, 0.0f, 0.0f};
bool s_dbg_valid = false;
std::chrono::steady_clock::time_point s_last_log{};

// On-screen reticle projection gate (sub_18093C170). The gate that drops InteractiveScene usables
// (shrines/beds/doors) when the body is not turned: it projects the candidate through the gameplay camera and
// rejects it if off the reticle. We force-pass candidates whose world point lies on the crosshair ray.
using OnScreenCheckFunc = char(__fastcall *)(uintptr_t a1, uintptr_t a2, float *a3, uintptr_t a4);
OnScreenCheckFunc s_onscreen_original = nullptr;
std::atomic<unsigned long long> s_onscreen_calls{0};  // on-screen reticle checks intercepted
std::atomic<unsigned long long> s_onscreen_forced{0}; // checks force-passed for a crosshair-ray candidate

/**
 * @brief SEH-guarded rewrite of the interaction ray to the crosshair forward, re-origined toward the eye.
 * @details Sets the direction Vec3 to the crosshair forward (preserving the engine's ray reach, so the
 *          max interaction distance is unchanged) and slides the origin Vec3 forward along that same ray
 *          to the player-eye's projection onto it, so the engine's interaction-range cap measures from
 *          ~eye rather than from the camera standing FollowDistance behind. Captures a before/after
 *          snapshot for the diagnostic log. Only a reference, floats and POD arrays live in the guarded
 *          scope, so __try/__except is valid; a bad pointer degrades to no-op.
 * @return true if the ray was rewritten; false if the foreign pointers faulted (ray left unchanged).
 */
bool redirect_interaction_ray(uintptr_t origin, uintptr_t dir) noexcept
{
    float px, py, pz, dx, dy, dz;
    if (!interaction_aim_pose().load(px, py, pz, dx, dy, dz))
    {
        // No coherent valid pose (first person, or raced with an invalidate); leave the ray unchanged.
        return false;
    }

    float eye[3] = {0.0f, 0.0f, 0.0f};
    float dold[3] = {0.0f, 0.0f, 0.0f};
    float dnew[3] = {0.0f, 0.0f, 0.0f};
    float neworigin[3] = {0.0f, 0.0f, 0.0f};
    bool done = false;

    __try
    {
        float *o = reinterpret_cast<float *>(origin);
        float *d = reinterpret_cast<float *>(dir);
        eye[0] = o[0]; eye[1] = o[1]; eye[2] = o[2];
        dold[0] = d[0]; dold[1] = d[1]; dold[2] = d[2];
        const float r = std::sqrt(dold[0] * dold[0] + dold[1] * dold[1] + dold[2] * dold[2]);
        const float reach = (r > 1e-3f) ? r : 1.0f; // preserve the engine's interaction range
        dnew[0] = dx * reach; dnew[1] = dy * reach; dnew[2] = dz * reach;
        // Slide the origin FORWARD along the SAME crosshair ray to the player-eye's projection onto it, so
        // the interaction range cap (sub_1809F94D0, 3.5 m, measured from the ray origin) sees ~eye->object
        // instead of camera->object. The render camera sits FollowDistance behind, so without this a usable
        // at your feet is >3.5 m FROM THE CAMERA and gets culled (worse at high FollowDistance) even though
        // it is <1 m from you. Sliding ALONG the ray keeps the exact same hit (no parallax); only the
        // distance reference moves. eye[] is the original gameplay origin captured above.
        const float proj = (eye[0] - px) * dx + (eye[1] - py) * dy + (eye[2] - pz) * dz;
        const float adv = (proj > 0.0f) ? proj : 0.0f; // never slide backward past the camera
        neworigin[0] = px + dx * adv; neworigin[1] = py + dy * adv; neworigin[2] = pz + dz * adv;
        o[0] = neworigin[0]; o[1] = neworigin[1]; o[2] = neworigin[2];
        d[0] = dnew[0]; d[1] = dnew[1]; d[2] = dnew[2];
        done = true;
        // NOTE: this re-aims ONLY the look-ray (entity interaction). Beds/shrines/doors are NOT selected via
        // the look-ray; they go through a separate body-proximity candidate list whose facing verdict is a Lua
        // interaction-constraint (sub_1809D2538 -> sub_181ED69D0 -> CScriptSystem reading the real use-actor),
        // which the render camera cannot influence (sub_18091C138 is the entity-subsystem registry, not a
        // camera). The crosshair-driven path for those usables is instead the on-screen reticle gate
        // (on_screen_check_detour below). See tpv_shrine_facing_lua_wall.
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    if (done)
    {
        for (int i = 0; i < 3; ++i)
        {
            s_dbg_eye[i] = eye[i];
            s_dbg_dir_old[i] = dold[i];
            s_dbg_dir_new[i] = dnew[i];
        }
        s_dbg_cam[0] = neworigin[0]; s_dbg_cam[1] = neworigin[1]; s_dbg_cam[2] = neworigin[2];
        s_dbg_valid = true;
    }
    return done;
}

/** @brief Rate-limited (2 s) trace line; only while InteractFromCamera is on (caller checks that). */
void maybe_log_status() noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last_log < std::chrono::seconds(2))
    {
        return;
    }
    s_last_log = now;

    DMK::Logger &log = DMK::Logger::get_instance();
    const unsigned long long total = s_calls_total.load(std::memory_order_relaxed);
    const unsigned long long look = s_calls_lookray.load(std::memory_order_relaxed);
    const unsigned long long red = s_redirects.load(std::memory_order_relaxed);
    const unsigned long long ofc = s_onscreen_forced.load(std::memory_order_relaxed);
    const unsigned long long osc = s_onscreen_calls.load(std::memory_order_relaxed);

    if (s_dbg_valid)
    {
        log.trace("InteractionHook[run]: rayBuilds={} interactionCalls={} redirects={} lastReason={} "
                  "onscreenChecks={} onscreenForced={} | eye=({}, {}, {}) -> cam=({}, {}, {}) ; "
                  "dirOld=({}, {}, {}) -> dirNew=({}, {}, {})",
                  total, look, red, s_last_reason, osc, ofc,
                  s_dbg_eye[0], s_dbg_eye[1], s_dbg_eye[2], s_dbg_cam[0], s_dbg_cam[1], s_dbg_cam[2],
                  s_dbg_dir_old[0], s_dbg_dir_old[1], s_dbg_dir_old[2],
                  s_dbg_dir_new[0], s_dbg_dir_new[1], s_dbg_dir_new[2]);
    }
    else
    {
        log.trace("InteractionHook[run]: rayBuilds={} interactionCalls={} redirects={} lastReason={} "
                  "onscreenChecks={} onscreenForced={} (no ray rewrite captured yet -- if interactionCalls "
                  "stays 0, the look-ray does not use the builder)",
                  total, look, red, s_last_reason, osc, ofc);
    }
}

/**
 * @brief Detour for the ray-query builder; redirects ONLY the interactor look-ray.
 */
uintptr_t __fastcall ray_query_build_detour(uintptr_t out, uintptr_t origin, uintptr_t dir,
                                            int objtypes, int flags, uintptr_t skip_ents,
                                            unsigned __int8 n, char mode)
{
    s_calls_total.fetch_add(1, std::memory_order_relaxed);

    const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (ra >= s_lookray_lo && ra < s_lookray_hi)
    {
        s_calls_lookray.fetch_add(1, std::memory_order_relaxed);
        if (!settings().interact_from_camera.load(std::memory_order_relaxed))
        {
            s_last_reason = "InteractFromCamera=off";
        }
        else if (!interaction_aim_pose().is_valid())
        {
            s_last_reason = "first-person (offset not engaged)";
        }
        else if (origin == 0 || dir == 0)
        {
            s_last_reason = "null ray args";
        }
        else if (redirect_interaction_ray(origin, dir))
        {
            s_redirects.fetch_add(1, std::memory_order_relaxed);
            s_last_reason = "redirected to camera+crosshair";
        }
        else
        {
            s_last_reason = "ray rewrite faulted (unchanged)";
        }
    }

    if (settings().interact_from_camera.load(std::memory_order_relaxed))
    {
        maybe_log_status();
    }

    return s_original(out, origin, dir, objtypes, flags, skip_ents, n, mode);
}

/**
 * @brief Detour for the on-screen reticle projection gate (sub_18093C170). While InteractFromCamera is on,
 *        force-passes a candidate whose world interaction point (a2 = Vec3) lies along the published crosshair
 *        ray (perpendicular distance below the configured max), writing centered 2D coords (a3) so it ranks as
 *        the top selection; all other candidates fall through to the original projection test. This is what
 *        lets the crosshair-pointed shrine/bed/door survive the proximity build with the offset camera.
 */
char __fastcall on_screen_check_detour(uintptr_t a1, uintptr_t a2, float *a3, uintptr_t a4)
{
    s_onscreen_calls.fetch_add(1, std::memory_order_relaxed);

    float ex, ey, ez, dx, dy, dz;
    if (a2 == 0 || a3 == nullptr || !settings().interact_from_camera.load(std::memory_order_relaxed) ||
        !interaction_aim_pose().load(ex, ey, ez, dx, dy, dz))
    {
        return s_onscreen_original(a1, a2, a3, a4);
    }

    bool forced = false;
    __try
    {
        const float *p = reinterpret_cast<const float *>(a2); // candidate world interaction point
        const float vx = p[0] - ex, vy = p[1] - ey, vz = p[2] - ez;
        const float t = vx * dx + vy * dy + vz * dz; // projection onto the crosshair ray (unit dir)
        if (t > 0.1f)                                 // candidate must be in front of the camera
        {
            const float perp2 = (vx * vx + vy * vy + vz * vz) - t * t; // squared perpendicular distance
            const float thr = Constants::INTERACTION_ONSCREEN_RAY_PERP_MAX;
            if (perp2 >= 0.0f && perp2 < thr * thr)
            {
                a3[0] = Constants::INTERACTION_ONSCREEN_CENTER; // centered -> ranks as top selection priority
                a3[1] = Constants::INTERACTION_ONSCREEN_CENTER;
                forced = true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    if (forced)
    {
        s_onscreen_forced.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    return s_onscreen_original(a1, a2, a3, a4);
}

} // namespace

bool initialize_interaction_hook()
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    // Fail closed if a resolved entry leads with a call/breakpoint byte; a sibling mod's E9 jump-hook
    // does not trip this, so the cascade's entry-anchored layering still works.
    const DMK::HookConfig hook_config{.prologue_policy = DMK::InlineProloguePolicy::Fail};

    try
    {
        // The interactor look-ray builder entry -> the caller-range filter bound. The module-scoped
        // cascade kept the resolved entry inside the game image or returns 0, so a cross-module collision
        // can no longer yield a bogus return-address range.
        const uintptr_t lookray = anchor_address(AnchorId::InteractorLookRay);
        if (lookray == 0)
        {
            throw std::runtime_error("Interactor look-ray builder cascade did not resolve");
        }
        s_lookray_lo = lookray;
        s_lookray_hi = s_lookray_lo + Constants::INTERACTOR_LOOKRAY_SPAN;

        // Hook the ray-query builder.
        const uintptr_t hook_addr = anchor_address(AnchorId::InteractionRayBuild);
        if (hook_addr == 0)
        {
            throw std::runtime_error("Interaction ray-build cascade did not resolve");
        }

        auto result = DMK::HookManager::get_instance().create_inline_hook(
            "InteractionRayBuild",
            hook_addr,
            reinterpret_cast<void *>(ray_query_build_detour),
            reinterpret_cast<void **>(&s_original),
            hook_config);
        if (!result.has_value())
        {
            throw std::runtime_error("Failed to create interaction ray hook: " +
                                     std::string(DMK::Hook::error_to_string(result.error())));
        }

        // Hook the on-screen reticle projection gate -- the gate that drops shrines/beds/doors when the
        // body is not turned. Non-fatal: failure leaves shrine interaction body-driven.
        const uintptr_t onscreen = anchor_address(AnchorId::InteractionOnScreen);
        if (onscreen != 0)
        {
            auto onscreen_result = DMK::HookManager::get_instance().create_inline_hook(
                "InteractionOnScreenCheck",
                onscreen,
                reinterpret_cast<void *>(on_screen_check_detour),
                reinterpret_cast<void **>(&s_onscreen_original),
                hook_config);
            if (!onscreen_result.has_value())
            {
                logger.warning("InteractionHook[init]: on-screen reticle hook failed ({}); shrine "
                               "interaction stays body-driven (look-ray/loot unaffected).",
                               std::string(DMK::Hook::error_to_string(onscreen_result.error())));
            }
        }
        else
        {
            logger.warning("InteractionHook[init]: on-screen reticle cascade unresolved; shrine "
                           "interaction stays body-driven (look-ray/loot unaffected).");
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("InteractionHook: Initialization failed: {}", e.what());
        return false;
    }
}

} // namespace TPVCamera
