/**
 * @file dev/hook_profile.cpp
 * @brief Implementation of the hook profile (see hook_profile.hpp). Empty outside DEV profiling builds.
 */

#include "hook_profile.hpp"

#if defined(TPVCAMERA_DEV_BUILD) && defined(DMK_ENABLE_PROFILING)
#include "query_ledger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <type_traits>
#include <windows.h>

namespace TPVCamera::HookProfile
{
    namespace
    {
        constexpr std::size_t k_threads = 64;
        constexpr std::size_t k_seconds = 120;
        constexpr std::size_t k_slow_capacity = 4096;
        constexpr std::int64_t k_slow_threshold_us = 1000;
        constexpr std::size_t k_paths = static_cast<std::size_t>(Path::Count);
        constexpr std::array k_names = {
            "render.add",
            "render.upload",
            "pso.dispatch",
            "engine.add.stock",
            "engine.add.twin",
            "engine.pso.stock",
            "engine.pso.twin",
            "engine.pso.passthrough",
            "camera.other",
            "engine.camera.other",
            "engine.pso.twin.forward",
            "camera.game",
            "engine.camera.game",
            "twins.clone.dryrun",
            "camera.game.repeat",
            "engine.camera.game.repeat",
        };
        static_assert(k_names.size() == k_paths);
        constexpr std::size_t k_counters = static_cast<std::size_t>(Counter::Count);
        constexpr std::array k_counter_names = {
            "render.scan_truncated", "render.scan_full",     "render.scan_dynamic",
            "twins.untwinned_retry", "collision.sphere_win", "twins.char_build_over_budget",
        };
        static_assert(k_counter_names.size() == k_counters);

        struct Metric
        {
            std::uint64_t calls = 0;
            std::int64_t elapsed_ticks = 0;
            std::int64_t engine_ticks = 0;
            std::int64_t max_own_ticks = 0;
            std::int64_t max_elapsed_ticks = 0;
            std::int64_t max_own_call_start = 0;
            std::int64_t max_own_call_end = 0;
            std::uint64_t own_over_1ms = 0;
            std::array<std::uint64_t, 6> outcomes{};
        };
        struct Window
        {
            std::int64_t second = 0;
            bool occupied = false;
            std::array<Metric, k_paths> metrics{};
        };
        struct ThreadData
        {
            DWORD id = 0;
            std::uint64_t rotated_windows = 0;
            std::array<Metric, k_paths> totals{};
            std::array<Window, k_seconds> windows{};
            struct SlowCall
            {
                std::int64_t start = 0, end = 0, engine_ticks = 0;
                std::size_t path = 0, outcome = 0;
            };
            std::uint64_t slow_written = 0;
            std::int64_t slow_overwritten_through_end = 0;
            std::array<SlowCall, k_slow_capacity> slow_calls{};
        };

        // Each slot has exactly one owning thread, the one whose id claimed it in s_owners. Export is deliberately
        // deferred until all hooks have drained, so no concurrent reader or lock is needed.
        std::array<ThreadData, k_threads> s_threads{};
        // A host thread may not use C++ thread_local (DMK [B-86]), and a Win32 TLS index would clear the engine
        // thread's last error on every read and need freeing at unload. A thread instead claims a slot with its id
        // and finds it again by the same probe on every record. An exited thread's id can come back for a new
        // thread, which then adds to the same slot: the aggregates only sum.
        std::array<std::atomic<DWORD>, k_threads> s_owners{};
        std::atomic<std::size_t> s_claimed{0};
        std::atomic<std::uint64_t> s_unmeasured{0};
        std::array<std::atomic<std::uint64_t>, k_counters> s_counters{};
        std::int64_t s_frequency = 0;
        std::int64_t s_started = 0;

        std::int64_t now() noexcept
        {
            LARGE_INTEGER value{};
            QueryPerformanceCounter(&value);
            return value.QuadPart;
        }

