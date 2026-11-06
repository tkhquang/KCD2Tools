/**
 * @file dllmain.cpp
 * @brief DLL entry point of the production ASI and KCSE plugin, which wires the mod lifecycle to the DetourModKit
 *        Session.
 *
 * bootstrap_attach() runs on_ready() and ~Session on its worker thread, off the loader lock (DetourModKit/session.hpp).
 *
 * DetourModKit does not own the mod's state. The hooks are caller-owned handles, and AnimationFramework::shutdown()
 * retires them. It must run off the loader lock, so DllMain never calls it. The production module lives for the whole
 * process: a bare FreeLibrary cannot reach DLL_PROCESS_DETACH while the bootstrap worker holds its module reference,
 * and process exit takes the abandon path of bootstrap_detach(). The dev build loads the logic through a loader ASI
 * instead (src/dev/mod_logic.cpp), so this file is not part of it.
 */

#include "animation_framework.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <array>
#include <cstring>
#include <string_view>

namespace
{
    /**
     * @brief The log's absolute UTF-8 path in the game's bin folder, beside KingdomCome.exe and the DEV folder.
     * @return That path, or the bare file name (the log then sits beside this module) when the bin folder's path does
     *         not fit the buffers or does not convert.
     * @details The ASI build already sits in the bin folder. The KCSE build sits in Mods/<modid>/KCSE/Plugins/, a
     *          folder a mod manager may only show to the game: Mod Organizer 2 redirects new files there to its
     *          overwrite folder. The log stays beside the DEV folder for both builds, where the modding guide and the
     *          Blender add-on read it. The buffers are static because DllMain runs under the loader lock, where
     *          bootstrap_attach() takes no heap allocation either.
     */
    [[nodiscard]] std::string_view bin_folder_log_path() noexcept
    {
        static std::array<wchar_t, 4096> executable{};
        static std::array<char, 4096 * 3> utf8{};
        const std::string_view file_name = AnimationFramework::constants::LOG_FILE_NAME;

        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size())
        {
            return file_name;
        }
        DWORD folder_length = length;
        while (folder_length > 0 && executable[folder_length - 1] != L'\\')
        {
            --folder_length;
        }
        const int capacity = static_cast<int>(utf8.size() - file_name.size());
        const int written = folder_length == 0 ? 0
                                               : WideCharToMultiByte(
                                                     CP_UTF8,
                                                     WC_ERR_INVALID_CHARS,
                                                     executable.data(),
                                                     static_cast<int>(folder_length),
                                                     utf8.data(),
                                                     capacity,
                                                     nullptr,
                                                     nullptr
                                                 );
        if (written <= 0)
        {
            return file_name;
        }
        std::memcpy(utf8.data() + written, file_name.data(), file_name.size());
        return {utf8.data(), static_cast<std::size_t>(written) + file_name.size()};
    }

    /**
     * @brief Bootstrap callback: runs the mod init on the DMK worker thread.
     * @param session The live Session handed over by bootstrap_attach().
     * @return The init result. A failure leaves the mod inert.
     */
    DMK::Result<void> on_ready(DMK::Session &session)
    {
        DMK::Result<void> result;
        try
        {
            result = AnimationFramework::init(session);
        }
        catch (...)
        {
            result = std::unexpected(DMK::Error{DMK::ErrorCode::Unknown, "AnimationFramework::init/exception"});
        }
        if (!result)
        {
            // The bootstrap worker keeps its Session and module reference until process exit, even after on_ready
            // reports failure, so hooks that did not retire stay valid. shutdown() logs that outcome itself.
            (void)AnimationFramework::shutdown();
        }
        return result;
    }
} // namespace

/**
 * @brief Production DLL entry point.
 * @param ul_reason_for_call The loader notification.
 * @param lp_reserved Null for an explicit FreeLibrary, non-null for process exit.
 * @return TRUE unless the attach gate refused this load.
 * @note The module handle is unused, because DetourModKit captures its own module from a code address.
 */
BOOL APIENTRY DllMain(HMODULE, DWORD ul_reason_for_call, LPVOID lp_reserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DMK::AsyncLoggerConfig async_cfg;
        // Detours share this logger. Queue overflow must never move file I/O onto a game callback.
        async_cfg.overflow_policy = DMK::OverflowPolicy::DropNewest;

        // No process-name gate: the WHGame.dll module check in init() is the gate.
        const DMK::ModInfo info{
            .name = AnimationFramework::constants::MOD_NAME,
            .log_file = bin_folder_log_path(),
            .game_process_name = "",
            .instance_mutex_prefix = AnimationFramework::constants::INSTANCE_MUTEX_PREFIX,
            .log = async_cfg,
            // The [file:line] stamp stays only on Trace, where a debug session reads it. The default Truncate open
            // mode is correct here, because this build is a single DLL with one Session per run.
            .log_source_stamp_mode = DMK::LogSourceStampMode::at_or_below(DMK::LogLevel::Trace),
        };

        // A gate refusal (wrong process, or a duplicate load that holds the mutex) is a reason for this DLL to go
        // away, not for the host to fail. A failed attach lets the loader unmap it.
        return DMK::bootstrap_attach(info, &on_ready).has_value() ? TRUE : FALSE;
    }

    case DLL_PROCESS_DETACH:
        // Loader-lock safe on both paths ([B-100]): it neither waits nor joins, and at process exit it abandons the
        // Session, whose threads the OS already ended.
        DMK::bootstrap_detach(lp_reserved);
        break;

    default:
        break;
    }

    return TRUE;
}
