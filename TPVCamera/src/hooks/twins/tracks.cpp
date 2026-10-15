/**
 * @file hooks/twins/tracks.cpp
 * @brief The shader twins' main-thread tracks: the refills, the PSO and warm-set records, the character's readiness,
 * the published swap set and rows, and the per-second log.
 */

#include "hooks/twins/twin_state.hpp"

namespace TPVCamera::Twins
{
    namespace
    {

        /** @brief Schedules a retry refill of @p track, which lost a wait; an earlier one still due keeps its time. */
        void mark_retry(Track &track, std::uint64_t now) noexcept
        {
            if (!track.retry)
            {
                trace_event(trace::Event::RetryScheduled, track.node, 0, 0, 0, Constants::TREE_HOLE_RETRY_MS);
                track.retry = true;
                track.retry_ms = now;
            }
        }

        /** @brief The twin PSOs a ready record brings, for the log. */
        [[nodiscard]] int twin_pso_count(const PsoRecord &record) noexcept
        {
            int count = 0;
            for (int i = 0; i < k_pso_slots; ++i)
            {
                count += ((record.slots & (1u << i)) != 0 && record.twin[static_cast<std::size_t>(i)] != 0) ? 1 : 0;
            }
            return count;
        }

        /** @brief The source index whose twin is @p twin, or -1. */
        [[nodiscard]] int find_source_by_twin(std::uintptr_t twin) noexcept
        {
            for (int i = 0; i < s_source_count; ++i)
            {
                if (s_sources[static_cast<std::size_t>(i)].twin == twin)
                {
                    return i;
                }
            }
            return -1;
        }

        /**
         * @brief Sets whether @p source's twin is warm for the fade, for the swap set and the detour; the entry of a
         *        given-up twin reads done whatever it is set to, so no dissolving build of its source builds a set of
         *        it aside any more.
         */
        void set_fade_warm(Source &source, bool warm) noexcept
        {
            source.warm = warm;
            if (source.warm_slot >= 0)
            {
                s_warm_done[static_cast<std::size_t>(source.warm_slot)].store(warm || source.state == TwinState::Failed,
                                                                              std::memory_order_release);
            }
        }