        void update(Metric &metric, std::int64_t start, std::int64_t end, std::int64_t engine,
                    std::size_t outcome) noexcept
        {
            const auto elapsed = std::max<std::int64_t>(0, end - start);
            engine = std::clamp(engine, std::int64_t{0}, elapsed);
            const auto own = elapsed - engine;
            ++metric.calls;
            metric.elapsed_ticks += elapsed;
            metric.engine_ticks += engine;
            metric.max_elapsed_ticks = std::max(metric.max_elapsed_ticks, elapsed);
            if (metric.calls == 1 || own > metric.max_own_ticks)
            {
                metric.max_own_ticks = own;
                metric.max_own_call_start = start;
                metric.max_own_call_end = end;
            }
            metric.own_over_1ms += own >= std::max<std::int64_t>(s_frequency / 1000, 1) ? 1 : 0;
            ++metric.outcomes[outcome];
        }

        /** @brief The calling thread's slot, claimed on its first record; nullptr once every slot is taken. */
        [[nodiscard]] ThreadData *thread_data() noexcept
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
                    s_claimed.fetch_add(1, std::memory_order_relaxed);
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

        void record(Path path, std::int64_t start, std::int64_t end, std::int64_t engine, std::size_t outcome) noexcept
        {
            ThreadData *const thread = s_frequency > 0 ? thread_data() : nullptr;
            if (thread == nullptr)
            {
                s_unmeasured.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto at = static_cast<std::size_t>(path);
            update(thread->totals[at], start, end, engine, outcome);
            const auto second = end / s_frequency;
            auto &window = thread->windows[static_cast<std::size_t>(second) % k_seconds];
            if (!window.occupied || window.second != second)
            {
                thread->rotated_windows += window.occupied ? 1 : 0;
                window = {};
                window.second = second;
                window.occupied = true;
            }
            update(window.metrics[at], start, end, engine, outcome);
            // Preserve individual slow-call bounds independently of the high-volume DMK trace ring. A long native
            // call and its containing hook are both retained; consumers must union intervals, never sum nesting.
            if (end - start >= std::max<std::int64_t>(s_frequency * k_slow_threshold_us / 1000000, 1))
            {
                auto &sample = thread->slow_calls[thread->slow_written % k_slow_capacity];
                if (thread->slow_written >= k_slow_capacity)
                {
                    thread->slow_overwritten_through_end = std::max(thread->slow_overwritten_through_end, sample.end);
                }
                sample = {start, end, std::clamp(engine, std::int64_t{0}, end - start), at, outcome};
                ++thread->slow_written;
            }
        }

        nlohmann::json metrics_json(const std::array<Metric, k_paths> &metrics)
        {
            auto result = nlohmann::json::object();
            for (std::size_t i = 0; i < metrics.size(); ++i)
            {
                const auto &m = metrics[i];
                if (m.calls == 0)
                {
                    continue;
                }
                result[k_names[i]] = {{"calls", m.calls},
                                      {"elapsed_ticks", m.elapsed_ticks},
                                      {"engine_ticks", m.engine_ticks},
                                      {"own_ticks", m.elapsed_ticks - m.engine_ticks},
                                      {"max_own_ticks", m.max_own_ticks},
                                      {"max_elapsed_ticks", m.max_elapsed_ticks},
                                      {"max_own_call_start_qpc", m.max_own_call_start},
                                      {"max_own_call_end_qpc", m.max_own_call_end},
                                      {"own_over_1ms", m.own_over_1ms},
                                      {"outcomes", m.outcomes}};
            }
            return result;
        }

        /**
         * @brief The query ledger's totals per tag and kind, and per motion class the frame count, the per-frame
         *        physics query percentiles and the slowest frame's physics time.
         */
        nlohmann::json ledger_json()
        {
            const QueryLedger::Snapshot snap = QueryLedger::snapshot();
            auto totals = nlohmann::json::object();
            for (std::size_t t = 0; t < QueryLedger::TAG_COUNT; ++t)
            {
                auto kinds = nlohmann::json::object();
                for (std::size_t k = 0; k < QueryLedger::KIND_COUNT; ++k)
                {
                    const QueryLedger::TagStats &stats = snap.totals[t][k];
                    if (stats.calls == 0)
                    {
                        continue;
                    }
                    kinds[QueryLedger::kind_name(static_cast<QueryLedger::Kind>(k))] = {
                        {"calls", stats.calls},
                        {"ticks", stats.ticks},
                        {"max_ticks", stats.max_ticks},
                    };
                }
                if (!kinds.empty())
                {
                    totals[QueryLedger::tag_name(static_cast<QueryLedger::Tag>(t))] = std::move(kinds);
                }
            }
            auto frames = nlohmann::json::object();
            for (std::size_t m = 0; m < QueryLedger::MOTION_COUNT; ++m)
            {
                const auto &bins = snap.physics_per_frame[m];
                const std::uint64_t count = snap.frames[m];
                // The smallest bin whose cumulative share reaches the percentile; the last bin is open-ended.
                const auto percentile = [&](double share) -> std::size_t
                {
                    const auto needed = static_cast<std::uint64_t>(std::ceil(share * static_cast<double>(count)));
                    std::uint64_t seen = 0;
                    for (std::size_t bin = 0; bin < bins.size(); ++bin)
                    {
                        seen += bins[bin];
                        if (seen >= needed && needed > 0)
                        {
                            return bin;
                        }
                    }
                    return 0;
                };
                frames[QueryLedger::motion_name(static_cast<QueryLedger::Motion>(m))] = {
                    {"frames", count},
                    {"physics_p50", percentile(0.5)},
                    {"physics_p99", percentile(0.99)},
                    {"physics_max_bin", percentile(1.0)},
                    {"max_physics_ticks", snap.max_physics_ticks_per_frame[m]},
                    {"physics_per_frame_bins", bins},
                };
            }
            return {
                {"totals", std::move(totals)},
                {"frames", std::move(frames)},
                {"frame_query_bins", QueryLedger::FRAME_QUERY_BINS},
            };
        }
    } // namespace

