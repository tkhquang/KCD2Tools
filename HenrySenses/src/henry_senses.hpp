/**
 * @file henry_senses.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef HENRYSENSES_HENRY_SENSES_HPP
#define HENRYSENSES_HENRY_SENSES_HPP

#include <DetourModKit.hpp>
#include <DetourModKit/abi/wheel_host.h>

#include <cstdint>
#include <span>

namespace HenrySenses
{
    /**
     * @brief Initializes the whole mod: config, anchors, hooks, backends and input bindings.
     * @param session The live Session.
     * @param wheel_host Resident wheel-host table owned by a never-unloaded module (the dev loader), or nullptr.
     *        A mouse-wheel binding makes the input engine take a permanent keepalive on whichever module hosts wheel
     *        capture; under the dev loader that must be the loader, so it passes its table here. Passing the pointer
     *        rather than branching on a build macro keeps one code path for dev and release.
     * @param persistent_slots Process-lifetime slots owned by the dev loader, or an empty span (release). Engine
     *        objects that outlive a generation (the silhouette stage) travel through them to the next generation.
     * @return An empty Result on success, or the Error of the failing mandatory step (already logged).
     * @note Runs on the bootstrap worker thread (production) or the loader thread (dev), off the loader lock. A failed
     *       init can leave hooks installed. Each host calls shutdown() before it releases the image and retains the
     *       image when that teardown refuses the unload.
     */
    [[nodiscard]] DMK::Result<void> init(
        DMK::Session &session,
        const WheelHostTable *wheel_host = nullptr,
        std::span<std::uint64_t> persistent_slots = {}
    );

    /** @brief The outcome of @ref shutdown. */
    struct ShutdownResult
    {
        /// The hook retirement. Busy and Refused leave every hook armed, so a later shutdown() can retry it.
        DMK::hook::HookRetirement hooks{};
        /// False when a worker or game state outside the hooks still needs this image, so the image must stay mapped.
        bool complete = false;
    };

    /**
     * @brief Tears the mod down: restores every highlighted node, unpublishes the silhouette stage, retires every
     *        hook, and clears the config.
     * @details The node restore and the stage unpublish need the hooks, so they run once, before the retirement. A
     *          stage that stays published keeps every hook as its guard, so the result is incomplete with no
     *          retirement. A retirement that is not Retired, or that pinned a hook, stops the teardown there, so
     *          everything a detour reads stays.
     * @return The hook retirement and whether every worker joined and every node was restored.
     * @note Run OFF the loader lock, on the dev loader's control thread, and never from DllMain. Idempotent: a call
     *       after Busy retries the retirement.
     */
    [[nodiscard]] ShutdownResult shutdown() noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_HENRY_SENSES_HPP
