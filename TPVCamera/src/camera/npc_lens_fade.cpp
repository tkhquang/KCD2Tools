/**
 * @file camera/npc_lens_fade.cpp
 * @brief The NPC lens fade (see npc_lens_fade.hpp).
 */

#include "camera/npc_lens_fade.hpp"
#include "camera/blend.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "hooks/character_fade.hpp"
#include "hooks/shader_twins.hpp"
#include "render_occlusion.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace TPVCamera
{
    namespace
    {
        // The depth band of the fade (meters of view depth): fully faded from k_npc_full_depth in, where the lens near
        // plane (up to 0.4 m) cuts into a body, solid from k_npc_clear_depth out. The band in between eases a person
        // walking into the lens instead of switching it at the plane.
        constexpr float k_npc_full_depth = 0.45f;
        constexpr float k_npc_clear_depth = 1.2f;
        // Seconds an actor takes to fade fully out, and back in, as its target changes (real time, linear).
        constexpr float k_npc_fade_in_seconds = 0.1f;
        constexpr float k_npc_fade_out_seconds = 0.3f;
        // The actors faded at once (the nearest are kept), the parts kept per actor, and the parts read per frame.
        constexpr int k_max_actors = 8;
        constexpr int k_max_parts_per_actor = 8;
        constexpr int k_max_parts = 64;

        /** @brief The fade of one person or animal, keyed by its EntityId. */
        struct Track
        {
            std::uint32_t id = 0;
            std::uintptr_t actor = 0; // its CEntity, for the trace
            void *node = nullptr;     // its own render proxy, for the trace's name and rule
            float level = 0.0f;       // 0 solid .. 1 faded as far as NpcLensFadeMinOpacity allows
            float target = 0.0f;
            float depth = 0.0f;   // the nearest part's view depth this frame
            float extent[3] = {}; // the nearest part's box size, for the trace
            int quarter = 0;      // the quarter of level the trace last named
            bool seen = false;    // its parts showed in the band this frame
            std::array<std::uintptr_t, k_max_parts_per_actor> parts{};
            // Each part's render node, which the shader twins match its eye, hair and eye film items by.
            std::array<std::uintptr_t, k_max_parts_per_actor> nodes{};
            int part_count = 0;
        };

        std::array<Track, k_max_actors> s_tracks{};
        int s_track_count = 0;
        // The once-a-second debug line: frames with a fade, and when the window started.
        int s_fading_frames = 0;
        std::uint64_t s_window_start_ms = 0;

        /** @brief The track of @p id, a new one when there is room, or nullptr. */
        [[nodiscard]] Track *track_for(std::uint32_t id) noexcept
        {
            for (int i = 0; i < s_track_count; ++i)
            {
                if (s_tracks[static_cast<std::size_t>(i)].id == id)
                {
                    return &s_tracks[static_cast<std::size_t>(i)];
                }
            }
            if (s_track_count == k_max_actors)
            {
                return nullptr;
            }
            Track &track = s_tracks[static_cast<std::size_t>(s_track_count++)];
            track = Track{.id = id};
            return &track;
        }

        /** @brief Adds the part of @p entity drawn by @p node to @p track once. */
        void add_part(Track &track, std::uintptr_t entity, std::uintptr_t node) noexcept
        {
            const auto begin = track.parts.begin();
            const auto end = begin + track.part_count;
            if (std::find(begin, end, entity) == end && track.part_count < k_max_parts_per_actor)
            {
                track.nodes[static_cast<std::size_t>(track.part_count)] = node;
                track.parts[static_cast<std::size_t>(track.part_count++)] = entity;
            }
        }

        /** @brief Whether @p box holds @p point across: x and y only, so a rider above the mount's box still counts. */
        [[nodiscard]] bool box_holds_across(const float (&box)[6], const Vector3 &point) noexcept
        {
            return point.x >= box[0] && point.x <= box[3] && point.y >= box[1] && point.y <= box[4];
        }

        /**
         * @brief Logs @p track's start and end of a fade, and its level as it crosses each quarter, at Trace.
         * @details A level falls back to a lower quarter only once it is k_trace_hysteresis under the mark, so a level
         *          resting on a mark (a person standing still at that depth) is logged once, not on every wobble.
         */
        void trace_track(Track &track) noexcept
        {
            constexpr float k_trace_hysteresis = 0.05f;
            const float level = std::clamp(track.level, 0.0f, 1.0f);
            int quarter = track.quarter;
            if (level >= 0.25f * static_cast<float>(quarter + 1) ||
                level < 0.25f * static_cast<float>(quarter) - k_trace_hysteresis || level <= 0.0f)
            {
                quarter = static_cast<int>(std::floor(level * 4.0f));
            }
            if (quarter == track.quarter || !DMK::log().is_enabled(DMK::LogLevel::Trace))
            {
                track.quarter = quarter;
                return;
            }
            char name[160] = {};
            render_node_name(track.node, name, static_cast<int>(sizeof(name)));
            char rule[160] = {};
            occluder_rule_where(track.node, rule, static_cast<int>(sizeof(rule)));
            const char *what = (track.quarter == 0)        ? "starts to fade"
                               : (quarter == 0)            ? "is solid again"
                               : (quarter > track.quarter) ? "fades further"
                                                           : "fades back";
            (void)DMK::log().try_log(DMK::LogLevel::Trace,
                                     "NpcLensFade: \"{}\" (id {:#x}, rule {}) {}: level {:.2f}, {} at {:.2f} m (box "
                                     "{:.2f} x {:.2f} x {:.2f} m), {} part(s)",
                                     name[0] != '\0' ? name : "?", track.id, rule[0] != '\0' ? rule : "-", what,
                                     track.level, track.seen ? "nearest part" : "out of the band, last", track.depth,
                                     track.extent[0], track.extent[1], track.extent[2], track.part_count);
            track.quarter = quarter;
        }

        /** @brief Publishes every track's parts at its level, and drops the tracks that are solid and gone. */
        void publish(float min_opacity) noexcept
        {
            std::array<NpcFadeEntry, Constants::NPC_FADE_MAX_ENTITIES> entries{};
            std::array<std::uintptr_t, Constants::NPC_FADE_MAX_ENTITIES> nodes{};
            std::size_t n = 0;
            int kept = 0;
            for (int i = 0; i < s_track_count; ++i)
            {
                const Track &track = s_tracks[static_cast<std::size_t>(i)];
                if (track.level <= 0.0f && !track.seen)
                {
                    continue; // solid and out of the band: done
                }
                s_tracks[static_cast<std::size_t>(kept++)] = track;
                const int byte =
                    static_cast<int>(std::lround(std::clamp(track.level, 0.0f, 1.0f) * (1.0f - min_opacity) *
                                                 static_cast<float>(Constants::DISSOLVE_MAX_BYTE)));
                for (int p = 0; p < track.part_count && byte > 0 && n < entries.size(); ++p)
                {
                    nodes[n] = track.nodes[static_cast<std::size_t>(p)];
                    entries[n++] = NpcFadeEntry{.entity = track.parts[static_cast<std::size_t>(p)],
                                                .byte = static_cast<std::uint8_t>(byte)};
                }
            }
            s_track_count = kept;
            set_npc_fade(std::span<const NpcFadeEntry>(entries.data(), n));
            set_npc_twin_nodes(std::span<const std::uintptr_t>(nodes.data(), n));
        }

        /** @brief The once-a-second debug line while anyone fades: how many, and the draws the detour raised. */
        void log_window(bool fading) noexcept
        {
            if (fading)
            {
                ++s_fading_frames;
            }
            const std::uint64_t now_ms = GetTickCount64();
            if (now_ms - s_window_start_ms < 1000)
            {
                return;
            }
            const std::uint32_t applies = take_npc_fade_applies();
            if (s_fading_frames > 0)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug, "NpcLensFade: {} actor(s) tracked, raised draws per fading frame {:.1f}",
                    s_track_count, static_cast<float>(applies) / static_cast<float>(s_fading_frames));
            }
            s_fading_frames = 0;
            s_window_start_ms = now_ms;
        }
    } // namespace

    void update_npc_lens_fade(const NpcLensFadeInput &in)
    {
        if (!in.enabled && s_track_count == 0)
        {
            return;
        }
        DMK_PROFILE_SCOPE("camera.npc_lens_fade");
        const LiveSettings &cfg = settings();
        const float min_opacity = std::clamp(cfg.npc_lens_fade_min_opacity.load(std::memory_order_relaxed), 0.0f, 1.0f);
        for (int i = 0; i < s_track_count; ++i)
        {
            s_tracks[static_cast<std::size_t>(i)].seen = false;
        }
        if (in.enabled)
        {
            std::array<LensActorPart, k_max_parts> parts{};
            const int n = actors_before_lens(in.view, k_npc_clear_depth, in.player_entity, parts.data(), k_max_parts);
            // The ridden mount: the actor one of whose parts holds the player's feet across.
            std::uint32_t mount_id = 0;
            if (in.mounted)
            {
                for (int i = 0; i < n && mount_id == 0; ++i)
                {
                    if (box_holds_across(parts[static_cast<std::size_t>(i)].bbox, in.player_origin))
                    {
                        mount_id = parts[static_cast<std::size_t>(i)].actor_id;
                    }
                }
            }
            // Nearest parts first, so the actors kept when more show than there are tracks are the nearest.
            std::sort(parts.begin(), parts.begin() + n,
                      [](const LensActorPart &a, const LensActorPart &b) { return a.depth < b.depth; });
            for (int i = 0; i < n; ++i)
            {
                const LensActorPart &part = parts[static_cast<std::size_t>(i)];
                if (part.actor_id == 0 || part.actor_id == mount_id)
                {
                    continue;
                }
                Track *track = track_for(part.actor_id);
                if (track == nullptr)
                {
                    continue;
                }
                if (!track->seen)
                {
                    // The first (nearest) part of this actor this frame: the parts list is rebuilt from it.
                    track->seen = true;
                    track->depth = part.depth;
                    track->part_count = 0;
                    track->actor = part.actor;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        track->extent[axis] = part.bbox[axis + 3] - part.bbox[axis];
                    }
                }
                if (part.entity == part.actor || track->node == nullptr)
                {
                    track->node = part.node;
                }
                add_part(*track, part.entity, reinterpret_cast<std::uintptr_t>(part.node));
            }
        }
        bool fading = false;
        for (int i = 0; i < s_track_count; ++i)
        {
            Track &track = s_tracks[static_cast<std::size_t>(i)];
            track.target =
                track.seen
                    ? 1.0f - smoothstep(std::clamp(
                                 (track.depth - k_npc_full_depth) / (k_npc_clear_depth - k_npc_full_depth), 0.0f, 1.0f))
                    : 0.0f;
            track.level = ramp_weight(track.level, track.target,
                                      track.target > track.level ? k_npc_fade_in_seconds : k_npc_fade_out_seconds,
                                      in.real_seconds);
            trace_track(track);
            fading = fading || track.level > 0.0f;
        }
        publish(min_opacity);
        log_window(fading);
    }

    void reset_npc_lens_fade() noexcept
    {
        if (s_track_count == 0)
        {
            return;
        }
        s_track_count = 0;
        set_npc_fade({});
        set_npc_twin_nodes({});
    }
} // namespace TPVCamera
