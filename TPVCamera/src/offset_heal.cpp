/**
 * @file offset_heal.cpp
 * @brief Landmark tables, heal groups, and the resolved-offset store.
 *
 * The cadence, per-group latch, and one-shot drift Warning are rtti::HealScheduler's. What lives here is
 * the mod-specific part: which landmarks describe which struct, the corroborated top-of-struct bracket that
 * recovers two members from one delta, and the gates that hold each group back until its base is live.
 */

#include "offset_heal.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "global_state.hpp"

#include "dmk_aliases.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace TPVCamera
{

    namespace
    {

        // Search radius per side for every heal, in bytes (applied to both the heal_landmark windows and the
        // solve_fingerprint span). This is a correctness trade-off, NOT "bigger is safer": too small misses a large
        // insertion but fails CLOSED (keeps the nominal offset = the current hardcoded behaviour, never a crash); too
        // large would, for an INDEPENDENT single-landmark scan, risk a confident WRONG heal onto a same-typed
        // neighbour. That decoy risk is why the window can be this wide: the one common-typed member
        // (entity/CEntity) is recovered via the corroborated bracket (k_player_top_bracket), not an independent
        // scan, so EVERY remaining independent heal keys on a type that is UNIQUE within its parent struct, where a
        // wide window cannot find a decoy; the bracket itself rejects decoys structurally (one delta must satisfy
        // two anchors). The default 0x100 covers a 32-qword insertion before a field while staying far below
        // MAX_HEAL_WINDOW (4096), so the init-time probe count stays bounded. It is exposed in the INI
        // ([Advanced] SelfHealWindow) for the rare case a real patch shifts a field further than the default.
        constexpr std::size_t k_heal_window_default = 0x100;

        // Frames between retry scans of an un-latched group, handed to the scheduler as HealConfig. The RTTI
        // prelude is syscall-heavy, so a not-yet-live offset is retried on this cadence rather than every frame.
        // The scheduler applies no attempt cap: however long the player lingers at the main menu or a load takes,
        // a group keeps retrying until it resolves, then latches and stops.
        constexpr std::uint32_t k_heal_retry_interval_frames = 30; // about 0.5s at 60 FPS

        // Resolved search radius: the INI value (read once when the heal runs, not a hot path), clamped to the DMK
        // maximum; a non-positive or unset value falls back to the default. A wider window also widens the
        // uncorroborated entity fallback's decoy exposure, which is the deliberate cost the INI knob trades for
        // reach.
        [[nodiscard]] std::size_t heal_window() noexcept
        {
            const int configured = settings().self_heal_window.load(std::memory_order_relaxed);
            if (configured <= 0)
            {
                return k_heal_window_default;
            }
            return std::min(static_cast<std::size_t>(configured), DMK::rtti::MAX_HEAL_WINDOW);
        }

        // These are `const`, not `constexpr`: rtti::Landmark holds expected_mangled as an owned std::string,
        // so the table is dynamically initialized at load rather than baked into .rdata. That is safe here and
        // must not be reverted to constexpr. The table lives in this anonymous namespace and only functions
        // that run after DllMain read it, and dynamic initialization within one translation unit runs
        // top-to-bottom, so k_player_top_bracket below always initializes after the landmarks it copies.
        //
        // Self-heal landmarks: "at this nominal offset within the struct there is a slot referring to an object of
        // this mangled type." Each is keyed on a type that is stable across patches (an engine/base type or an
        // already-trusted concrete type), per the rtti_dissect guidance. base is filled by the scheduler at scan
        // time. The indirection records the slot SHAPE: every member here is a pointer-to-object EXCEPT the missile
        // controller, which is constructed in-place inside C_Player (its first qword is the vtable), so it is a
        // direct object base matched with CompleteObject (see k_missile_lm for why CompleteObject, not ObjectBase).
        const DMK::rtti::Landmark k_entity_lm{
            .nominal_offset = Constants::C_PLAYER_ENTITY_OFFSET,
            .expected_mangled = Constants::C_ENTITY_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark k_animhuman_lm{
            .nominal_offset = Constants::C_PLAYER_ANIMATED_HUMAN_OFFSET,
            .expected_mangled = Constants::C_ANIMATED_HUMAN_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark k_actormodel_lm{
            .nominal_offset = Constants::C_PLAYER_ACTOR_MODEL_OFFSET,
            .expected_mangled = Constants::C_ACTOR_MODEL_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark k_missile_lm{
            .nominal_offset = Constants::C_PLAYER_MISSILE_CONTROLLER_OFFSET,
            .expected_mangled = Constants::C_MISSILE_CONTROLLER_RTTI_NAME,
            // Embedded object, not a pointer. CompleteObject matches only the primary subobject (COL.offset == 0),
            // where ObjectBase would match any subobject: under multiple inheritance every base carries its own
            // vtable and each vtable's COL names the same most-derived type, so a window scan keyed on ObjectBase
            // could latch a secondary base and heal to an offset shifted by that subobject delta (a silent,
            // confidence-full off-by-a-subobject heal). The embedded member's nominal slot already holds the primary
            // vtable, so CompleteObject only adds the MI guard for the drift scan and never changes the nominal match.
            .indirection = DMK::rtti::Indirection::CompleteObject};
        const DMK::rtti::Landmark k_animchar_lm{
            .nominal_offset = Constants::ANIMATED_HUMAN_ANIMCHAR_OFFSET,
            .expected_mangled = Constants::ANIMATED_CHARACTER_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark k_actiongame_lm{
            .nominal_offset = Constants::CCRYACTION_ACTIONGAME_OFFSET,
            .expected_mangled = Constants::CACTIONGAME_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark k_localactor_lm{
            .nominal_offset = Constants::CACTIONGAME_LOCAL_ACTOR_OFFSET,
            .expected_mangled = Constants::C_PLAYER_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark k_manager_lm{
            .nominal_offset = Constants::OFFSET_MANAGER_PTR_STORAGE,
            .expected_mangled = Constants::C_CAMERA_MANAGER_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };
        const DMK::rtti::Landmark k_minigame_subsystem_lm{
            .nominal_offset = Constants::OFFSET_MINIGAME_SUBSYSTEM,
            .expected_mangled = Constants::C_PLAYER_MODULE_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };

        const DMK::rtti::Landmark k_hitdeath_lm{
            .nominal_offset = Constants::C_PLAYER_HIT_DEATH_REACTIONS_OFFSET,
            .expected_mangled = Constants::C_HIT_DEATH_REACTIONS_RTTI_NAME,
            .indirection = DMK::rtti::Indirection::PointerToObject,
        };

        // Corroborated top-of-struct bracket. It recovers TWO members' offsets from one uniform delta that BOTH
        // straddling RTTI anchors agree on:
        //   - the look controller, whose own pointee is a non-polymorphic struct with NO RTTI, so it cannot be
        //     matched by type at all; and
        //   - the entity pointer, whose pointee type (CEntity) is COMMON, so an independent window scan could heal
        //     onto the wrong same-typed neighbour. Requiring entity and HitDeathReactions to agree on one delta
        //     rejects that decoy STRUCTURALLY, which is what lets the search window be widened safely.
        // Both anchors are required (the Landmark default), so the agreement is genuine. entity is the TOPMOST
        // anchor: a shift that moves it moves HitDeathReactions equally (the bracket then succeeds), and a shift
        // below entity leaves it at nominal (correct); a net insertion BETWEEN the anchors makes them disagree, so
        // solve_fingerprint returns NoMatch and both offsets stay nominal (fail-closed). Built from the named
        // landmarks so entity has a single definition shared with nothing else.
        const std::array<DMK::rtti::Landmark, 2> k_player_top_bracket{{k_entity_lm, k_hitdeath_lm}};

        // Live bases, published by the render path as it resolves each one and read by the group gates on the
        // same thread the scheduler ticks on. Relaxed is sufficient: each is a standalone word with no dependent
        // data published through it, and a base that is one frame stale simply defers the group by one interval.
        std::atomic<std::uintptr_t> s_cry_action_base{0};
        std::atomic<std::uintptr_t> s_action_game_base{0};
        std::atomic<std::uintptr_t> s_player_base{0};
        std::atomic<std::uintptr_t> s_context_base{0};
        std::atomic<bool> s_local_actor_recovery{false};

        // The scheduler owns the cadence, the per-group latch, and the one-shot drift Warning. Created on the
        // init thread; s_scheduler_ready publishes it (release) to the render thread that ticks it (acquire), so
        // a tick can never observe a half-built group list.
        std::optional<DMK::rtti::HealScheduler> s_scheduler;
        std::atomic<bool> s_scheduler_ready{false};

        // Accumulated per-landmark drift report, appended by record_drift() as each group heals. rtti::heal_report
        // is the one-base batch form of this; these landmarks span four bases that become live at different times,
        // so building the report from the results the groups already produced avoids a second full scan.
        constexpr std::size_t k_drift_capacity = 12;
        std::array<DMK::rtti::DriftEntry, k_drift_capacity> s_drift{};
        std::atomic<std::size_t> s_drift_count{0};

        /**
         * @struct WindowedLandmarks
         * @brief The landmark templates with the single tunable search radius stamped in.
         * @details Built ONCE by start_offset_heal(), after config::load() has applied [Advanced] SelfHealWindow.
         *          Landmark owns its expected_mangled as a std::string, so copying one allocates; building the
         *          windowed set up front is what lets every scheduler work callback below be noexcept and
         *          allocation-free, as the heal contract wants. A later hot-reload of SelfHealWindow therefore
         *          does not change the radius of an already-registered group, which is the intended trade: the
         *          knob exists for a patch-day re-author, not for live tuning.
         */
        struct WindowedLandmarks
        {
            DMK::rtti::Landmark entity;
            DMK::rtti::Landmark animhuman;
            DMK::rtti::Landmark actormodel;
            DMK::rtti::Landmark missile;
            DMK::rtti::Landmark animchar;
            DMK::rtti::Landmark actiongame;
            DMK::rtti::Landmark localactor;
            DMK::rtti::Landmark manager;
            DMK::rtti::Landmark minigame;
            std::array<DMK::rtti::Landmark, 2> bracket;
        };

        std::optional<WindowedLandmarks> s_windowed;

        /// Copies a template and stamps the resolved search radius on it.
        [[nodiscard]] DMK::rtti::Landmark with_window(const DMK::rtti::Landmark &tmpl, std::size_t window)
        {
            DMK::rtti::Landmark lm = tmpl;
            lm.window = window;
            return lm;
        }

        /**
         * @brief Records one landmark's latest outcome in the drift report, keyed by name.
         * @details An UPSERT, not an append: an un-latched group re-scans on every interval, so appending would
         *          fill the report with repeated misses of one landmark and crowd out the others. The report
         *          therefore always holds the LAST outcome per landmark, which is the one that matters. Silently
         *          drops a new name past k_drift_capacity: the report is diagnostics, and losing a row must never
         *          cost a heal. Called from the scheduler's work callbacks, so single-threaded by construction.
         * @param name Short field label, also the report key.
         * @param nominal The field's last-known offset.
         * @param healed The recovered offset (meaningful only when @p ok).
         * @param ok Whether the landmark resolved.
         * @param error The failure code when it did not.
         */
        void record_drift(std::string_view name, std::ptrdiff_t nominal, std::ptrdiff_t healed, bool ok,
                          DMK::ErrorCode error) noexcept
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
            if (index >= k_drift_capacity)
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

        /// Records a heal_into outcome. See record_drift.
        void record_drift(std::string_view name, std::ptrdiff_t nominal,
                          const DMK::Result<DMK::rtti::HealHit> &result) noexcept
        {
            record_drift(name, nominal, result ? result->healed_offset : 0, result.has_value(),
                         result ? DMK::ErrorCode::Ok : result.error().code);
        }

        /**
         * @brief Heals one landmark through the run and records its outcome.
         * @return true when the landmark resolved (healed or confirmed at nominal).
         * @details heal_into does the logging itself under the scheduler's escalation policy (a moved field at
         *          Info behind the one-shot drift Warning, a no-drift hit at Debug, a miss at Debug for an
         *          optional landmark), so this adds only the drift-report row. Every landmark here is optional:
         *          a miss means the target is not constructed yet far more often than it means real drift, and
         *          the retained nominal is still correct on an undrifted build.
         */
        bool heal_and_record(DMK::rtti::HealRun &run, std::string_view label, const DMK::rtti::Landmark &landmark,
                             std::uintptr_t base, DMK::rtti::HealedSlot &slot) noexcept
        {
            const auto result = run.heal_into(label, landmark, Address{base}, slot, false);
            record_drift(label, landmark.nominal_offset, result);
            return result.has_value();
        }

        /**
         * @brief Recovers the entity + look-controller offsets from the corroborated top-of-struct bracket.
         * @details Publishes both slots itself (solve_fingerprint yields one delta, not a per-slot HealHit), so
         *          it stamps the game image's own generation and reports each move through HealRun::note_drift.
         *          That routes the bracket into the SAME one-shot layout-drift Warning and the same per-field
         *          Info line the scheduler emits for heal_into, instead of a parallel warn-once of our own.
         * @return true when the bracket solved (so the group may latch).
         */
        bool heal_player_bracket(DMK::rtti::HealRun &run, std::uintptr_t c_player, RuntimeOffsets &offsets) noexcept
        {
            const WindowedLandmarks &lm = *s_windowed;
            const auto fit = DMK::rtti::solve_fingerprint(Address{c_player}, lm.bracket, lm.bracket[0].window);
            if (!fit)
            {
                // Bracket disagreed (non-uniform shift across the span). lookController has no RTTI of its own,
                // so it cannot be recovered independently and stays nominal AND Unverified, which withdraws the
                // write authorization the orbit aim control needs. entity CAN still be scanned for by type as a
                // LAST RESORT: this reintroduces the decoy risk the corroborated solve avoids (a wrong same-typed
                // CEntity neighbour could be picked), but a best-effort offset beats a guaranteed-stale one for a
                // read-only walk, so try it. The solve is deterministic on a validated C_Player, so an undrifted
                // build never reaches here; the diagnostic is logged at Debug.
                (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                         "Self-heal: bracket unresolved ({}); lookController kept nominal; entity "
                                         "via uncorroborated scan",
                                         DMK::to_string(fit.error().code));
                return heal_and_record(run, "entity (uncorroborated fallback)", lm.entity, c_player,
                                       offsets.c_player_entity);
            }

            // The bracket's evidence is C_Player's own members, so the resolving image is the game module the
            // landmark types live in. Stamping its generation is what lets a later authorized(generation) call
            // reject the offset if that image is ever replaced under us.
            const std::uint64_t generation = DMK::rtti::image_generation(Address{module_info().base});
            const DMK::rtti::OffsetValidity validity =
                generation != 0 ? DMK::rtti::OffsetValidity::Confirmed : DMK::rtti::OffsetValidity::Unverified;

            const std::ptrdiff_t entity_healed = Constants::C_PLAYER_ENTITY_OFFSET + fit->delta;
            const std::ptrdiff_t look_healed = Constants::C_PLAYER_LOOK_CONTROLLER_OFFSET + fit->delta;
            offsets.c_player_entity.publish(entity_healed, generation, validity);
            offsets.c_player_look_controller.publish(look_healed, generation, validity);

            run.note_drift("entity", Constants::C_PLAYER_ENTITY_OFFSET, entity_healed);
            run.note_drift("lookController", Constants::C_PLAYER_LOOK_CONTROLLER_OFFSET, look_healed);

            // The bracket publishes its slots itself, so it also files its own report rows; without these two
            // the drift summary would under-count by the exact members the bracket exists to recover.
            const bool confirmed = validity == DMK::rtti::OffsetValidity::Confirmed;
            record_drift("entity", Constants::C_PLAYER_ENTITY_OFFSET, entity_healed, confirmed,
                         DMK::ErrorCode::OffsetNotConfirmed);
            record_drift("lookController", Constants::C_PLAYER_LOOK_CONTROLLER_OFFSET, look_healed, confirmed,
                         DMK::ErrorCode::OffsetNotConfirmed);
            return true;
        }

    } // namespace

    RuntimeOffsets::RuntimeOffsets() noexcept
    {
        // Seed every slot with its constants.hpp nominal so a read before the first heal returns the hardcoded
        // value with an explicit Unverified status, never a Confirmed one and never a zero.
        ccryaction_actiongame.seed_nominal(Constants::CCRYACTION_ACTIONGAME_OFFSET);
        cactiongame_local_actor.seed_nominal(Constants::CACTIONGAME_LOCAL_ACTOR_OFFSET);
        c_player_entity.seed_nominal(Constants::C_PLAYER_ENTITY_OFFSET);
        c_player_look_controller.seed_nominal(Constants::C_PLAYER_LOOK_CONTROLLER_OFFSET);
        c_player_animated_human.seed_nominal(Constants::C_PLAYER_ANIMATED_HUMAN_OFFSET);
        c_player_actor_model.seed_nominal(Constants::C_PLAYER_ACTOR_MODEL_OFFSET);
        c_player_missile_controller.seed_nominal(Constants::C_PLAYER_MISSILE_CONTROLLER_OFFSET);
        animated_human_animchar.seed_nominal(Constants::ANIMATED_HUMAN_ANIMCHAR_OFFSET);
        context_manager.seed_nominal(Constants::OFFSET_MANAGER_PTR_STORAGE);
        context_minigame_subsystem.seed_nominal(Constants::OFFSET_MINIGAME_SUBSYSTEM);
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

    DMK::Result<std::ptrdiff_t> write_authorized_offset(const DMK::rtti::HealedSlot &slot) noexcept
    {
        return slot.authorized();
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
            DMK::rtti::HealConfig{.interval_frames = k_heal_retry_interval_frames});
        if (!started)
        {
            logger.warning("Self-heal: scheduler did not start ({}); every offset stays at its nominal",
                           started.error().message());
            return;
        }
        // Stamp the configured radius onto every template once, before any group is registered, so the work
        // callbacks below allocate nothing and never throw.
        const std::size_t window = heal_window();
        s_windowed.emplace(WindowedLandmarks{
            .entity = with_window(k_entity_lm, window),
            .animhuman = with_window(k_animhuman_lm, window),
            .actormodel = with_window(k_actormodel_lm, window),
            .missile = with_window(k_missile_lm, window),
            .animchar = with_window(k_animchar_lm, window),
            .actiongame = with_window(k_actiongame_lm, window),
            .localactor = with_window(k_localactor_lm, window),
            .manager = with_window(k_manager_lm, window),
            .minigame = with_window(k_minigame_subsystem_lm, window),
            .bracket = {with_window(k_player_top_bracket[0], window), with_window(k_player_top_bracket[1], window)},
        });

        s_scheduler.emplace(std::move(*started));
        DMK::rtti::HealScheduler &sched = *s_scheduler;
        RuntimeOffsets &offsets = runtime_offsets();
        const WindowedLandmarks &lm = *s_windowed;

        // Chain root: CCryAction -> CActionGame. Every actor walk reads CActionGame through this offset before
        // any C_Player-rooted group can run, so a shift here would defeat the whole chain. The gate keeps the
        // group SILENT while CActionGame does not exist yet: an unpopulated slot is the normal pre-session state
        // (main menu, a slow load), not a layout drift, so it must not spend the retry budget or log.
        sched.add_group(
            [&offsets, &lm](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t cry_action = s_cry_action_base.load(std::memory_order_relaxed);
                return heal_and_record(run, "actionGame", lm.actiongame, cry_action,
                                       offsets.ccryaction_actiongame);
            },
            [&offsets]() noexcept
            {
                const std::uintptr_t cry_action = s_cry_action_base.load(std::memory_order_relaxed);
                if (cry_action == 0)
                {
                    return false;
                }
                const auto action_game =
                    mem::read<std::uintptr_t>(Address{cry_action + offset_value(offsets.ccryaction_actiongame)});
                return action_game.has_value() && mem::is_plausible_ptr(Address{*action_game});
            });

        // CActionGame -> local actor. C_Player is found THROUGH this offset, so it cannot be healed from a
        // resolved C_Player; the gate instead waits for the resolver to report the drift signature (a populated
        // slot holding something that is not a C_Player).
        sched.add_group(
            [&offsets, &lm](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t action_game = s_action_game_base.load(std::memory_order_relaxed);
                return heal_and_record(run, "localActor", lm.localactor, action_game,
                                       offsets.cactiongame_local_actor);
            },
            []() noexcept
            {
                return s_action_game_base.load(std::memory_order_relaxed) != 0 &&
                       s_local_actor_recovery.load(std::memory_order_relaxed);
            });

        // C_Player-direct members whose type is UNIQUE within C_Player, so an independent window scan cannot land
        // on a wrong same-typed neighbour; they heal independently, which keeps each one resilient to a shift that
        // is not uniform across the struct. entity and lookController are handled by the corroborated bracket
        // instead. The caller only publishes a vtable-validated C_Player, so this group is deterministic on a
        // valid frame and normally latches after one pass.
        //
        // The latch is keyed on the members that AUTHORIZE A WRITE, not on the group having run: animatedHuman
        // is the first hop of the body-turn chain, and the bracket owns lookController. Latching regardless
        // would leave a member that missed its one scan Unverified forever, which now costs the feature rather
        // than merely falling back to the nominal. The read-only members (actorModel, missileController) do not
        // hold the group open; a settled bracket does latch even when it fell back, because it is deterministic
        // on a validated C_Player and a re-scan would reach the same verdict.
        sched.add_group(
            [&offsets, &lm](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t c_player = s_player_base.load(std::memory_order_relaxed);
                const bool animated_human_ok =
                    heal_and_record(run, "animatedHuman", lm.animhuman, c_player, offsets.c_player_animated_human);
                (void)heal_and_record(run, "actorModel", lm.actormodel, c_player, offsets.c_player_actor_model);
                (void)heal_and_record(run, "missileController", lm.missile, c_player,
                                      offsets.c_player_missile_controller);
                const bool bracket_settled = heal_player_bracket(run, c_player, offsets);
                return animated_human_ok && bracket_settled;
            },
            []() noexcept { return s_player_base.load(std::memory_order_relaxed) != 0; });

        // animChar lives one hop out, on C_AnimatedHuman, so it is its own group: a single shared latch would
        // freeze it at nominal forever on a frame where C_AnimatedHuman is briefly null. Returning false retries
        // on the next interval instead of abandoning the heal; the gate keeps the wait silent.
        sched.add_group(
            [&offsets, &lm](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t c_player = s_player_base.load(std::memory_order_relaxed);
                const auto anim_human =
                    mem::read<std::uintptr_t>(Address{c_player + offset_value(offsets.c_player_animated_human)});
                if (!anim_human || !mem::is_plausible_ptr(Address{*anim_human}))
                {
                    return false;
                }
                return heal_and_record(run, "animChar", lm.animchar, *anim_human, offsets.animated_human_animchar);
            },
            []() noexcept { return s_player_base.load(std::memory_order_relaxed) != 0; });

        // The two global-context members latch INDEPENDENTLY, so a frame where one is not yet live retries only
        // that member. The context base is anchored (not navigated through a possibly-drifted offset), so unlike
        // the local-actor offset a context-member drift IS recoverable here.
        sched.add_group(
            [&offsets, &lm](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t context = s_context_base.load(std::memory_order_relaxed);
                return heal_and_record(run, "cameraManager", lm.manager, context, offsets.context_manager);
            },
            []() noexcept { return s_context_base.load(std::memory_order_relaxed) != 0; });

        sched.add_group(
            [&offsets, &lm](DMK::rtti::HealRun &run) noexcept
            {
                const std::uintptr_t context = s_context_base.load(std::memory_order_relaxed);
                return heal_and_record(run, "minigameSubsystem", lm.minigame, context,
                                       offsets.context_minigame_subsystem);
            },
            []() noexcept { return s_context_base.load(std::memory_order_relaxed) != 0; });

        s_scheduler_ready.store(true, std::memory_order_release);
        logger.info("Self-heal: scheduler started ({} frame retry interval, 6 groups)",
                    k_heal_retry_interval_frames);
    }

    void offset_heal_tick() noexcept
    {
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

} // namespace TPVCamera
