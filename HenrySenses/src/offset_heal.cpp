/**
 * @file offset_heal.cpp
 * @brief Landmark tables, heal groups, and the resolved-offset store.
 *
 * The cadence, per-group latch and one-shot drift warning are rtti::HealScheduler's. What lives here is the
 * mod-specific part: which landmarks describe which struct, the corroborated bracket that recovers the entity
 * offset, and the gates that hold each group back until its base is live.
 */

#include "offset_heal.hpp"
#include "constants.hpp"
#include "global_state.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace HenrySenses
{
    namespace
    {
        // Search radius per side, in bytes. A miss retains the nominal offset for guarded reads. Too large
        // risks an independent scan latching onto a same-typed neighbour, which is why the one common-typed member
        // (the CEntity pointer) is recovered through the corroborated bracket below rather than an independent scan.
        constexpr std::size_t HEAL_WINDOW = std::min<std::size_t>(0x100, DMK::rtti::MAX_HEAL_WINDOW);

        // Scheduler steps between retry scans of an un-latched group. The RTTI prelude is syscall-heavy, so a
        // not-yet-live offset is retried on this cadence rather than on every step.
        constexpr std::uint32_t HEAL_RETRY_INTERVAL_FRAMES = 30;

        // Landmark templates: "at this nominal offset within the struct there is a slot pointing at an object of
        // this mangled type". rtti::Landmark owns its mangled name as a std::string, so these are const, not
        // constexpr; they initialize top-to-bottom within this translation unit before any function runs.
        const DMK::rtti::Landmark ENTITY_LM{
            .nominal_offset = constants::C_PLAYER_ENTITY_OFFSET,
            .expected_mangled = constants::C_ENTITY_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark HITDEATH_LM{
            .nominal_offset = constants::C_PLAYER_HIT_DEATH_REACTIONS_OFFSET,
            .expected_mangled = constants::C_HIT_DEATH_REACTIONS_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark ACTIONGAME_LM{
            .nominal_offset = constants::CCRYACTION_ACTIONGAME_OFFSET,
            .expected_mangled = constants::CACTIONGAME_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark LOCALACTOR_LM{
            .nominal_offset = constants::CACTIONGAME_LOCAL_ACTOR_OFFSET,
            .expected_mangled = constants::C_PLAYER_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark MANAGER_LM{
            .nominal_offset = constants::OFFSET_MANAGER_PTR_STORAGE,
            .expected_mangled = constants::C_CAMERA_MANAGER_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark MINIGAME_SUBSYSTEM_LM{
            .nominal_offset = constants::OFFSET_MINIGAME_SUBSYSTEM,
            .expected_mangled = constants::C_PLAYER_MODULE_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };

        // Live bases published by the main thread and read by the group gates on the same thread the scheduler
        // steps on. Relaxed: each is a standalone word, and a stale base only defers a group by one interval.
        std::atomic<std::uintptr_t> s_cry_action_base{0};
        std::atomic<std::uintptr_t> s_action_game_base{0};
        std::atomic<std::uintptr_t> s_player_base{0};
        std::atomic<std::uintptr_t> s_context_base{0};
        std::atomic<bool> s_local_actor_recovery{false};

        // Created on the init thread; s_scheduler_ready publishes it (release) to the stepping thread (acquire).
        std::optional<DMK::rtti::HealScheduler> s_scheduler;
        std::atomic<bool> s_scheduler_ready{false};

        // Per-landmark drift report, upserted by record_drift() as each group heals.
        constexpr std::size_t DRIFT_CAPACITY = 8;
        std::array<DMK::rtti::DriftEntry, DRIFT_CAPACITY> s_drift{};
        std::atomic<std::size_t> s_drift_count{0};

        /**
         * @struct WindowedLandmarks
         * @brief The landmark templates with the configured search radius stamped in.
         * @details Built once by start_offset_heal() so every scheduler work callback stays noexcept and
         *          allocation-free.
         */
        struct WindowedLandmarks
        {
            DMK::rtti::Landmark entity;
            DMK::rtti::Landmark actiongame;
            DMK::rtti::Landmark localactor;
            DMK::rtti::Landmark manager;
            DMK::rtti::Landmark minigame;
            std::array<DMK::rtti::Landmark, 2> bracket;
        };

        std::optional<WindowedLandmarks> s_windowed;

        /**
         * @brief Copies a template and stamps the search radius on it.
         * @param tmpl The template.
         * @param window The radius in bytes.
         * @return The windowed landmark.
         */
        [[nodiscard]] DMK::rtti::Landmark with_window(const DMK::rtti::Landmark &tmpl, std::size_t window)
        {
            DMK::rtti::Landmark lm = tmpl;
            lm.window = window;
            return lm;
        }

        /**
         * @brief Records one landmark's latest outcome in the drift report, keyed by name (upsert).
         * @details An un-latched group re-scans on every interval, so appending would crowd the report with repeated
         *          misses; the report keeps the last outcome per landmark. A new name past capacity is dropped.
         * @param name Field label, also the report key.
         * @param nominal The field's nominal offset.
         * @param healed The recovered offset (meaningful only when @p ok).
         * @param ok Whether the landmark resolved.
         * @param error The failure code when it did not.
         */
        void record_drift(
            std::string_view name,
            std::ptrdiff_t nominal,
            std::ptrdiff_t healed,
            bool ok,
            DMK::ErrorCode error
        ) noexcept
        {
            const std::size_t count = s_drift_count.load(std::memory_order_relaxed);
            std::size_t index = count;
            for (std::size_t i = 0; i < count; ++i)
            {
                if (s_drift[i].name == name)
                {
                    index = i;
                    break;
                }
            }
            if (index >= DRIFT_CAPACITY)
            {
                return;
            }

            DMK::rtti::DriftEntry &entry = s_drift[index];
            entry.name = name;
            entry.nominal_offset = nominal;
            entry.ok = ok;
            entry.healed_offset = ok ? healed : 0;
            entry.delta = ok ? healed - nominal : 0;
            entry.error = ok ? DMK::ErrorCode::Ok : error;
            if (index == count)
            {
                s_drift_count.store(count + 1, std::memory_order_relaxed);
            }
        }

        /**
         * @brief Heals one landmark through the run and records its outcome.
         * @param run The scheduler run.
         * @param label Field label.
         * @param landmark The windowed landmark.
         * @param base Live struct base.
         * @param slot Destination slot.
         * @return True when the landmark resolved (healed or confirmed at nominal).
         */
        bool heal_and_record(
            DMK::rtti::HealRun &run,
            std::string_view label,
            const DMK::rtti::Landmark &landmark,
            std::uintptr_t base,
            DMK::rtti::HealedSlot &slot
        ) noexcept
        {
            const auto result = run.heal_into(label, landmark, DMK::Address{base}, slot, false);
            record_drift(
                label,
                landmark.nominal_offset,
                result ? result->healed_offset : 0,
                result.has_value(),
                result ? DMK::ErrorCode::Ok : result.error().code
            );
            return result.has_value();
        }

        /**
         * @brief Recovers the entity offset from the corroborated C_Player bracket.
         * @details The CEntity pointee type is common, so an independent window scan could latch a wrong same-typed
         *          neighbour. Requiring the entity and C_HitDeathReactions slots to agree on one uniform delta
         *          rejects that decoy structurally. When the bracket disagrees, the entity offset is scanned for
         *          independently as a last resort, which beats a guaranteed-stale offset for a read-only walk.
         * @param run The scheduler run.
         * @param c_player Validated C_Player base.
         * @param offsets The offset cache.
         * @return True when the entity offset settled.
         */
        bool heal_player_bracket(DMK::rtti::HealRun &run, std::uintptr_t c_player, RuntimeOffsets &offsets) noexcept
        {
            const WindowedLandmarks &lm = *s_windowed;
            // The bracket's evidence is C_Player's own members, so the resolving image is the game module. Its
            // generation is captured on both sides of the evidence walk: an image replaced at its own base in between
            // must not authorize the offset under the replacement's token.
            const std::uint64_t generation_before = DMK::rtti::image_generation(DMK::Address{module_info().base});
            const auto fit = DMK::rtti::solve_fingerprint(DMK::Address{c_player}, lm.bracket, lm.bracket[0].window);
            if (!fit)
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Self-heal: player bracket unresolved ({}); entity via uncorroborated scan",
                    DMK::to_string(fit.error().code)
                );
                return heal_and_record(run, "entity (uncorroborated)", lm.entity, c_player, offsets.c_player_entity);
            }

            const std::uint64_t generation_after = DMK::rtti::image_generation(DMK::Address{module_info().base});
            const std::uint64_t generation = generation_before == generation_after ? generation_after : 0;
            const DMK::rtti::OffsetValidity validity =
                generation != 0 ? DMK::rtti::OffsetValidity::Confirmed : DMK::rtti::OffsetValidity::Unverified;

            const std::ptrdiff_t entity_healed = constants::C_PLAYER_ENTITY_OFFSET + fit->delta;
            offsets.c_player_entity.publish(entity_healed, generation, validity);
            run.note_drift("entity", constants::C_PLAYER_ENTITY_OFFSET, entity_healed);
            record_drift(
                "entity",
                constants::C_PLAYER_ENTITY_OFFSET,
                entity_healed,
                validity == DMK::rtti::OffsetValidity::Confirmed,
                DMK::ErrorCode::OffsetNotConfirmed
            );
            return true;
        }
    } // namespace

    RuntimeOffsets::RuntimeOffsets() noexcept
    {
        ccryaction_actiongame.seed_nominal(constants::CCRYACTION_ACTIONGAME_OFFSET);
        cactiongame_local_actor.seed_nominal(constants::CACTIONGAME_LOCAL_ACTOR_OFFSET);
        c_player_entity.seed_nominal(constants::C_PLAYER_ENTITY_OFFSET);
        context_manager.seed_nominal(constants::OFFSET_MANAGER_PTR_STORAGE);
        context_minigame_subsystem.seed_nominal(constants::OFFSET_MINIGAME_SUBSYSTEM);
    }

    RuntimeOffsets &runtime_offsets() noexcept
    {
        static RuntimeOffsets offsets;
        return offsets;
    }

    std::ptrdiff_t offset_value(const DMK::rtti::HealedSlot &slot) noexcept
    {
        return slot.load().value;
    }

    void note_framework_base(std::uintptr_t cry_action) noexcept
    {
        s_cry_action_base.store(cry_action, std::memory_order_relaxed);
    }

    void note_action_game_base(std::uintptr_t action_game) noexcept
    {
        s_action_game_base.store(action_game, std::memory_order_relaxed);
    }

    void note_player_base(std::uintptr_t c_player) noexcept
    {
        s_player_base.store(c_player, std::memory_order_relaxed);
    }

    void note_context_base(std::uintptr_t context) noexcept
    {
        s_context_base.store(context, std::memory_order_relaxed);
    }

    void request_local_actor_recovery() noexcept
    {
        s_local_actor_recovery.store(true, std::memory_order_relaxed);
    }

    void start_offset_heal()
    {
        DMK::Logger &logger = DMK::log();

        auto started = DMK::rtti::HealScheduler::start(
            DMK::rtti::HealConfig{
                .interval_frames = HEAL_RETRY_INTERVAL_FRAMES,
            }
        );
        if (!started)
        {
            logger.warning(
                "Self-heal: scheduler did not start ({}); every offset stays at its nominal",
                started.error().message()
            );
            return;
        }

        const std::size_t window = HEAL_WINDOW;
        s_windowed.emplace(
            WindowedLandmarks{
                .entity = with_window(ENTITY_LM, window),
                .actiongame = with_window(ACTIONGAME_LM, window),
                .localactor = with_window(LOCALACTOR_LM, window),
                .manager = with_window(MANAGER_LM, window),
                .minigame = with_window(MINIGAME_SUBSYSTEM_LM, window),
                .bracket = {with_window(ENTITY_LM, window), with_window(HITDEATH_LM, window)},
            }
        );

        s_scheduler.emplace(std::move(*started));
        DMK::rtti::HealScheduler &sched = *s_scheduler;
        RuntimeOffsets &offsets = runtime_offsets();
        const WindowedLandmarks &lm = *s_windowed;

        // A group whose registration allocation failed stays unregistered and its offset keeps the nominal.
        std::size_t registered = 0;
        std::size_t rejected = 0;
        const auto count_group = [&registered, &rejected](const DMK::Result<void> &added) noexcept
        { (added.has_value() ? ++registered : ++rejected); };

        // Chain root: CCryAction -> CActionGame. The gate keeps the group silent while CActionGame does not exist
        // yet, the normal state at the main menu or during a load.
        count_group(sched.add_group(
            [&offsets, &lm](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t cry_action = s_cry_action_base.load(std::memory_order_relaxed);
                return heal_and_record(run, "actionGame", lm.actiongame, cry_action, offsets.ccryaction_actiongame);
            },
            [&offsets]() noexcept
            {
                const std::uintptr_t cry_action = s_cry_action_base.load(std::memory_order_relaxed);
                if (cry_action == 0)
                {
                    return false;
                }
                const auto action_game = DMK::memory::read<std::uintptr_t>(
                    DMK::Address{cry_action + offset_value(offsets.ccryaction_actiongame)}
                );
                return action_game.has_value() && DMK::memory::is_plausible_ptr(DMK::Address{*action_game});
            }
        ));

        // CActionGame -> local actor, opened only by the resolver's drift signature.
        count_group(sched.add_group(
            [&offsets, &lm](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t action_game = s_action_game_base.load(std::memory_order_relaxed);
                return heal_and_record(run, "localActor", lm.localactor, action_game, offsets.cactiongame_local_actor);
            },
            []() noexcept
            {
                return s_action_game_base.load(std::memory_order_relaxed) != 0 &&
                       s_local_actor_recovery.load(std::memory_order_relaxed);
            }
        ));

        // C_Player -> CEntity through the corroborated bracket. Deterministic on a validated C_Player, so it
        // normally latches after one pass.
        count_group(sched.add_group(
            [&offsets](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t c_player = s_player_base.load(std::memory_order_relaxed);
                return heal_player_bracket(run, c_player, offsets);
            },
            []() noexcept { return s_player_base.load(std::memory_order_relaxed) != 0; }
        ));

        // The two global-context members latch independently, so a step where one is not yet live retries only
        // that member. The context base is anchored, not navigated through a healed offset, so its drift is
        // recoverable here.
        count_group(sched.add_group(
            [&offsets, &lm](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t context = s_context_base.load(std::memory_order_relaxed);
                return heal_and_record(run, "cameraManager", lm.manager, context, offsets.context_manager);
            },
            []() noexcept { return s_context_base.load(std::memory_order_relaxed) != 0; }
        ));

        count_group(sched.add_group(
            [&offsets, &lm](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t context = s_context_base.load(std::memory_order_relaxed);
                return heal_and_record(
                    run,
                    "minigameSubsystem",
                    lm.minigame,
                    context,
                    offsets.context_minigame_subsystem
                );
            },
            []() noexcept { return s_context_base.load(std::memory_order_relaxed) != 0; }
        ));

        s_scheduler_ready.store(true, std::memory_order_release);
        if (rejected != 0)
        {
            logger.warning("Self-heal: {} group(s) failed to register; their offsets stay at the nominal", rejected);
        }
        logger.info(
            "Self-heal: scheduler started ({} step retry interval, {} groups)",
            HEAL_RETRY_INTERVAL_FRAMES,
            registered
        );
    }

    void offset_heal_tick() noexcept
    {
        DMK_PROFILE_FUNCTION();
        if (!s_scheduler_ready.load(std::memory_order_acquire))
        {
            return;
        }
        s_scheduler->tick();
    }

    std::size_t offset_heal_drift_report(std::span<DMK::rtti::DriftEntry> out) noexcept
    {
        const std::size_t count = std::min(out.size(), s_drift_count.load(std::memory_order_relaxed));
        for (std::size_t i = 0; i < count; ++i)
        {
            out[i] = s_drift[i];
        }
        return count;
    }

} // namespace HenrySenses
