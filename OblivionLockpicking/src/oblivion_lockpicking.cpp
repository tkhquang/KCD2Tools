/**
 * @file oblivion_lockpicking.cpp
 * @brief Mod lifecycle: configuration, anchors, the hooks and the INI auto-reload.
 *
 * init() runs off the Windows loader lock and receives the live Session. shutdown() runs after a failed init in
 * production, and from the logic DLL's Shutdown() export in the dev build. It must run off the loader lock, because
 * the hooks are caller-owned and shutdown() is the only path that restores the patched prologues.
 *
 * Threads: C_LockPicking::Update runs the tumbler lock and drives the mod's Scaleform movie, on the thread where the
 * game drives its own lockpicking view. The game's input dispatch delivers key events to the listener on the main
 * thread. The INI watcher thread stores each new setting in an atomic and logs the change.
 */

#include "oblivion_lockpicking.hpp"
#include "anchors.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "controls/lock_controls.hpp"
#include "game/lockpicking.hpp"
#include "hooks/lockpicking_hook.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <chrono>

namespace OblivionLockpicking
{
    namespace
    {
        DMK::hook::HookStack s_hooks;
        /// The settings the INI change log reads. Only the INI watcher thread touches it.
        SettingsCache s_reload_settings;

        /**
         * @brief Finds WHGame.dll, with a short poll, because an ASI can attach before the game module maps.
         * @return The image, or an empty Region.
         */
        [[nodiscard]] DMK::Region find_game_module()
        {
            DMK::Region image{};
            for (int i = 0; i < 300 && image.size == 0; ++i)
            {
                image = DMK::Region::module_named(constants::GAME_MODULE);
                if (image.size == 0)
                {
                    Sleep(10);
                }
            }
            return image;
        }

        /** @brief Logs INI changes. The bound atomics already carry the new values to the next frame. */
        void enable_ini_reload()
        {
            const DMK::config::AutoReloadStatus status = DMK::config::enable_auto_reload(
                std::chrono::milliseconds{250},
                [](bool content_changed) -> void
                {
                    if (!content_changed)
                    {
                        return;
                    }
                    const Settings &settings = s_reload_settings.refresh();
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Info,
                        "INI changed: enabled {}, time scale {:.2f}, speed {:.2f}-{:.2f}, "
                        "keep on fail {}, one at a time {}",
                        settings.enabled,
                        settings.time_scale,
                        settings.speed_no_skill,
                        settings.speed_full_skill,
                        keep_on_fail_name(settings.keep_set_tumblers_on_fail),
                        settings.one_tumbler_at_a_time
                    );
                }
            );
            if (status != DMK::config::AutoReloadStatus::Started)
            {
                DMK::log().warning("INI auto-reload not started (status {})", static_cast<int>(status));
            }
        }

        /** @brief The Busy result that makes the caller retry. It logs what the shutdown waits for. */
        [[nodiscard]] ShutdownResult busy(const char *waiting_for) noexcept
        {
            (void)DMK::log().try_log(DMK::LogLevel::Info, "Shutdown: waiting for {}", waiting_for);
            return ShutdownResult{
                .hooks = DMK::hook::HookRetirement{.outcome = DMK::hook::RetirementOutcome::Busy, .in_flight = 1},
            };
        }
    } // namespace

    DMK::Result<void> init(DMK::Session &session, const WheelHostTable *, std::span<std::uint64_t>)
    {
        DMK::Logger &logger = session.log();
        logger.info("========================================");
        version::log_version_info();

        register_config_items();
        DMK::config::load(constants::get_config_filename());
        DMK::config::log_all();

        const DMK::Region game = find_game_module();
        if (game.size == 0)
        {
            logger.error("Init: {} is not loaded", constants::GAME_MODULE);
            return std::unexpected(DMK::Error{DMK::ErrorCode::ProcessMismatch, "OblivionLockpicking::init/module"});
        }
        logger.info(
            "Init: {} at {} ({} bytes)",
            constants::GAME_MODULE,
            DMK::format::format_address(game.base.raw()),
            game.size
        );

        const Anchors anchors = resolve_anchors(game);
        if (!anchors.minigame)
        {
            logger.error("Init: an anchor of the minigame did not resolve, so the vanilla lockpicking stays untouched");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "OblivionLockpicking::init/anchors"});
        }
        game::bind(anchors);
        DMK_TRY_VOID(install_lockpicking_hook(s_hooks, anchors.lockpicking_update));
        // The failure text hook is optional. Without it, a wrong set also shows "Lockpicking attempt failed".
        if (anchors.quiet_failure)
        {
            if (const auto hud = install_hud_message_hook(s_hooks, anchors.hud_lockpicking_message); !hud)
            {
                logger.warning(
                    "Init: the HUD message hook is not installed ({}), so wrong sets show the game's failure text",
                    hud.error().message()
                );
            }
        }
        enable_ini_reload();
        logger.info("Init: ready");
        return {};
    }

    ShutdownResult shutdown() noexcept
    {
        // The engine reads its listener set without a lock while it dispatches input, so only the main thread can
        // take the input listener out, at its next input event or play frame. Until then the lock gets no key input.
        if (!controls::release_listener())
        {
            return busy("the input listener to detach");
        }
        // The retirement switches the detours off and proves them idle before anything they read goes away. Busy and
        // Refused leave the hooks armed, so the mod keeps its state and takes its input listener back.
        const DMK::hook::HookRetirement retirement = s_hooks.retire(constants::HOOK_RETIREMENT_DEADLINE);
        if (retirement.outcome != DMK::hook::RetirementOutcome::Retired || retirement.pinned != 0)
        {
            controls::keep_listener();
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Shutdown: the hooks did not retire ({}, {} pinned, {} call(s) in a detour), so the mod keeps its "
                "state",
                DMK::to_string(retirement.refusal),
                retirement.pinned,
                retirement.in_flight
            );
            return ShutdownResult{
                .hooks = retirement,
            };
        }
        // A play frame that ran inside the drain can attach the listener after the first check. No frame runs after
        // the retirement, so this check is final. The anchors stay bound until the main thread detaches it.
        if (!controls::release_listener())
        {
            return busy("the input listener to detach");
        }
        DMK::config::disable_auto_reload();
        reset_lockpicking_session();
        game::unbind();
        DMK::config::clear();
        // A StoppableWorker that did not join keeps its module reference, and its thread still runs this image.
        const bool workers_joined = DMK::diagnostics::module_pin_count(DMK::diagnostics::ModulePinReason::Worker) == 0;
        (void)DMK::log().try_log(
            DMK::LogLevel::Info,
            "Shutdown: hooks retired ({} restored, {} vacated), workers {}",
            retirement.restored,
            retirement.vacated,
            workers_joined ? "joined" : "did not join"
        );
        return ShutdownResult{
            .hooks = retirement,
            .complete = workers_joined,
        };
    }
} // namespace OblivionLockpicking
