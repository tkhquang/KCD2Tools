/**
 * @file hot_pak.hpp
 * @brief Mod lifecycle entry points driven by the DetourModKit Session.
 */
#ifndef HOTPAK_HOT_PAK_HPP
#define HOTPAK_HOT_PAK_HPP

#include <DetourModKit.hpp>

#include <cstdint>
#include <span>

namespace HotPak
{
    /**
     * @brief Initializes the tool: config, anchors, engine access, the two hooks and the pak watcher.
     * @param session The live Session.
     * @param persistent_slots The dev loader's process-lifetime slots, or an empty span in the production ASI. They
     *        carry the retired-material sequence (initialize_engine). The overlay paks a generation mounted need no
     *        slot: the next generation adopts them from CryPak's own pak list.
     * @return An empty Result on success, or the Error of the failing mandatory step (already logged).
     * @note Runs off the loader lock. Each host calls shutdown() before it releases the image.
     */
    [[nodiscard]] DMK::Result<void> init(DMK::Session &session, std::span<std::uint64_t> persistent_slots = {});

    /** @brief The outcome of @ref shutdown. */
    struct ShutdownResult
    {
        /// The hook retirement. Busy and Refused leave every hook armed, so a later shutdown() can retry it.
        DMK::hook::HookRetirement hooks{};
        /// False when a worker outside the hooks still runs code in this image, so the image must stay mapped.
        bool complete = false;
    };

    /**
     * @brief Retires both hooks, then stops the watcher and releases the engine access. The mounted overlay paks stay
     *        mounted: the engine keeps reading assets from them, and the next generation adopts them as its baseline.
     * @return The hook retirement and whether every worker joined. A retirement that is not Retired, or that pinned a
     *         hook, stops the teardown before the watcher, so everything a detour reads stays.
     * @note Run off the loader lock. Idempotent.
     */
    [[nodiscard]] ShutdownResult shutdown() noexcept;

} // namespace HotPak

#endif // HOTPAK_HOT_PAK_HPP
