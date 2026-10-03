/**
 * @file mod_logic.cpp
 * @brief The hot-reloaded half of the dev build: one generation of mod logic behind a C ABI.
 *
 * @details The resident loader owns the process and this DLL owns one generation. The loader calls Init() after
 *          LoadLibrary and Shutdown() before FreeLibrary. There is no DllMain bootstrap on this path, so Init() owns
 *          the Session directly and Shutdown() drops it.
 *
 *          Shutdown()'s return value is an UNMAP AUTHORIZATION, not a status. A success returned while a detour body
 *          in this image is still reachable turns a reload into a stale image and then a crash. The verdict follows
 *          DetourModKit's hot-reload guide: drain, clear hooks newest-first, drop the Session, then read module pins
 *          AS STATE.
 *
 *          "As state" is the part that matters. A DELTA of diagnostics::total_intentional_leaks() across teardown
 *          reads zero for a wheel keepalive, because that pin is booked at install time, so the delta authorizes the
 *          unmap of an image that can never unmap. diagnostics::module_pin_count() reports the open reference itself
 *          and stays readable after ~Session.
 *
 *          Only compiled when HENRYSENSES_DEV_BUILD is set by the dev preset.
 */

#include "../constants.hpp"
#include "../henry_senses.hpp"
#include "../version.hpp"

#include "loader_log.hpp"
#include "abi/reload.h"

#include <DetourModKit/async_logger_config.hpp>
#include <DetourModKit/diagnostics.hpp>
#include <DetourModKit/filesystem.hpp>
#include <DetourModKit/logger.hpp>
#include <DetourModKit/session.hpp>

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <utility>

/// The version tag as a string literal, so Revision() can append the build stamp at compile time.
#define HS_DEV_VERSION "v" VERSION_STR(VERSION_MAJOR) "." VERSION_STR(VERSION_MINOR) "." VERSION_STR(VERSION_PATCH)

namespace
{
    /// The live Session for this generation. Empty between Shutdown() and the next Init().
    std::optional<DMK::Session> s_session;

    /**
     * @brief Set when HenrySenses::shutdown() could not prove the image unreferenced.
     * @details A pinned hook or a detour still running keeps it referenced.
     */

    /// This generation runs on the loader's resident wheel host (ExternalHost), so it books no wheel keepalive.
    bool s_external_host = false;

    /**
     * @brief Appends one line to the LOADER's log, which outlives every generation.
     * @details The unload verdict is computed after ~Session, so DMK::log() is gone by then. The line takes the
     *          loader log's own format (loader_log.hpp), so it sorts in with the loader's records. A line sent only to
     *          OutputDebugStringA hides from anyone without a debugger attached, which is exactly how a refusal loop
     *          goes unexplained.
     * @note get_runtime_directory() allocates, and catch(...) handlers call this with an exception already in
     *       flight, so the path build runs inside try/catch to hold the noexcept boundary.
     */
    void append_loader_log(const char *line) noexcept
    {
        std::wstring log_path;
        try
        {
            log_path = HenrySenses::dev::loader_log_path(
                DMK::filesystem::get_runtime_directory(),
                HenrySenses::constants::MOD_NAME
            );
        }
        catch (...)
        {
            HenrySenses::dev::echo_to_debugger(line);
            return;
        }
        HenrySenses::dev::append_line(log_path.c_str(), line);
    }

