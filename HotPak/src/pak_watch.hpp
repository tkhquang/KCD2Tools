/**
 * @file pak_watch.hpp
 * @brief Watches built paks and stages each new build as a uniquely named shadow copy for CryPak to mount.
 *
 * CryPak opens a pak with FILE_SHARE_READ and keeps it open, so a mounted file can never be rebuilt in place. The
 * watcher therefore never hands CryPak the build output itself: each stable new build is copied to
 * %LOCALAPPDATA%\KCD2_HotPak\<name>.gNNNN.pak, a name no build writes and no earlier mount holds, and only that copy
 * is mounted. The build folder stays writable for the next build.
 */
#ifndef HOTPAK_PAK_WATCH_HPP
#define HOTPAK_PAK_WATCH_HPP

#include "zip_index.hpp"

#include <DetourModKit.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace HotPak
{
    /** @brief One staged build waiting for the main thread to mount it. */
    struct PendingGeneration
    {
        /// The watched build output.
        std::wstring source;
        /// The shadow copy to mount.
        std::wstring shadow;
        /// The source file name, normalized (for example "mutt_the_shiba.pak").
        std::string file_name;
        /// Process-wide generation number in the shadow name.
        std::uint32_t generation = 0;
        /// The shadow's central directory.
        std::vector<ZipEntry> index;
    };

    /**
     * @brief Stops the running watcher, then starts one over @p sources.
     * @details The first call of a logic generation also creates the shadow folder and deletes every shadow copy that
     *          no pak mount holds. Generations staged earlier stay queued for the main thread.
     * @param sources The build outputs to watch.
     * @param poll How often each source is checked. A source must look unchanged on two consecutive checks before it
     *        is copied, so a build still writing it is never staged.
     * @return An empty value, or the failure (already logged).
     * @note Setup/control-plane only. The first call must precede the frame hook, which reads the shadow folder.
     */
    [[nodiscard]] DMK::Result<void>
    start_pak_watcher(std::vector<std::wstring> sources, std::chrono::milliseconds poll);

    /**
     * @brief Stops and joins the watcher thread. Idempotent.
     * @details A failed join detaches the thread and keeps its ModulePinReason::Worker reference, which shutdown()
     *          reports as an incomplete teardown.
     */
    void stop_pak_watcher() noexcept;

    /** @brief Moves out every generation staged since the last call. */
    [[nodiscard]] std::vector<PendingGeneration> take_pending_generations();

    /** @brief The shadow folder (no trailing separator), empty before the first start_pak_watcher(). */
    [[nodiscard]] const std::wstring &shadow_directory() noexcept;

} // namespace HotPak

#endif // HOTPAK_PAK_WATCH_HPP
