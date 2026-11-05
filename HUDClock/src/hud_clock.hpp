/**
 * @file hud_clock.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef HUDCLOCK_HUD_CLOCK_HPP
#define HUDCLOCK_HUD_CLOCK_HPP

#include <DetourModKit.hpp>
#include <DetourModKit/abi/wheel_host.h>

#include <cstdint>
#include <span>

namespace HUDClock
{
    /**
     * @brief Initializes the mod: config, anchors, the toggle key, the frame hook and the INI auto-reload.
     * @param session The live Session.
     * @param wheel_host Resident wheel-host table of the dev loader, or nullptr, so a mouse-wheel toggle key keeps its
     *        keepalive in the loader.
     * @param persistent_slots The dev loader's process-lifetime slots, or an empty span. Unused, because the clock's
     *        element belongs to the game and every generation finds it by name.
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
        /// The hook retirement. Busy and Refused leave the hook armed, so a later shutdown() can retry it.
        DMK::hook::HookRetirement hooks{};
        /// False when a worker outside the hook still runs code in this image, so the image must stay mapped.
        bool complete{false};
    };

    /**
     * @brief Hides the clock, retires the frame hook, then forgets the anchors.
     * @details The main thread hides the clock at its next frame. When no frame runs within a short wait, the clock
     *          keeps its last text until a later generation takes it over.
     * @return The hook retirement and whether every worker joined.
     * @note Run off the loader lock. Idempotent.
     */
    [[nodiscard]] ShutdownResult shutdown() noexcept;
} // namespace HUDClock

#endif // HUDCLOCK_HUD_CLOCK_HPP
