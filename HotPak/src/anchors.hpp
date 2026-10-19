/**
 * @file anchors.hpp
 * @brief The anchors the tool resolves at startup and the features they enable.
 *
 * Every function the tool hooks and the global it reads first resolve from byte-signature ladders inside WHGame.dll.
 * Every interface it calls resolves from an RTTI type name, and the skin cache's mutex functions from the C++ runtime's
 * export table. Each feature turns on only when every anchor it uses resolved (anchor::evaluate_gate).
 */
#ifndef HOTPAK_ANCHORS_HPP
#define HOTPAK_ANCHORS_HPP

#include "engine.hpp"

#include <DetourModKit.hpp>

#include <cstdint>

namespace HotPak
{
    /** @brief The resolved anchors, 0 when unresolved or gated off, and the features they enable. */
    struct Anchors
    {
        /// SSystemGlobalEnvironment (gEnv), a static struct inside the game image.
        std::uintptr_t genv = 0;
        /// CCryAction::PostUpdate: once per frame on the main thread, also at the menus and while paused.
        std::uintptr_t post_update = 0;
        /// C_ComponentAttachmentManager's streaming pass over its clothing proxies.
        std::uintptr_t proxy_manager_update = 0;
        /// The interfaces and runtime functions the engine layer calls. A member of a failed feature stays 0.
        EngineAnchors engine{};

        /// gEnv, CryPak and the main-thread tick: without them the tool cannot mount anything.
        bool core = false;
        /// Resets of the clothing proxies whose files changed (the manager-pass hook and the proxy types).
        bool proxy_resets = false;
    };

    /**
     * @brief Resolves every anchor inside @p game, logs each one, and gates each feature on its own anchors.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] Anchors resolve_anchors(DMK::Region game);

} // namespace HotPak

#endif // HOTPAK_ANCHORS_HPP
