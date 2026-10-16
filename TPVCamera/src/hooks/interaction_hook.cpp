/**
 * @file hooks/interaction_hook.cpp
 * @brief Camera-space interaction: redirect the player look-at ray to the render camera + crosshair.
 *
 * @details The interactor casts a look ray each tick to find the "press to use" target. The look-ray builder
 *          (sub_1808333C8) computes the ray origin (interactor+0x3E8) and direction (interactor+0x3F4) from
 *          the gameplay VIEW SUBSYSTEM - reached via sub_18091C138()+56, the entity subsystem registry, NOT
 *          the render camera - eye-anchored, which the mod never offsets. So in third person the screen-centre
 *          crosshair and the look-ray target diverge by the shoulder offset (worst at close range).
 *          sub_1808333C8 hands that origin+direction to the generic ray-query builder (sub_180530584, called
 *          at fn+0x259). This hook intercepts that builder and - ONLY when the caller's return address is
 *          inside sub_1808333C8 - rewrites the origin to the render camera and the direction to the crosshair
 *          forward (preserving the engine's ray reach), so the look-ray follows the screen centre at all
 *          ranges. After the rewrite interactor+0x3E8/+0x3F4 equal the render-cam pose. The
 *          player is in the cast's skip list, so originating from the (behind-the-player) camera does not
 *          self-hit. Off unless InteractFromCamera is set; a no-op in first person (the published pose is
 *          invalid); SEH-guarded; the caller filter guarantees camera/AI/audio ray queries are never touched.
 *
 *          SCOPE: the look-ray redirect fixes ENTITY interaction (NPCs, dropped loot) that resolves through
 *          the look-ray. Beds, shrines, doors and cooking spots (InteractiveScene usables) are instead picked by
 *          the interactor's candidate update, and candidate_update_detour hands that update the crosshair pose in
 *          place of the eye pose. segment_box_point_detour lays the update's prompt segment along the crosshair
 *          ray, so a cross-centered prompt such as a herb's sits at the crosshair. All three are gated behind
 *          InteractFromCamera and no-op in first person.
 *
 *          DIAGNOSTICS: while InteractFromCamera is on, a rate-limited trace line reports rayBuilds (all
 *          intercepted builds), interactionCalls (builds whose caller is the look-ray - if this stays 0, the
 *          look-ray does NOT use this builder / the filter missed), redirects (builds actually rewritten),
 *          candidateUpdates and crosshairPoses (candidate updates intercepted, and those handed the crosshair
 *          pose), promptSegments (prompt segments laid along the crosshair), and the before/after eye->camera +
 *          dir values so the rewrite is visible in the log.
 */

#include "interaction_hook.hpp"
#include "aob_resolver.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "global_state.hpp"

