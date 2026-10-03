/**
 * @file henry_senses.cpp
 * @brief Mod lifecycle: configuration, anchors, hooks, backends and input bindings.
 *
 * init() runs off the Windows loader lock and receives the live Session. shutdown() is driven by the host's detach
 * path (dllmain.cpp in production, the logic DLL's Shutdown() export in the dev build) and must likewise run off the
 * loader lock, because hooks are caller-owned and this is the only path that restores the patched prologues and the
 * engine fields the highlight wrote.
 *
 * Init order matters for the engine-silhouette backend: the custom stage must be registered inside
 * CStandardGraphicsPipeline::Init, which runs once when the renderer is created, shortly after WHGame.dll maps. So
 * the three early anchors are resolved and P1/P2 armed before anything else (the full anchor table takes much
 * longer); when that still arrives too late, the render-thread late attach takes over. Under the dev loader, a stage
 * the previous generation unpublished is adopted instead of registered again.
 *
 * Render hooks own stage registration, mask submission and composition. CCryAction::PostUpdate owns the main-thread
 * tick. The game-state gates use guarded memory reads.
 */

#include "henry_senses.hpp"
#include "aob_resolver.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "game_interface.hpp"
#include "game_state.hpp"
#include "global_state.hpp"
#include "offset_heal.hpp"
#include "rtti_types.hpp"
#include "version.hpp"
#include "detect/loot_scanner.hpp"
#include "engine/engine_env.hpp"
#include "engine/entity_access.hpp"
#include "engine/game_natives.hpp"
#include "engine/herb_scan.hpp"
#include "engine/interactables.hpp"
#include "highlight/controller.hpp"
#include "highlight/groups.hpp"
#include "highlight/registry.hpp"
#include "hooks/frame_hook.hpp"
#include "hooks/hook_set.hpp"
#include "render/aux_markers.hpp"
#include "render/engine_silhouette.hpp"
#include "render/herb_outline.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace HenrySenses
{
    namespace
    {
        // Every hook the mod installs, disabled and destroyed newest first: the early P1/P2 hooks are pushed first, so
        // they are removed last (P4 before P2, as the stage requires). A teardown whose detours did not drain keeps
        // them here, disabled, so their trampolines stay allocated.
        HookSet s_hooks;

        // Teardown latch. It lives at namespace scope so init() can re-arm it: a stale-image reload (an image kept
        // mapped by a pinned hook) hands back these statics intact, and a latch that survived would make the next
        // shutdown() a silent no-op.
        std::atomic<bool> s_teardown_done{false};
        std::atomic<bool> s_unload_safe{true};

        // How long shutdown waits for the main-thread tick to restore the nodes. A loading screen can hold one
        // PostUpdate for several seconds, so this matches the frame-hook rundown.
        constexpr std::chrono::milliseconds MAIN_THREAD_SHUTDOWN_TIMEOUT{10000};
        // How long shutdown waits for the render thread to unpublish the stage. A pipeline that renders no frame in
        // this window keeps its stage published.
        constexpr std::chrono::milliseconds RENDER_THREAD_UNPUBLISH_TIMEOUT{3000};

        /**
         * @brief Resolves the game module base and size into module_info().
         * @details Polls for up to about 3 seconds because an ASI can attach before WHGame.dll finishes mapping.
         *          Every scan and identity sweep in the mod is confined to the region stored here.
         * @return An empty value, or ProcessMismatch when WHGame.dll never mapped.
         */
        [[nodiscard]] DMK::Result<void> validate_game_module()
        {
            DMK::Logger &logger = DMK::log();

            DMK::Region image{};
            for (int i = 0; i < 300 && image.size == 0; ++i)
            {
                image = DMK::Region::module_named(constants::MODULE_NAME);
                if (image.size == 0)
                {
                    Sleep(10);
                }
            }

            if (image.size == 0)
            {
                logger.error("Failed to find module: {}", constants::MODULE_NAME);
                return std::unexpected(DMK::Error{DMK::ErrorCode::ProcessMismatch, "HenrySenses::init/module"});
            }

            ModuleInfo &mod = module_info();
            mod.base = image.base.raw();
            mod.size = image.size;

            // The ASLR-insensitive PE build identity makes a patch-day log self-diagnosing: two logs whose tokens
            // differ are two different game builds, so an anchor miss is drift rather than a broken install.
            const DMK::scan::ImageIdentity identity = DMK::scan::image_identity(image);
            logger.info(
                "Module validated: {} (size {} bytes, build {:#x}, timestamp {:#x})",
                DMK::format::format_address(mod.base),
                mod.size,
                identity.token(),
                identity.timestamp
            );
            return {};
        }

        /**
         * @brief Reports a best-effort subsystem that refused to initialize, keeping the mod running.
         * @param outcome The subsystem's result.
         * @param degraded What the mod loses when this subsystem is absent.
         */
        void warn_if_degraded(const DMK::Result<void> &outcome, std::string_view degraded)
        {
            if (!outcome.has_value())
            {
                DMK::log().warning("{} ({})", degraded, outcome.error().message());
            }
        }

        /**
         * @brief Disables every hook, drains the detours, and destroys the hooks only after both drains succeeded.
         * @details Destroying an inline hook frees its trampoline, and a thread that took the patch jump just before
         *          the prologue was restored still calls the original through it. So the prologues are restored first
         *          with the trampolines kept, the detours drain, and only then are the handles destroyed. A hook that
         *          cannot be disabled leaves its detour reachable, so nothing is drained or destroyed. Hooks that are
         *          not destroyed stay in s_hooks with their trampolines allocated.
         * @return True when every hook is gone with its prologue restored and no thread is left inside a detour.
         */
        [[nodiscard]] bool retire_hooks()
        {
            DMK::Logger &logger = DMK::log();
            if (s_hooks.empty())
            {
                return true;
            }
            if (!s_hooks.disable_all())
            {
                logger.error(
                    "Hooks: a hook could not be disabled, so its detour stays reachable; every hook is kept "
                    "and the module must stay mapped"
                );
                return false;
            }
            const bool frame_drained = rundown_frame_hook();
            const bool render_drained = shutdown_engine_silhouette();
            if (!frame_drained || !render_drained)
            {
                logger.error(
                    "Hooks: a detour did not drain; the hooks stay disabled with their trampolines allocated, "
                    "and the module must stay mapped"
                );
                return false;
            }
            // A hook that cannot prove it restored its target pins its backend; the leak-count delta reports it.
            const std::size_t pins_before =
                DMK::diagnostics::intentional_leak_count(DMK::diagnostics::LeakSubsystem::HookManager);
            s_hooks.clear();
            if (DMK::diagnostics::intentional_leak_count(DMK::diagnostics::LeakSubsystem::HookManager) != pins_before)
            {
                logger.error(
                    "Hooks: a hook could not restore its prologue and pinned its backend; the module must "
                    "stay mapped"
                );
                return false;
            }
            return true;
        }

        /**
         * @brief Resolves the early anchors and arms the stage-registration hooks (P1, P2).
         * @details Runs before everything else in init because CStandardGraphicsPipeline::Init runs once, when the
         *          renderer is created. Best-effort: a miss leaves the late attach as the only registration path.
         */
        void initialize_early_render_hooks()
        {
            const ModuleInfo &mod = module_info();
            const auto started = std::chrono::steady_clock::now();
            resolve_early_anchors(mod.base, mod.size);
            warn_if_degraded(
                install_engine_silhouette_early(s_hooks),
                "Engine silhouette P1/P2 unavailable - the custom stage cannot be registered"
            );
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            DMK::log().info("Early render hooks done in {} ms", elapsed.count());
        }

        /**
         * @brief Resolves the anchors and installs every subsystem in dependency order.
         * @details gEnv is mandatory (every engine interface hangs off it), so its Error propagates verbatim. Every
         *          other subsystem is best-effort and degrades to a warning. The frame hook is installed last, so the
         *          first tick sees every subsystem ready.
         * @return An empty value, or the mandatory step's Error.
         */
        [[nodiscard]] DMK::Result<void> initialize_subsystems()
        {
            DMK::Logger &logger = DMK::log();
            const ModuleInfo &mod = module_info();

            resolve_all_anchors(mod.base, mod.size);

            // Build the cached class identities over the WHGame.dll image before any hook arms, so later "is this
            // vtable X" questions are pointer compares instead of RTTI walks.
            init_game_types(DMK::Region{DMK::Address{mod.base}, mod.size});

            // Start the self-heal scheduler before any base is published, so the first main-thread step can scan.
            start_offset_heal();

            if (auto env = initialize_engine_env(); !env.has_value())
            {
                logger.error(
                    "Critical: gEnv did not resolve ({}); the mod cannot reach the engine",
                    env.error().message()
                );
                return env;
            }

            warn_if_degraded(
                initialize_game_interface(),
                "Game interface unavailable - dialogue / combat / minigame gates disabled"
            );

            warn_if_degraded(initialize_entity_access(), "Entity access unavailable - highlights disabled");
            warn_if_degraded(initialize_game_natives(), "Game natives unavailable - loot detection degraded");
            warn_if_degraded(initialize_loot_scanner(), "Loot scanner unavailable - no loot detection");
            warn_if_degraded(initialize_registry(), "Highlight registry unavailable - highlights disabled");
            warn_if_degraded(
                initialize_engine_silhouette(s_hooks),
                "Engine silhouette unavailable - Backend=Engine disabled"
            );
            warn_if_degraded(initialize_aux_markers(), "Aux markers unavailable - Backend=Markers disabled");
            warn_if_degraded(initialize_herb_scan(), "Herb scan unavailable - herbs are not highlighted");
            (void)initialize_herb_outline();
            warn_if_degraded(
                initialize_interactables(),
                "Interactable scan unavailable - interactive objects are not highlighted"
            );
            warn_if_degraded(initialize_controller(), "Controller unavailable");
            warn_if_degraded(initialize_frame_hook(s_hooks), "Main-thread tick unavailable - no scans, no markers");

            return {};
        }

        /**
         * @brief Starts the INI hot-reload watcher.
         * @details The bound atomic setters re-apply the live settings on each reload; the callback then reads the
         *          highlight groups again (with the new [Settings] fallbacks) and tells the controller, whose next
         *          tick scans again at once.
         */
        void enable_hot_reload()
        {
            DMK::Logger &logger = DMK::log();

            const DMK::config::AutoReloadStatus status = DMK::config::enable_auto_reload(
                std::chrono::milliseconds{250},
                [](bool content_changed)
                {
                    DMK::Logger &reload_logger = DMK::log();
                    reload_groups();
                    if (content_changed)
                    {
                        controller_notify_config_changed();
                        reload_logger.info("INI auto-reload: live settings applied");
                    }
                    else
                    {
                        reload_logger.info("INI auto-reload: no content change");
                    }
                }
            );

            if (status == DMK::config::AutoReloadStatus::Started)
            {
                logger.info("INI hot-reload watcher started (250 ms debounce)");
            }
            else
            {
                logger.warning("INI hot-reload watcher not started (status {})", static_cast<int>(status));
            }
        }
    } // namespace

    DMK::Result<void>
    init(DMK::Session &session, const WheelHostTable *wheel_host, std::span<std::uint64_t> persistent_slots)
    {
        DMK::Logger &logger = session.log();
        logger.info("========================================");
        version::log_version_info();

        // A refused teardown of this image can have kept its hooks, disabled, because a detour did not drain. They are
        // retired before anything is installed again: this kit refuses a second hook on a target it still tracks.
        // Until they are, the teardown latch stays set, so the failure path's shutdown() keeps its refusal.
        if (!s_hooks.empty())
        {
            logger.warning(
                "Init: {} hook(s) kept by an earlier teardown of this image; retiring them first",
                s_hooks.size()
            );
            if (!retire_hooks())
            {
                logger.error("Init: the kept hooks could not be retired; this image stays inert and mapped");
                return std::unexpected(DMK::Error{DMK::ErrorCode::InvalidHookState, "HenrySenses::init/kept_hooks"});
            }
        }

        // Re-arm the teardown latch before anything is installed (see s_teardown_done).
        s_unload_safe.store(true, std::memory_order_relaxed);
        s_teardown_done.store(false, std::memory_order_release);

        register_config_items();
        DMK::config::load(constants::get_config_filename());
        // The groups read their own [Highlight.<Name>] sections, with the [Settings] fallbacks just loaded, and
        // register their hotkeys before the input engine starts.
        if (auto groups = initialize_groups(); !groups)
        {
            logger.warning("Highlight groups unavailable ({}); nothing is highlighted", groups.error().message());
        }

        if (auto validated = validate_game_module(); !validated)
        {
            return validated;
        }

        // The stage-registration hooks go in before the (slow) rest of init; see the file comment. The stash binds
        // first, so P4 can adopt a stage from its first frame.
        bind_engine_stage_stash(persistent_slots);
        initialize_early_render_hooks();

        DMK::config::log_all();

        // The memory cache accelerates readability checks; a failure falls back to direct syscalls.
        if (DMK::memory::init_cache())
        {
            logger.info("Memory cache system initialized");
        }
        else
        {
            logger.warning("Memory cache init failed; readability checks fall back to syscalls");
        }

        DMK_TRY_VOID(initialize_subsystems());

        // A resident wheel host books the wheel keepalive against the dev loader so this image can unmap; without
        // one, the local MessageHook backend is correct for the never-unloaded release ASI.
        DMK::input::Input::Settings input_settings{};
        if (wheel_host != nullptr)
        {
            input_settings.wheel_backend = DMK::input::Input::WheelBackend::ExternalHost;
            input_settings.wheel_host = wheel_host;
            input_settings.wheel_host_required = true;
        }
        set_group_input_settings(input_settings);
        if (auto started = DMK::input::Input::instance().start(input_settings); !started.has_value())
        {
            logger.error("Input engine failed to start ({}); hotkeys unavailable", started.error().message());
            return std::unexpected(started.error());
        }
        logger.info(
            "Input engine started ({} wheel backend)",
            wheel_host != nullptr ? "resident-host" : "local MessageHook"
        );

        enable_hot_reload();

        logger.info("Initialization completed successfully");
        return {};
    }

    bool shutdown() noexcept
    {
        if (s_teardown_done.exchange(true, std::memory_order_acq_rel))
        {
            return s_unload_safe.load(std::memory_order_acquire);
        }

        // An exception before the final commit must leave every later shutdown call unable to authorize unload.
        s_unload_safe.store(false, std::memory_order_release);
        try
        {
            DMK::Logger &logger = DMK::log();
            logger.info("Shutdown: starting teardown");

            // Stop the INI watcher so no reload setter runs during teardown, then release the group hotkeys.
            DMK::config::disable_auto_reload();
            shutdown_groups();

            // Restore every highlighted node on the main thread, while the hooks that read the words are still
            // installed. Node writes and 3D-engine calls race the main thread from here, so a tick that does not answer
            // leaves them undone: the words go inert once P3 (their only reader) and P4 (the only consumer of list 27)
            // are removed.
            const MainThreadShutdown main_shutdown = controller_request_shutdown(MAIN_THREAD_SHUTDOWN_TIMEOUT);
            switch (main_shutdown)
            {
            case MainThreadShutdown::Completed:
                logger.info("Shutdown: main thread restored the highlighted nodes");
                break;
            case MainThreadShutdown::NeverTicked:
                logger.info("Shutdown: the main-thread tick never ran; nothing to restore");
                break;
            case MainThreadShutdown::Abandoned:
                logger.warning(
                    "Shutdown: the main-thread tick did not answer; native effects and node state may remain, "
                    "so the module must stay mapped"
                );
                break;
            case MainThreadShutdown::Unfinished:
            default:
                logger.warning("Shutdown: the main thread did not finish restoring the highlighted nodes in time");
                break;
            }

            // Take the stage off its pipeline while P2 still guards it and P4 still runs.
            // After this, P+0x50 is null again, so the engine's custom-PSO block and debug views are gated off before
            // P2 goes. The unpublished stage is never freed: objects compiled while it was published keep PSOs built
            // from its passes.
            const std::size_t stages_kept = unpublish_engine_stage(RENDER_THREAD_UNPUBLISH_TIMEOUT);
            if (stages_kept != 0)
            {
                logger.warning(
                    "Shutdown: {} silhouette stage(s) remain published; the guard hooks and module stay active",
                    stages_kept
                );
                // P2 prevents incomplete debug-view PSOs from hiding objects. A stalled pipeline can resume at any
                // time, so keep its guard and the session that supports it until process exit. P4 can still finish
                // the pending unpublish request when that pipeline renders again.
                disarm_frame_hook();
                return false;
            }

            // Make every detour pass straight through before the patches are removed.
            disarm_frame_hook();
            disarm_engine_silhouette();
            // Hooks are caller-owned, so this is where the patched prologues are restored.
            const bool hooks_retired = retire_hooks();
            if (!hooks_retired)
            {
                return false;
            }

            shutdown_loot_scanner();
            shutdown_aux_markers();
            shutdown_herb_scan();
            shutdown_herb_outline();
            shutdown_interactables();

            // The frame hook has drained, so the self-heal report cannot change during this copy.
            std::array<DMK::rtti::DriftEntry, 8> drift_report{};
            const std::size_t drift_count = offset_heal_drift_report(drift_report);
            const DMK::diagnostics::Snapshot diag = DMK::diagnostics::collect(
                std::span<const DMK::rtti::DriftEntry>(drift_report.data(), drift_count),
                anchor_report()
            );
            logger.info(
                "Diagnostics: {} hooks ({} active, {} disabled), {} intentional leaks",
                diag.hooks_total,
                diag.hooks_active,
                diag.hooks_disabled,
                diag.total_intentional_leaks
            );
            logger.info(
                "Diagnostics: self-heal {}/{} landmarks healed; anchors {}/{} resolved ({} failed)",
                diag.drift_healed,
                diag.drift_total,
                diag.anchor_quality.resolved,
                diag.anchor_quality.total,
                diag.anchor_quality.failed
            );

            // Removing the hooks makes silhouette words inert, but it does not restore focus settings or remove
            // native particle emitters. An abandoned request must retain their owner and stop generation swaps.
            const bool unload_safe =
                main_shutdown == MainThreadShutdown::Completed || main_shutdown == MainThreadShutdown::NeverTicked;

            // Drop the config registry's bound setters. The group hotkeys were released by shutdown_groups() above.
            DMK::config::clear();

            shutdown_game_natives();
            shutdown_entity_access();
            cleanup_game_interface();
            shutdown_engine_env();

            logger.info("Shutdown: teardown complete ({})", unload_safe ? "safe to unload" : "module must stay mapped");
            s_unload_safe.store(unload_safe, std::memory_order_release);
            return unload_safe;
        }
        catch (...)
        {
            disarm_frame_hook();
            disarm_engine_silhouette();
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Shutdown: cleanup failed; the module stays mapped");
            return false;
        }
    }

} // namespace HenrySenses
