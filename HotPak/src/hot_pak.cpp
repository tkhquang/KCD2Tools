/**
 * @file hot_pak.cpp
 * @brief Mod lifecycle: configuration, anchors, engine access, hooks and the pak watcher.
 *
 * init() runs off the Windows loader lock and receives the live Session. shutdown() is driven by the host's detach
 * path (dllmain.cpp in production, the logic DLL's Shutdown() export in the dev build) and must likewise run off the
 * loader lock, because hooks are caller-owned and this is the only path that restores the patched prologues.
 *
 * Threads: the watcher thread polls the build outputs and stages shadow copies; CCryAction::PostUpdate (main thread)
 * mounts them and dispatches the reloads; the clothing manager pass (its own thread) resets the affected proxies.
 */

#include "hot_pak.hpp"
#include "anchors.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "engine.hpp"
#include "pak_watch.hpp"
#include "proxy_reload.hpp"
#include "reload_pipeline.hpp"
#include "text_util.hpp"
#include "version.hpp"
#include "hooks/frame_hook.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace HotPak
{
    namespace
    {
        DMK::hook::HookStack s_hooks;

        /**
         * @brief Finds WHGame.dll, polling briefly because an ASI can attach before the game module maps.
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

        void warn_if_degraded(const DMK::Result<void> &outcome, std::string_view degraded)
        {
            if (!outcome.has_value())
            {
                DMK::log().warning("{} ({})", degraded, outcome.error().message());
            }
        }

        /**
         * @brief Restarts the watcher whenever the INI changes, so a pak can be added or removed in a running game.
         * @details The callback runs on DMK's INI watcher thread. Restarting the pak watcher only touches the watcher
         *          thread and the shadow folder; already mounted overlays and the reset window are left alone.
         */
        void enable_ini_reload()
        {
            const DMK::config::AutoReloadStatus status = DMK::config::enable_auto_reload(
                std::chrono::milliseconds{250},
                [](bool content_changed)
                {
                    if (!content_changed)
                    {
                        return;
                    }
                    WatchSettings settings = watch_settings();
                    DMK::log().info("INI changed: restarting the watcher over {} pak(s)", settings.paks.size());
                    warn_if_degraded(
                        start_pak_watcher(std::move(settings.paks), settings.poll),
                        "Watcher restart failed"
                    );
                }
            );
            if (status != DMK::config::AutoReloadStatus::Started)
            {
                DMK::log().warning("INI auto-reload not started (status {})", static_cast<int>(status));
            }
        }
    } // namespace

    DMK::Result<void> init(DMK::Session &session, std::span<std::uint64_t> persistent_slots)
    {
        DMK::Logger &logger = session.log();
        logger.info("========================================");
        version::log_version_info();

        register_config_items();
        DMK::config::load(constants::get_config_filename());
        DMK::config::log_all();
        WatchSettings settings = watch_settings();

        const DMK::Region game = find_game_module();
        if (game.size == 0)
        {
            logger.error("Init: {} is not loaded", constants::GAME_MODULE);
            return std::unexpected(DMK::Error{DMK::ErrorCode::ProcessMismatch, "HotPak::init/module"});
        }
        logger.info(
            "Init: {} at {} ({} bytes)",
            constants::GAME_MODULE,
            DMK::format::format_address(game.base.raw()),
            game.size
        );

        const Anchors anchors = resolve_anchors(game);
        if (!anchors.core)
        {
            logger.error("Init: the core anchors did not resolve; the tool stays off");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "HotPak::init/anchors"});
        }
        initialize_engine(anchors.genv, game, anchors.engine, persistent_slots);

        reset_reload_pipeline();
        if (anchors.proxy_resets)
        {
            warn_if_degraded(
                initialize_proxy_reload(s_hooks, anchors.proxy_manager_update, anchors.engine),
                "Proxy reload unavailable - changed skins and materials apply only to characters loaded after "
                "the change"
            );
        }
        const std::size_t pak_count = settings.paks.size();
        DMK_TRY_VOID(start_pak_watcher(std::move(settings.paks), settings.poll));
        DMK_TRY_VOID(initialize_frame_hook(s_hooks, anchors.post_update));
        enable_ini_reload();
        logger.info("Init: watching {} pak(s); shadow copies in {}", pak_count, to_utf8(shadow_directory()));
        return {};
    }

    ShutdownResult shutdown() noexcept
    {
        // The retirement switches both detours off and proves them idle before anything they read goes away. Busy
        // and Refused change nothing, so the watcher keeps feeding a tool that still runs.
        const DMK::hook::HookRetirement retirement = s_hooks.retire(constants::HOOK_RETIREMENT_DEADLINE);
        if (retirement.outcome != DMK::hook::RetirementOutcome::Retired || retirement.pinned != 0)
        {
            // A pinned hook keeps its detour reachable, so everything that detour reads stays.
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Shutdown: the hooks did not retire ({}, {} pinned, {} call(s) in a detour); the tool keeps its state",
                DMK::to_string(retirement.refusal),
                retirement.pinned,
                retirement.in_flight
            );
            return ShutdownResult{
                .hooks = retirement,
            };
        }
        // The INI watcher restarts the pak watcher from its own thread, so it stops first.
        DMK::config::disable_auto_reload();
        stop_pak_watcher();
        DMK::config::clear();
        shutdown_engine();
        // A StoppableWorker that did not join keeps its module reference, and its thread still runs this image.
        const bool workers_joined = DMK::diagnostics::module_pin_count(DMK::diagnostics::ModulePinReason::Worker) == 0;
        (void)DMK::log().try_log(
            DMK::LogLevel::Info,
            "Shutdown: hooks retired ({} restored, {} vacated); workers {}",
            retirement.restored,
            retirement.vacated,
            workers_joined ? "joined" : "did not join"
        );
        return ShutdownResult{
            .hooks = retirement,
            .complete = workers_joined,
        };
    }

} // namespace HotPak
