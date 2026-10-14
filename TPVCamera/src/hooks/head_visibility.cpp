/**
 * @file hooks/head_visibility.cpp
 * @brief The head-visibility setter detour and the per-frame head re-assert (see head_visibility.hpp).
 */

#include "hooks/head_visibility.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "hooks/camera_hook.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace TPVCamera
{
    namespace
    {
        // Head-visibility setter: SetHeadHidden(this /*rcx*/, bool hide_head /*dl*/, char flags /*r8b*/).
        // hide_head == false keeps the head rendered.
        using SetHeadVisibilityFunc = void(__fastcall *)(uintptr_t entity, bool hide_head, char flags);
        std::atomic<SetHeadVisibilityFunc> s_set_head_visibility_original{nullptr};

        // Latched from the head-visibility setter (or adopted from the player's stored setter
        // arguments, see adopt_head_state) so the per-frame re-assert can drive the head without
        // waiting for the game to call the setter again (toggling the offset does not make the
        // game call it, so without the re-assert the head would stay hidden after an FPV/TPV
        // switch). The setter only ever runs for the player (it toggles the FirstPersonView
        // rig), so the entity is the player. s_head_was_active tracks the offset's active state
        // across frames so the head is restored to the game's intended value exactly once when
        // the offset turns off.
        std::atomic<uintptr_t> s_head_entity{0};
        std::atomic<uint8_t> s_head_flags{0};
        std::atomic<bool> s_game_intended_hide_head{false};
        std::atomic<bool> s_head_was_active{false};

        // First-person fallback, published by the offset each frame: true while the camera has no room behind the
        // player (the arm is shorter than HeadClearance) and sits at the eye, so the head must be hidden like in first
        // person or the view would be inside it. Read by the head re-assert and the head-visibility detour.
        std::atomic<bool> s_head_fallback{false};
        // Set while the close-up fade has the character mostly dithered out and the eye fade is not ready, since the
        // stock Eye shader ignores the dissolve, or with it ready once his face is gone, since the eyelashes and the
        // eye film ignore it too: the head is hidden like in first person. Read with s_head_fallback by the head
        // re-assert and the head-visibility detour; unlike it, it leaves the camera where it is.
        std::atomic<bool> s_head_close_fade{false};

        /**
         * @brief Calls the head-visibility setter on the player entity.
         * @details Invokes the original (the trampoline, so this does not re-enter our detour).
         *          Faults are caught by the frustum detour's SEH wrapper this runs under. Runs on
         *          the view-update thread, the same thread the engine drives the setter on.
         * @param entity Player entity (C_Player).
         * @param hide_head Desired hide state (false = head shown).
         * @param flags The setter's third argument.
         */
        void call_head_visibility(uintptr_t entity, bool hide_head, uint8_t flags)
        {
            if (const SetHeadVisibilityFunc original = s_set_head_visibility_original.load(std::memory_order_acquire))
            {
                original(entity, hide_head, static_cast<char>(flags));
            }
        }

        /**
         * @brief Latches @p player as if the game had just called the head-visibility setter on it.
         * @details The setter stores its hide value at OFFSET_ENTITY_HIDE_HEAD_FLAG and its third
         *          argument at OFFSET_ENTITY_HIDE_HEAD_ARG, and the game re-applies the head from
         *          exactly those two fields. A player the detour has not seen a call on (a new level,
         *          or a freshly loaded generation) is adopted from them before the re-assert first
         *          changes the head. Turning the offset off then restores what the game applied, not
         *          the default of a never-latched state, which would show the head in first person.
         * @param player The live C_Player.
         * @return False when either field cannot be read; nothing is latched then.
         */
        bool adopt_head_state(uintptr_t player)
        {
            const auto hide_head =
                DMK::memory::read<uint8_t>(DMK::Address{player + Constants::OFFSET_ENTITY_HIDE_HEAD_FLAG});
            const auto flags =
                DMK::memory::read<uint8_t>(DMK::Address{player + Constants::OFFSET_ENTITY_HIDE_HEAD_ARG});
            if (!hide_head || !flags)
            {
                return false;
            }
            s_head_entity.store(player, std::memory_order_relaxed);
            s_head_flags.store(*flags, std::memory_order_relaxed);
            s_game_intended_hide_head.store(*hide_head != 0, std::memory_order_relaxed);
            return true;
        }

        /**
         * @brief Runs the head-visibility latch and the original setter under SEH.
         * @details Apart from the detour, because the detour's DetourScope needs C++ object unwinding, which a __try
         *          frame cannot hold.
         */
        void set_head_visibility_guarded(SetHeadVisibilityFunc original, uintptr_t entity, bool hide_head,
                                         char flags) noexcept
        {
            __try
            {
                // Latch the player entity, the flags, and the game's intended hide value so
                // the per-frame re-assert can keep the head shown while the offset is active
                // and restore the game's value when it turns off. This is the player because
                // the setter only toggles the FirstPersonView rig.
                s_head_entity.store(entity, std::memory_order_relaxed);
                s_head_flags.store(static_cast<uint8_t>(flags), std::memory_order_relaxed);
                s_game_intended_hide_head.store(hide_head, std::memory_order_relaxed);

                // Mirror the head to the offset's effective active state (published by the frustum
                // detour) rather than the raw toggle, so a forced-FPV/TPV state shows or hides the head
                // to match the view the player actually sees. The first-person fallback (camera at the
                // eye, no room behind) hides it like first person does, and so does a close-up fade far
                // enough along that the eyes would float in the dithered face (when the eye fade is not
                // ready), or the eyelashes and eye film once the face is gone (when it is).
                const bool active = camera_offset_active();
                const bool final_hide_head = active ? (s_head_fallback.load(std::memory_order_relaxed) ||
                                                       s_head_close_fade.load(std::memory_order_relaxed))
                                                    : hide_head;
                original(entity, final_hide_head, flags);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // Swallow: a layout change in the head setter must never crash the game.
            }
        }

        /**
         * @brief Head-visibility detour: keep the player head while the offset is rendering.
         * @details The first-person rig hides the head so it does not clip the eye camera.
         *          While the third-person view is active, force hide_head to false
         *          so the player is not headless from behind. Otherwise pass the game's
         *          intended value through unchanged.
         */
        void __fastcall detour_set_head_visibility(uintptr_t entity, bool hide_head, char flags) noexcept
        {
            const DetourScope in_flight;
            if (const SetHeadVisibilityFunc original = s_set_head_visibility_original.load(std::memory_order_acquire))
            {
                set_head_visibility_guarded(original, entity, hide_head, flags);
            }
        }

    } // namespace

    void install_head_visibility_hook(HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();
        // The head-visibility hook is best-effort: if it fails the camera still works,
        // the player just appears headless from behind, so a miss is a warning.
        const uintptr_t head_addr = gated_anchor_address(Feature::HeadVisibility, AnchorId::HeadVisibility);
        if (head_addr == 0)
        {
            logger.warning("Camera: Head visibility cascade unresolved; player may appear headless from behind");
        }
        else
        {
            auto head_result = DMK::hook::inline_at(
                DMK::hook::InlineRequest{.name = "SetHeadVisibility", .target = DMK::Address{head_addr}},
                detour_set_head_visibility);

            if (!head_result.has_value())
            {
                logger.warning("Camera: Head visibility hook failed ({}); player may appear headless from behind",
                               head_result.error().message());
            }
            else
            {
                s_set_head_visibility_original.store(head_result->original<SetHeadVisibilityFunc>(),
                                                     std::memory_order_release);
                if (auto armed = hooks.push(std::move(*head_result)).enable(); !armed.has_value())
                {
                    logger.warning("Camera: Head visibility hook could not be armed ({}); player may appear "
                                   "headless from behind",
                                   armed.error().message());
                }
            }
        }
    }

    void reassert_head_visibility(bool active, uintptr_t c_player)
    {
        const uintptr_t latched = s_head_entity.load(std::memory_order_relaxed);
        const uintptr_t entity = c_player != 0 ? c_player : latched;
        if (entity != 0 && DMK::memory::is_plausible_ptr(DMK::Address{entity}) &&
            (entity == latched || adopt_head_state(entity)))
        {
            const uint8_t flags = s_head_flags.load(std::memory_order_relaxed);
            if (active)
            {
                const bool want_hidden = s_head_fallback.load(std::memory_order_relaxed) ||
                                         s_head_close_fade.load(std::memory_order_relaxed);
                const auto hide_flag =
                    DMK::memory::read<uint8_t>(DMK::Address{entity + Constants::OFFSET_ENTITY_HIDE_HEAD_FLAG});
                if (hide_flag && (*hide_flag != 0) != want_hidden)
                {
                    call_head_visibility(entity, want_hidden, flags);
                }
            }
            else if (s_head_was_active.load(std::memory_order_relaxed))
            {
                call_head_visibility(entity, s_game_intended_hide_head.load(std::memory_order_relaxed), flags);
            }
        }
        s_head_was_active.store(active, std::memory_order_relaxed);
    }

    bool head_was_active() noexcept
    {
        return s_head_was_active.load(std::memory_order_relaxed);
    }

    void set_head_fallback(bool hidden) noexcept
    {
        s_head_fallback.store(hidden, std::memory_order_relaxed);
    }

    bool head_fallback() noexcept
    {
        return s_head_fallback.load(std::memory_order_relaxed);
    }

    void set_head_close_fade(bool hidden) noexcept
    {
        s_head_close_fade.store(hidden, std::memory_order_relaxed);
    }

    bool head_close_fade() noexcept
    {
        return s_head_close_fade.load(std::memory_order_relaxed);
    }
} // namespace TPVCamera
