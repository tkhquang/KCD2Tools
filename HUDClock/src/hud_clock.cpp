/**
 * @file hud_clock.cpp
 * @brief Mod lifecycle: configuration, anchors, the toggle key, the frame hook and the INI auto-reload.
 *
 * init() runs off the Windows loader lock and receives the live Session. shutdown() runs after a failed init in
 * production, and from the logic DLL's Shutdown() export in the dev build. It must run off the loader lock, because
 * the hook is caller-owned and shutdown() is the only path that restores the patched prologue.
 *
 * Threads: the clock runs on the main thread, in the CharacterManager::Update detour. The INI watcher thread stores
 * each new setting and publishes it for the next frame. DMK's input thread flips the clock on the toggle key.
 */

#include "hud_clock.hpp"
#include "anchors.hpp"
#include "clock.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "game/game_memory.hpp"
#include "hooks/frame_tick.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <chrono>

namespace HUDClock
{
    namespace
    {
        DMK::hook::HookStack s_hooks;

        /// How long shutdown() waits for a frame to hide the clock.
        constexpr int STOP_WAIT_STEPS = 50;
        constexpr DWORD STOP_WAIT_STEP_MS = 10;

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

        /** @brief Publishes each INI change to the next frame. */
        void enable_ini_reload(const DMK::config::Ini &ini)
        {
            const DMK::config::AutoReloadStatus status = ini.enable_auto_reload(
                std::chrono::milliseconds{250},
                [](bool content_changed) -> void
                {
                    if (!content_changed)
                    {
                        return;
                    }
                    publish_settings();
                    (void)DMK::log().try_log(DMK::LogLevel::Info, "INI changed: the clock takes the new settings");
                }
            );
            if (status != DMK::config::AutoReloadStatus::Started)
            {
                DMK::log().warning("INI auto-reload not started (status {})", static_cast<int>(status));
            }
        }

        /**
         * @brief Registers ToggleKey, which pins or hides the clock, and ShowKey, which shows it for ShowSeconds.
         * @details Both are hold bindings, so the frame sees how long a key stays down (HoldTimeInSeconds). Each also
         *          gets <Key>.Consume, on by default, which hides its controller trigger button from the game.
         */
        void register_keys(DMK::Session &session)
        {
            const DMK::config::SectionBinder visibility = DMK::config::section("Visibility");
            session.scope().add(visibility.hold_combo(
                "ToggleKey",
                "Toggle Key",
                "toggle_clock",
                [](bool down) { clock::key_changed(clock::Key::Toggle, down); },
                "",
                true
            ));
            session.scope().add(visibility.hold_combo(
                "ShowKey",
                "Show Key",
                "show_clock",
                [](bool down) { clock::key_changed(clock::Key::Show, down); },
                "",
                true
            ));
        }

        /** @brief Starts the input engine for the keys. A resident host keeps a wheel keepalive in the dev loader. */
        void start_input(DMK::Session &session, const WheelHostTable *wheel_host)
        {
            DMK::input::Input::Settings input_settings{};
            if (wheel_host != nullptr)
            {
                input_settings.wheel_backend = DMK::input::Input::WheelBackend::ExternalHost;
                input_settings.wheel_host = wheel_host;
                input_settings.wheel_host_required = true;
            }
            // The clock works without the keys, so a failed start only costs them.
            if (auto started = session.input().start(input_settings); !started.has_value())
            {
                session.log().warning(
                    "Input engine not started ({}), so the keys do nothing",
                    started.error().message()
                );
            }
        }
    } // namespace

    DMK::Result<void> init(DMK::Session &session, const WheelHostTable *wheel_host, std::span<std::uint64_t>)
    {
        DMK::Logger &logger = session.log();
        logger.info("========================================");
        version::log_version_info();

        const DMK::config::Ini ini = session.ini();
        register_config_items();
        register_keys(session);
        ini.load(constants::get_config_filename());
        ini.log_all();
        publish_settings();

        const DMK::Region game = find_game_module();
        if (game.size == 0)
        {
            logger.error("Init: {} is not loaded", constants::GAME_MODULE);
            return std::unexpected(DMK::Error{DMK::ErrorCode::ProcessMismatch, "HUDClock::init/module"});
        }
        logger.info(
            "Init: {} at {} ({} bytes)",
            constants::GAME_MODULE,
            DMK::format::format_address(game.base.raw()),
            game.size
        );

        const Anchors anchors = resolve_anchors(game);
        if (!anchors.clock)
        {
            logger.error("Init: an anchor of the clock did not resolve, so the mod stays inert");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "HUDClock::init/anchors"});
        }
        game::bind(anchors);
        DMK_TRY_VOID(install_frame_tick_hook(anchors.character_manager_update, s_hooks));
        start_input(session, wheel_host);
        enable_ini_reload(ini);
        logger.info("Init: ready");
        return {};
    }

    ShutdownResult shutdown() noexcept
    {
        // Only the main thread may touch the element. Its next frame hides the clock and stops drawing it.
        if (game::bound_anchors() != nullptr)
        {
            clock::request_stop();
            for (int i = 0; i < STOP_WAIT_STEPS && !clock::stopped(); ++i)
            {
                Sleep(STOP_WAIT_STEP_MS);
            }
            if (!clock::stopped())
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Info,
                    "Shutdown: no frame ran to hide the clock, so it keeps its last text"
                );
            }
        }
        // The retirement switches the detour off and proves it idle before anything it reads goes away. Busy and
        // Refused leave the hook armed, and the stop request keeps the clock hidden until a retry.
        const DMK::hook::HookRetirement retirement = s_hooks.retire(constants::HOOK_RETIREMENT_DEADLINE);
        if (retirement.outcome != DMK::hook::RetirementOutcome::Retired || retirement.pinned != 0)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Shutdown: the hook did not retire ({}, {} pinned, {} call(s) in a detour), so the mod keeps its state",
                DMK::to_string(retirement.refusal),
                retirement.pinned,
                retirement.in_flight
            );
            return ShutdownResult{
                .hooks = retirement,
            };
        }
        DMK::config::disable_auto_reload();
        clock::reset();
        game::unbind();
        DMK::config::clear();
        // A StoppableWorker that did not join keeps its module reference, and its thread still runs this image.
        const bool workers_joined = DMK::diagnostics::module_pin_count(DMK::diagnostics::ModulePinReason::Worker) == 0;
        (void)DMK::log().try_log(
            DMK::LogLevel::Info,
            "Shutdown: hook retired ({} restored, {} vacated), workers {}",
            retirement.restored,
            retirement.vacated,
            workers_joined ? "joined" : "did not join"
        );
        return ShutdownResult{
            .hooks = retirement,
            .complete = workers_joined,
        };
    }
} // namespace HUDClock
