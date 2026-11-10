/**
 * @file animation_framework.cpp
 * @brief Mod lifecycle: configuration, anchors and the hooks.
 *
 * init() runs off the Windows loader lock and receives the live Session. shutdown() runs from the dev build's
 * Shutdown() export and, in production, after a failed init() (the production ASI otherwise lives for the process). It
 * must run off the loader lock too, because hooks are caller-owned and this is the only path that restores the patched
 * prologues.
 *
 * Threads: the frame tick (CharacterManager::Update) runs on the main thread. The database loader and the clip index
 * loaders run where the game loads them, and the option queries run on the game's animation threads.
 */

#include "animation_framework.hpp"
#include "anchors.hpp"
#include "camera_view.hpp"
#include "console_vars.hpp"
#include "custom_tags.hpp"
#include "cutscenes.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "hooks/anim_patch.hpp"
#include "hooks/frame_tick.hpp"
#include "hooks/quest_console.hpp"
#include "hooks/view_variants.hpp"
#include "live_bridge.hpp"
#include "npc_actions.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

namespace AnimationFramework
{
    namespace
    {
        // The game module poll gives up after about 3 s.
        constexpr int GAME_MODULE_POLL_ATTEMPTS = 300;
        constexpr DWORD GAME_MODULE_POLL_INTERVAL_MS = 10;

        DMK::hook::HookStack s_hooks;

        /**
         * @brief Finds WHGame.dll, polling briefly because an ASI can attach before the game module maps.
         * @return The image, or an empty Region.
         */
        [[nodiscard]] DMK::Region find_game_module()
        {
            DMK::Region image{};
            for (int i = 0; i < GAME_MODULE_POLL_ATTEMPTS && image.size == 0; ++i)
            {
                image = DMK::Region::module_named(constants::GAME_MODULE);
                if (image.size == 0)
                {
                    Sleep(GAME_MODULE_POLL_INTERVAL_MS);
                }
            }
            return image;
        }
    } // namespace

    DMK::Result<void> init(DMK::Session &session)
    {
        DMK::Logger &logger = session.log();
        logger.info("========================================");
        version::log_version_info();

        register_config_items();
        DMK::config::load(constants::get_config_filename());
        DMK::config::log_all();
        const Settings settings = settings_snapshot();

        const DMK::Region game = find_game_module();
        if (game.size == 0)
        {
            logger.error("Init: {} is not loaded", constants::GAME_MODULE);
            return std::unexpected(DMK::Error{DMK::ErrorCode::ProcessMismatch, "AnimationFramework::init/module"});
        }
        logger.info(
            "Init: {} at {} ({} bytes)",
            constants::GAME_MODULE,
            DMK::format::format_address(game.base.raw()),
            game.size
        );

        const Anchors anchors = resolve_anchors(game);
        bind_console(anchors.genv, anchors.console_vtable);
        bind_camera_view();
        if (settings.cutscenes)
        {
            bind_cutscenes(anchors);
        }
        // The frame tick carries the late clip registration, the camera-view read and the Blender bridge's requests.
        // The load-time hooks below work without it.
        const auto ticking = install_frame_tick_hook(anchors.character_manager_update, s_hooks);
        if (!ticking.has_value())
        {
            logger.warning(
                "Init: no frame tick ({}); clips register at load only, the camera view is not read and the Blender "
                "add-on cannot reach the game",
                ticking.error().message()
            );
        }
        // The built-in tag set goes out before the tag definition loads, so its declarations land on top of it.
        configure_custom_tags(
            settings.third_person_variants,
            settings.custom_tags,
            settings.custom_tag_priority,
            settings.custom_tag_disabled
        );
        install_anim_patch_hooks(anchors, settings, s_hooks);
        if (!settings.third_person_variants && !settings.custom_tags)
        {
            logger.info("TagVariants: third-person variants and custom tags are off in the INI");
        }
        else if (!settings.animation_patches)
        {
            logger.warning(
                "TagVariants: needs AnimationPatches, which adds the {} tag and mods' custom tags; both are off",
                constants::THIRD_PERSON_TAG
            );
        }
        else
        {
            install_view_variant_hooks(anchors, s_hooks);
        }
        install_npc_actions(anchors, settings.npc_actions, s_hooks);
        if (settings.cutscenes)
        {
            install_quest_console_hooks(anchors, s_hooks);
            install_scene_event_hook(anchors, s_hooks);
            install_scene_layer_id_hook(anchors, s_hooks);
        }
        else
        {
            logger.info("Cutscenes: off in the INI; mods' cutscenes and the quest console command do not run");
        }
        if (!settings.blender_bridge)
        {
            logger.info("Bridge: off in the INI; the Blender add-on's Play in game needs the Lua runner");
        }
        else if (ticking.has_value())
        {
            start_live_bridge(anchors.genv, anchors.script_system_vtable);
        }
        logger.info("Init: ready");
        return {};
    }

    ShutdownResult shutdown() noexcept
    {
        // The console commands go first, on the main thread, while the frame tick that removes them is still armed.
        // A missed frame changes nothing, so the loader can retry the unload later.
        if (!remove_console_commands(constants::CONSOLE_TEARDOWN_DEADLINE))
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Shutdown: the main thread ran no frame within {} ms, so the console commands stay; the framework "
                "keeps its state",
                constants::CONSOLE_TEARDOWN_DEADLINE.count()
            );
            return ShutdownResult{
                .hooks = DMK::hook::HookRetirement{.outcome = DMK::hook::RetirementOutcome::Busy},
            };
        }
        // The retirement switches every detour off and proves it idle before anything it reads goes away. Busy and
        // Refused change nothing, so the framework keeps running as before.
        const DMK::hook::HookRetirement retirement = s_hooks.retire(constants::HOOK_RETIREMENT_DEADLINE);
        if (retirement.outcome != DMK::hook::RetirementOutcome::Retired || retirement.pinned != 0)
        {
            if (retirement.outcome != DMK::hook::RetirementOutcome::Retired)
            {
                resume_frame_work();
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Shutdown: the hooks did not retire ({}, {} pinned, {} call(s) in a detour); the framework keeps its "
                "state",
                DMK::to_string(retirement.refusal),
                retirement.pinned,
                retirement.in_flight
            );
            return ShutdownResult{
                .hooks = retirement,
            };
        }
        // The frame tick that ran its requests is gone, so the worker only has its pipe left to close.
        stop_live_bridge();
        unbind_camera_view();
        reset_custom_tags();
        unbind_console();
        DMK::config::clear();
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
} // namespace AnimationFramework