    /**
     * @brief Pins this image for the process lifetime.
     * @details For a failed Init whose teardown could not prove the image unreferenced. The loader still calls
     *          FreeLibrary on a refused Init, and the pin keeps that call from unmapping code a detour or a DMK pin
     *          can still reach.
     */
    void pin_this_image() noexcept
    {
        const auto address = reinterpret_cast<LPCWSTR>(&append_loader_log);
        HMODULE self = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                address,
                &self
            ) != 0)
        {
            append_loader_log(
                "[KCD2_HenrySenses][DEV] Init failed and the teardown could not prove this image "
                "unreferenced; the image is pinned"
            );
            return;
        }
        const DWORD pin_error = GetLastError();
        // Without the pin, one leaked reference still outlasts the loader's single FreeLibrary.
        const bool referenced = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, address, &self) != 0;
        char line[200];
        (void)std::snprintf(
            line,
            sizeof(line),
            "[KCD2_HenrySenses][DEV] Init failed and the teardown refused the unload; pinning failed "
            "(error %lu), %s",
            pin_error,
            referenced ? "an extra module reference keeps the image mapped instead" : "and the image is NOT held mapped"
        );
        append_loader_log(line);
    }

    /**
     * @brief Tears the generation down and decides whether the loader may retire its image.
     * @details DetourModKit's hot-reload order: the mod teardown while the code pages are mapped, the typed drain,
     *          ~Session, then the module pins read AS STATE. Only the documented-inert pins may remain: the local
     *          backend's wheel keepalive, and a retained XInput chain. A Key.Consume binding installs the XInput hook,
     *          and when another writer (the Steam overlay) owns the XInputGetState prologue, DetourModKit must keep
     *          the chain (restoring its bytes would leave that writer jumping into freed memory). That retention holds
     *          the XInput pins and records one Input intentional leak; it only forwards calls into the game's own
     *          XInput, so the image stays mapped but none of the mod's code runs. Any other intentional leak can
     *          still identify live code and refuses. On a true return the loader frees the image; an image the pins
     *          keep mapped is charged against the retained-generation budget, and the next generation maps as a new,
     *          uniquely named copy beside it.
     * @return True when the loader may retire the image.
     */
    [[nodiscard]] bool teardown_generation()
    {
        namespace diag = DMK::diagnostics;

        // It restores every engine write, unpublishes the silhouette stage into the loader's persistent area, removes
        // every hook, and reports whether the image is unreferenced.
        if (!HenrySenses::shutdown())
        {
            // A retained guard or an unfinished callback still uses this generation's logger, state and hooks.
            // Keep the session intact along with the loader's module reference.
            append_loader_log(
                "[KCD2_HenrySenses][DEV] unload REFUSED: mod cleanup is incomplete; the session remains active"
            );
            return false;
        }

        // The library's own authorization to unmap. It retires every binding and config setter DMK still owns and
        // delivers a held hold-combo's balancing edge while this module's code is mapped, so no callback body can be
        // entered afterwards.
        const DMK::LogicDllUnloadStatus drain = DMK::prepare_logic_dll_unload_all();
        if (drain != DMK::LogicDllUnloadStatus::SafeToUnload)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Error,
                "[DEV] drain refused unload (status {}); module stays mapped",
                static_cast<int>(drain)
            );
            return false;
        }

        // Drop the Session last: it shuts the library subsystems down in order, input included. The XInput layer can
        // decide to retain there, which is why the verdict below runs after this.
        s_session.reset();

        const std::size_t wheel_pins = diag::module_pin_count(diag::ModulePinReason::MessageHookKeepalive);
        const std::size_t xinput_self = diag::module_pin_count(diag::ModulePinReason::XInputKeepalive);
        const std::size_t xinput_targets = diag::module_pin_count(diag::ModulePinReason::XInputTarget);
        const std::size_t total_pins = diag::total_module_pins();
        const std::size_t leaks = diag::total_intentional_leaks();
        const std::size_t input_leaks = diag::intentional_leak_count(diag::LeakSubsystem::Input);
        const std::size_t inert_pins = wheel_pins + xinput_self + xinput_targets;
        // The XInput retention's own leak record; it counts as inert only while the retained chain it describes holds.
        const std::size_t inert_leaks = xinput_self != 0 ? input_leaks : 0;

        char verdict[400];
        (void)std::snprintf(
            verdict,
            sizeof(verdict),
            "[KCD2_HenrySenses][DEV] unload verdict: wheel=%zu xinput_self=%zu xinput_targets=%zu "
            "total=%zu (inert=%zu) leaks=%zu (xinput retention=%zu) host=%s, teardown_safe=yes",
            wheel_pins,
            xinput_self,
            xinput_targets,
            total_pins,
            inert_pins,
            leaks,
            inert_leaks,
            s_external_host ? "resident" : "local"
        );
        append_loader_log(verdict);

        if (total_pins != inert_pins)
        {
            append_loader_log(
                "[KCD2_HenrySenses][DEV] unload REFUSED: a pin reason outside the documented-inert set "
                "remains; live code may still be reachable"
            );
            return false;
        }
        if (leaks != inert_leaks)
        {
            append_loader_log(
                "[KCD2_HenrySenses][DEV] unload REFUSED: an intentional leak other than a retained XInput chain "
                "remains; live code may still be reachable"
            );
            return false;
        }
        if (s_external_host && wheel_pins != 0)
        {
            // Inert, but the resident host should own the keepalive: the generation took the local backend anyway.
            append_loader_log(
                "[KCD2_HenrySenses][DEV] a generation on the resident wheel host booked its own wheel keepalive; "
                "its image is retained"
            );
        }
        return true;
    }

    /**
     * @brief Returns the loader's persistent slots after a header check.
     * @param state The area from the request.
     * @return The slots, or an empty span when the header does not match this build.
     */
    [[nodiscard]] std::span<std::uint64_t> persistent_slots(HenrySensesPersistentState *state) noexcept
    {
        if (state == nullptr || state->struct_size < sizeof(HenrySensesPersistentState) ||
            state->abi_version != HENRYSENSES_RELOAD_ABI_VERSION || state->magic != HENRYSENSES_PERSISTENT_MAGIC)
        {
            return {};
        }
        return std::span<std::uint64_t>(state->slots, HENRYSENSES_PERSISTENT_SLOT_COUNT);
    }
} // namespace

