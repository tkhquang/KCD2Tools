/**
 * @file hooks/combat_retarget.cpp
 * @brief The combat-player update detour that moves the lock-on after a kill (see combat_retarget.hpp).
 */

#include "hooks/combat_retarget.hpp"
#include "aob_resolver.hpp"
#include "combat_lock.hpp"
#include "constants.hpp"
#include "hooks/vtable_slots.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <atomic>
#include <optional>

namespace TPVCamera
{
    namespace
    {
        // C_CombatPlayer's update (vtable slot COMBAT_PLAYER_VTABLE_UPDATE_SLOT): (player /*rcx*/, float dt /*xmm1*/).
        // It runs the lock-on controller, then tail-calls the rest of the update. The result register is passed
        // through.
        using CombatPlayerUpdateFunc = std::uintptr_t(__fastcall *)(std::uintptr_t player, float dt);
        std::atomic<CombatPlayerUpdateFunc> s_update_original{nullptr};

        // The game's set-opponent: void(C_CombatPlayer *player /*rcx*/, CEntity *opponent /*rdx*/).
        using SetOpponentFunc = void(__fastcall *)(std::uintptr_t player, std::uintptr_t entity);
        std::atomic<SetOpponentFunc> s_set_opponent{nullptr};

        // The game's threat test: bool(source /*rcx*/, C_CombatActor *other /*rdx*/), see AnchorId::CombatThreat.
        using CombatThreatFunc = std::uint8_t(__fastcall *)(std::uintptr_t source, std::uintptr_t other);
        std::atomic<CombatThreatFunc> s_is_threat{nullptr};

        // The standing request, rewritten every camera frame: the actor the lock-on should move to (EntityId 0 = none)
        // and the game clock the kill's end-of-stance timer must not run out before while it stands. The detour checks
        // that the C_Human still carries the EntityId, so a pair read across a rewrite is refused, not mixed.
        std::atomic<std::uint32_t> s_requested{0};
        std::atomic<std::uintptr_t> s_requested_human{0};
        std::atomic<std::int64_t> s_hold_end_until{0};
        // The EntityId last set as the combat target (0 = none since the last read).
        std::atomic<std::uint32_t> s_done{0};
        // The EntityId last refused because he fails the game's threat test (0 = none since the last read).
        std::atomic<std::uint32_t> s_refused{0};
        // The CEntity the detour last set as the combat target, 0 when none. Only the detour touches it, and the game
        // runs the combat player's update on one thread at a time. The atomic keeps a job-thread hand-over well
        // defined.
        std::atomic<std::uintptr_t> s_target_entity{0};

        /**
         * @brief Combat-player update detour: the original runs first, then the standing request is served.
         * @details Right after the update the controller has finished its own pick for the frame, and the call happens
         *          on the thread that runs the controller's own set-opponent calls, so the move is one more call of the
         *          kind the controller makes itself. The call sets the combat target, and the game's own commit moves
         *          the lock-on to it once the attack under way ends, so the kill's end-of-stance timer is held off until
         *          then. The request is served only while the target is still the dead opponent, none, or the one the
         *          detour set: any other target is the player's choice (Tab, the next/previous opponent keys) or the
         *          game's own pick, and the detour leaves it alone. The requested actor must pass the game's threat test,
         *          as every pick of the controller's own does: the stance outlives the delay after a kill only while its
         *          opponent passes it, so a move to a listed hostile who is not fighting the player ends the stance on
         *          the next update. One who fails is refused (take_combat_retarget_refused) and leaves the target as it
         *          is, so a valid one set before stays while the camera picks again. A target the detour set goes back to
         *          the dead opponent when the request drops before the game takes it, so the stance ends as without the
         *          mod. The threat test runs here because the controller runs it on this thread too. No-throw: every
         *          read is guarded and nothing allocates.
         */
        std::uintptr_t __fastcall detour_combat_player_update(std::uintptr_t player, float dt) noexcept
        {
            const CombatPlayerUpdateFunc original = s_update_original.load(std::memory_order_acquire);
            const std::uintptr_t result = original != nullptr ? original(player, dt) : 0;
            const SetOpponentFunc set_opponent = s_set_opponent.load(std::memory_order_acquire);
            const CombatThreatFunc is_threat = s_is_threat.load(std::memory_order_acquire);
            if (set_opponent == nullptr || is_threat == nullptr)
            {
                return result;
            }
            const std::uint32_t id = s_requested.load(std::memory_order_acquire);
            const std::uintptr_t human = id != 0 ? s_requested_human.load(std::memory_order_relaxed) : 0;
            const std::optional<CombatRetarget> now = read_combat_retarget(human, id);
            const std::uintptr_t ours = s_target_entity.load(std::memory_order_relaxed);
            if (!now.has_value() || now->combat_player != player ||
                (now->target != 0 && now->target != now->held && now->target != ours))
            {
                s_target_entity.store(0, std::memory_order_relaxed);
                return result;
            }
            if (now->next != 0)
            {
                if (now->threat_source != 0 && is_threat(now->threat_source, now->next_combat_actor) != 0)
                {
                    set_opponent(player, now->next);
                    (void)defer_kill_end_timer(s_hold_end_until.load(std::memory_order_relaxed));
                    s_target_entity.store(now->next, std::memory_order_relaxed);
                    s_done.store(id, std::memory_order_release);
                }
                else
                {
                    s_refused.store(id, std::memory_order_release);
                }
            }
            else if (ours != 0)
            {
                if (now->target == ours)
                {
                    set_opponent(player, now->held);
                }
                s_target_entity.store(0, std::memory_order_relaxed);
            }
            return result;
        }
    } // namespace

