/**
 * @file proxy_reload.hpp
 * @brief Restreams clothing attachments that still show replaced files, from inside their own manager's pass.
 *
 * A character's skins are attached through wh::entitymodule::clothing proxies. Each proxy streams one C_SkinElement
 * row of the CharacterComponent table, which names its model and material files, and its manager runs one streaming
 * pass over all of its proxies at a time. Reset() (vtable slot 3) detaches and releases the attachment and the
 * material instance and puts the proxy back in Init; the pass that follows requests the files again.
 *
 * A proxy is first scoped by the files its row asks for, then reset only for a reason reset_rules.hpp lists: it shows
 * a replaced skin or material, it is in Error, its character existed before a table reload, or it is stuck after
 * HotPak reset its character. A character a save load creates during the window therefore keeps its fresh proxies.
 *
 * The manager swaps a character's proxies in together, so one proxy in Error also holds back every other proxy of that
 * character after a reload: they wait with nothing attached and the whole character disappears. Recovery therefore
 * resets the Error proxies whose files the pak ships on every generation, changed or not.
 *
 * The reloads only reach the new files because the caller first made the caches miss: the old CSkin is marked
 * unshareable and the old material is renamed out of CMatMan (engine.hpp).
 *
 * The resets run inside the manager's own pass, before the original, on whichever thread drives that manager: no other
 * code touches the same proxies at that moment, and it is where the game's own unload path calls Reset(). Each proxy is
 * reset at most once per window.
 */
#ifndef HOTPAK_PROXY_RELOAD_HPP
#define HOTPAK_PROXY_RELOAD_HPP

#include "engine.hpp"
#include "reset_rules.hpp"

#include <DetourModKit.hpp>

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace HotPak
{
    /** @brief Counters since the last take_proxy_reload_stats(). */
    struct ProxyReloadStats
    {
        std::uint64_t manager_passes = 0;
        std::uint64_t proxies_reset = 0;
    };

    /**
     * @brief What the reset window looks for. File names are lowercase and without folders ("shiba_fur.skin",
     *        "shiba_fur.mtl"), the form C_SkinElement rows store. At most 64 entries of each kind are tracked.
     */
    struct ResetTargets
    {
        /// Every skin and material the pak ships: only a proxy whose model or material is one of these is examined.
        std::span<const std::string> shipped_files;
        /// The skins whose wearers restream after a table reload, when their character existed before the window.
        std::span<const std::string> restream_files;
        /// The CSkin objects HotPak marked unshared: a proxy that still shows one is reset.
        std::span<const std::uintptr_t> replaced_skins;
    };

    /** @brief A proxy the window reset, with what it asks for. */
    struct ResetRecord
    {
        std::uintptr_t proxy = 0;
        std::string model;
        std::string material;
        std::uint8_t state_before = 0;
        ResetReason reason = ResetReason::Error;
    };

    /** @brief A reset proxy's state now. */
    struct ProxyOutcome
    {
        ResetRecord record;
        /// E_StreamingState now, or -1 when the proxy is gone or no longer a clothing proxy.
        int state = -1;
    };

    /**
     * @brief Installs the manager-pass hook.
     * @param hooks The mod's hook stack. The counted detour lets HookStack::retire prove that no pass is inside it.
     * @param target The ProxyManagerUpdate anchor.
     * @param engine The gated anchors. The proxy, skin-element, attachment and material-instance vtables gate every
     *        read, and the skin and material vtables gate the replaced-object checks.
     * @return An empty value, or the failure.
     * @note Setup/control-plane only. Requires initialize_engine(), which bounds every vtable slot to the game image.
     */
    [[nodiscard]] DMK::Result<void>
    initialize_proxy_reload(DMK::hook::HookStack &hooks, std::uintptr_t target, const EngineAnchors &engine);

    /**
     * @brief Publishes the targets and opens the reset window, forgetting which proxies the last window reset.
     * @note Main thread.
     */
    void begin_proxy_resets(const ResetTargets &targets, std::chrono::milliseconds window) noexcept;

    /** @brief Reports whether the reset window is open. */
    [[nodiscard]] bool proxy_resets_active() noexcept;

    /** @brief Closes the window once it has expired. Returns true on the call that closed it. */
    [[nodiscard]] bool expire_proxy_resets() noexcept;

    /** @brief Returns and clears the counters. */
    [[nodiscard]] ProxyReloadStats take_proxy_reload_stats() noexcept;

    /**
     * @brief Moves out the proxies reset since the last call.
     * @note Main thread.
     */
    [[nodiscard]] std::vector<ResetRecord> take_reset_records();

    /**
     * @brief Reads each reset proxy's state now (guarded, vtable-checked).
     * @note Main thread; the proxies may have been destroyed since, which reads as state -1.
     */
    [[nodiscard]] std::vector<ProxyOutcome> read_outcomes(const std::vector<ResetRecord> &records);

    /**
     * @brief Names the SKIN attachments that keep a waiting proxy's character from swapping anything in.
     * @details The character swaps its pending attachments only when every SKIN attachment on it has its skin. One
     *          left without a skin (an attachment created by a script and never given a skin, for example) blocks
     *          every reload of that character.
     * @param proxy A proxy waiting for its swap.
     * @return The names of the character's SKIN attachments that are still before their skin; empty when none.
     * @note Main thread.
     */
    [[nodiscard]] std::vector<std::string> blocking_attachments(std::uintptr_t proxy);

} // namespace HotPak

#endif // HOTPAK_PROXY_RELOAD_HPP
