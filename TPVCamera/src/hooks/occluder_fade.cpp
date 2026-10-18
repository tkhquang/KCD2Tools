/**
 * @file hooks/occluder_fade.cpp
 * @brief Implementation of the occluder fade (see occluder_fade.hpp).
 *
 * @details Two threads meet here. The camera frame on the main thread owns the per-node fade levels and publishes a
 *          small {node, dissolve byte} table through a TripleBuffer (core/frame/channels.hpp). The detour runs on the
 *          octree job threads, once per render node per pass, and only reads the published table: lock-free, no
 *          allocation, no logging. A table is reused two frames after it was replaced, and a pass finishes inside
 *          its frame, so a reader never sees a table being rewritten.
 *
 *          The detour raises the CLodValue dissolve byte of a listed node before the original runs (raise_dissolve
 *          combines it with the engine's own LOD-crossfade value). AddOrCreatePersistentRenderObject then copies the
 *          byte onto the node's persistent render object and sets the dissolve flags, and the renderer re-packs the
 *          per-draw constants while it changes. Shadow, recursive and aux-window passes are left alone: a dissolve
 *          byte in a shadow pass would make AddOrCreate free the node's persistent objects every frame.
 */

#include "occluder_fade.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "core/frame/channels.hpp"
#include "dissolve.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <utility>

namespace TPVCamera
{

    namespace
    {
        using AddOrCreateFn = char(__fastcall *)(void *manager, std::uintptr_t temp_data, void **out_object,
                                                 std::uint8_t *lod, const void *world, const std::uint8_t *pass,
                                                 const void *params);

        constexpr int k_max_nodes = Constants::OCCLUDER_FADE_MAX_NODES;

        /** @brief One published frame of the fade: the nodes and the dissolve byte each gets. */
        struct FadeSet
        {
            std::uint64_t stamp_ms = 0;
            int count = 0;
            std::array<std::uintptr_t, k_max_nodes> nodes{};
            std::array<std::uint8_t, k_max_nodes> bytes{};
        };

        /** @brief The camera thread's fade state for one node. */
        struct Track
        {
            std::uintptr_t node = 0;
            std::uint64_t identity = 0; // FadeRequest::identity of the object the track fades
            float level = 0.0f;         // 0 = solid, 1 = fully faded
            float strength = 0.0f;      // how far a fully faded node dithers away now
            float target = 0.0f;        // the strength it was last listed with, which strength eases toward
            float hold = 0.0f;          // seconds left before an unseen node starts to fade back
            float strength_hold = 0.0f; // seconds left before a lowered target eases the strength down
            float waited = 0.0f;        // seconds a solid node has been listed without a break, toward the fade delay
            bool immediate = false;     // listed this frame as one that fades without the delay
            bool seen = false;          // listed by the camera this frame
            bool fresh = false;         // given its slot this frame, so a second request for it merges in
        };

        std::atomic<AddOrCreateFn> s_original{nullptr};
        std::atomic<bool> s_available{false};
        // The published fade, filled by the camera frame and read by the octree jobs (see TripleBuffer).
        TripleBuffer<FadeSet> s_sets;
        std::array<Track, k_max_nodes> s_tracks{};
        int s_track_count = 0;
        // Main-view calls that raised a byte since the faded-node count last changed, for the debug log line.
        std::atomic<std::uint32_t> s_applied{0};

