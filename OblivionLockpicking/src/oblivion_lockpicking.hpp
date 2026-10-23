/**
 * @file oblivion_lockpicking.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef OBLIVIONLP_OBLIVION_LOCKPICKING_HPP
#define OBLIVIONLP_OBLIVION_LOCKPICKING_HPP

#include <DetourModKit.hpp>
#include <DetourModKit/abi/wheel_host.h>

#include <cstdint>
#include <span>

namespace OblivionLockpicking
{
    /**
     * @brief Initializes the mod: config, anchors, the hooks and the INI auto-reload.
     * @param session The live Session.
     * @param wheel_host Resident wheel-host table of the dev loader, or nullptr. Unused, because the mod binds no
     *        hotkey.
     * @param persistent_slots The dev loader's process-lifetime slots, or an empty span. Unused, because nothing the
     *        mod creates outlives a generation.
     * @return An empty Result on success, or the Error of the failed mandatory step (already logged).
     * @note Runs off the loader lock. Each host calls shutdown() before it releases the image.
     */
    [[nodiscard]] DMK::Result<void> init(
        DMK::Session &session,
        const WheelHostTable *wheel_host = nullptr,
        std::span<std::uint64_t> persistent_slots = {}
    );

    /** @brief The outcome of @ref shutdown. */
    struct ShutdownResult
    {
        /// The hook retirement. Busy and Refused leave the hooks armed, so a later shutdown() can retry it.
        DMK::hook::HookRetirement hooks{};
        /// False when a worker outside the hooks still runs code in this image, so the image must stay mapped.
        bool complete{false};
    };

    /**
     * @brief Retires the hooks, then forgets the anchors.
     * @details While the input listener is attached, the call only asks the main thread to detach it and reports
     *          Busy, so the caller retries. A session in progress continues as the vanilla minigame from its next
     *          frame. A vanilla lock element that the tumbler lock hid stays hidden for the rest of that one session,
     *          because the element belongs to the session and can already be gone. The next session starts visible.
     * @return The hook retirement and whether every worker joined.
     * @note Run off the loader lock. Idempotent.
     */
    [[nodiscard]] ShutdownResult shutdown() noexcept;
} // namespace OblivionLockpicking

#endif // OBLIVIONLP_OBLIVION_LOCKPICKING_HPP
