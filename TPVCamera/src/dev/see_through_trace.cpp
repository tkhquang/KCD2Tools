/**
 * @file dev/see_through_trace.cpp
 * @brief Implementation of the see-through trace (see see_through_trace.hpp). Empty outside DEV profiling builds.
 */

#include "see_through_trace.hpp"

#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <type_traits>
#include <windows.h>

namespace TPVCamera::SeeThroughTrace
{
    namespace
    {
        constexpr std::size_t k_threads = 64;
        constexpr std::size_t k_events_per_stream = 8192;
        constexpr std::size_t k_descriptors_per_thread = 2048, k_descriptor_probes = 16;
        constexpr std::array k_names = {
            "candidate.begin",   "candidate.end",      "candidate.cancelled", "camera.window",
            "update.skipped",    "track.added",        "track.rejected",      "binding.admitted",
            "binding.expired",   "track.released",     "source.request",      "source.result",
            "source.state",      "item.stock",         "item.twin",           "submit.twin.returned",
            "build.begin",       "compiled.binding",   "build.end",           "build.deferred",
            "pso.selected.twin", "pso.selected.stock", "record.queued",       "record.lost",
            "record.received",   "record.replaced",    "record.dropped",      "record.state",
            "retry.scheduled",   "deferred.scheduled", "refill.begin",        "refill.end",
            "set.published",     "rows.published",     "rows.disabled",       "rows.write",
            "pso.slot_state"};
        static_assert(k_names.size() == static_cast<std::size_t>(Event::Count));
        constexpr bool is_lifecycle(Event event) noexcept
        {
            switch (event)
            {
            case Event::CandidateBegin:
            case Event::CandidateEnd:
            case Event::CandidateCancelled:
            case Event::TrackAdded:
            case Event::TrackRejected:
            case Event::BindingAdmitted:
            case Event::BindingExpired:
            case Event::TrackReleased:
            case Event::SourceRequest:
            case Event::SourceResult:
            case Event::SourceState:
            case Event::BuildDeferred:
            case Event::PsoSelectedStock:
            case Event::RecordQueued:
            case Event::RecordLost:
            case Event::RecordReceived:
            case Event::RecordReplaced:
            case Event::RecordDropped:
            case Event::RecordState:
            case Event::RetryScheduled:
            case Event::DeferredScheduled:
            case Event::RefillBegin:
            case Event::RefillEnd:
            case Event::PsoSlotState:
                return true;
            default:
                return false;
            }
        }
        struct Entry
        {
            std::int64_t qpc = 0;
            std::uint64_t sequence = 0; // Merge streams without reordering equal-QPC observations.
            std::uintptr_t node = 0, object = 0, shader = 0, resources = 0;
            std::uint64_t a = 0, b = 0;
            std::uint32_t frame = 0;
            Event event{};
        };
        struct Stream
        {
            std::uint64_t written = 0;
            std::int64_t overwritten_through_qpc = 0;
            std::array<Entry, k_events_per_stream> events{};
        };
        struct Thread
        {
            DWORD id = 0;
            std::uint64_t written = 0;
            std::array<std::uint64_t, k_names.size()> totals{};
            Stream activity{}, lifecycle{};
            struct DescriptorRecord
            {
                PsoDescriptor key{};
                std::uint64_t stock_calls = 0, ready_stock_calls = 0, demand_calls = 0;
                std::int64_t first_stock = 0, last_stock = 0, first_ready_stock = 0;
                std::int64_t first_demand = 0, last_demand = 0;
                std::uintptr_t stock_node = 0, stock_compiled = 0, ready_twin = 0;
                std::uintptr_t demand_node = 0, demand_compiled = 0, demand_twin = 0;
            };
            std::array<DescriptorRecord, k_descriptors_per_thread> descriptors{};
            std::uint64_t stock_observations = 0, demand_observations = 0;
            std::uint64_t stock_missed = 0, demand_missed = 0;
        };
        // One owning thread per slot, no concurrent readers; no hot-path allocation, formatting, disk I/O or locks.
        std::array<Thread, k_threads> s_threads{};
        // The thread id that claimed each slot (thread_state()): a host thread may not use C++ thread_local (DMK
        // [B-86]), and a Win32 TLS index would clear the engine thread's last error on every read and need freeing at
        // unload. An exited thread's id can come back for a new thread, which then writes on in the same slot.
        std::array<std::atomic<DWORD>, k_threads> s_owners{};
        std::atomic<std::uint64_t> s_unmeasured{0};
        std::atomic<std::uint64_t> s_descriptor_unmeasured{0}, s_descriptor_faults{0};
        std::int64_t s_frequency = 0, s_started = 0;
        struct Candidate
        {
            std::uintptr_t node = 0;
            std::uint64_t episode = 0;
            bool frame = false;
        };
        std::array<Candidate, 128> s_candidates{};
        std::size_t s_candidate_count = 0;
        std::uint64_t s_episode = 0, s_candidate_overflow = 0;
        struct Label
        {
            std::uintptr_t node = 0;
            std::int64_t qpc = 0;
            std::array<char, 96> name{};
        };
        std::array<Label, 1024> s_labels{};
        std::size_t s_label_count = 0;
        std::uint64_t s_label_overflow = 0;