        /**
         * @brief Turns @p source cold when a render thread reported a fading build of its twin that had to keep the
         *        stock PSOs (build_fading_psos()): the next swap set leaves its dissolving items on the stock shader,
         *        and the warm-up holds the set that build handed over until it can draw.
         */
        void take_cold_report(Source &source, std::uint64_t now) noexcept
        {
            if (source.warm_slot < 0 ||
                !s_warm_cold[static_cast<std::size_t>(source.warm_slot)].exchange(false, std::memory_order_acq_rel))
            {
                return;
            }
            const bool was_warm = source.warm;
            source.cold = true;
            source.cold_ms = now;
            source.drawable_since_ms = 0;
            set_fade_warm(source, false);
            if (was_warm)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "ShaderTwins: {} twin {:#x} for {} turned cold: a fading build asked for a permutation not "
                    "compiled yet and kept the stock PSOs; its dissolving items go back to the stock shader while it "
                    "warms up again",
                    blobs::k_twins[source.kind].label, source.twin, source.name.data());
            }
        }

        /** @brief The index of the kept set whose key is @p key, or -1. */
        [[nodiscard]] int find_warm_entry(std::uint64_t key) noexcept
        {
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                if (s_warm_entries[static_cast<std::size_t>(i)].record.key == key)
                {
                    return i;
                }
            }
            return -1;
        }

        /** @brief Drops the PSO references a kept set holds, so a warm one holds none. */
        void release_entry_psos(WarmEntry &entry) noexcept
        {
            drop_record(entry.record);
            entry.record.stock = {};
            entry.record.twin = {};
        }

        /**
         * @brief Makes room for one more kept set by dropping the one that turned warm first (it holds no PSO, and
         *        its key stays taken, so its set is not built again).
         * @return The freed index, or -1 when every kept set still waits.
         */
        [[nodiscard]] int make_warm_room() noexcept
        {
            int oldest = -1;
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                const WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
                if (entry.warm &&
                    (oldest < 0 || entry.warm_ms < s_warm_entries[static_cast<std::size_t>(oldest)].warm_ms))
                {
                    oldest = i;
                }
            }
            return oldest;
        }

        [[nodiscard]] const char *origin_text(WarmOrigin origin) noexcept
        {
            switch (origin)
            {
            case WarmOrigin::Ahead:
                return "built ahead";
            case WarmOrigin::Warming:
                return "built aside by a fading build of the stock shader";
            case WarmOrigin::Cold:
                return "from a fading build that kept the stock PSOs";
            case WarmOrigin::None:
            default:
                return "of unknown origin";
            }
        }

        /** @brief The kept sets that still wait. */
        [[nodiscard]] int count_waiting_entries() noexcept
        {
            int waiting = 0;
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                waiting += s_warm_entries[static_cast<std::size_t>(i)].warm ? 0 : 1;
            }
            return waiting;
        }

        /**
         * @brief Gives the twin of the kept set at @p at up for the session, because that set cannot draw (see
         *        Constants::CHARACTER_TWIN_GIVE_UP_MS): its source is Failed, so its items keep the stock shader, at
         *        rest too, no dissolving build of it builds a set aside any more, and every kept set of it goes (their
         *        keys stay taken, so none is built again).
         * @details refresh_character_ready() does not let a given-up Hair or IllumFade twin hold the eyes back, so
         *          they still fade and the close-up fade hides the head only at the very end; a given-up Eye twin
         *          keeps the eyes from fading, and the head is hidden most of the way along.
         */
        void give_up_fade(int at, std::uint64_t now) noexcept
        {
            const WarmEntry stuck = s_warm_entries[static_cast<std::size_t>(at)];
            Source &source = s_sources[static_cast<std::size_t>(stuck.source)];
            source.state = TwinState::Failed;
            source.cold = false;
            source.drawable_since_ms = 0;
            set_fade_warm(source, false);
            int kept = 0;
            for (int i = 0; i < s_warm_entry_count; ++i)
            {
                const WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
                if (entry.source == stuck.source)
                {
                    drop_record(entry.record);
                    continue;
                }
                if (kept != i)
                {
                    s_warm_entries[static_cast<std::size_t>(kept)] = entry;
                }
                ++kept;
            }
            const int dropped = s_warm_entry_count - kept;
            s_warm_entry_count = kept;
            const bool eye = source.kind == k_eye_kind;
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "ShaderTwins: {} twin {:#x} for {} given up for the fade: a set of it ({}, RT mask {:#x}, flags {:#x}) "
                "{} for {:.0f} s ({:.0f} s after it arrived); its items keep the stock shader for the session ({} kept "
                "set(s) dropped), {}",
                blobs::k_twins[source.kind].label, source.twin, source.name.data(), origin_text(stuck.record.origin),
                stuck.record.rt_mask, stuck.record.flags,
                stuck.record.twin_built ? "has a pass with no twin PSO, or one the engine stopped trying to create "
                                          "(its permutation failed to compile),"
                                        : "could not be built (the twin build failed)",
                static_cast<double>(now - stuck.stalled_ms) / 1000.0,
                static_cast<double>(now - stuck.first_ms) / 1000.0, dropped,
                eye ? "and the close-up fade hides the head most of the way along"
                    : "the eyes can still fade, and then the close-up fade hides the head only at the very end");
        }

        [[nodiscard]] float finite_or(float value, float fallback) noexcept
        {
            return std::isfinite(value) ? value : fallback;
        }
    } // namespace

    void update_tracks(std::span<void *const> trees, std::span<void *const> frames, float delta_time,
                       std::uint64_t now) noexcept
    {
        for (int i = 0; i < s_track_count; ++i)
        {
            s_tracks[static_cast<std::size_t>(i)].seen = false;
        }
        std::array<void *, Constants::TREE_HOLE_MAX_NODES> nodes{};
        const std::size_t n_frames = std::min(frames.size(), nodes.size());
        const std::size_t listed = n_frames + std::min(trees.size(), nodes.size() - n_frames);
        std::copy_n(frames.begin(), n_frames, nodes.begin());
        std::copy_n(trees.begin(), listed - n_frames, nodes.begin() + static_cast<std::ptrdiff_t>(n_frames));
        for (std::size_t i = 0; i < listed; ++i)
        {
            const int found = find_track(reinterpret_cast<std::uintptr_t>(nodes[i]));
            if (found < 0)
            {
                continue;
            }
            Track &track = s_tracks[static_cast<std::size_t>(found)];
            if (track.frame != (i < n_frames))
            {
                // The camera rules moved the node between a plant and a frame (a rule whose check gives the other
                // kind, or new rules), so its twin kind changed: it is swapped off and taken again below.
                queue_release(track, "kind changed");
                const auto at = s_tracks.begin() + static_cast<std::ptrdiff_t>(found);
                std::copy(at + 1, s_tracks.begin() + static_cast<std::ptrdiff_t>(s_track_count), at);
                --s_track_count;
                continue;
            }
            track.seen = true;
            track.hold = Constants::TREE_HOLE_HOLD_SECONDS;
        }
        for (std::size_t i = 0; i < listed; ++i)
        {
            const auto node = reinterpret_cast<std::uintptr_t>(nodes[i]);
            if (node == 0 || find_track(node) >= 0)
            {
                continue;
            }
            if (const Invalidate state = node_state_guarded(node);
                state == Invalidate::NotTheNode || state == Invalidate::Fault)
            {
                trace_event(trace::Event::TrackRejected, node, 0, 0, 0, static_cast<std::uint64_t>(state));
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "ShaderTwins: node {:#x} skipped ({})", node,
                                         invalidate_text(state));
                continue;
            }
            int slot = -1;
            if (s_track_count < Constants::TREE_HOLE_MAX_NODES)
            {
                slot = s_track_count++;
            }
            else
            {
                for (int t = 0; t < s_track_count; ++t)
                {
                    const Track &candidate = s_tracks[static_cast<std::size_t>(t)];
                    if (!candidate.seen && (slot < 0 || candidate.hold < s_tracks[static_cast<std::size_t>(slot)].hold))
                    {
                        slot = t;
                    }
                }
                if (slot < 0)
                {
                    trace_event(trace::Event::TrackRejected, node, 0, 0, 0, 4);
                    continue;
                }
                queue_release(s_tracks[static_cast<std::size_t>(slot)], "evicted");
            }
            Track &track = s_tracks[static_cast<std::size_t>(slot)];
            track = Track{.node = node,
                          .hold = Constants::TREE_HOLE_HOLD_SECONDS,
                          .seen = true,
                          .fresh = true,
                          .swapped_ms = now};
            track.frame = i < n_frames;
            track.merged = merged_origin_guarded(node, track.origin.data());
            if (track.merged)
            {
                // A merged grass patch has no single mesh to name; its origin tells the patches apart.
                (void)std::format_to_n(track.name.data(), track.name.size() - 1, "grass patch at ({:.0f}, {:.0f})",
                                       track.origin[0], track.origin[1]);
            }
            else
            {
                render_node_name(nodes[i], track.name.data(), static_cast<int>(track.name.size()));
            }
            trace_event(trace::Event::TrackAdded, node, 0, 0, 0, track.frame, track.merged);
            trace::label(node, track.name.data());
        }

        int kept = 0;
        for (int i = 0; i < s_track_count; ++i)
        {
            Track &track = s_tracks[static_cast<std::size_t>(i)];
            if (!track.seen)
            {
                track.hold -= delta_time;
                if (track.hold <= 0.0f)
                {
                    queue_release(track, "unlisted");
                    continue;
                }
            }
            s_tracks[static_cast<std::size_t>(kept++)] = track;
        }
        s_track_count = kept;
    }

    void apply_refills(std::uint64_t now) noexcept
    {
        for (int i = 0; i < s_released_count; ++i)
        {
            Track &released = s_released[static_cast<std::size_t>(i)];
            release_track(released, now, released.why);
        }
        s_released_count = 0;
        const bool plants_ready = scope_ready(blobs::TwinScope::ListedPlants);
        const bool frames_ready = scope_ready(blobs::TwinScope::ListedFrames);
        int refilled = 0;
        for (int i = 0; i < s_track_count; ++i)
        {
            Track &track = s_tracks[static_cast<std::size_t>(i)];
            if (!track.fresh && !s_refill_all)
            {
                continue;
            }
            // Publishing a target permits passive discovery on natural render submissions. Until a twin can
            // be selected, forcing its persistent objects to rebuild only recreates the stock rendering.
            if (!(track.frame ? frames_ready : plants_ready))
            {
                continue;
            }
            track.activated = true;
            const Invalidate result =
                refill(track, track.fresh ? trace::RefillReason::Activate : trace::RefillReason::SourceReady);
            refilled += result == Invalidate::Done ? 1 : 0;
            if (track.fresh)
            {
                track.fresh = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug, "ShaderTwins: swapped on {} (node {:#x}, {})",
                                         track.name.data(), track.node, invalidate_text(result));
            }
        }
        if (s_refill_all)
        {
            s_refill_all = false;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: refilled {} of {} swapped tree(s) for a twin that became ready",
                                     refilled, s_track_count);
        }
    }

    void drain_pso_records(std::uint64_t now) noexcept
    {
        if (s_pso_record_lost.exchange(false, std::memory_order_relaxed))
        {
            for (int t = 0; t < s_track_count; ++t)
            {
                mark_retry(s_tracks[static_cast<std::size_t>(t)], now);
            }
        }
        std::uintptr_t deferred_node = 0;
        while (s_deferred_nodes.pop(deferred_node))
        {
            const int track = find_track(deferred_node);
            if (track >= 0 && !s_tracks[static_cast<std::size_t>(track)].deferred)
            {
                trace_event(trace::Event::DeferredScheduled, deferred_node, 0, 0, 0,
                            Constants::TREE_HOLE_DEFER_RETRY_MS);
                s_tracks[static_cast<std::size_t>(track)].deferred = true;
                s_tracks[static_cast<std::size_t>(track)].deferred_ms = now;
            }
        }
        PsoRecord record{};
        while (s_pso_records.pop(record))
        {
            trace_event(trace::Event::RecordReceived, record.node, record.object, record.twin_shader, 0, record.slots,
                        record.rt_mask);
            const int track = find_track(record.node);
            if (track < 0)
            {
                trace_event(trace::Event::RecordDropped, record.node, record.object, record.twin_shader, 0, 3);
                drop_record(record);
                continue;
            }
            int slot = -1;
            for (int i = 0; i < s_pending_count && slot < 0; ++i)
            {
                const PsoRecord &held = s_pending[static_cast<std::size_t>(i)].record;
                slot = (held.object == record.object && held.node == record.node) ? i : -1;
            }
            if (slot >= 0)
            {
                PendingPsos &pending = s_pending[static_cast<std::size_t>(slot)];
                drop_record(pending.record);
                pending.record = record;
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
                pending.traced_state = 255;
                pending.traced_slots = 0;
                pending.traced_attempts_ms = 0;
#endif
                trace_event(trace::Event::RecordReplaced, record.node, record.object, record.twin_shader, 0,
                            record.slots, pending.first_ms);
                continue;
            }
            if (s_pending_count >= static_cast<int>(s_pending.size()))
            {
                trace_event(trace::Event::RecordLost, record.node, record.object, record.twin_shader, 0, 2);
                s_pso_dropped.fetch_add(1, std::memory_order_relaxed);
                mark_retry(s_tracks[static_cast<std::size_t>(track)], now);
                drop_record(record);
                continue;
            }
            s_pending[static_cast<std::size_t>(s_pending_count++)] = PendingPsos{.record = record, .first_ms = now};
        }
    }

    void poll_pso_records(std::uint64_t now) noexcept
    {
        for (int t = 0; t < s_track_count; ++t)
        {
            Track &track = s_tracks[static_cast<std::size_t>(t)];
            if (!track.activated)
            {
                continue;
            }
            int waiting = 0;
            int ready = 0;
            int psos = 0;
            std::uint64_t oldest = now;
            std::uintptr_t stuck_twin = 0;
            std::uint64_t stuck_mask = 0;
            bool stuck_built = true;
            for (int i = 0; i < s_pending_count; ++i)
            {
                PendingPsos &pending = s_pending[static_cast<std::size_t>(i)];
                const PsoRecord &record = pending.record;
                if (record.node != track.node)
                {
                    continue;
                }
                ++waiting;
                oldest = std::min(oldest, pending.first_ms);
                const auto state = record.twin_built
                                       ? pso_set_ready_guarded(record.stock.data(), record.twin.data(), record.slots)
                                       : PsoReady::Waiting;
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
                trace_pending_psos(pending, now);
                const auto traced_state = record.twin_built ? static_cast<std::uint8_t>(state) : std::uint8_t{3};
                if (pending.traced_state != traced_state)
                {
                    pending.traced_state = traced_state;
                    trace_event(trace::Event::RecordState, record.node, record.object, record.twin_shader, 0,
                                traced_state, pending.first_ms);
                }
#endif
                if (record.twin_built && state == PsoReady::Ready)
                {
                    ++ready;
                    psos += twin_pso_count(record);
                }
                else if (stuck_twin == 0)
                {
                    stuck_twin = record.twin_shader;
                    stuck_mask = record.rt_mask;
                    stuck_built = record.twin_built;
                }
            }
            if (waiting == 0)
            {
                // A compiled object that kept its stock look past the frame's twin-build budget is built again
                // shortly, in another frame.
                if (track.deferred && now - track.deferred_ms >= Constants::TREE_HOLE_DEFER_RETRY_MS)
                {
                    (void)refill(track, trace::RefillReason::Deferred);
                    continue;
                }
                // Nothing holds a wait of this tree, so a dropped one would leave it on the stock look for as long
                // as it is listed: refill it, and its compiled objects hand over new waits (or take the twin).
                if (track.retry && now - track.retry_ms >= Constants::TREE_HOLE_RETRY_MS)
                {
                    const Invalidate result = refill(track, trace::RefillReason::LostRecord);
                    (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                             "ShaderTwins: {} refilled again, a wait of it was dropped {:.1f} s ago "
                                             "({})",
                                             track.name.data(), static_cast<double>(now - track.retry_ms) / 1000.0,
                                             invalidate_text(result));
                }
                continue;
            }
            const double waited = static_cast<double>(now - oldest) / 1000.0;
            const bool overdue = waited > static_cast<double>(Constants::SHADER_TWIN_PSO_WAIT_LOG_SECONDS);
            if (ready > 0 && (ready == waiting || overdue))
            {
                const Invalidate result = refill(track, trace::RefillReason::PsosReady);
                ++s_promotions;
                track.wait_logged = false;
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "ShaderTwins: {} twin PSOs ready ({} of {} compiled object(s), {} PSO(s)) "
                                         "{:.2f} s after track creation, {:.2f} s since oldest pending record "
                                         "(internal timing; visible delay unmeasured; {})",
                                         track.name.data(), ready, waiting, psos,
                                         static_cast<double>(now - track.swapped_ms) / 1000.0, waited,
                                         invalidate_text(result));
            }
            else if (overdue && !track.wait_logged)
            {
                track.wait_logged = true;
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: {} still waits for the twin's PSOs after {:.0f} s ({} of {} "
                                         "compiled object(s) ready; twin {:#x}, RT mask {:#x}{}); it keeps the "
                                         "stock look, and a permutation that failed to compile keeps it so",
                                         track.name.data(), waited, ready, waiting, stuck_twin, stuck_mask,
                                         stuck_built ? "" : ", the twin build failed");
            }
        }
    }

    void take_cold_reports(std::uint64_t now) noexcept
    {
        for (int i = 0; i < s_source_count; ++i)
        {
            take_cold_report(s_sources[static_cast<std::size_t>(i)], now);
        }
    }

    void drain_warm_records(std::uint64_t now) noexcept
    {
        PsoRecord record{};
        while (s_warm_records.pop(record))
        {
            const int index = find_source_by_twin(record.twin_shader);
            if (index < 0 || s_sources[static_cast<std::size_t>(index)].state != TwinState::Ready)
            {
                drop_record(record);
                continue;
            }
            Source &source = s_sources[static_cast<std::size_t>(index)];
            const bool cold_set = record.origin == WarmOrigin::Cold;
            int at = find_warm_entry(record.key);
            if (at >= 0)
            {
                WarmEntry &kept = s_warm_entries[static_cast<std::size_t>(at)];
                if (!cold_set)
                {
                    drop_record(record);
                    continue;
                }
                // The fading build's set replaces the kept one; the masks at rest, for the log, stay.
                const std::uint64_t rest_rt_mask = kept.record.rest_rt_mask;
                const std::uint64_t rest_flags = kept.record.rest_flags;
                drop_record(kept.record);
                kept.record = record;
                kept.record.rest_rt_mask = rest_rt_mask;
                kept.record.rest_flags = rest_flags;
                kept.twin_attempts = {};
                kept.stalled_ms = 0;
                kept.stalled_polls = 0;
                if (kept.warm)
                {
                    kept.warm = false;
                    kept.wait_logged = false;
                    kept.first_ms = now;
                }
                source.cold = false;
                continue;
            }
            at = s_warm_entry_count < static_cast<int>(s_warm_entries.size()) ? s_warm_entry_count++ : make_warm_room();
            if (at < 0)
            {
                ++s_warm_entries_dropped;
                if (!cold_set)
                {
                    release_key(record.key);
                }
                drop_record(record);
                continue;
            }
            s_warm_entries[static_cast<std::size_t>(at)] =
                WarmEntry{.record = record, .source = index, .first_ms = now};
            source.cold = source.cold && !cold_set;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: {} twin {:#x} for {}: a set for the fade {} (RT mask {:#x}, flags "
                                     "{:#x}; at rest RT mask {:#x}, flags {:#x}), {} PSO slot(s)",
                                     blobs::k_twins[source.kind].label, source.twin, source.name.data(),
                                     origin_text(record.origin), record.rt_mask, record.flags, record.rest_rt_mask,
                                     record.rest_flags, std::popcount(record.slots));
        }
    }

    void poll_warm_entries(std::uint64_t now) noexcept
    {
        int pending = 0;
        int stuck = -1;
        for (int i = 0; i < s_warm_entry_count; ++i)
        {
            WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
            if (entry.warm)
            {
                continue;
            }
            PsoRecord &record = entry.record;
            bool stock_idle = false;
            const bool ready =
                record.twin_built && warm_set_ready_guarded(record.stock.data(), record.twin.data(), record.slots,
                                                            record.attempts.data(), stock_idle) == PsoReady::Ready;
            if (ready && (!stock_idle || now - entry.first_ms >= Constants::CHARACTER_TWIN_SETTLE_MS))
            {
                entry.warm = true;
                entry.warm_ms = now;
                release_entry_psos(entry);
                continue;
            }
            ++pending;
            const bool stalled = !record.twin_built || twin_stalled_guarded(record.stock.data(), record.twin.data(),
                                                                            record.slots, entry.twin_attempts.data());
            if (!stalled)
            {
                entry.stalled_ms = 0;
                entry.stalled_polls = 0;
            }
            else
            {
                entry.stalled_ms = entry.stalled_ms != 0 ? entry.stalled_ms : now;
                ++entry.stalled_polls;
                if (stuck < 0 && now - entry.stalled_ms >= Constants::CHARACTER_TWIN_GIVE_UP_MS &&
                    entry.stalled_polls >= Constants::CHARACTER_TWIN_GIVE_UP_POLLS)
                {
                    stuck = i;
                }
            }
            const double waited = static_cast<double>(now - entry.first_ms) / 1000.0;
            if (!entry.wait_logged && waited > static_cast<double>(Constants::SHADER_TWIN_PSO_WAIT_LOG_SECONDS))
            {
                entry.wait_logged = true;
                const Source &source = s_sources[static_cast<std::size_t>(entry.source)];
                (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                         "ShaderTwins: {} twin {:#x} for {}: a set for the fade ({}, RT mask {:#x}{}) "
                                         "still waits for its PSOs after {:.0f} s; it is held while they compile, "
                                         "and until then its dissolving items keep the stock shader and the "
                                         "close-up fade hides the head",
                                         blobs::k_twins[source.kind].label, source.twin, source.name.data(),
                                         origin_text(record.origin), record.rt_mask,
                                         !record.twin_built ? ", the twin build failed"
                                         : stalled          ? ", a twin PSO no longer compiling"
                                                            : "",
                                         waited);
            }
        }
        if (stuck >= 0)
        {
            give_up_fade(stuck, now);
            pending = count_waiting_entries();
        }
        s_warm_pending_count = pending;
    }

    void forget_warm_sets(std::uintptr_t root, const char *why) noexcept
    {
        int kept = 0;
        for (int i = 0; i < s_warm_entry_count; ++i)
        {
            const WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
            if (entry.record.node != root)
            {
                drop_record(entry.record);
                continue;
            }
            if (kept != i)
            {
                s_warm_entries[static_cast<std::size_t>(kept)] = entry;
            }
            ++kept;
        }
        const int dropped = s_warm_entry_count - kept;
        s_warm_entry_count = kept;
        for (std::atomic<std::uint64_t> &key : s_warm_keys)
        {
            key.store(k_key_empty, std::memory_order_relaxed);
        }
        std::array<int, k_max_sources> kept_sets{};
        std::array<int, k_max_sources> waiting_sets{};
        for (int i = 0; i < kept; ++i)
        {
            const WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
            (void)claim_key(entry.record.key);
            ++kept_sets[static_cast<std::size_t>(entry.source)];
            waiting_sets[static_cast<std::size_t>(entry.source)] += entry.warm ? 0 : 1;
        }
        s_warm_pending_count = count_waiting_entries();
        int still_warm = 0;
        for (int i = 0; i < s_source_count; ++i)
        {
            Source &source = s_sources[static_cast<std::size_t>(i)];
            source.cold = false;
            source.drawable_since_ms = 0;
            if (source.warm_slot >= 0 && source.state == TwinState::Ready)
            {
                const auto at = static_cast<std::size_t>(i);
                set_fade_warm(source, source.warm && kept_sets[at] != 0 && waiting_sets[at] == 0);
                still_warm += source.warm ? 1 : 0;
            }
        }
        (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                 "ShaderTwins: {}; dropped the {} kept set(s) for the fade of the old one and kept "
                                 "the {} built for the new one ({} waiting); {} character twin(s) stay warm, the "
                                 "others warm up again",
                                 why, dropped, kept, s_warm_pending_count, still_warm);
    }

    void refresh_fade_warm(std::uint64_t now) noexcept
    {
        struct Count
        {
            int kept = 0;
            int waiting = 0;
            int first = -1;               // the first kept set, for the masks in the log
            std::uint64_t slowest_ms = 0; // the longest a warm set waited, for the log
        };
        std::array<Count, k_max_sources> counts{};
        for (int i = 0; i < s_warm_entry_count; ++i)
        {
            const WarmEntry &entry = s_warm_entries[static_cast<std::size_t>(i)];
            Count &count = counts[static_cast<std::size_t>(entry.source)];
            ++count.kept;
            count.waiting += entry.warm ? 0 : 1;
            count.first = count.first >= 0 ? count.first : i;
            count.slowest_ms =
                entry.warm ? std::max(count.slowest_ms, entry.warm_ms - entry.first_ms) : count.slowest_ms;
        }
        for (int i = 0; i < s_source_count; ++i)
        {
            Source &source = s_sources[static_cast<std::size_t>(i)];
            if (source.warm_slot < 0 || source.state != TwinState::Ready)
            {
                continue;
            }
            if (source.cold && now - source.cold_ms >= Constants::CHARACTER_TWIN_COLD_HOLD_MS)
            {
                source.cold = false;
            }
            // A warm twin stays warm while a new set of it waits (another LOD or RT mask at rest): only a fading
            // build that could not draw turns it cold (take_cold_report()), so the readiness never flickers.
            const Count &count = counts[static_cast<std::size_t>(i)];
            if (source.warm || count.kept == 0 || count.waiting != 0 || source.cold)
            {
                source.drawable_since_ms = 0;
                continue;
            }
            source.drawable_since_ms = source.drawable_since_ms != 0 ? source.drawable_since_ms : now;
            if (now - source.drawable_since_ms < Constants::CHARACTER_TWIN_SETTLE_MS)
            {
                continue;
            }
            set_fade_warm(source, true);
            source.drawable_since_ms = 0;
            const WarmEntry &first = s_warm_entries[static_cast<std::size_t>(count.first)];
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: {} twin {:#x} warm for the fade for {}: {} set(s) can draw (the "
                                     "first {}, RT mask {:#x} -> {:#x}, flags {:#x} -> {:#x}; the slowest waited "
                                     "{:.2f} s) {:.2f} s after the twin was ready; its dissolving items keep it "
                                     "from the next frame",
                                     blobs::k_twins[source.kind].label, source.twin, source.name.data(), count.kept,
                                     origin_text(first.record.origin), first.record.rest_rt_mask, first.record.rt_mask,
                                     first.record.rest_flags, first.record.flags,
                                     static_cast<double>(count.slowest_ms) / 1000.0,
                                     static_cast<double>(now - source.ready_ms) / 1000.0);
        }
    }

    void log_fade_miss() noexcept
    {
        if (s_fade_miss_logged || !s_fade_miss_ready.load(std::memory_order_acquire))
        {
            return;
        }
        s_fade_miss_logged = true;
        (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                 "ShaderTwins: a fading build of a character twin asked for a description no set was "
                                 "built ahead for (RT mask {:#x}, flags {:#x}); it takes the twin only if the "
                                 "engine has its permutations, else it turns the twin cold",
                                 s_fade_miss_rt_mask.load(std::memory_order_relaxed),
                                 s_fade_miss_flags.load(std::memory_order_relaxed));
    }

    void publish_set(std::span<const std::uintptr_t> character_nodes, std::uint64_t now) noexcept
    {
        if (s_track_count == 0 && character_nodes.empty())
        {
            trace_event(trace::Event::SetPublished, 0, 0, 0, now);
            s_set_published.store(nullptr, std::memory_order_release);
            return;
        }
        SwapSet &set = s_sets[static_cast<std::size_t>(s_next_set)];
        s_next_set = (s_next_set + 1) % static_cast<int>(s_sets.size());
        set.node_count = s_track_count;
        for (int i = 0; i < s_track_count; ++i)
        {
            const Track &track = s_tracks[static_cast<std::size_t>(i)];
            set.nodes[static_cast<std::size_t>(i)] = track.node;
            set.merged[static_cast<std::size_t>(i)] = track.merged;
            set.origins[static_cast<std::size_t>(i)] = track.origin;
            set.frames[static_cast<std::size_t>(i)] = track.frame;
        }
        set.character_count = static_cast<int>(std::min(character_nodes.size(), set.character_nodes.size()));
        std::copy_n(character_nodes.begin(), set.character_count, set.character_nodes.begin());
        set.sources_full = s_source_count >= k_max_sources;
        set.source_count = s_source_count;
        set.character_source_count = 0;
        for (int i = 0; i < s_source_count; ++i)
        {
            Source &source = s_sources[static_cast<std::size_t>(i)];
            const auto at = static_cast<std::size_t>(i);
            const bool usable = source.state == TwinState::Ready && !source.stale &&
                                !s_kind_failed[static_cast<std::size_t>(source.kind)];
#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
            const auto state = static_cast<std::uint64_t>(source.state) |
                               (static_cast<std::uint64_t>(s_kind_failed[static_cast<std::size_t>(source.kind)]) << 8) |
                               (static_cast<std::uint64_t>(source.recovered) << 9);
            if (source.traced_state != state)
            {
                source.traced_state = state;
                trace_event(trace::Event::SourceState, 0, source.twin, source.shader, source.mask, state,
                            static_cast<std::uint64_t>(source.kind));
            }
#endif
            set.sources[at] = source.shader;
            set.twins[at] = usable ? source.twin : 0;
            set.kinds[at] = static_cast<std::uint8_t>(source.kind);
            set.fade_ready[at] = usable && source.warm;
            if (blobs::k_twins[source.kind].scope == blobs::TwinScope::Dissolving)
            {
                set.character_sources[static_cast<std::size_t>(set.character_source_count++)] =
                    static_cast<std::uint8_t>(i);
            }
        }
        set.stamp_ms = now;
        s_set_published.store(&set, std::memory_order_release);
        trace_event(trace::Event::SetPublished, 0, 0, 0, now, set.node_count, set.source_count);
    }

    void refresh_character_ready(bool can_run, bool running, std::uint64_t now) noexcept
    {
        const bool unknown = s_character_unknown.exchange(false, std::memory_order_relaxed);
        if (can_run && !running)
        {
            return;
        }
        // Per kind: the sources seen on the character now, how many of them are warm, and how many were given up.
        struct KindCount
        {
            int recent = 0;
            int warm = 0;
            int given_up = 0;
        };
        std::array<KindCount, k_kinds> counts{};
        int recent = 0;
        int eye_warm = 0;
        bool eyes_held = false;
        bool all_held = false;
        const Source *first_cold = nullptr;
        for (int i = 0; i < s_source_count; ++i)
        {
            const Source &source = s_sources[static_cast<std::size_t>(i)];
            if (blobs::k_twins[source.kind].scope != blobs::TwinScope::Dissolving)
            {
                continue;
            }
            const std::uint64_t seen = s_character_seen_ms[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
            if (seen == 0 || (seen < now && now - seen > Constants::CHARACTER_TWIN_RECENT_MS))
            {
                continue;
            }
            const bool kind_failed = s_kind_failed[static_cast<std::size_t>(source.kind)];
            const bool usable = source.state == TwinState::Ready && !source.stale && source.warm && !kind_failed;
            const bool eye = source.kind == k_eye_kind;
            ++recent;
            eye_warm += (eye && usable) ? 1 : 0;
            KindCount &count = counts[static_cast<std::size_t>(source.kind)];
            ++count.recent;
            count.warm += usable ? 1 : 0;
            if (usable)
            {
                continue;
            }
            const bool given_up = source.state == TwinState::Failed || kind_failed;
            count.given_up += given_up ? 1 : 0;
            eyes_held = eyes_held || eye || !given_up;
            all_held = true;
            first_cold = first_cold != nullptr ? first_cold : &source;
        }
        bool eyes_ready = false;
        bool all_ready = false;
        if (running && recent == 0 && !unknown)
        {
            eyes_ready = s_eye_ready.load(std::memory_order_relaxed);
            all_ready = s_character_ready.load(std::memory_order_relaxed);
        }
        else if (running)
        {
            eyes_ready = eye_warm > 0 && !eyes_held && !unknown;
            all_ready = eyes_ready && !all_held;
        }
        s_eye_ready.store(eyes_ready, std::memory_order_release);
        s_character_ready.store(all_ready, std::memory_order_release);
        const int logged = (eyes_ready ? 1 : 0) | (all_ready ? 2 : 0);
        if (logged == s_ready_logged)
        {
            return;
        }
        s_ready_logged = logged;
        if (!DMK::log().is_enabled(DMK::LogLevel::Debug))
        {
            return;
        }
        // Per kind "Hair 4/5 warm" (of the sources on him now), then the first one not warm.
        std::array<char, 320> seen_text{};
        std::size_t used = 0;
        for (int kind = 0; kind < k_kinds; ++kind)
        {
            const KindCount &count = counts[static_cast<std::size_t>(kind)];
            if (blobs::k_twins[kind].scope != blobs::TwinScope::Dissolving || used + 1 >= seen_text.size())
            {
                continue;
            }
            const auto written = std::format_to_n(
                seen_text.data() + used, seen_text.size() - 1 - used, "{}{} {}/{} warm{}", used == 0 ? "" : ", ",
                blobs::k_twins[kind].label, count.warm, count.recent, count.given_up != 0 ? " (some given up)" : "");
            used = std::min(used + static_cast<std::size_t>(written.size), seen_text.size() - 1);
        }
        if (first_cold != nullptr && used + 1 < seen_text.size())
        {
            const auto written =
                std::format_to_n(seen_text.data() + used, seen_text.size() - 1 - used, "; first not warm: {} {}",
                                 blobs::k_twins[first_cold->kind].label, first_cold->name.data());
            used = std::min(used + static_cast<std::size_t>(written.size), seen_text.size() - 1);
        }
        seen_text[used] = '\0';
        (void)DMK::log().try_log(
            DMK::LogLevel::Debug,
            "ShaderTwins: the character's twins: the eyes {}, everything {} ({} Eye twin(s) "
            "warm for the fade; on him now: {}; {} of {} set(s) for the fade waiting{}{})",
            eyes_ready ? "fade with him" : "do not fade yet", all_ready ? "fades" : "does not fade yet", eye_warm,
            seen_text.data(), s_warm_pending_count, s_warm_entry_count,
            unknown ? "; a shader without a twin entry seen on him" : "", running ? "" : "; the twins do not run");
    }

    void publish_rows(const TreeHoleWindow &window, std::uint64_t now) noexcept
    {
        const bool anchored =
            std::isfinite(window.anchor.x) && std::isfinite(window.anchor.y) && std::isfinite(window.anchor.z);
        const float strength = anchored ? std::clamp(finite_or(window.strength, 0.0f), 0.0f, 1.0f) : 0.0f;
        const float near_strength = std::clamp(finite_or(window.near_strength, 0.0f), 0.0f, 1.0f);
        if (s_track_count == 0 || (strength <= 0.0f && near_strength <= 0.0f))
        {
            trace_event(trace::Event::RowsDisabled, 0, 0, 0, now, s_track_count == 0 ? 1 : 2);
            s_rows_published.store(nullptr, std::memory_order_release);
            return;
        }
        const float near_full = std::max(finite_or(window.near_full, 0.0f), 0.0f);
        HoleRows &rows = s_rows[static_cast<std::size_t>(s_next_rows)];
        s_next_rows = (s_next_rows + 1) % static_cast<int>(s_rows.size());
        rows.values = {
            anchored ? window.anchor.x : 0.0f,
            anchored ? window.anchor.y : 0.0f,
            anchored ? window.anchor.z : 0.0f,
            strength,
            std::max(finite_or(window.half_width, 0.0f), 0.0f),
            std::max(finite_or(window.half_height, 0.0f), 0.0f),
            std::max(finite_or(window.depth_margin, 0.0f), 0.0f),
            std::max(finite_or(window.depth_feather, 0.0f), 0.0f),
            std::clamp(finite_or(window.softness, 0.0f), 0.0f, 1.0f),
            finite_or(window.floor_z, -1.0e6f),
            std::max(finite_or(window.floor_feather, 0.0f), 0.0f),
            std::clamp(finite_or(window.tunnel, 0.0f), 0.0f, 1.0f),
            near_strength,
            near_full,
            std::max(finite_or(window.near_clear, 0.0f), near_full),
            0.0f,
        };
        rows.stamp_ms = now;
        s_rows_published.store(&rows, std::memory_order_release);
        trace_event(trace::Event::RowsPublished, 0, 0, 0, now, std::bit_cast<std::uint32_t>(strength),
                    std::bit_cast<std::uint32_t>(near_strength));
    }

    void log_second(std::uint64_t now) noexcept
    {
        int twins = 0;
        int ready = 0;
        for (int i = 0; i < s_source_count; ++i)
        {
            const Source &source = s_sources[static_cast<std::size_t>(i)];
            twins += source.twin != 0 ? 1 : 0;
            ready += source.state == TwinState::Ready ? 1 : 0;
        }
        int active = 0;
        for (int i = 0; i < s_track_count; ++i)
        {
            active += s_tracks[static_cast<std::size_t>(i)].activated ? 1 : 0;
        }
        s_stat_swapped.store(active, std::memory_order_relaxed);
        s_stat_twins.store(twins, std::memory_order_relaxed);
        s_stat_ready.store(ready, std::memory_order_relaxed);
        if (now - s_last_log_ms < 1000)
        {
            return;
        }
        s_last_log_ms = now;
        log_render_misses();
        log_pause_waits(now);
        const std::uint32_t swaps = s_item_swaps.exchange(0, std::memory_order_relaxed);
        const std::uint32_t rest_swaps = s_rest_swaps.exchange(0, std::memory_order_relaxed);
        std::array<std::uint32_t, k_kinds> kind_swaps{};
        std::uint32_t dissolve_swaps = 0;
        for (int kind = 0; kind < k_kinds; ++kind)
        {
            const auto at = static_cast<std::size_t>(kind);
            kind_swaps[at] = s_kind_swaps[at].exchange(0, std::memory_order_relaxed);
            dissolve_swaps += blobs::k_twins[kind].scope == blobs::TwinScope::Dissolving ? kind_swaps[at] : 0u;
        }
        const std::uint32_t node_rejects = s_node_rejects.exchange(0, std::memory_order_relaxed);
        const std::uint32_t opacity_skips = s_opacity_skips.exchange(0, std::memory_order_relaxed);
        const std::uint32_t aliases = s_ext_aliases.exchange(0, std::memory_order_relaxed);
        const std::uint32_t rows = s_rows_written.exchange(0, std::memory_order_relaxed);
        const std::uint32_t faults = s_guard_faults.exchange(0, std::memory_order_relaxed);
        const std::uint32_t dropped = s_dropped_requests.exchange(0, std::memory_order_relaxed);
        const std::uint32_t guarded = s_pso_guarded.exchange(0, std::memory_order_relaxed);
        const std::uint32_t prepass_only = s_pso_prepass_only.exchange(0, std::memory_order_relaxed);
        const std::uint32_t gbuffer_only = s_pso_gbuffer_only.exchange(0, std::memory_order_relaxed);
        const std::uint32_t took_twin = s_pso_took_twin.exchange(0, std::memory_order_relaxed);
        const std::uint32_t kept_stock = s_pso_kept_stock.exchange(0, std::memory_order_relaxed);
        const std::uint32_t records_dropped = s_pso_dropped.exchange(0, std::memory_order_relaxed);
        const std::uint32_t temporary = s_pso_temporary.exchange(0, std::memory_order_relaxed);
        const std::uint32_t slowest_us = s_pso_slowest_us.exchange(0, std::memory_order_relaxed);
        const std::uint32_t deferred = s_pso_deferred.exchange(0, std::memory_order_relaxed);
        const std::uint32_t carryovers = s_budget_carryovers.exchange(0, std::memory_order_relaxed);
        const std::uint32_t busy_deferrals = s_budget_busy_deferrals.exchange(0, std::memory_order_relaxed);
        const std::uint32_t promotions = std::exchange(s_promotions, 0u);
        const std::uint32_t stale = std::exchange(s_stale_requests, 0u);
        const std::uint32_t rest_builds = s_rest_builds.exchange(0, std::memory_order_relaxed);
        const std::uint32_t ahead = s_ahead_builds.exchange(0, std::memory_order_relaxed);
        const std::uint32_t ahead_known = s_ahead_known.exchange(0, std::memory_order_relaxed);
        const std::uint32_t ahead_full = s_ahead_full.exchange(0, std::memory_order_relaxed);
        const std::uint32_t warmups = s_warming_builds.exchange(0, std::memory_order_relaxed);
        const std::uint32_t warm_dropped = s_warm_records_dropped.exchange(0, std::memory_order_relaxed);
        const std::uint32_t fade_warm = s_fade_warm_builds.exchange(0, std::memory_order_relaxed);
        const std::uint32_t fade_warm_ahead = s_fade_warm_ahead.exchange(0, std::memory_order_relaxed);
        const std::uint32_t fade_cold = s_fade_cold_builds.exchange(0, std::memory_order_relaxed);
        const std::uint32_t kept_dropped = std::exchange(s_warm_entries_dropped, 0u);
        // The character's items at rest swap every frame in third person, and other characters fade by distance
        // all the time, so neither alone is news; everything else is.
        const bool character_news = rest_builds != 0 || ahead != 0 || ahead_full != 0 || warmups != 0 ||
                                    warm_dropped != 0 || fade_warm != 0 || fade_cold != 0 || kept_dropped != 0 ||
                                    s_warm_pending_count != s_last_logged_pending;
        if (swaps == rest_swaps && aliases == 0 && faults == 0 && dropped == 0 && guarded == 0 && deferred == 0 &&
            carryovers == 0 && busy_deferrals == 0 && promotions == 0 && records_dropped == 0 && stale == 0 &&
            !character_news && active == s_last_logged_swapped)
        {
            return;
        }
        if (character_news)
        {
            s_last_logged_pending = s_warm_pending_count;
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: character twins in the last second: {} build(s) at rest, {} set(s) "
                                     "for the fade built ahead ({} built before, {} with the key table full), {} "
                                     "built aside by a fading build of the stock shader; {} fading build(s) of a "
                                     "twin took its PSOs at once ({} of them from a set built ahead), {} kept the "
                                     "stock PSOs; {} set(s) not handed over, {} not kept; {} of {} kept set(s) "
                                     "waiting",
                                     rest_builds, ahead, ahead_known, ahead_full, warmups, fade_warm, fade_warm_ahead,
                                     fade_cold, warm_dropped, kept_dropped, s_warm_pending_count, s_warm_entry_count);
        }
        if (dissolve_swaps != 0)
        {
            std::array<char, 160> per_kind{};
            std::size_t used = 0;
            for (int kind = 0; kind < k_kinds; ++kind)
            {
                if (blobs::k_twins[kind].scope != blobs::TwinScope::Dissolving || used + 1 >= per_kind.size())
                {
                    continue;
                }
                const auto written = std::format_to_n(per_kind.data() + used, per_kind.size() - 1 - used, "{}{} {}",
                                                      used == 0 ? "" : " / ", blobs::k_twins[kind].label,
                                                      kind_swaps[static_cast<std::size_t>(kind)]);
                used = std::min(used + static_cast<std::size_t>(written.size), per_kind.size() - 1);
            }
            per_kind[used] = '\0';
            (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                     "ShaderTwins: dissolving swaps {} in the last second; {} dissolving item(s) of "
                                     "another character kept the stock shader (node filter), {} opaque item(s) "
                                     "kept it (opacity)",
                                     per_kind.data(), node_rejects, opacity_skips);
        }
        s_last_logged_swapped = active;
        (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                 "ShaderTwins: {} tree(s) swapped, {} twin item(s) added ({} of them the "
                                 "character's while he fades, {} his at rest), {} window upload(s), {} .ext "
                                 "alias(es) in the last second; twins {}/{} ready",
                                 active, swaps, dissolve_swaps, rest_swaps, rows, aliases, ready, twins);
        if (guarded != 0 || deferred != 0 || carryovers != 0 || busy_deferrals != 0 || promotions != 0 ||
            s_pending_count != 0)
        {
            std::uint64_t oldest = now;
            for (int i = 0; i < s_pending_count; ++i)
            {
                oldest = std::min(oldest, s_pending[static_cast<std::size_t>(i)].first_ms);
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "ShaderTwins: {} twin PSO build(s) in the last second ({} of only the depth prepass, {} of "
                "only the G-buffer passes; {} took the twin, {} kept the stock look of which {} temporary, "
                "slowest {:.1f} ms), {} deferred past the frame's budget ({} while another build runs); {} "
                "build(s) charged across camera updates; {} compiled object(s) waiting (oldest {:.1f} s), {} "
                "tree refill(s) for ready twin PSOs; {} PSO reference(s) left held",
                guarded, prepass_only, gbuffer_only, took_twin, kept_stock, temporary,
                static_cast<double>(slowest_us) / 1000.0, deferred, busy_deferrals, carryovers, s_pending_count,
                static_cast<double>(now - oldest) / 1000.0, promotions, s_pso_leaked.load(std::memory_order_relaxed));
        }
        if (faults != 0 || dropped != 0 || records_dropped != 0 || stale != 0)
        {
            (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                     "ShaderTwins: {} guarded read/write fault(s), {} dropped twin request(s), {} twin "
                                     "request(s) for a shader gone before it was handled and {} dropped PSO "
                                     "wait(s) in the last second (a tree that lost a wait is refilled again {:.0f} "
                                     "s later unless another refill comes first)",
                                     faults, dropped, stale, records_dropped,
                                     static_cast<double>(Constants::TREE_HOLE_RETRY_MS) / 1000.0);
        }
    }
} // namespace TPVCamera::Twins
