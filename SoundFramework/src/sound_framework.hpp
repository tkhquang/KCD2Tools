/**
 * @file sound_framework.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef SNDFW_SOUND_FRAMEWORK_HPP
#define SNDFW_SOUND_FRAMEWORK_HPP

#include <DetourModKit.hpp>
#include <DetourModKit/abi/wheel_host.h>

#include <cstdint>
#include <span>

namespace SoundFramework
{
    /**
     * @brief Initializes the framework: config, the game's FMOD API, and the listener hook.
     * @param session The live Session.
     * @param wheel_host Resident wheel-host table of the dev loader, or nullptr. Unused.
     * @param persistent_slots The dev loader's process-lifetime slots, or an empty span. Unused: the game's FMOD system
     *        is learned again from the next listener update.
     * @return An empty Result on success, or the Error of the failing step (already logged).
     * @note Runs off the loader lock.
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
        /// False when a worker outside the hooks still runs code in this image.
        bool complete = false;
    };

    /**
     * @brief Retires the listener hook, then stops and releases every sound the framework created.
     * @note Run off the loader lock. Idempotent.
     */
    [[nodiscard]] ShutdownResult shutdown() noexcept;
} // namespace SoundFramework

#endif // SNDFW_SOUND_FRAMEWORK_HPP
