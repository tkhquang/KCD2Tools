/**
 * @file mod_logic.cpp
 * @brief The hot-reloaded half of the dev build: one generation of mod logic behind a C ABI.
 *
 * @details The resident loader owns the process; this DLL owns one generation. The loader calls Init()
 *          after LoadLibrary and Shutdown() before FreeLibrary. There is no DllMain bootstrap on this
 *          path, so Init() owns the Session directly and Shutdown() drops it.
 *
 *          Shutdown()'s return value is an UNMAP AUTHORIZATION, not a status. Returning success while a
 *          detour body in this image is still reachable is what turns a reload into a stale image and
 *          then a crash. The verdict therefore follows DetourModKit's hot-reload guide: drain, clear
 *          hooks newest-first, drop the Session, then read module pins as STATE.
 *
 *          "As state" is the part that matters. An earlier version computed a DELTA of
 *          diagnostics::total_intentional_leaks() across teardown, which reads zero for a wheel
 *          keepalive because that pin is booked at install time - so it authorised unmapping an image
 *          that could never unmap. diagnostics::module_pin_count() reports the open reference itself
 *          and stays readable after ~Session.
 */

#include "constants.hpp"
#include "protocol.h"
#include "tpv_camera.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>

namespace
{
    /// The live Session for this generation. Empty between Shutdown() and the next Init().
    std::optional<DMK::Session> s_session;

    /// Set when TPVCamera::shutdown() could not prove every hooked prologue was restored.
    bool s_hook_restore_failed = false;

    /**
     * @brief Appends one line to the LOADER's log, which outlives every generation.
     * @details The unload verdict is computed after ~Session, so DMK::log() is gone by then. Sending it
     *          only to OutputDebugStringA hides it from anyone without a debugger attached - which is
     *          exactly how a refusal loop went unexplained: the loader reported "Shutdown refused" four
     *          times with the reason nowhere on disk.
     */
    void append_loader_log(const char *line) noexcept
    {
        char module_path[MAX_PATH]{};
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&append_loader_log), &self) == 0 ||
            GetModuleFileNameA(self, module_path, MAX_PATH) == 0)
        {
            OutputDebugStringA(line);
            return;
        }
        char *const slash = std::strrchr(module_path, '\\');
        if (slash == nullptr)
        {
            OutputDebugStringA(line);
            return;
        }
        slash[1] = '\0';

        char log_path[MAX_PATH]{};
        (void)std::snprintf(log_path, sizeof(log_path), "%sKCD2_TPVCamera_Loader.log", module_path);
        const HANDLE file = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            char stamped[640];
            const int len = std::snprintf(stamped, sizeof(stamped), "[%02u:%02u:%02u.%03u] %s\n", now.wHour,
                                          now.wMinute, now.wSecond, now.wMilliseconds, line);
            if (len > 0)
            {
                DWORD wrote = 0;
                (void)WriteFile(file, stamped, static_cast<DWORD>(len), &wrote, nullptr);
            }
            CloseHandle(file);
        }
        OutputDebugStringA(line);
    }
} // namespace

/**
 * @brief Bumped by hand when a specific rebuild needs to be identifiable in the loader's log.
 * @details The loader logs this through the Revision export. `__DATE__`/`__TIME__` alone move only when
 *          THIS translation unit recompiles, so a change elsewhere in the mod can produce an unchanged
 *          stamp. A macro rather than a constant so it pastes straight into the returned literal.
 */
#define TPVCAMERA_DEV_BUILD_REVISION "21"

