/**
 * @file hooks/lockpicking_hook.hpp
 * @brief The C_LockPicking::Update detour that runs the tumbler lock inside the vanilla session.
 */
#ifndef OBLIVIONLP_HOOKS_LOCKPICKING_HOOK_HPP
#define OBLIVIONLP_HOOKS_LOCKPICKING_HOOK_HPP

#include <DetourModKit.hpp>

#include <cstdint>

namespace OblivionLockpicking
{
    /**
     * @brief Hooks C_LockPicking::Update at @p update and arms it.
     * @details The hook only forwards to the original for a session that is not the player's. While the mod is
     *          disabled in the INI, it also gives the vanilla lock its view back.
     * @param hooks The stack that owns the hook through teardown.
     * @param update The resolved Update entry.
     * @return An empty value, or the install error.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] DMK::Result<void> install_lockpicking_hook(DMK::hook::HookStack &hooks, std::uintptr_t update);

    /**
     * @brief Hooks the lockpicking HUD message handler at @p handler, so a wrong set of the tumbler lock does not show
     *        "Lockpicking attempt failed": the attempt goes on, only a pick broke (or the break roll kept it).
     * @details Every other lockpicking message, and the failure text of a vanilla minigame, passes through unchanged.
     * @return An empty value, or the install error.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] DMK::Result<void> install_hud_message_hook(DMK::hook::HookStack &hooks, std::uintptr_t handler);

    /** @brief Forgets the session of the detour. Call after the hook retired. */
    void reset_lockpicking_session() noexcept;
} // namespace OblivionLockpicking

#endif // OBLIVIONLP_HOOKS_LOCKPICKING_HOOK_HPP
