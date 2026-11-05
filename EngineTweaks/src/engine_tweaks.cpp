/**
 * @file engine_tweaks.cpp
 * @brief Mod lifecycle: configuration, anchors, the hooks of every fix and the INI auto-reload.
 *
 * init() runs off the Windows loader lock and receives the live Session. shutdown() runs after a failed init in
 * production, and from the logic DLL's Shutdown() export in the dev build. It must run off the loader lock, because
 * the hooks are caller-owned and shutdown() is the only path that restores the patched prologues.
 *
 * A fix whose anchors did not resolve stays off, and the others still install. Each detour reads its INI switch per
 * call, so the INI watcher only publishes the new values.
 */

#include "engine_tweaks.hpp"
#include "anchors.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "fixes/diag.hpp"
#include "fixes/fixes.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <chrono>
#include <string_view>

namespace EngineTweaks
{
    namespace
    {
        DMK::hook::HookStack s_hooks;

        /**
         * @brief Finds WHGame.dll, with a short poll, because an ASI can attach before the game module maps.
         * @return The image, or an empty Region.
         */
        [[nodiscard]] DMK::Region find_game_module()
        {
            DMK::Region image{};
            for (int i = 0; i < constants::GAME_MODULE_POLL_ATTEMPTS && image.size == 0; ++i)
            {
                image = DMK::Region::module_named(constants::GAME_MODULE);
                if (image.size == 0)
                {
                    Sleep(static_cast<DWORD>(constants::GAME_MODULE_POLL_INTERVAL.count()));
                }
            }
            return image;
        }

        /** @brief Starts the INI watcher, which republishes every switch when the file changes. */
        void enable_ini_reload(const DMK::config::Ini &ini)
        {
            const DMK::config::AutoReloadStatus status = ini.enable_auto_reload(
                constants::INI_RELOAD_INTERVAL,
                [](bool content_changed) -> void
                {
                    if (content_changed)
                    {
                        (void)DMK::log().try_log(DMK::LogLevel::Info, "INI changed: the fixes take the new switches");
                        try
                        {
                            DMK::config::log_all();
                        }
                        catch (...)
                        {
                            // The settings dump is diagnostics only.
                        }
                    }
                }
            );
            if (status != DMK::config::AutoReloadStatus::Started)
            {
                DMK::log().warning("INI auto-reload not started (status {})", static_cast<int>(status));
            }
        }

        /** @brief Logs a fix whose hook install failed. That fix stays off and the others stay on. */
        void install_fix(std::string_view name, const DMK::Result<void> &installed)
        {
            if (!installed)
            {
                DMK::log()
                    .error("Fix {}: hook install failed ({}), so the fix stays off", name, installed.error().message());
            }
        }
    } // namespace

    DMK::Result<void> init(DMK::Session &session, const WheelHostTable *, std::span<std::uint64_t>)
    {
        DMK::Logger &logger = session.log();
        logger.info("========================================");
        version::log_version_info();

        const DMK::config::Ini ini = session.ini();
        register_config_items();
        ini.load(constants::get_config_filename());
        ini.log_all();

        const DMK::Region game = find_game_module();
        if (game.size == 0)
        {
            logger.error("Init: {} is not loaded", constants::GAME_MODULE);
            return std::unexpected(DMK::Error{DMK::ErrorCode::ProcessMismatch, "EngineTweaks::init/module"});
        }
        logger.info(
            "Init: {} at {} ({} bytes)",
            constants::GAME_MODULE,
            DMK::format::format_address(game.base.raw()),
            game.size
        );

        const auto started = std::chrono::steady_clock::now();
        const Anchors anchors = resolve_anchors(game);
        logger.info(
            "Init: anchors resolved in {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()
        );
        if (!anchors.menu_view_distance && !anchors.link_index && !anchors.inventory_refresh &&
            !anchors.perk_copy_fix && !anchors.sleep_prediction_fix)
        {
            logger.error("Init: no fix is available, so the mod stays inert");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "EngineTweaks::init/anchors"});
        }

        install_fix("MenuViewDistance", fixes::install_menu_view_distance(anchors, s_hooks));
        install_fix("LinkIndex", fixes::install_link_index(anchors, s_hooks));
        install_fix("InventoryRefresh", fixes::install_inventory_refresh(anchors, s_hooks));
        install_fix("PerkCopy", fixes::install_perk_copy(anchors, s_hooks));
        install_fix("SleepPrediction", fixes::install_sleep_prediction(anchors, s_hooks));

        enable_ini_reload(ini);
        logger.info("Init: ready");
        return {};
    }

    ShutdownResult shutdown() noexcept
    {
        fixes::diag::log_summary_now();
        // The retirement switches every detour off and proves it idle before anything it reads goes away. Busy and
        // Refused leave the hooks armed, so their state must stay too.
        const DMK::hook::HookRetirement retirement = s_hooks.retire(constants::HOOK_RETIREMENT_DEADLINE);
        if (retirement.outcome != DMK::hook::RetirementOutcome::Retired || retirement.pinned != 0)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Shutdown: the hooks did not retire ({}, {} pinned, {} call(s) in a detour), so the mod keeps its state",
                DMK::to_string(retirement.refusal),
                retirement.pinned,
                retirement.in_flight
            );
            return ShutdownResult{
                .hooks = retirement,
            };
        }
        DMK::config::disable_auto_reload();
        fixes::reset_link_index();
        fixes::reset_sleep_prediction();
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
} // namespace EngineTweaks