    void initialize() noexcept
    {
        if (s_frequency != 0)
        {
            return;
        }
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency))
        {
            // All fields have an all-zero empty representation. Touch the fixed storage before hooks run, so
            // registering a worker or opening a new window does not first commit its diagnostic pages in gameplay.
            static_assert(std::is_trivially_copyable_v<ThreadData>);
            std::memset(s_threads.data(), 0, sizeof(s_threads));
            s_frequency = frequency.QuadPart;
            s_started = now();
        }
    }

    Scope::Scope(Path path) noexcept : m_start(now()), m_path(path) {}

    void record_other_frustum(std::int64_t start, std::int64_t original, std::int64_t end) noexcept
    {
        record(Path::OtherFrustum, start, end, end - original, 0);
        record(Path::EngineOtherFrustum, original, end, 0, 0);
    }

    void record_game_frustum(std::int64_t start, std::int64_t original, std::int64_t end, bool repeat) noexcept
    {
        record(repeat ? Path::GameFrustumRepeat : Path::GameFrustum, start, end, end - original, 0);
        record(repeat ? Path::EngineGameFrustumRepeat : Path::EngineGameFrustum, original, end, 0, 0);
    }

    void count(Counter counter) noexcept
    {
        const auto index = static_cast<std::size_t>(counter);
        if (index < k_counters)
        {
            s_counters[index].fetch_add(1, std::memory_order_relaxed);
        }
    }

    std::int64_t stamp() noexcept
    {
        return now();
    }

    void count_slower_than(Counter counter, std::int64_t started, std::int64_t limit_us) noexcept
    {
        if (s_frequency > 0 && now() - started > s_frequency * limit_us / 1000000)
        {
            count(counter);
        }
    }

    Scope::~Scope() noexcept
    {
        const auto end = now();
        record(m_path, m_start, end, m_engine_ticks, static_cast<std::size_t>(m_outcome));
    }

    EngineScope::EngineScope(Path path, Scope &owner) noexcept : m_owner(owner), m_start(now()), m_path(path) {}

    EngineScope::~EngineScope() noexcept
    {
        const auto end = now();
        m_owner.m_engine_ticks += std::max<std::int64_t>(0, end - m_start);
        record(m_path, m_start, end, 0, m_success ? 0 : 1);
    }

    bool export_after_rundown(const char *path) noexcept
    {
        try
        {
            const auto ended = now();
            nlohmann::json result = {
                {"schema", 3},
                {"qpc_frequency", s_frequency},
                {"start_qpc", s_started},
                {"end_qpc", ended},
                {"thread_capacity", k_threads},
                {"storage_bytes", sizeof(s_threads)},
                {"threads_seen", s_claimed.load(std::memory_order_relaxed)},
                {"unmeasured_records", s_unmeasured.load(std::memory_order_relaxed)},
                {"window_capacity_seconds", k_seconds},
                {"slow_call_threshold_us", k_slow_threshold_us},
                {"slow_call_capacity_per_thread", k_slow_capacity},
                {"slow_call_columns", {"start_qpc", "end_qpc", "engine_ticks", "path_index", "outcome"}},
                {"path_names", k_names},
                {"notes",
                 "Exported after hook rundown. Own elapsed = hook elapsed minus measured synchronous engine calls; "
                 "includes waits/preemption and some instrumentation overhead, not CPU time. Native paths report "
                 "engine elapsed. Nested paths/threads overlap: do not sum into frame time. Windows bucket complete "
                 "calls by end QPC second; long calls can cross windows. Totals/maxima cover the whole generation "
                 "including loading/retirement. Max-own call bounds include its engine calls. Slow-call coverage is "
                 "explicit; absence below threshold or before overwritten-through is not evidence of no cost. Frame "
                 "display requires an external presentation trace."},
                {"outcome_labels",
                 {{"render.add", {"stock", "twin"}},
                  {"render.upload", {"skipped", "written", "in_use", "fault"}},
                  {"pso.dispatch", {"passthrough", "guarded", "resting", "fading", "warming", "shadow"}},
                  {"twins.clone.dryrun", {"exact", "allowed", "mismatch", "not_compared"}},
                  {"engine.*", {"success", "failure"}}}},
                {"threads", nlohmann::json::array()}};
            auto counters = nlohmann::json::object();
            for (std::size_t i = 0; i < k_counters; ++i)
            {
                counters[k_counter_names[i]] = s_counters[i].load(std::memory_order_relaxed);
            }
            result["counters"] = std::move(counters);
            result["query_ledger"] = ledger_json();
            for (std::size_t i = 0; i < k_threads; ++i)
            {
                // A thread claims the slot its id probes to, so the claimed slots are scattered.
                if (s_owners[i].load(std::memory_order_relaxed) == 0)
                {
                    continue;
                }
                const auto &thread = s_threads[i];
                auto windows = nlohmann::json::array();
                auto slow_calls = nlohmann::json::array();
                const auto first = thread.slow_written > k_slow_capacity ? thread.slow_written - k_slow_capacity : 0;
                for (auto ordinal = first; ordinal < thread.slow_written; ++ordinal)
                {
                    const auto &call = thread.slow_calls[ordinal % k_slow_capacity];
                    slow_calls.push_back({call.start, call.end, call.engine_ticks, call.path, call.outcome});
                }
                for (const auto &window : thread.windows)
                {
                    if (s_frequency > 0 && window.occupied && window.second <= ended / s_frequency &&
                        window.second > ended / s_frequency - static_cast<std::int64_t>(k_seconds))
                    {
                        windows.push_back(
                            {{"end_qpc_second", window.second}, {"metrics", metrics_json(window.metrics)}});
                    }
                }
                result["threads"].push_back({{"slot", i},
                                             {"thread_id", thread.id},
                                             {"rotated_windows", thread.rotated_windows},
                                             {"slow_written", thread.slow_written},
                                             {"slow_overwritten", first},
                                             {"slow_overwritten_through_end_qpc", thread.slow_overwritten_through_end},
                                             {"slow_calls", std::move(slow_calls)},
                                             {"totals", metrics_json(thread.totals)},
                                             {"windows", std::move(windows)}});
            }
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << result.dump(2) << '\n';
            output.close();
            return !output.fail();
        }
        catch (...)
        {
            return false;
        }
    }
} // namespace TPVCamera::HookProfile
#endif
