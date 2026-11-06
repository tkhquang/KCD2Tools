/**
 * @file animation_framework.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef ANIMFW_ANIMATION_FRAMEWORK_HPP
#define ANIMFW_ANIMATION_FRAMEWORK_HPP

#include <DetourModKit.hpp>

namespace AnimationFramework
{
    /**
     * @brief Initializes the framework: config, anchors, the frame tick, and the database, clip and option-query hooks.
     * @details The framework binds no hotkey and keeps no state across a dev reload: what it changes in the engine is
     *          either retired at shutdown or kept on purpose (see shutdown()).
     * @param session The live Session.
     * @return An empty Result once the game module is found. Each feature is best-effort and logs why it stays off.
     * @note Runs off the loader lock. Each host calls shutdown() before it releases the image, and after init()
     *       fails or throws, because the hooks armed by then stay armed.
     */
    [[nodiscard]] DMK::Result<void> init(DMK::Session &session);

    /** @brief The outcome of @ref shutdown. */
    struct ShutdownResult
    {
        /**
         * @brief The hook retirement. Busy and Refused leave the hooks and the console commands in place, so a later
         *        shutdown() can retry it. A main thread that runs no frame in time also reports Busy.
         */
        DMK::hook::HookRetirement hooks{};
        /// False when a worker outside the hooks still runs code in this image, so the image must stay mapped.
        bool complete{false};
    };

    /**
     * @brief Removes the console commands on the main thread, retires every hook, then drops the console bindings and
     *        clears the config.
     * @details Merged databases and registered clips stay as the game loaded them: they are engine data, not code in
     *          this image. The .dba mismatch check stays off, because a replaced clip can still be indexed as loose.
     * @return The hook retirement and whether every worker joined.
     * @note Run off the loader lock. Idempotent.
     */
    [[nodiscard]] ShutdownResult shutdown() noexcept;
} // namespace AnimationFramework

#endif // ANIMFW_ANIMATION_FRAMEWORK_HPP