        std::int64_t now() noexcept
        {
            LARGE_INTEGER value{};
            QueryPerformanceCounter(&value);
            return value.QuadPart;
        }

        /** @brief The calling thread's slot, claimed on its first event; nullptr once every slot is taken. */
        [[nodiscard]] Thread *thread_state() noexcept
        {
            const DWORD id = GetCurrentThreadId();
            // Thread ids are multiples of 4.
            const std::size_t first = (id >> 2) % k_threads;
            for (std::size_t probe = 0; probe < k_threads; ++probe)
            {
                const std::size_t slot = (first + probe) % k_threads;
                DWORD owner = s_owners[slot].load(std::memory_order_acquire);
                if (owner == 0 && s_owners[slot].compare_exchange_strong(owner, id, std::memory_order_acq_rel))
                {
                    s_threads[slot].id = id;
                    return &s_threads[slot];
                }
                if (owner == id)
                {
                    return &s_threads[slot];
                }
            }
            return nullptr;
        }

        std::uint64_t descriptor_hash(const PsoDescriptor &key) noexcept
        {
            std::uint64_t hash = 0xCBF29CE484222325ull;
            const auto mix = [&hash](std::uint64_t word)
            {
                for (unsigned i = 0; i < 8; ++i)
                {
                    hash = (hash ^ static_cast<std::uint8_t>(word)) * 0x100000001B3ull;
                    word >>= 8;
                }
            };
            for (const auto word : key.words)
                mix(word);
            mix(key.pipeline);
            mix(key.cache);
            mix(key.load_epoch);
            mix(key.kind);
            return hash;
        }
    } // namespace