extern "C"
{
    /**
     * @brief Reports which bytes the loader actually mapped.
     * @details Exported so the LOADER can log it. Logging the build identity from inside Init() loses it
     *          whenever Init() fails, which is exactly when knowing the build matters most.
     */
    __declspec(dllexport) const char *Revision() noexcept
    {
        return "rev " TPVCAMERA_DEV_BUILD_REVISION " (" __DATE__ " " __TIME__ ")";
    }

    /**
     * @brief Starts one generation.
     * @param request Loader-owned request. Its wheel-host table is valid for the whole process.
     * @return TPVCAMERA_RELOAD_OK when the generation is live, otherwise zero after rollback.
     */
    __declspec(dllexport) unsigned Init(const TpvReloadInitRequest *request) noexcept
    {
        // Validate the whole request before touching a field: a stale generation left in the deploy
        // directory must fail loudly rather than read through a shifted layout.
        //
        // A NULL wheel host is a deliberate, supported choice, not an error. The loader decides whether a
        // generation leases its resident host; when it does not, this generation uses the local
        // MessageHook backend and books its own permanent wheel keepalive, so its image is retained after
        // teardown. The loader charges that against its reload budget. A NON-null table must still match
        // the expected identity, so a foreign table is never accepted.
        if (request == nullptr || request->struct_size < sizeof(TpvReloadInitRequest) ||
            request->abi_version != TPVCAMERA_RELOAD_ABI_VERSION || request->generation_id == 0 ||
            s_session.has_value())
        {
            append_loader_log("[KCD2_TPVCamera][DEV] Init rejected an invalid or stale reload request");
            return 0;
        }
        if (request->wheel_host != nullptr &&
            (request->expected_host_identity == 0 ||
             request->wheel_host->host_identity != request->expected_host_identity))
        {
            append_loader_log("[KCD2_TPVCamera][DEV] Init rejected a foreign wheel-host table");
            return 0;
        }

        // The loader calls this through a C function pointer, so an exception must never unwind across the
        // boundary. Guard the whole body and return zero on any failure.
        try
        {
            DMK::AsyncLoggerConfig async_cfg;
            async_cfg.overflow_policy = DMK::OverflowPolicy::SyncFallback;

            // LogOpenMode::Append is what keeps a reload diagnosable. Under the default Truncate, this
            // generation's first sink open erases the PREVIOUS generation's teardown records - including
            // the XInput retention warning naming which writer owns the prologue, which is the only line
            // that explains a retained image.
            auto opened = DMK::Session::start(DMK::ModInfo{
                .name = Constants::MOD_NAME,
                .log_file = Constants::LOG_FILE_NAME,
                .game_process_name = "",
                .instance_mutex_prefix = Constants::INSTANCE_MUTEX_PREFIX,
                .log = async_cfg,
                .log_open_mode = DMK::LogOpenMode::Append,
                // Keep the [file:line] stamp only where it earns its place. Trace records are the ones
                // read while actively debugging, so they keep their call site; Debug and above render
                // clean, which matters most under LogOpenMode::Append where every generation's records
                // accumulate in one file.
                .log_source_stamp_mode = DMK::LogSourceStampMode::at_or_below(DMK::LogLevel::Trace),
            });
            if (!opened.has_value())
            {
                append_loader_log("[KCD2_TPVCamera][DEV] Session::start failed");
                return 0;
            }
            s_session.emplace(std::move(*opened));
            s_hook_restore_failed = false;

            DMK::log().info("[DEV] Init generation {} -- {}", request->generation_id, Revision());

            // The resident host table travels all the way to Input::start, so the wheel keepalive is booked
            // against the loader instead of this image.
            if (auto ready = TPVCamera::init(*s_session, request->wheel_host); !ready.has_value())
            {
                DMK::log().error("[DEV] TPVCamera initialization FAILED ({})", ready.error().message());
                s_session.reset();
                return 0;
            }
            return TPVCAMERA_RELOAD_OK;
        }
        catch (...)
        {
            // The logger may not have come up yet, so report through the loader's file.
            append_loader_log("[KCD2_TPVCamera][DEV] Init threw an exception");
            s_session.reset();
            return 0;
        }
    }

    /**
     * @brief Tears this generation down and reports whether the image may be unmapped.
     * @return TPVCAMERA_RELOAD_OK when the drain succeeded, every prologue was restored, and the only
     *         remaining module pins are the documented-inert ones.
     * @note The loader must keep the DLL mapped on a zero return.
     */
    __declspec(dllexport) unsigned Shutdown() noexcept
    {
        namespace diag = DMK::diagnostics;

        if (!s_session.has_value())
        {
            return 0;
        }

        try
        {
            DMK::log().info("[DEV] Shutdown called");

            // Mod teardown first, while this module's code pages are still mapped. It joins the overlay
            // thread, removes every hook, and reports whether each prologue was restored.
            s_hook_restore_failed = !TPVCamera::shutdown();

            // The library's own authorization to unmap. It retires every binding and config setter DMK
            // still owns and delivers a held hold-combo's balancing edge while this module's code is
            // mapped, so no callback body can be entered afterwards.
            const DMK::LogicDllUnloadStatus drain = DMK::prepare_logic_dll_unload_all();
            if (drain != DMK::LogicDllUnloadStatus::SafeToUnload)
            {
                DMK::log().error("[DEV] drain refused unload (status {}); module stays mapped",
                                 static_cast<int>(drain));
                return 0;
            }

            // Drop the Session last: it shuts the library subsystems down in order, input included. The
            // XInput layer can decide to retain here, which is why the verdict below runs after this.
            s_session.reset();

            // Which pins are TOLERABLE is the part worth getting right. The reference example demands a
            // global zero, but it registers no consume gamepad binding and therefore never installs XInput
            // interception. This mod does, and when a rival writer (the Steam overlay) owns the
            // XInputGetState prologue, retention is MANDATORY: restoring our bytes would leave that
            // writer's chain jumping into freed memory. The guide's rule is the one that applies here -
            // a retained XInput set and a wheel keepalive are INERT after teardown, while every other
            // nonzero reason can still identify live code.
            //
            // So the image may stay mapped, but nothing in it can run. The loader treats that as a
            // retained generation and charges it against its reload budget.
            const std::size_t wheel_pins = diag::module_pin_count(diag::ModulePinReason::MessageHookKeepalive);
            const std::size_t xinput_self = diag::module_pin_count(diag::ModulePinReason::XInputKeepalive);
            const std::size_t xinput_targets = diag::module_pin_count(diag::ModulePinReason::XInputTarget);
            const std::size_t total_pins = diag::total_module_pins();
            const std::size_t inert_pins = wheel_pins + xinput_self + xinput_targets;

            char verdict[400];
            (void)std::snprintf(verdict, sizeof(verdict),
                                "[KCD2_TPVCamera][DEV] unload verdict: wheel=%zu xinput_self=%zu "
                                "xinput_targets=%zu total=%zu (inert=%zu), hooks_restored=%s",
                                wheel_pins, xinput_self, xinput_targets, total_pins, inert_pins,
                                s_hook_restore_failed ? "NO" : "yes");
            append_loader_log(verdict);

            // A wheel keepalive here is EXPECTED while the local MessageHook backend is in use: this
            // image is then retained, which the loader charges against its reload budget. It would only be
            // a defect if the loader had leased its resident host for this generation.
            if (s_hook_restore_failed)
            {
                append_loader_log("[KCD2_TPVCamera][DEV] unload REFUSED: a hooked prologue was not restored");
                return 0;
            }
            if (total_pins != inert_pins)
            {
                append_loader_log("[KCD2_TPVCamera][DEV] unload REFUSED: a pin reason outside the "
                                  "documented-inert set remains; live code may still be reachable");
                return 0;
            }
            return TPVCAMERA_RELOAD_OK;
        }
        catch (...)
        {
            append_loader_log("[KCD2_TPVCamera][DEV] Shutdown threw an exception; refusing unload");
            return 0;
        }
    }
} // extern "C"

/// The loader drives Init and Shutdown explicitly, so attach and detach have no work of their own.
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID)
{
    return TRUE;
}
