/**
 * @file fixes/sleep_prediction.cpp
 * @brief The cached sleep prediction and soul simulator step detours (see fixes.hpp).
 *
 * The prediction turns its duration into hours exactly as the game does (seconds times the time ratio, divided by the
 * seconds of an hour, truncated). A duration of more hours than one table covers takes the game's uncached path,
 * which the fix leaves alone. Otherwise the game reads entry max(hours - 1, 0) of the table, so the steps up to that
 * hour are the only ones whose result is read. The shortened table stays in the game's one-slot cache under the
 * request's shape (the sleep and alternative flags). The fix remembers how many hours it built per shape. A request of
 * that shape that needs more hours comes from the sleep or wait slider, which reads the next hours too, so the fix
 * clears the cache stamp and builds the whole table once. The record starts at zero, so the first request of each
 * shape in a generation rebuilds, and no table from an earlier generation is trusted.
 */

#include "config.hpp"
#include "constants.hpp"
#include "fixes/diag.hpp"
#include "fixes/fixes.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <climits>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace EngineTweaks::fixes
{
    namespace
    {
        using PredictionFunc = void *(__fastcall *)(void *utils,
                                                    void *out,
                                                    void *soul,
                                                    std::uint8_t sleep,
                                                    float seconds,
                                                    std::uint8_t alternative,
                                                    std::int32_t extra);
        using SimStepFunc = std::uintptr_t(__fastcall *)(
            void *simulator,
            float duration,
            std::uint8_t sleep,
            float extra,
            std::uint8_t alternative,
            std::uint8_t continuing
        );
        using ParamsGetterFunc = const std::uint8_t *(__fastcall *)();
        using CloneItemsFunc =
            void(__fastcall *)(void *source, void *destination, std::uint8_t map_clones, std::uint32_t item_type);

        std::atomic<PredictionFunc> s_prediction_original{nullptr};
        std::atomic<SimStepFunc> s_step_original{nullptr};
        std::atomic<CloneItemsFunc> s_clone_original{nullptr};
        std::atomic<ParamsGetterFunc> s_params_getter{nullptr};
        std::atomic<float> s_seconds_per_hour{0.0f};
        /**
         * @brief The C_RPGUtils that owns the cache, so teardown can clear its stamp.
         * @details C_RPGUtils is a function-local static of WHGame.dll that lives until process exit, so the pointer
         *          stays valid for every teardown before that.
         */
        std::atomic<void *> s_utils{nullptr};
        /**
         * @brief True while the cache can hold a table the fix shortened or built without the items.
         * @details A request that passes through to the game reads the cache as it is, so it clears the stamp first.
         */
        std::atomic<bool> s_cache_shortened{false};

        /// The game's threshold under which a duration takes the instant path, without a table.
        constexpr float INSTANT_SECONDS = 0.0000099999997f;
        /// The floats that convert to int without overflow: from -2^31 up to, not including, 2^31.
        constexpr float INT_RANGE_MIN = -2147483648.0f;
        constexpr float INT_RANGE_END = 2147483648.0f;
        /// The request shapes: the sleep flag and the alternative flag.
        constexpr std::size_t SHAPE_COUNT = 4;

        /** @brief The thread inside a capped request, 0 when none. Only that thread touches the fields below. */
        std::atomic<DWORD> s_window_owner{0};
        int s_window_hours{0};
        int s_window_steps{0};
        bool s_window_without_items{false};
        /// Hour steps of the open window returned unchanged, for the log.
        int s_window_skipped{0};
        /// Item clones the open window skipped, for the log.
        int s_window_clones_skipped{0};
        /// Why the open window dropped the cached table, or nullptr, for the log.
        const char *s_window_rebuild{nullptr};
        /// Hours of the table the fix last built per shape. 0 until the first build of a generation.
        std::array<int, SHAPE_COUNT> s_built_hours{};
        /// Whether that table left the items out, so a request of the other mode rebuilds it.
        std::array<bool, SHAPE_COUNT> s_built_without_items{};
        /// Bytes of one prediction result.
        constexpr std::size_t RESULT_SIZE = 16;

        [[nodiscard]] std::size_t shape_of(std::uint8_t sleep, std::uint8_t alternative) noexcept
        {
            return (sleep != 0 ? 1u : 0u) | (alternative != 0 ? 2u : 0u);
        }

        void clear_cache_stamp(void *utils) noexcept
        {
            *reinterpret_cast<float *>(static_cast<std::uint8_t *>(utils) + constants::RPG_UTILS_CACHE_STAMP_OFFSET) =
                0.0f;
        }

        /// The most table hours one verification compares.
        constexpr std::size_t MAX_VERIFY_HOURS = 64;
        using TableCopy = std::array<std::uint8_t, RESULT_SIZE * MAX_VERIFY_HOURS>;

        /** @brief Copies the first @p hours results of the cached table. False when the cache has no table. */
        [[nodiscard]] bool copy_table(void *utils, std::size_t hours, TableCopy &copy) noexcept
        {
            const auto *table = *reinterpret_cast<const std::uint8_t *const *>(
                static_cast<const std::uint8_t *>(utils) + constants::RPG_UTILS_CACHE_TABLE_OFFSET
            );
            if (table == nullptr)
            {
                return false;
            }
            std::memcpy(copy.data(), table, hours * RESULT_SIZE);
            return true;
        }

        /** @brief Hex text of one result, for the verification log. */
        [[nodiscard]] std::string hex_of(const std::uint8_t *bytes)
        {
            constexpr std::string_view digits = "0123456789ABCDEF";
            std::string text;
            text.reserve(RESULT_SIZE * 2);
            for (std::size_t i = 0; i < RESULT_SIZE; ++i)
            {
                text.push_back(digits[bytes[i] >> 4]);
                text.push_back(digits[bytes[i] & 0xF]);
            }
            return text;
        }

        /** @brief The hours a request reads and the hour count of its table. Both 0 for the paths without a table. */
        struct HourPlan
        {
            int hours{0};
            int table_hours{0};
        };

        /**
         * @brief The hours the game reads for a request of @p seconds.
         * @details A quotient outside the int range, or NaN, gets an empty plan and takes the game's own path, because
         *          its conversion to int is undefined behavior in C++.
         */
        [[nodiscard]] HourPlan plan_hours(float seconds) noexcept
        {
            const ParamsGetterFunc getter = s_params_getter.load(std::memory_order_acquire);
            const float seconds_per_hour = s_seconds_per_hour.load(std::memory_order_relaxed);
            if (!std::isfinite(seconds) || !(seconds > INSTANT_SECONDS) || getter == nullptr)
            {
                return {};
            }
            const std::uint8_t *params = getter();
            if (params == nullptr)
            {
                return {};
            }
            const float ratio = *reinterpret_cast<const float *>(params + constants::RPG_PARAMS_TIME_RATIO_OFFSET);
            const int table_hours =
                *reinterpret_cast<const std::int32_t *>(params + constants::RPG_PARAMS_PREDICTION_HOURS_OFFSET);
            // The same single-precision multiply, divide and truncation as the game's hour count.
            const float product = seconds * ratio;
            const float quotient = product / seconds_per_hour;
            if (!(quotient >= INT_RANGE_MIN && quotient < INT_RANGE_END))
            {
                return {};
            }
            const int hours = static_cast<int>(quotient);
            if (hours > table_hours || table_hours <= 0)
            {
                return {};
            }
            return HourPlan{
                .hours = std::max(hours, 1),
                .table_hours = table_hours,
            };
        }

        /** @brief One request run inside an owned window. Requires the window. */
        void *run_capped(
            PredictionFunc original,
            void *utils,
            void *out,
            void *soul,
            std::uint8_t sleep,
            float seconds,
            std::uint8_t alternative,
            std::int32_t extra,
            int hours,
            bool without_items
        ) noexcept
        {
            const std::size_t shape = shape_of(sleep, alternative);
            if (hours > s_built_hours[shape] || without_items != s_built_without_items[shape])
            {
                clear_cache_stamp(utils);
                s_window_rebuild = s_built_hours[shape] == 0      ? "no table of this shape yet"
                                   : hours > s_built_hours[shape] ? "the table holds fewer hours"
                                                                  : "the item mode changed";
            }
            s_window_hours = hours;
            s_window_steps = 0;
            s_window_without_items = without_items;
            void *const result = original(utils, out, soul, sleep, seconds, alternative, extra);
            if (s_window_steps != 0)
            {
                s_built_hours[shape] = hours;
                s_built_without_items[shape] = without_items;
            }
            return result;
        }

        /**
         * @brief Runs the request with the items and, when that built a table, compares every simulated hour of it with
         *        a build without them.
         * @details The result with the items stands, and a last build with the items leaves their table cached, so a
         *          request the cache answers runs once and the comparison costs three builds only when the game builds.
         *          The last build is compared too, which shows whether two builds of the same request agree at all.
         */
        void *run_verified(
            PredictionFunc original,
            void *utils,
            void *out,
            void *soul,
            std::uint8_t sleep,
            float seconds,
            std::uint8_t alternative,
            std::int32_t extra,
            int hours
        ) noexcept
        {
            void *const result =
                run_capped(original, utils, out, soul, sleep, seconds, alternative, extra, hours, false);
            if (s_window_steps == 0)
            {
                return result;
            }
            const char *const rebuild = s_window_rebuild;
            const std::size_t compared = std::min(static_cast<std::size_t>(hours), MAX_VERIFY_HOURS);
            TableCopy with{};
            TableCopy without{};
            TableCopy rebuilt{};
            bool copied = copy_table(utils, compared, with);
            // Each run changes the item mode, so each rebuilds the table.
            (void)run_capped(original, utils, out, soul, sleep, seconds, alternative, extra, hours, true);
            copied = copy_table(utils, compared, without) && copied;
            // The request's log line describes the last build, the one that stays cached.
            s_window_skipped = 0;
            s_window_clones_skipped = 0;
            (void)run_capped(original, utils, out, soul, sleep, seconds, alternative, extra, hours, false);
            copied = copy_table(utils, compared, rebuilt) && copied;
            s_window_rebuild = rebuild;
            if (!copied)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Warning, "SleepPrediction: no cached table to compare");
                return result;
            }
            std::size_t differing = 0;
            std::size_t unstable = 0;
            std::size_t first = compared;
            for (std::size_t hour = 0; hour < compared; ++hour)
            {
                const std::size_t at = hour * RESULT_SIZE;
                const bool differs = std::memcmp(with.data() + at, without.data() + at, RESULT_SIZE) != 0;
                const bool moved = std::memcmp(with.data() + at, rebuilt.data() + at, RESULT_SIZE) != 0;
                differing += differs ? 1 : 0;
                unstable += moved ? 1 : 0;
                if ((differs || moved) && first == compared)
                {
                    first = hour;
                }
            }
            if (differing != 0 || unstable != 0)
            {
                diag::counters().sleep_verify_mismatches.add();
            }
            try
            {
                const std::size_t shown = first < compared ? first : compared - 1;
                (void)DMK::log().try_log(
                    differing == 0 && unstable == 0 ? DMK::LogLevel::Info : DMK::LogLevel::Warning,
                    "SleepPrediction: {} simulated hour(s) compared, {} DIFFER without items, {} differ between two "
                    "builds with items; hour {}: {} without vs {} with, {} rebuilt",
                    compared,
                    differing,
                    unstable,
                    shown + 1,
                    hex_of(without.data() + shown * RESULT_SIZE),
                    hex_of(with.data() + shown * RESULT_SIZE),
                    hex_of(rebuilt.data() + shown * RESULT_SIZE)
                );
            }
            catch (...)
            {
                // The verification line is diagnostics only.
            }
            return result;
        }

        /**
         * @brief The prediction detour: the game's request with the steps past the read hour skipped.
         * @note The thread that asks for a prediction, normally the main thread. No-throw.
         */
        void *__fastcall detour_prediction(
            void *utils,
            void *out,
            void *soul,
            std::uint8_t sleep,
            float seconds,
            std::uint8_t alternative,
            std::int32_t extra
        ) noexcept
        {
            const PredictionFunc original = s_prediction_original.load(std::memory_order_acquire);
            const DWORD self = GetCurrentThreadId();
            if (s_window_owner.load(std::memory_order_acquire) == self)
            {
                // A request nested inside a capped one runs every step of its own table, with its items.
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "SleepPrediction: request of {} s nested in a capped one runs every hour with its items",
                    seconds
                );
                const int outer_hours = std::exchange(s_window_hours, INT_MAX);
                const int outer_steps = std::exchange(s_window_steps, 0);
                const bool outer_without_items = std::exchange(s_window_without_items, false);
                void *const nested = original(utils, out, soul, sleep, seconds, alternative, extra);
                s_window_hours = outer_hours;
                s_window_steps = outer_steps;
                s_window_without_items = outer_without_items;
                return nested;
            }
            const bool enabled = fix_enabled(Fix::SleepPrediction);
            const HourPlan plan = enabled && utils != nullptr && out != nullptr ? plan_hours(seconds) : HourPlan{};
            DWORD owner = 0;
            if (plan.hours == 0 || !s_window_owner.compare_exchange_strong(owner, self, std::memory_order_acq_rel))
            {
                // With the fix on, a plan of 0 hours is the game's instant or uncached path, which reads no table.
                const bool reads_table = !enabled || plan.hours != 0;
                const bool cleared =
                    reads_table && utils != nullptr && s_cache_shortened.exchange(false, std::memory_order_acq_rel);
                if (cleared)
                {
                    // A float store the game reads as one value: at worst it rebuilds a table it did not need to.
                    clear_cache_stamp(utils);
                }
                diag::counters().sleep_passthroughs.add();
                (void)DMK::log().try_log(
                    cleared ? DMK::LogLevel::Debug : DMK::LogLevel::Trace,
                    "SleepPrediction: request of {} s (sleep {}, alternative {}) passes through ({}){}",
                    seconds,
                    sleep,
                    alternative,
                    !enabled                             ? "fix off"
                    : utils == nullptr || out == nullptr ? "no cache or result"
                    : plan.hours == 0                    ? "no table, or past the table"
                                                         : "another thread holds the window",
                    cleared ? ", the shortened table is dropped first" : ""
                );
                return original(utils, out, soul, sleep, seconds, alternative, extra);
            }
            s_utils.store(utils, std::memory_order_release);
            const bool without_items = s_clone_original.load(std::memory_order_acquire) != nullptr &&
                                       fix_enabled(Fix::SleepPredictionWithoutItems);
            const bool verify = without_items && verify_sleep_prediction();
            const int built_hours = s_built_hours[shape_of(sleep, alternative)];
            const int hours = built_hours != 0 && plan.hours > built_hours ? plan.table_hours : plan.hours;
            s_window_skipped = 0;
            s_window_clones_skipped = 0;
            s_window_rebuild = nullptr;
            const diag::Stopwatch watch;
            void *const result =
                verify
                    ? run_verified(original, utils, out, soul, sleep, seconds, alternative, extra, hours)
                    : run_capped(original, utils, out, soul, sleep, seconds, alternative, extra, hours, without_items);
            const bool built = s_window_steps != 0;
            const int steps = s_window_steps;
            const int skipped = s_window_skipped;
            const int clones_skipped = s_window_clones_skipped;
            const char *const rebuild = s_window_rebuild;
            if (built)
            {
                // A verified request ends on a build with the items, so only its hour count can shorten the table.
                const bool items_left_out = without_items && !verify;
                s_cache_shortened.store(hours < plan.table_hours || items_left_out, std::memory_order_release);
            }
            s_window_owner.store(0, std::memory_order_release);
            diag::Counters &counters = diag::counters();
            counters.sleep_requests.add();
            counters.sleep_steps_skipped.add(static_cast<std::uint64_t>(skipped));
            counters.sleep_clones_skipped.add(static_cast<std::uint64_t>(clones_skipped));
            if (built)
            {
                counters.sleep_builds.add();
            }
            (void)DMK::log().try_log(
                built ? DMK::LogLevel::Debug : DMK::LogLevel::Trace,
                "SleepPrediction: request of {} s (sleep {}, alternative {}) reads {} of {} hour(s), simulates {}: "
                "{}{}{}, {} step(s), {} skipped, {} item clone(s) skipped, items {}, {:.2f} ms",
                seconds,
                sleep,
                alternative,
                plan.hours,
                plan.table_hours,
                hours,
                built ? "table built" : "cached table",
                rebuild != nullptr ? " because " : "",
                rebuild != nullptr ? rebuild : "",
                steps,
                skipped,
                clones_skipped,
                verify          ? "kept, compared with a build without"
                : without_items ? "left out"
                                : "kept",
                watch.elapsed_ms()
            );
            diag::maybe_log_summary();
            return result;
        }

        /**
         * @brief The hour step detour: inside a capped request, a step past the read hour returns the copy unchanged.
         * @note Any thread. No-throw.
         */
        std::uintptr_t __fastcall detour_sim_step(
            void *simulator,
            float duration,
            std::uint8_t sleep,
            float extra,
            std::uint8_t alternative,
            std::uint8_t continuing
        ) noexcept
        {
            const SimStepFunc original = s_step_original.load(std::memory_order_acquire);
            // The owner check comes first: only the owner thread reads the plain fields. The first step creates the
            // copy, so it always runs.
            if (s_window_owner.load(std::memory_order_acquire) == GetCurrentThreadId())
            {
                const int step = s_window_steps++;
                if (continuing != 0 && step >= s_window_hours)
                {
                    ++s_window_skipped;
                    return *reinterpret_cast<const std::uintptr_t *>(
                        static_cast<const std::uint8_t *>(simulator) + constants::SOUL_SIMULATOR_COPY_OFFSET
                    );
                }
            }
            return original(simulator, duration, sleep, extra, alternative, continuing);
        }

        /**
         * @brief The item clone detour: inside a capped request that leaves the items out, clones nothing.
         * @details The simulated soul is a fresh copy that the request destroys, so it has no clones to map.
         * @note Any thread. No-throw.
         */
        void __fastcall
        detour_clone_items(void *source, void *destination, std::uint8_t map_clones, std::uint32_t item_type) noexcept
        {
            const CloneItemsFunc original = s_clone_original.load(std::memory_order_acquire);
            if (s_window_owner.load(std::memory_order_acquire) == GetCurrentThreadId() && s_window_without_items)
            {
                ++s_window_clones_skipped;
                return;
            }
            original(source, destination, map_clones, item_type);
        }
    } // namespace

    DMK::Result<void> install_sleep_prediction(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        if (!anchors.sleep_prediction_fix)
        {
            return {};
        }
        s_params_getter.store(reinterpret_cast<ParamsGetterFunc>(anchors.rpg_params_getter), std::memory_order_release);
        s_seconds_per_hour.store(anchors.seconds_per_hour, std::memory_order_relaxed);
        // The step and clone hooks go first, so a request never opens a window whose callees are not yet hooked.
        DMK_TRY_VOID(
            install_inline_hook<&detour_sim_step>(hooks, "SoulSimStep", anchors.soul_sim_step, s_step_original)
        );
        if (anchors.sleep_prediction_items_fix)
        {
            DMK_TRY_VOID(
                install_inline_hook<&detour_clone_items>(
                    hooks,
                    "InventoryCloneItems",
                    anchors.inventory_clone_items,
                    s_clone_original
                )
            );
        }
        return install_inline_hook<&detour_prediction>(
            hooks,
            "SleepPrediction",
            anchors.sleep_prediction,
            s_prediction_original
        );
    }

    void reset_sleep_prediction() noexcept
    {
        void *const utils = s_utils.exchange(nullptr, std::memory_order_acq_rel);
        if (utils != nullptr)
        {
            // A float store the game reads as one value: at worst its next request rebuilds.
            clear_cache_stamp(utils);
        }
        s_cache_shortened.store(false, std::memory_order_release);
        s_built_hours = {};
        s_built_without_items = {};
        (void)DMK::log().try_log(
            DMK::LogLevel::Debug,
            "SleepPrediction: reset, the next request rebuilds its table (cache {})",
            utils
        );
    }
} // namespace EngineTweaks::fixes
