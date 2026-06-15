/**
 * @file offset_heal.cpp
 * @brief Landmark tables and heal logic backing the runtime offset cache.
 */

#include "offset_heal.hpp"
#include "config.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
            return std::min(static_cast<std::size_t>(configured), DMK::Rtti::MAX_HEAL_WINDOW);
        }

        // Self-heal landmarks: "at this nominal offset within the struct there is a slot referring to an object of
        // this mangled type." Each is keyed on a type that is stable across patches (an engine/base type or an
        // already-trusted concrete type), per the rtti_dissect guidance. base is filled at call time. The
        // indirection records the slot SHAPE: every member here is a pointer-to-object EXCEPT the missile
        // controller, which is constructed in-place inside C_Player (its first qword is the vtable), so it is a
        // direct object base matched with CompleteObject (see k_missile_lm for why CompleteObject, not ObjectBase).
        constexpr DMK::Rtti::Landmark k_entity_lm{.nominal_offset = Constants::C_PLAYER_ENTITY_OFFSET,
                                                  .expected_mangled = Constants::C_ENTITY_RTTI_NAME,
                                                  .indirection = DMK::Rtti::Indirection::PointerToObject};
        constexpr DMK::Rtti::Landmark k_animhuman_lm{.nominal_offset = Constants::C_PLAYER_ANIMATED_HUMAN_OFFSET,
                                                     .expected_mangled = Constants::C_ANIMATED_HUMAN_RTTI_NAME,
                                                     .indirection = DMK::Rtti::Indirection::PointerToObject};
        constexpr DMK::Rtti::Landmark k_actormodel_lm{.nominal_offset = Constants::C_PLAYER_ACTOR_MODEL_OFFSET,
                                                      .expected_mangled = Constants::C_ACTOR_MODEL_RTTI_NAME,
                                                      .indirection = DMK::Rtti::Indirection::PointerToObject};
        constexpr DMK::Rtti::Landmark k_missile_lm{
            .nominal_offset = Constants::C_PLAYER_MISSILE_CONTROLLER_OFFSET,
            .expected_mangled = Constants::C_MISSILE_CONTROLLER_RTTI_NAME,
            // Embedded object, not a pointer. CompleteObject matches only the primary subobject (COL.offset == 0),
            // where ObjectBase would match any subobject: under multiple inheritance every base carries its own
            // vtable and each vtable's COL names the same most-derived type, so a window scan keyed on ObjectBase
            // could latch a secondary base and heal to an offset shifted by that subobject delta (a silent,
            // confidence-full off-by-a-subobject heal). The embedded member's nominal slot already holds the primary
            // vtable, so CompleteObject only adds the MI guard for the drift scan and never changes the nominal match.
            .indirection = DMK::Rtti::Indirection::CompleteObject};
        constexpr DMK::Rtti::Landmark k_animchar_lm{.nominal_offset = Constants::ANIMATED_HUMAN_ANIMCHAR_OFFSET,
                                                    .expected_mangled = Constants::ANIMATED_CHARACTER_RTTI_NAME,
                                                    .indirection = DMK::Rtti::Indirection::PointerToObject};
        constexpr DMK::Rtti::Landmark k_localactor_lm{.nominal_offset = Constants::CACTIONGAME_LOCAL_ACTOR_OFFSET,
                                                      .expected_mangled = Constants::C_PLAYER_RTTI_NAME,
                                                      .indirection = DMK::Rtti::Indirection::PointerToObject};
        constexpr DMK::Rtti::Landmark k_manager_lm{.nominal_offset = Constants::OFFSET_MANAGER_PTR_STORAGE,
                                                   .expected_mangled = Constants::C_CAMERA_MANAGER_RTTI_NAME,
                                                   .indirection = DMK::Rtti::Indirection::PointerToObject};
        constexpr DMK::Rtti::Landmark k_minigame_subsystem_lm{.nominal_offset = Constants::OFFSET_MINIGAME_SUBSYSTEM,
                                                              .expected_mangled = Constants::C_PLAYER_MODULE_RTTI_NAME,
                                                              .indirection = DMK::Rtti::Indirection::PointerToObject};

        constexpr DMK::Rtti::Landmark k_hitdeath_lm{.nominal_offset = Constants::C_PLAYER_HIT_DEATH_REACTIONS_OFFSET,
                                                    .expected_mangled = Constants::C_HIT_DEATH_REACTIONS_RTTI_NAME,
                                                    .indirection = DMK::Rtti::Indirection::PointerToObject};

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
        constexpr std::array<DMK::Rtti::Landmark, 2> k_player_top_bracket{{k_entity_lm, k_hitdeath_lm}};

        /**
         * @brief Emits one process-wide Warning the first time any offset is found to have drifted.
         * @details The per-landmark "moved" lines are logged at Info (a recovery is a success, not a fault). This
         *          single Warning is the actionable headline: a drift means a game update reshaped a struct, and
         *          while the RTTI-typed POINTER offsets here auto-recovered, the NON-healable scalar/flag offsets in
         *          the same structs (look pitch/yaw, stance, the missile aim byte, hide-head, the CryEngine struct
         *          layouts) cannot self-heal and silently ride the same shift, so they need a human to re-verify.
         *          CAS one-shot, and callers invoke it BEFORE the first moved Info line, so it fires exactly once and
         *          reads as a header above the per-offset lines rather than interleaving with them.
         */
        void warn_layout_drift_once() noexcept
        {
            static std::atomic<bool> s_warned{false};
            bool expected = false;
            if (s_warned.compare_exchange_strong(expected, true, std::memory_order_relaxed))
            {
                DMK::Logger::get_instance().warning(
                    "Self-heal: layout drifted; pointer offsets recovered (below). Re-verify non-healable scalars "
                    "(pitch/yaw, stance, aim flag).");
            }
        }

        /**
         * @brief Heals one landmark from a resolved base and writes its cache slot, logging the outcome.
         * @details Copies the constexpr template, fills its base, and runs heal_landmark. On a resolve the cache
         *          slot takes the healed offset (which equals the nominal when nothing drifted, via the nominal
         *          short-circuit); on a miss the slot keeps its current (nominal) value. A non-drift heal logs at
         *          Debug; a recovered shift at Info; an unresolved REQUIRED landmark at Warning (it should have
         *          been present, so a miss flags a layout the heal could not recover). An unresolved OPTIONAL
         *          landmark (one that may legitimately not resolve in some game state) logs at Debug, since a miss
         *          there is not necessarily drift.
         * @return true when the landmark resolved (healed or confirmed at nominal), false on a miss.
         */
        [[nodiscard]] bool heal_one(std::string_view label, const DMK::Rtti::Landmark &tmpl, std::uintptr_t base,
                                    std::atomic<std::ptrdiff_t> &slot, bool optional) noexcept
        {
            DMK::Rtti::Landmark lm = tmpl;
            lm.base = base;
            lm.window = heal_window(); // override the per-struct default with the single tunable radius above
            DMK::Logger &logger = DMK::Logger::get_instance();
            const auto result = DMK::Rtti::heal_landmark(lm);
            if (result)
            {
                slot.store(result->healed_offset, std::memory_order_relaxed);
                if (result->healed_offset != tmpl.nominal_offset)
                {
                    // Header first: emit the one drift Warning before the first moved line so it groups ABOVE the
                    // per-offset Info lines instead of interleaving (CAS one-shot, so later moves do not re-emit).
                    warn_layout_drift_once();
                    logger.info("Self-heal: {} moved {:+#x} ({:#x} -> {:#x})", label,
                                result->healed_offset - tmpl.nominal_offset, tmpl.nominal_offset,
                                result->healed_offset);
                }
                else
                {
                    logger.debug("Self-heal: {} confirmed at nominal {:#x}", label, tmpl.nominal_offset);
                }
                return true;
            }
            const std::string_view reason = DMK::Rtti::heal_error_to_string(result.error());
            if (optional)
            {
                logger.debug("Self-heal: {} not resolvable now ({}); keeping nominal {:#x}", label, reason,
                             tmpl.nominal_offset);
            }
            else
            {
                logger.warning("Self-heal: {} unresolved ({}); kept nominal {:#x} (re-author if drifted)", label,
                               reason, tmpl.nominal_offset);
            }
            return false;
        }

    } // namespace

    RuntimeOffsets &runtime_offsets() noexcept
    {
        static RuntimeOffsets offsets;
        return offsets;
    }

    void heal_player_offsets(std::uintptr_t c_player) noexcept
    {
        // One-shot: the first call on a validated C_Player heals; later calls return immediately. The heals run
        // on the render thread inside the already-SEH-guarded resolve path, so a single guard is sufficient.
        static std::atomic<bool> s_done{false};
        bool expected = false;
        if (!s_done.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        {
            return;
        }

        RuntimeOffsets &offsets = runtime_offsets();
        // These members key on types that are UNIQUE within C_Player, so an independent window scan cannot land
        // on a wrong same-typed neighbour; they heal independently, which keeps each one resilient to a shift
        // that is not uniform across the struct. (entity is handled by the corroborated bracket below instead:
        // CEntity is a common type, so an independent scan would be decoy-prone.) The missile controller is
        // embedded in C_Player (constructed in its ctor), so its RTTI is normally resolvable at all times and it
        // heals like the rest; it is passed optional only defensively, so any weapon/inventory state that left
        // its slot unresolvable degrades to a Debug note rather than a spurious drift Warning.
        (void)heal_one("animatedHuman", k_animhuman_lm, c_player, offsets.c_player_animated_human, false);
        (void)heal_one("actorModel", k_actormodel_lm, c_player, offsets.c_player_actor_model, false);
        (void)heal_one("missileController", k_missile_lm, c_player, offsets.c_player_missile_controller, true);

        // animChar lives one hop out, on C_AnimatedHuman, so resolve that pointer through the (now healed)
        // animated-human offset before healing it. A missing C_AnimatedHuman just leaves the nominal in place.
        DMK::Logger &logger = DMK::Logger::get_instance();
        const auto anim_human = DMK::Memory::seh_read<std::uintptr_t>(
            c_player + offsets.c_player_animated_human.load(std::memory_order_relaxed));
        if (anim_human && DMK::Memory::plausible_userspace_ptr(*anim_human))
        {
            (void)heal_one("animChar", k_animchar_lm, *anim_human, offsets.animated_human_animchar, false);
        }
        else
        {
            logger.debug("Self-heal: animChar skipped (C_AnimatedHuman not resolvable); keeping nominal {:#x}",
                         Constants::ANIMATED_HUMAN_ANIMCHAR_OFFSET);
        }

        // Corroborated bracket: recover BOTH the entity pointer (common type, decoy-prone for a blind scan) and
        // the look controller (no RTTI of its own) from the single top-of-struct delta that entity and
        // HitDeathReactions both agree on (see k_player_top_bracket). Each rides the delta on success; a
        // non-uniform shift fails the solve and both stay nominal (fail-closed).
        const auto fit = DMK::Rtti::solve_fingerprint(c_player, k_player_top_bracket, heal_window());
        if (fit)
        {
            const std::ptrdiff_t entity_healed = Constants::C_PLAYER_ENTITY_OFFSET + fit->delta;
            const std::ptrdiff_t look_healed = Constants::C_PLAYER_LOOK_CONTROLLER_OFFSET + fit->delta;
            offsets.c_player_entity.store(entity_healed, std::memory_order_relaxed);
            offsets.c_player_look_controller.store(look_healed, std::memory_order_relaxed);
            if (fit->delta != 0)
            {
                warn_layout_drift_once(); // header before the moved lines (see heal_one)
                logger.info("Self-heal: entity moved {:+#x} ({:#x} -> {:#x})", fit->delta,
                            Constants::C_PLAYER_ENTITY_OFFSET, entity_healed);
                logger.info("Self-heal: lookController moved {:+#x} ({:#x} -> {:#x})", fit->delta,
                            Constants::C_PLAYER_LOOK_CONTROLLER_OFFSET, look_healed);
            }
            else
            {
                logger.debug("Self-heal: entity + lookController confirmed at nominal (entity {:#x}, "
                             "lookController {:#x})",
                             Constants::C_PLAYER_ENTITY_OFFSET, Constants::C_PLAYER_LOOK_CONTROLLER_OFFSET);
            }
        }
        else
        {
            // Bracket disagreed (non-uniform shift across the span). lookController has no RTTI of its own, so
            // it cannot be recovered independently and stays nominal. entity CAN still be scanned for by type as
            // a LAST RESORT: this reintroduces the decoy risk the corroborated solve avoided (a wrong same-typed
            // CEntity neighbour could be picked), but a best-effort offset beats a guaranteed-stale one when the
            // bracket has already failed, so try it and warn that the result is unverified.
            logger.warning("Self-heal: bracket unresolved ({}); lookController kept nominal; entity via "
                           "uncorroborated scan (decoy risk)",
                           DMK::Rtti::heal_error_to_string(fit.error()));
            (void)heal_one("entity (uncorroborated fallback)", k_entity_lm, c_player, offsets.c_player_entity, false);
        }
    }

    std::ptrdiff_t heal_local_actor_offset(std::uintptr_t action_game) noexcept
    {
        RuntimeOffsets &offsets = runtime_offsets();
        (void)heal_one("localActor", k_localactor_lm, action_game, offsets.cactiongame_local_actor, false);
        return offsets.cactiongame_local_actor.load(std::memory_order_relaxed);
    }

    void heal_context_offsets(std::uintptr_t context) noexcept
    {
        // Each context member latches INDEPENDENTLY once its OWN heal resolves, so a rare frame where one member
        // is not yet live retries only that member -- it never freezes the other at nominal (a single shared
        // latch keyed on the manager would freeze the minigame member if it missed the latching frame) and never
        // rescans a member that already healed. The context base is anchored, so unlike the local-actor offset a
        // context-member drift IS recoverable here (the scan does not navigate through the offset it is healing).
        // The caller gates this on the world being ready, when both subsystems are live.
        static std::atomic<bool> s_manager_done{false};
        static std::atomic<bool> s_minigame_done{false};
        RuntimeOffsets &offsets = runtime_offsets();
        if (!s_manager_done.load(std::memory_order_relaxed) &&
            heal_one("cameraManager", k_manager_lm, context, offsets.context_manager, false))
        {
            s_manager_done.store(true, std::memory_order_relaxed);
        }
        if (!s_minigame_done.load(std::memory_order_relaxed) &&
            heal_one("minigameSubsystem", k_minigame_subsystem_lm, context, offsets.context_minigame_subsystem, false))
        {
            s_minigame_done.store(true, std::memory_order_relaxed);
        }
    }

} // namespace TPVCamera