    void install_combat_retarget_hook(std::uintptr_t module_base, std::size_t module_size, DMK::hook::HookStack &hooks)
    {
        DMK::Logger &logger = DMK::log();
        const std::uintptr_t wrapper = gated_anchor_address(Feature::CombatRetarget, AnchorId::SetCombatOpponent);
        const std::uintptr_t threat = gated_anchor_address(Feature::CombatRetarget, AnchorId::CombatThreat);
        if (wrapper == 0 || threat == 0)
        {
            logger.warning("Combat: set-opponent or the threat test not found; after a kill the lock-on waits for the "
                           "game to move on");
            return;
        }
        // Every rung ends in the wrapper's call. Its target must still be the setter that builds the combat target.
        const DMK::Region image{DMK::Address{module_base}, module_size};
        const auto setter =
            DMK::scan::resolve_rip_relative(DMK::Address{wrapper + Aob::k_setCombatOpponentCallSite}, 1, 5);
        if (!setter || !(code_window_matches(setter->raw(), Aob::k_setCombatOpponentSetterBody,
                                             Aob::k_setCombatOpponentSetterBody.max_match_length(), image) ||
                         code_window_matches(setter->raw(), Aob::k_setCombatOpponentSetterTargetLoad,
                                             Aob::k_setCombatOpponentSetterTargetLoadWindow, image)))
        {
            logger.warning("Combat: the set-opponent call did not prove out; after a kill the lock-on waits for the "
                           "game to move on");
            return;
        }
        const std::optional<std::uintptr_t> vtable = class_vtable(GameClass::CombatPlayer);
        if (!vtable.has_value())
        {
            logger.warning("Combat: C_CombatPlayer did not resolve; after a kill the lock-on waits for the game to "
                           "move on");
            return;
        }
        std::uintptr_t target = read_checked_vtable_slot(
            *vtable, Constants::COMBAT_PLAYER_VTABLE_UPDATE_SLOT, Aob::k_combatPlayerUpdateBody,
            Aob::k_combatPlayerUpdateBody.max_match_length(), module_base, module_size);
        if (target == 0)
        {
            target = read_checked_vtable_slot(*vtable, Constants::COMBAT_PLAYER_VTABLE_UPDATE_SLOT,
                                              Aob::k_combatPlayerUpdateController,
                                              Aob::k_combatPlayerUpdateControllerWindow, module_base, module_size);
        }
        if (target == 0)
        {
            logger.warning("Combat: C_CombatPlayer slot {} is not the update that runs the lock-on; after a kill the "
                           "lock-on waits for the game to move on",
                           Constants::COMBAT_PLAYER_VTABLE_UPDATE_SLOT);
            return;
        }

        auto result = DMK::hook::inline_at<&detour_combat_player_update>(DMK::hook::InlineRequest{
            .name = "CombatPlayerUpdate",
            .target = DMK::Address{target},
        });
        if (!result.has_value())
        {
            logger.warning("Combat: combat-player update hook failed ({}); after a kill the lock-on waits for the game "
                           "to move on",
                           result.error().message());
            return;
        }
        // Publish the trampoline and the call before enable() arms the patch, so the detour never sees a null original.
        s_set_opponent.store(reinterpret_cast<SetOpponentFunc>(wrapper), std::memory_order_release);
        s_is_threat.store(reinterpret_cast<CombatThreatFunc>(threat), std::memory_order_release);
        s_update_original.store(result->original<CombatPlayerUpdateFunc>(), std::memory_order_release);
        const auto armed = hooks.push(std::move(*result)).enable();
        if (!armed.has_value())
        {
            s_set_opponent.store(nullptr, std::memory_order_release);
            s_is_threat.store(nullptr, std::memory_order_release);
            logger.warning("Combat: combat-player update hook could not be armed ({}); after a kill the lock-on waits "
                           "for the game to move on",
                           armed.error().message());
            return;
        }
        logger.info("Combat: after a kill the lock-on can move to the hostile in view (C_CombatPlayer slot {} at {}, "
                    "set-opponent at {}, threat test at {})",
                    Constants::COMBAT_PLAYER_VTABLE_UPDATE_SLOT, DMK::format::format_address(target),
                    DMK::format::format_address(wrapper), DMK::format::format_address(threat));
    }

    void request_combat_retarget(std::uintptr_t human, std::uint32_t id, std::int64_t hold_end_until) noexcept
    {
        s_hold_end_until.store(id != 0 ? hold_end_until : 0, std::memory_order_relaxed);
        s_requested_human.store(id != 0 ? human : 0, std::memory_order_relaxed);
        s_requested.store(id, std::memory_order_release);
    }

    std::uint32_t take_combat_retarget_done() noexcept
    {
        return s_done.exchange(0, std::memory_order_acq_rel);
    }

    std::uint32_t take_combat_retarget_refused() noexcept
    {
        return s_refused.exchange(0, std::memory_order_acq_rel);
    }
} // namespace TPVCamera