        /**
         * @brief Raises the dissolve byte of @p lod when the node that owns @p temp_data is in @p set.
         * @note Runs on the octree job threads. Reads engine-owned data that is valid for the call.
         */
        void apply_fade(const FadeSet &set, std::uintptr_t temp_data, std::uint8_t *lod) noexcept
        {
            const std::uintptr_t node =
                *reinterpret_cast<const std::uintptr_t *>(temp_data + Constants::RENDER_TEMP_DATA_OWNER_OFFSET);
            if (node == 0)
            {
                return;
            }
            const int count = std::min(set.count, k_max_nodes);
            for (int i = 0; i < count; ++i)
            {
                if (set.nodes[i] != node)
                {
                    continue;
                }
                // The camera stopped updating the table (first person, a menu, a reload): let the node go.
                if (GetTickCount64() - set.stamp_ms > Constants::FADE_STALE_MS)
                {
                    return;
                }
                raise_dissolve(lod, set.bytes[i]);
                s_applied.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }

        /**
         * @brief AddOrCreatePersistentRenderObject detour: fades the listed nodes in the main view.
         * @note Raw arbitrary-signature detour (DMK [B-84]): it must not throw, and every call in it is noexcept.
         */
        char __fastcall add_or_create_detour(void *manager, std::uintptr_t temp_data, void **out_object,
                                             std::uint8_t *lod, const void *world, const std::uint8_t *pass,
                                             const void *params) noexcept
        {
            const AddOrCreateFn original = s_original.load(std::memory_order_acquire);
            const FadeSet *set = s_sets.latest();
            if (set != nullptr && temp_data != 0 && lod != nullptr && pass != nullptr && main_view_pass(pass))
            {
                apply_fade(*set, temp_data, lod);
            }
            return original(manager, temp_data, out_object, lod, world, pass, params);
        }

        /** @brief Publishes the current fade levels, or nothing when no node is faded. */
        void publish() noexcept
        {
            FadeSet &set = s_sets.claim();
            constexpr auto k_full = static_cast<float>(Constants::DISSOLVE_MAX_BYTE);
            int n = 0;
            for (int i = 0; i < s_track_count; ++i)
            {
                const Track &track = s_tracks[static_cast<std::size_t>(i)];
                const float amount = std::clamp(track.level, 0.0f, 1.0f) * std::clamp(track.strength, 0.0f, 1.0f);
                const int byte = static_cast<int>(std::lround(amount * k_full));
                if (byte <= 0)
                {
                    continue;
                }
                set.nodes[static_cast<std::size_t>(n)] = track.node;
                set.bytes[static_cast<std::size_t>(n)] =
                    static_cast<std::uint8_t>(std::min(byte, Constants::DISSOLVE_MAX_BYTE));
                ++n;
            }
            set.count = n;
            set.stamp_ms = GetTickCount64();
            if (n > 0)
            {
                s_sets.publish(set);
            }
            else
            {
                s_sets.withdraw();
            }
        }

    } // namespace