    void initialize() noexcept
    {
        if (s_frequency != 0)
            return;
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency))
        {
            static_assert(std::is_trivially_copyable_v<Thread>);
            std::memset(s_threads.data(), 0, sizeof(s_threads));
            s_frequency = frequency.QuadPart;
            s_started = now();
        }
    }

    void emit(Event event, std::uint32_t frame, std::uintptr_t node, std::uintptr_t object, std::uintptr_t shader,
              std::uintptr_t resources, std::uint64_t a, std::uint64_t b) noexcept
    {
        if (s_frequency <= 0 || event >= Event::Count)
        {
            s_unmeasured.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Thread *const thread = thread_state();
        if (thread == nullptr)
        {
            s_unmeasured.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto &stream = is_lifecycle(event) ? thread->lifecycle : thread->activity;
        auto &entry = stream.events[static_cast<std::size_t>(stream.written % k_events_per_stream)];
        if (stream.written >= k_events_per_stream)
            stream.overwritten_through_qpc = entry.qpc;
        entry = {now(), thread->written, node, object, shader, resources, a, b, frame, event};
        ++stream.written;
        ++thread->written;
        ++thread->totals[static_cast<std::size_t>(event)];
    }

    void pso_descriptor_fault() noexcept
    {
        s_descriptor_faults.fetch_add(1, std::memory_order_relaxed);
    }

    void observe_pso_descriptor(const PsoDescriptor &key, std::uintptr_t node, std::uintptr_t compiled,
                                std::uintptr_t ready_twin, bool twin_demand) noexcept
    {
        Thread *const thread = s_frequency > 0 ? thread_state() : nullptr;
        if (thread == nullptr)
        {
            s_descriptor_unmeasured.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        auto &t = *thread;
        ++(twin_demand ? t.demand_observations : t.stock_observations);
        const auto hash = descriptor_hash(key);
        for (std::size_t probe = 0; probe < k_descriptor_probes; ++probe)
        {
            auto &record = t.descriptors[(hash + probe) % k_descriptors_per_thread];
            if (record.stock_calls == 0 && record.demand_calls == 0)
                record.key = key;
            else if (!(record.key == key))
                continue;
            const auto tick = now();
            if (twin_demand)
            {
                if (record.demand_calls++ == 0)
                {
                    record.first_demand = tick;
                    record.demand_node = node;
                    record.demand_compiled = compiled;
                    record.demand_twin = ready_twin;
                }
                record.last_demand = tick;
            }
            else
            {
                if (record.stock_calls++ == 0)
                {
                    record.first_stock = tick;
                    record.stock_node = node;
                    record.stock_compiled = compiled;
                }
                record.last_stock = tick;
                if (ready_twin != 0 && record.ready_stock_calls++ == 0)
                {
                    record.first_ready_stock = tick;
                    record.ready_twin = ready_twin;
                }
            }
            return;
        }
        ++(twin_demand ? t.demand_missed : t.stock_missed);
    }

    void candidates(std::uint32_t frame, std::span<void *const> trees, std::span<void *const> frames, float strength,
                    float near_strength) noexcept
    {
        emit(Event::CameraWindow, frame, 0, 0, trees.size(), frames.size(), std::bit_cast<std::uint32_t>(strength),
             std::bit_cast<std::uint32_t>(near_strength));
        std::array<Candidate, 128> next{};
        std::size_t count = 0;
        const auto add = [&](std::span<void *const> nodes, bool is_frame)
        {
            for (void *ptr : nodes)
            {
                const auto node = reinterpret_cast<std::uintptr_t>(ptr);
                if (node == 0 || std::any_of(next.begin(), next.begin() + count,
                                             [node](const Candidate &c) { return c.node == node; }))
                    continue;
                if (count == next.size())
                {
                    ++s_candidate_overflow;
                    continue;
                }
                const auto end = s_candidates.begin() + s_candidate_count;
                const auto old = std::find_if(s_candidates.begin(), end, [node, is_frame](const Candidate &c)
                                              { return c.node == node && c.frame == is_frame; });
                const auto episode = old != end ? old->episode : ++s_episode;
                next[count++] = {node, episode, is_frame};
            }
        };
        add(frames, true);
        add(trees, false);
        for (std::size_t i = 0; i < s_candidate_count; ++i)
        {
            const auto &old = s_candidates[i];
            if (std::none_of(next.begin(), next.begin() + count,
                             [&old](const Candidate &c) { return c.episode == old.episode; }))
            {
                emit(Event::CandidateEnd, frame, old.node, 0, 0, 0, old.episode, old.frame);
            }
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto &c = next[i];
            if (std::none_of(s_candidates.begin(), s_candidates.begin() + s_candidate_count,
                             [&c](const Candidate &old) { return c.episode == old.episode; }))
            {
                emit(Event::CandidateBegin, frame, c.node, 0, 0, 0, c.episode, c.frame);
            }
        }
        s_candidates = next;
        s_candidate_count = count;
    }

    void cancel_candidates(std::uint32_t frame) noexcept
    {
        for (std::size_t i = 0; i < s_candidate_count; ++i)
        {
            const auto &c = s_candidates[i];
            emit(Event::CandidateCancelled, frame, c.node, 0, 0, 0, c.episode, c.frame);
        }
        s_candidate_count = 0;
    }

    void label(std::uintptr_t node, const char *name) noexcept
    {
        if (name == nullptr)
            return;
        // Retain time-stamped mappings on reuse, but do not spend capacity on unchanged names.
        for (std::size_t i = s_label_count; i > 0; --i)
        {
            if (s_labels[i - 1].node != node)
                continue;
            if (std::strncmp(s_labels[i - 1].name.data(), name, 95) == 0)
                return;
            break;
        }
        if (s_label_count == s_labels.size())
        {
            ++s_label_overflow;
            return;
        }
        auto &entry = s_labels[s_label_count++];
        entry.node = node;
        entry.qpc = now();
        for (std::size_t i = 0; i + 1 < entry.name.size() && name[i] != '\0'; ++i)
            entry.name[i] = name[i];
    }

    bool export_after_rundown(const char *path) noexcept
    {
        try
        {
            using nlohmann::json;
            json root{
                {"schema", 2},
                {"qpc_frequency", s_frequency},
                {"started_qpc", s_started},
                {"export_qpc", now()},
                {"storage_bytes", sizeof(s_threads) + sizeof(s_labels)},
                {"events_per_thread", 2 * k_events_per_stream},
                {"events_per_stream", k_events_per_stream},
                {"unmeasured_records", s_unmeasured.load()},
                {"candidate_overflow", s_candidate_overflow},
                {"label_overflow", s_label_overflow},
                {"columns", {"qpc", "frame", "event", "node", "object", "shader", "resources", "a", "b"}},
                {"event_names", k_names},
                {"pso_slot_state",
                 {{"version", 1},
                  {"attempt_sample_ms", 100},
                  {"interpretation", "Observed stock/twin PSO fields, not an atomic snapshot. "
                                     "Creation-requested does not distinguish queued/running/failed device work."}}},
                {"observability", "CPU observations only; candidates are broad-phase. Twin submission and "
                                  "PSO selection do not prove GPU execution or visible pixels."}};
            root["stream_event_names"] = {{"activity", json::array()}, {"lifecycle", json::array()}};
            for (std::size_t i = 0; i < k_names.size(); ++i)
            {
                const char *stream = is_lifecycle(static_cast<Event>(i)) ? "lifecycle" : "activity";
                root["stream_event_names"][stream].push_back(k_names[i]);
            }
            root["retention"] = "Independent bounded activity/lifecycle rings. Merged events can contain "
                                "activity gaps inside retained lifecycle history; use per-stream coverage.";
            root["pso_discovery"] = {
                {"version", 1},
                {"slots_per_thread", k_descriptors_per_thread},
                {"max_probes", k_descriptor_probes},
                {"unmeasured", s_descriptor_unmeasured.load()},
                {"read_faults", s_descriptor_faults.load()},
                {"interpretation",
                 "Natural non-shadow stock builds of supported plant shader kinds and twin demand. "
                 "Exact copied descriptors, not owned pointers or PSO readiness. Shader kind alone "
                 "does not classify a node as a plant. First/last/counters only; no per-call timeline."}};
            root["threads"] = json::array();
            for (std::size_t i = 0; i < k_threads; ++i)
            {
                // A thread claims the slot its id probes to, so the claimed slots are scattered.
                if (s_owners[i].load(std::memory_order_relaxed) == 0)
                {
                    continue;
                }
                const auto &t = s_threads[i];
                const auto first = [](const Stream &stream)
                { return stream.written > k_events_per_stream ? stream.written - k_events_per_stream : 0; };
                const auto stream_info = [&](const Stream &stream)
                {
                    const auto begin = first(stream);
                    return json{
                        {"written", stream.written},
                        {"overwritten", begin},
                        {"overwritten_through_qpc", stream.overwritten_through_qpc},
                        {"first_retained_qpc",
                         stream.written == 0 ? 0 : stream.events[begin % k_events_per_stream].qpc},
                        {"last_retained_qpc",
                         stream.written == 0 ? 0 : stream.events[(stream.written - 1) % k_events_per_stream].qpc}};
                };
                json thread{
                    {"slot", i},
                    {"tid", t.id},
                    {"written", t.written},
                    {"overwritten", first(t.activity) + first(t.lifecycle)},
                    {"totals", t.totals},
                    {"streams", {{"activity", stream_info(t.activity)}, {"lifecycle", stream_info(t.lifecycle)}}},
                    {"events", json::array()}};
                auto activity = first(t.activity), lifecycle = first(t.lifecycle);
                while (activity < t.activity.written || lifecycle < t.lifecycle.written)
                {
                    const bool take_activity = activity < t.activity.written &&
                                               (lifecycle == t.lifecycle.written ||
                                                t.activity.events[activity % k_events_per_stream].sequence <
                                                    t.lifecycle.events[lifecycle % k_events_per_stream].sequence);
                    const auto &e = take_activity ? t.activity.events[activity++ % k_events_per_stream]
                                                  : t.lifecycle.events[lifecycle++ % k_events_per_stream];
                    thread["events"].push_back({e.qpc, e.frame, static_cast<std::uint8_t>(e.event), e.node, e.object,
                                                e.shader, e.resources, e.a, e.b});
                }
                thread["pso_descriptors"] = {{"stock_observations", t.stock_observations},
                                             {"demand_observations", t.demand_observations},
                                             {"stock_missed", t.stock_missed},
                                             {"demand_missed", t.demand_missed},
                                             {"records", json::array()}};
                for (const auto &record : t.descriptors)
                {
                    if (record.stock_calls == 0 && record.demand_calls == 0)
                        continue;
                    thread["pso_descriptors"]["records"].push_back({{"words", record.key.words},
                                                                    {"pipeline", record.key.pipeline},
                                                                    {"cache", record.key.cache},
                                                                    {"load_epoch", record.key.load_epoch},
                                                                    {"kind", record.key.kind},
                                                                    {"stock_calls", record.stock_calls},
                                                                    {"ready_stock_calls", record.ready_stock_calls},
                                                                    {"demand_calls", record.demand_calls},
                                                                    {"first_stock_qpc", record.first_stock},
                                                                    {"last_stock_qpc", record.last_stock},
                                                                    {"first_ready_stock_qpc", record.first_ready_stock},
                                                                    {"first_demand_qpc", record.first_demand},
                                                                    {"last_demand_qpc", record.last_demand},
                                                                    {"stock_node", record.stock_node},
                                                                    {"stock_compiled", record.stock_compiled},
                                                                    {"ready_twin", record.ready_twin},
                                                                    {"demand_node", record.demand_node},
                                                                    {"demand_compiled", record.demand_compiled},
                                                                    {"demand_twin", record.demand_twin}});
                }
                root["threads"].push_back(std::move(thread));
            }
            root["labels"] = json::array();
            for (std::size_t i = 0; i < s_label_count; ++i)
                root["labels"].push_back({s_labels[i].qpc, s_labels[i].node, s_labels[i].name.data()});
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;
            file << root.dump();
            file.flush();
            return file.good();
        }
        catch (...)
        {
            return false;
        }
    }
} // namespace TPVCamera::SeeThroughTrace
#endif
