/**
 * @file engine_tweaks.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef ENGINETWEAKS_ENGINE_TWEAKS_HPP
#define ENGINETWEAKS_ENGINE_TWEAKS_HPP

#include <DetourModKit.hpp>
#include <DetourModKit/abi/wheel_host.h>

#include <cstdint>
#include <span>

namespace EngineTweaks
{
    /**
     * @brief Initializes the mod: config, anchors, the hooks of every available fix and the INI auto-reload.
     * @param session The live Session.
     * @param wheel_host Resident wheel-host table of the dev loader, or nullptr. Unused: the mod binds no key.
     * @param persistent_slots The dev loader's process-lifetime slots, or an empty span. Unused: every generation
     *        builds its state from the game's own data.
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
        /** @brief The hook retirement. Busy and Refused leave the hooks armed, so a later shutdown() can retry it. */
        DMK::hook::HookRetirement hooks{};
        /// False when a worker outside the hooks still runs code in this image, so the image must stay mapped.
        bool complete{false};
    };

    /**
     * @brief Retires every hook, then drops the link index and the shortened sleep prediction.
     * @return The hook retirement and whether every worker joined.
     * @note Run off the loader lock. Idempotent.
     */
    [[nodiscard]] ShutdownResult shutdown() noexcept;
} // namespace EngineTweaks

#endif // ENGINETWEAKS_ENGINE_TWEAKS_HPP