    DMK::Result<void> initialize_occluder_fade(DMK::hook::HookStack &hooks)
    {
        const std::uintptr_t target = gated_anchor_address(Feature::OccluderFade, AnchorId::AddOrCreateRO);
        if (target == 0)
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "occluder_fade/add_or_create_anchor"});
        }
        auto result = DMK::hook::inline_at<&add_or_create_detour>(DMK::hook::InlineRequest{
            .name = "AddOrCreatePersistentRenderObject",
            .target = DMK::Address{target},
        });
        if (!result.has_value())
        {
            return std::unexpected(result.error());
        }
        s_original.store(result->original<AddOrCreateFn>(), std::memory_order_release);
        if (auto armed = hooks.push(std::move(*result)).enable(); !armed.has_value())
        {
            return std::unexpected(armed.error());
        }
        s_available.store(true, std::memory_order_release);
        DMK::log().info("OccluderFade: dissolve hook on AddOrCreatePersistentRenderObject at {}",
                        DMK::format::format_address(target));
        return {};
    }

    bool occluder_fade_available() noexcept
    {
        return s_available.load(std::memory_order_acquire);
    }

    void update_occluder_fade(std::span<const FadeRequest> requests, float delta_time) noexcept
    {
        if (!s_available.load(std::memory_order_relaxed))
        {
            return;
        }
        for (int i = 0; i < s_track_count; ++i)
        {
            s_tracks[static_cast<std::size_t>(i)].seen = false;
            s_tracks[static_cast<std::size_t>(i)].fresh = false;
        }
        const auto find_track = [](std::uintptr_t node)
        {
            for (int i = 0; i < s_track_count; ++i)
            {
                if (s_tracks[static_cast<std::size_t>(i)].node == node)
                {
                    return i;
                }
            }
            return -1;
        };
        // Two passes, so a node listed this frame is never the one evicted: first mark the listed nodes that already
        // have a track, then give the new ones a slot. With the table full a new node takes the slot of the faintest
        // track nobody listed this frame (one still in its hold or fade-out), so what hides the character now always
        // fades; the caller lists at most OCCLUDER_FADE_MAX_NODES nodes, so such a track always exists.
        for (std::size_t pass = 0; pass < 2; ++pass)
        {
            for (const FadeRequest &request : requests)
            {
                const auto node = reinterpret_cast<std::uintptr_t>(request.node);
                if (node == 0)
                {
                    continue;
                }
                int found = find_track(node);
                if (found >= 0 && !s_tracks[static_cast<std::size_t>(found)].fresh &&
                    s_tracks[static_cast<std::size_t>(found)].identity != request.identity)
                {
                    // The address holds another object now: it starts solid in the old one's slot.
                    s_tracks[static_cast<std::size_t>(found)] =
                        Track{.node = node, .identity = request.identity, .fresh = true};
                }
                // Pass 0 takes the nodes that already had a track; pass 1 gives the others a slot, and merges a second
                // request for a node that got its slot earlier in the same pass.
                if ((pass == 0 && found < 0) ||
                    (pass == 1 && found >= 0 && !s_tracks[static_cast<std::size_t>(found)].fresh))
                {
                    continue;
                }
                if (found < 0)
                {
                    if (s_track_count < k_max_nodes)
                    {
                        found = s_track_count++;
                    }
                    else
                    {
                        for (int i = 0; i < s_track_count; ++i)
                        {
                            const Track &t = s_tracks[static_cast<std::size_t>(i)];
                            if (!t.seen && (found < 0 || t.level < s_tracks[static_cast<std::size_t>(found)].level))
                            {
                                found = i;
                            }
                        }
                        if (found < 0)
                        {
                            continue;
                        }
                    }
                    s_tracks[static_cast<std::size_t>(found)] =
                        Track{.node = node, .identity = request.identity, .fresh = true};
                }
                Track &track = s_tracks[static_cast<std::size_t>(found)];
                // A node listed twice in one frame keeps the stronger request.
                track.target = track.seen ? std::max(track.target, request.strength) : request.strength;
                track.immediate = (track.seen && track.immediate) || request.immediate;
                track.seen = true;
            }
        }

        const float fade_in = delta_time / Constants::OCCLUDER_FADE_IN_SECONDS;
        const float fade_out = delta_time / Constants::OCCLUDER_FADE_OUT_SECONDS;
        int kept = 0;
        for (int i = 0; i < s_track_count; ++i)
        {
            Track track = s_tracks[static_cast<std::size_t>(i)];
            if (track.seen)
            {
                // A solid node fades in only once it has been listed for the delay; one already fading (or fading
                // back out) carries on at once.
                if (track.level <= 0.0f && !track.immediate)
                {
                    track.waited += delta_time;
                }
                if (track.level > 0.0f || track.immediate || track.waited >= Constants::OCCLUDER_FADE_DELAY_SECONDS)
                {
                    track.level = std::min(1.0f, track.level + fade_in);
                }
                track.hold = Constants::OCCLUDER_FADE_HOLD_SECONDS;
                // The strength follows the target like the level does, so a node whose target changes (a live edit of
                // OccluderFadeMinOpacity) does not jump: it rises at the fade-in rate, and drops at the fade-out rate
                // only once the target has stayed lower for the hold.
                if (track.level <= fade_in || track.target >= track.strength)
                {
                    track.strength =
                        (track.level <= fade_in) ? track.target : std::min(track.target, track.strength + fade_in);
                    track.strength_hold = Constants::OCCLUDER_FADE_HOLD_SECONDS;
                }
                else if (track.strength_hold > 0.0f)
                {
                    track.strength_hold -= delta_time;
                }
                else
                {
                    track.strength = std::max(track.target, track.strength - fade_out);
                }
            }
            else if (track.hold > 0.0f && track.level > 0.0f)
            {
                track.hold -= delta_time;
            }
            else
            {
                track.level -= fade_out;
            }
            if (track.level > 0.0f || track.seen)
            {
                s_tracks[static_cast<std::size_t>(kept++)] = track;
            }
        }
        const int previous = s_track_count;
        s_track_count = kept;
        publish();

        if (kept != previous)
        {
            (void)DMK::log().try_log(DMK::LogLevel::Debug, "OccluderFade: {} node(s) faded ({} main-view applies)",
                                     kept, s_applied.exchange(0, std::memory_order_relaxed));
        }
    }

    void reset_occluder_fade() noexcept
    {
        s_track_count = 0;
        // A job thread that already loaded the old table keeps reading it safely: the tables are static.
        s_sets.withdraw();
    }

} // namespace TPVCamera