#include <DetourModKit.hpp>

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace TPVCamera
{
    namespace
    {

        // sub_180530584(out /*rcx*/, origin /*rdx*/, dir /*r8*/, objtypes, flags, skip_ents, n, mode): packs a ray
        // query (copies origin+dir into out) and returns out. We forward all args after optionally rewriting the
        // origin+dir Vec3s the caller passed.
        using RayQueryBuildFunc = uintptr_t(__fastcall *)(uintptr_t out, uintptr_t origin, uintptr_t dir, int objtypes,
                                                          int flags, uintptr_t skip_ents, unsigned __int8 n, char mode);
        std::atomic<RayQueryBuildFunc> s_ray_query_build_original{nullptr};

        // [lo, hi) of sub_1808333C8 (the interactor look-ray builder). The redirect fires only when the query
        // builder's return address lies in this range, so ONLY the interaction look-ray is touched.
        uintptr_t s_lookray_lo = 0;
        uintptr_t s_lookray_hi = 0;

        // diagnostics (touched on the game thread; atomic for race-free reads in the trace line)
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

        // Interactor candidate update (see k_interactorCandidateUpdateCandidates in constants.hpp).
        using CandidateUpdateFunc = uintptr_t(__fastcall *)(uintptr_t interactor, const float *pose, unsigned int mask);
        std::atomic<CandidateUpdateFunc> s_candidate_update_original{nullptr};
        std::atomic<unsigned long long> s_candidate_updates{0}; // candidate updates intercepted
        std::atomic<unsigned long long> s_crosshair_poses{0};   // updates handed the crosshair pose

        /// The pose the candidate update reads, packed as the game stores it.
        struct InteractorPose
        {
            /// World position.
            float position[3];
            /// Rotation quaternion x, y, z, w. Its +Y axis is the look direction.
            float rotation[4];
        };
        static_assert(sizeof(InteractorPose) == 7 * sizeof(float), "InteractorPose must match the game's packed pose.");

        // Closest point of a box to a segment (see k_segmentBoxPointCandidates in constants.hpp).
        using SegmentBoxPointFunc = __m128(__fastcall *)(float *segment, uintptr_t box, float *out);
        std::atomic<SegmentBoxPointFunc> s_segment_box_point_original{nullptr};
        // Return address of the interactor update's call, the only call whose segment the detour rewrites.
        uintptr_t s_prompt_segment_return = 0;
        std::atomic<unsigned long long> s_prompt_segments{0}; // prompt segments laid along the crosshair ray

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
            if (!last_final_pose(px, py, pz, dx, dy, dz))
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
                eye[0] = o[0];
                eye[1] = o[1];
                eye[2] = o[2];
                dold[0] = d[0];
                dold[1] = d[1];
                dold[2] = d[2];
                const float r = std::sqrt(dold[0] * dold[0] + dold[1] * dold[1] + dold[2] * dold[2]);
                const float reach = (r > 1e-3f) ? r : 1.0f; // preserve the engine's interaction range
                dnew[0] = dx * reach;
                dnew[1] = dy * reach;
                dnew[2] = dz * reach;
                // Slide the origin FORWARD along the SAME crosshair ray to the player-eye's projection onto it, so
                // the interaction range cap (sub_1809F94D0, 3.5 m, measured from the ray origin) sees ~eye->object
                // instead of camera->object. The render camera sits FollowDistance behind, so without this a usable
                // at your feet is >3.5 m FROM THE CAMERA and gets culled (worse at high FollowDistance) even though
                // it is <1 m from you. Sliding ALONG the ray keeps the exact same hit (no parallax); only the
                // distance reference moves. eye[] is the original gameplay origin captured above.
                const float proj = (eye[0] - px) * dx + (eye[1] - py) * dy + (eye[2] - pz) * dz;
                const float adv = (proj > 0.0f) ? proj : 0.0f; // never slide backward past the camera
                neworigin[0] = px + dx * adv;
                neworigin[1] = py + dy * adv;
                neworigin[2] = pz + dz * adv;
                o[0] = neworigin[0];
                o[1] = neworigin[1];
                o[2] = neworigin[2];
                d[0] = dnew[0];
                d[1] = dnew[1];
                d[2] = dnew[2];
                done = true;
                // NOTE: this re-aims ONLY the look-ray (entity interaction). Beds/shrines/doors are NOT selected via
                // the look-ray; they go through a separate body-proximity candidate list whose facing verdict is a Lua
                // interaction-constraint (sub_1809D2538 -> sub_181ED69D0 -> CScriptSystem reading the real use-actor),
                // which the render camera cannot influence (sub_18091C138 is the entity-subsystem registry, not a
                // camera). The crosshair-driven path for those usables is instead the candidate update
                // (candidate_update_detour below).
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
                s_dbg_cam[0] = neworigin[0];
                s_dbg_cam[1] = neworigin[1];
                s_dbg_cam[2] = neworigin[2];
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

            DMK::Logger &log = DMK::log();
            const unsigned long long total = s_calls_total.load(std::memory_order_relaxed);
            const unsigned long long look = s_calls_lookray.load(std::memory_order_relaxed);
            const unsigned long long red = s_redirects.load(std::memory_order_relaxed);
            const unsigned long long updates = s_candidate_updates.load(std::memory_order_relaxed);
            const unsigned long long reposed = s_crosshair_poses.load(std::memory_order_relaxed);
            const unsigned long long prompts = s_prompt_segments.load(std::memory_order_relaxed);

            if (s_dbg_valid)
            {
                (void)log.try_log(DMK::LogLevel::Trace,
                                  "InteractionHook[run]: rayBuilds={} interactionCalls={} redirects={} lastReason={} "
                                  "candidateUpdates={} crosshairPoses={} promptSegments={} | eye=({}, {}, {}) -> "
                                  "cam=({}, {}, {}) ; dirOld=({}, {}, {}) -> dirNew=({}, {}, {})",
                                  total, look, red, s_last_reason, updates, reposed, prompts, s_dbg_eye[0],
                                  s_dbg_eye[1], s_dbg_eye[2], s_dbg_cam[0], s_dbg_cam[1], s_dbg_cam[2],
                                  s_dbg_dir_old[0], s_dbg_dir_old[1], s_dbg_dir_old[2], s_dbg_dir_new[0],
                                  s_dbg_dir_new[1], s_dbg_dir_new[2]);
            }
            else
            {
                (void)log.try_log(
                    DMK::LogLevel::Trace,
                    "InteractionHook[run]: rayBuilds={} interactionCalls={} redirects={} lastReason={} "
                    "candidateUpdates={} crosshairPoses={} promptSegments={} (no ray rewrite captured yet: "
                    "if interactionCalls stays 0, the look-ray does not use the builder)",
                    total, look, red, s_last_reason, updates, reposed, prompts);
            }
        }

        /**
         * @brief Detour for the ray-query builder; redirects ONLY the interactor look-ray.
         */
        uintptr_t __fastcall ray_query_build_detour(uintptr_t out, uintptr_t origin, uintptr_t dir, int objtypes,
                                                    int flags, uintptr_t skip_ents, unsigned __int8 n,
                                                    char mode) noexcept
        {
            const DetourScope in_flight;
            s_calls_total.fetch_add(1, std::memory_order_relaxed);

            const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
            if (ra >= s_lookray_lo && ra < s_lookray_hi)
            {
                s_calls_lookray.fetch_add(1, std::memory_order_relaxed);
                if (!settings().interact_from_camera.load(std::memory_order_relaxed))
                {
                    s_last_reason = "InteractFromCamera=off";
                }
                else if (!last_final().valid)
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

            const RayQueryBuildFunc original = s_ray_query_build_original.load(std::memory_order_acquire);
            return original(out, origin, dir, objtypes, flags, skip_ents, n, mode);
        }

        /**
         * @brief Reads the position of the game-owned @p pose under SEH.
         * @details Kept apart from the detour, because the detour's DetourScope needs C++ object unwinding, which a
         *          __try frame cannot hold.
         * @return False when the pose could not be read.
         */
        bool read_pose_position(const float *pose, float (&position)[3]) noexcept
        {
            __try
            {
                position[0] = pose[0];
                position[1] = pose[1];
                position[2] = pose[2];
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /**
         * @brief Depth along the crosshair ray from the camera (@p px, @p py, @p pz) in the unit direction
         *        (@p dx, @p dy, @p dz) of the projection of @p point onto that ray, never behind the camera.
         */
        float crosshair_depth(const float (&point)[3], float px, float py, float pz, float dx, float dy,
                              float dz) noexcept
        {
            return std::max((point[0] - px) * dx + (point[1] - py) * dy + (point[2] - pz) * dz, 0.0f);
        }

        /**
         * @brief Builds the pose that looks along the crosshair ray from the camera (@p px, @p py, @p pz) in the unit
         *        direction (@p dx, @p dy, @p dz), placed at the projection of the eye @p eye onto that ray.
         * @details The position slides along the ray to the eye's projection, as redirect_interaction_ray does, so the
         *          update's reach and distances still measure from the eye. The rotation is RotZ(heading) *
         *          RotX(pitch) with no roll, the first-person look form, so its +Y is the crosshair direction.
         */
        InteractorPose crosshair_pose(const float (&eye)[3], float px, float py, float pz, float dx, float dy,
                                      float dz) noexcept
        {
            const float along = crosshair_depth(eye, px, py, pz, dx, dy, dz);
            const float heading = std::atan2(-dx, dy);
            const float pitch = std::asin(std::clamp(dz, -1.0f, 1.0f));
            const float sin_heading = std::sin(heading * 0.5f);
            const float cos_heading = std::cos(heading * 0.5f);
            const float sin_pitch = std::sin(pitch * 0.5f);
            const float cos_pitch = std::cos(pitch * 0.5f);
            return InteractorPose{
                {px + dx * along, py + dy * along, pz + dz * along},
                {cos_heading * sin_pitch, sin_heading * sin_pitch, sin_heading * cos_pitch, cos_heading * cos_pitch}};
        }

        /**
         * @brief Detour for the interactor candidate update. Hands the update the crosshair pose in place of the
         *        first-person eye pose @p pose.
         * @details The update gives each usable the point on its bounds nearest the pose's look segment, and the
         *          game's reticle gate and ranking pick the point that the view camera projects nearest the reticle.
         *          With the eye pose, a camera that looks away from the eye line sees those points off the crosshair,
         *          and the idle breathing moves them, so the pick flips between usables side by side. With the
         *          crosshair pose, the usable under the crosshair has its point on the crosshair ray and wins outright.
         */
        uintptr_t __fastcall candidate_update_detour(uintptr_t interactor, const float *pose,
                                                     unsigned int mask) noexcept
        {
            const DetourScope in_flight;
            s_candidate_updates.fetch_add(1, std::memory_order_relaxed);

            const CandidateUpdateFunc original = s_candidate_update_original.load(std::memory_order_acquire);
            float px, py, pz, dx, dy, dz;
            float eye[3] = {0.0f, 0.0f, 0.0f};
            if (pose == nullptr || !settings().interact_from_camera.load(std::memory_order_relaxed) ||
                !last_final_pose(px, py, pz, dx, dy, dz) || !read_pose_position(pose, eye))
            {
                return original(interactor, pose, mask);
            }
            const InteractorPose crosshair = crosshair_pose(eye, px, py, pz, dx, dy, dz);
            s_crosshair_poses.fetch_add(1, std::memory_order_relaxed);
            return original(interactor, reinterpret_cast<const float *>(&crosshair), mask);
        }

        /**
         * @brief Lays the game-owned look segment @p segment ({Vec3 start; Vec3 end}) along the crosshair ray from
         *        the camera (@p px, @p py, @p pz) in the unit direction (@p dx, @p dy, @p dz) and keeps its length.
         * @details The start moves to its projection onto the ray, as redirect_interaction_ray does. The segment is
         *          rewritten in place, because the caller reads its end back as the prompt anchor. Kept apart from
         *          the detour, because the detour's DetourScope needs C++ object unwinding, which a __try frame
         *          cannot hold.
         * @return False when the segment could not be read or written.
         */
        bool lay_segment_on_crosshair(float *segment, float px, float py, float pz, float dx, float dy,
                                      float dz) noexcept
        {
            __try
            {
                const float start[3] = {segment[0], segment[1], segment[2]};
                const float ex = segment[3] - start[0];
                const float ey = segment[4] - start[1];
                const float ez = segment[5] - start[2];
                const float length = std::sqrt(ex * ex + ey * ey + ez * ez);
                const float along = crosshair_depth(start, px, py, pz, dx, dy, dz);
                segment[0] = px + dx * along;
                segment[1] = py + dy * along;
                segment[2] = pz + dz * along;
                segment[3] = segment[0] + dx * length;
                segment[4] = segment[1] + dy * length;
                segment[5] = segment[2] + dz * length;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        /**
         * @brief Detour for the segment-to-box closest point. Lays the interactor update's look segment along the
         *        crosshair ray before the original runs. Every other call passes through.
         * @details The HUD places a cross-centered prompt at the end of that segment, 40 m along the eye's look. In
         *          free-look the eye looks away from the camera's view, so that end projects off the screen and the
         *          prompt clamps to the screen edge. Laid along the crosshair ray, the end projects onto the
         *          crosshair, as the eye's end does in first person.
         */
        __m128 __fastcall segment_box_point_detour(float *segment, uintptr_t box, float *out) noexcept
        {
            const DetourScope in_flight;
            float px, py, pz, dx, dy, dz;
            if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == s_prompt_segment_return && segment != nullptr &&
                settings().interact_from_camera.load(std::memory_order_relaxed) &&
                last_final_pose(px, py, pz, dx, dy, dz) && lay_segment_on_crosshair(segment, px, py, pz, dx, dy, dz))
            {
                s_prompt_segments.fetch_add(1, std::memory_order_relaxed);
            }
            const SegmentBoxPointFunc original = s_segment_box_point_original.load(std::memory_order_acquire);
            return original(segment, box, out);
        }

    } // namespace

    DMK::Result<void> initialize_interaction_hook(HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();

        // The default hook::Options prologue policy is Fail: refuse the install when a resolved entry leads
        // with a call or breakpoint byte. A sibling mod's E9 jump hook decodes as a relocatable branch
        // rather than a refusal, so the cascade's entry-anchored layering still works. Each refusal is
        // returned as the library's own typed Error so the caller keeps the ErrorCode.

        // The interactor look-ray builder entry -> the caller-range filter bound. The module-scoped
        // cascade kept the resolved entry inside the game image or returns 0, so a cross-module collision
        // can no longer yield a bogus return-address range.
        const uintptr_t lookray = gated_anchor_address(Feature::Interaction, AnchorId::InteractorLookRay);
        if (lookray == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "interaction_hook/lookray_anchor"});
        }
        s_lookray_lo = lookray;
        s_lookray_hi = s_lookray_lo + Constants::INTERACTOR_LOOKRAY_SPAN;

        // Hook the ray-query builder.
        const uintptr_t hook_addr = gated_anchor_address(Feature::Interaction, AnchorId::InteractionRayBuild);
        if (hook_addr == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "interaction_hook/raybuild_anchor"});
        }

        if (auto armed = install_inline_hook(hooks, "InteractionRayBuild", hook_addr, s_ray_query_build_original,
                                             ray_query_build_detour);
            !armed)
        {
            return armed;
        }

        // The candidate update and the prompt segment are independent features. A failure only warns and leaves that
        // feature on the first-person look, so the look-ray redirect that already installed stays live.
        const uintptr_t candidates = gated_anchor_address(Feature::CrosshairCandidates, AnchorId::InteractorCandidates);
        if (candidates == 0)
        {
            logger.warning("InteractionHook[init]: the candidate update gate failed, so shrines, beds and doors stay "
                           "picked by the first-person look");
        }
        else if (auto armed = install_inline_hook(hooks, "InteractorCandidateUpdate", candidates,
                                                  s_candidate_update_original, candidate_update_detour);
                 !armed)
        {
            logger.warning("InteractionHook[init]: the candidate update hook failed ({}), so shrines, beds and doors "
                           "stay picked by the first-person look",
                           armed.error().message());
        }

        // The detour compares its return address with this one, so the address is published before the hook arms.
        // The gate covers both anchors, so the hook never runs without it.
        s_prompt_segment_return = gated_anchor_address(Feature::CrosshairPrompt, AnchorId::PromptSegmentReturn);
        const uintptr_t segment_box = gated_anchor_address(Feature::CrosshairPrompt, AnchorId::SegmentBoxPoint);
        if (segment_box == 0)
        {
            logger.warning("InteractionHook[init]: the prompt segment gate failed, so cross-centered use prompts stay "
                           "placed by the first-person look");
        }
        else if (auto armed = install_inline_hook(hooks, "SegmentBoxPoint", segment_box, s_segment_box_point_original,
                                                  segment_box_point_detour);
                 !armed)
        {
            logger.warning("InteractionHook[init]: the prompt segment hook failed ({}), so cross-centered use prompts "
                           "stay placed by the first-person look",
                           armed.error().message());
        }

        return {};
    }

} // namespace TPVCamera
