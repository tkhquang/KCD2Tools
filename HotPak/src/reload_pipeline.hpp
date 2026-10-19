/**
 * @file reload_pipeline.hpp
 * @brief The main-thread half of a hot reload: mount a staged generation and invalidate what it changed.
 *
 * For each generation the watcher staged, the tick:
 *   1. diffs its central directory against what the game currently reads for that pak (the newest overlay this tool
 *      mounted, else the installed copy CryPak opened at startup) and drops a generation that changes nothing;
 *   2. mounts the shadow copy with the mod manager's root and flags, so it wins every lookup;
 *   3. queues a texture reload per changed .dds (split mips map to their base file), refreshes each changed .cgf,
 *      retires each changed .mtl, reloads each changed table database, and marks each changed .skin's cached CSkin
 *      unshareable before opening the proxy reset window;
 *   4. when any .lua changed, calls each mod's `<modid>:OnHotReload()` and reruns its "scripts/mods/<modid>.lua" with
 *      a forced reload;
 *   5. reports what it cannot reload live (storm XML, other files) so the log says when a restart is still needed.
 */
#ifndef HOTPAK_RELOAD_PIPELINE_HPP
#define HOTPAK_RELOAD_PIPELINE_HPP

namespace HotPak
{
    /**
     * @brief Forgets every per-pak baseline and pending report.
     * @note Call from init before the frame hook arms.
     */
    void reset_reload_pipeline();

    /** @brief One main-thread step. Never throws. */
    void reload_tick() noexcept;

} // namespace HotPak

#endif // HOTPAK_RELOAD_PIPELINE_HPP