extern "C"
{
    /**
     * @brief Reports this generation's source version.
     * @details Exported so the LOADER can log it even when Init() fails, which is exactly when knowing the version
     *          matters most.
     * @warning This is NOT a witness of which bytes were mapped. The embedded build stamp comes from __DATE__ /
     *          __TIME__ in THIS translation unit, so it advances only when this file recompiles. A build that changed
     *          any other source relinks the DLL and leaves the stamp behind. The loader logs the mapped file's own
     *          size and write time alongside this string, and that pair is the authoritative identity.
     */
    __declspec(dllexport) const char *Revision() noexcept
    {
        return HS_DEV_VERSION " (" __DATE__ " " __TIME__ ")";
    }

    /**
     * @brief Starts one generation.
     * @param request Loader-owned request. Its wheel-host table and persistent area are valid for the whole process.
     * @return HENRYSENSES_RELOAD_OK when the generation is live, zero after a rollback that proved the image
     *         unreferenced, or HENRYSENSES_RELOAD_INIT_UNSAFE when the rollback could not (the image is then pinned).
     */
    __declspec(dllexport) unsigned Init(const HenrySensesReloadInitRequest *request) noexcept
    {
        // Validate the whole request before touching a field: a stale generation left in the deploy directory must
        // fail loudly rather than read through a shifted layout.
        //
        // A NULL wheel host is a deliberate, supported choice, not an error. Without that lease, this generation uses
        // the local message-hook backend and books its own permanent wheel keepalive, so its image is retained after
        // teardown. The loader charges that against its reload budget. A NON-null table must still match the
        // expected identity, so a foreign table is never accepted.
        if (request == nullptr || request->struct_size < sizeof(HenrySensesReloadInitRequest) ||
            request->abi_version != HENRYSENSES_RELOAD_ABI_VERSION || request->generation_id == 0 ||
            s_session.has_value())
        {
            append_loader_log("[KCD2_HenrySenses][DEV] Init rejected an invalid or stale reload request");
            return 0;
        }
        if (request->wheel_host != nullptr && (request->expected_host_identity == 0 ||
                                               request->wheel_host->host_identity != request->expected_host_identity))
        {
            append_loader_log("[KCD2_HenrySenses][DEV] Init rejected a foreign wheel-host table");
            return 0;
        }
        const std::span<std::uint64_t> slots = persistent_slots(request->persistent);
        if (slots.empty())
        {
            // Without the area the engine state a previous generation left behind cannot be adopted, and this
            // generation would create a second copy of it.
            append_loader_log("[KCD2_HenrySenses][DEV] Init rejected a request without a valid persistent-state area");
            return 0;
        }

        // The loader calls this through a C function pointer, so an exception must never unwind across the boundary.
        // A failed rollback returns the unsafe result so the loader retains the generation and stops reloading.
        try
        {
            DMK::AsyncLoggerConfig async_config;
            async_config.overflow_policy = DMK::OverflowPolicy::DropNewest;

            // LogOpenMode::Append is what keeps a reload diagnosable. Under the default Truncate, this generation's
            // first sink open erases the PREVIOUS generation's teardown records, which are the only lines that
            // explain a retained image. The loader truncates the file once per game run.
            auto opened = DMK::Session::start(
                DMK::ModInfo{
                    .name = HenrySenses::constants::MOD_NAME,
                    .log_file = HenrySenses::constants::LOG_FILE_NAME,
                    .game_process_name = "",
                    .instance_mutex_prefix = HenrySenses::constants::INSTANCE_MUTEX_PREFIX,
                    .log = async_config,
                    .log_open_mode = DMK::LogOpenMode::Append,
                    .log_source_stamp_mode = DMK::LogSourceStampMode::at_or_below(DMK::LogLevel::Trace),
                }
            );
            if (!opened)
            {
                append_loader_log("[KCD2_HenrySenses][DEV] Session::start failed");
                return 0;
            }
            s_session.emplace(std::move(*opened));
            s_external_host = request->wheel_host != nullptr;

            DMK::log().info("[DEV] Init generation {} - {}", request->generation_id, Revision());

            if (auto ready = HenrySenses::init(*s_session, request->wheel_host, slots); !ready)
            {
                DMK::log().error("[DEV] Henry's Senses initialization FAILED ({})", ready.error().message());
                // init() armed hooks and may have ticked before a later step failed, so the full Shutdown sequence
                // runs. An image it cannot prove unreferenced is pinned, and the loader stops reloading.
                if (!teardown_generation())
                {
                    pin_this_image();
                    return HENRYSENSES_RELOAD_INIT_UNSAFE;
                }
                return 0;
            }
            return HENRYSENSES_RELOAD_OK;
        }
        catch (...)
        {
            append_loader_log("[KCD2_HenrySenses][DEV] Init threw an exception");
            // Init can throw after a hook is armed. The loader must retain this image unless teardown proves
            // that every detour and registered callback has retired.
            if (s_session.has_value())
            {
                try
                {
                    if (teardown_generation())
                    {
                        return 0;
                    }
                }
                catch (...)
                {
                    append_loader_log("[KCD2_HenrySenses][DEV] Failed Init teardown threw an exception");
                }
                pin_this_image();
                return HENRYSENSES_RELOAD_INIT_UNSAFE;
            }
            return 0;
        }
    }

    /**
     * @brief Tears this generation down and reports whether the image may be unmapped.
     * @return HENRYSENSES_RELOAD_OK when the drain succeeded, the mod teardown proved the image unreferenced (every
     *         prologue restored, no detour running), and the only remaining module pins are
     *         the documented-inert ones.
     * @note The loader must keep the DLL mapped on a zero return.
     */
    __declspec(dllexport) unsigned Shutdown() noexcept
    {
        if (!s_session.has_value())
        {
            return 0;
        }

        try
        {
            (void)DMK::log().try_log(DMK::LogLevel::Info, "[DEV] Shutdown called");
            return teardown_generation() ? HENRYSENSES_RELOAD_OK : 0;
        }
        catch (...)
        {
            append_loader_log("[KCD2_HenrySenses][DEV] Shutdown threw an exception; refusing unload");
            return 0;
        }
    }
} // extern "C"

/**
 * @brief The loader drives Init and Shutdown explicitly, so attach and an explicit detach have no work of their own.
 * @details At process exit (a non-null @p reserved) a live generation's Session is abandoned: the C runtime would
 *          otherwise run ~Session here, under the loader lock, after the OS already ended every other thread.
 */
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr && s_session.has_value())
    {
        s_session->abandon();
    }
    return TRUE;
}
