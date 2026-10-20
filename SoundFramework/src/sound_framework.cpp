/**
 * @file sound_framework.cpp
 * @brief Mod lifecycle: configuration, the game's FMOD API, the listener hook, the engine and the console commands.
 *
 * init() runs off the Windows loader lock and receives the live Session. shutdown() is driven by the host's detach
 * path and must likewise run off the loader lock: hooks are caller-owned and this path restores the patched prologue.
 */

#include "sound_framework.hpp"
#include "anchors.hpp"
#include "audio/engine.hpp"
#include "audio/lip_model.hpp"
#include "audio/replacements.hpp"
#include "constants.hpp"
#include "fmod/event_hook.hpp"
#include "fmod/fmod_api.hpp"
#include "fmod/listener_hook.hpp"
#include "fmod/parameter_hook.hpp"
#include "fmod/update_hook.hpp"
#include "game/console_commands.hpp"
#include "game/entity_tracker.hpp"
#include "game/game_clock.hpp"
#include "game/lip_drive.hpp"
#include "game/main_thread.hpp"
#include "game/save_load_hook.hpp"
#include "game/speak_hook.hpp"
#include "game/speech.hpp"
#include "packs/sound_def.hpp"
#include "version.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace SoundFramework
{
    namespace
    {
        DMK::hook::HookStack s_hooks;
        /// [LipSync] Enabled: voice-driven mouth movement for lines without lip-sync animation. Read at startup only.
        std::atomic<bool> s_lip_sync_enabled{true};
        /// [LipSync] Mode: true for the learned model (ADVANCED), false for the spectral mouth (BASIC).
        std::atomic<bool> s_lip_sync_advanced{true};

        /** @brief Waits briefly for WHGame.dll and the FMOD modules it imports, since an ASI can attach first. */
        [[nodiscard]] bool wait_for_game_audio()
        {
            for (int i = 0; i < 300; ++i)
            {
                if (DMK::Region::module_named(constants::GAME_MODULE).size != 0 &&
                    GetModuleHandleW(constants::FMOD_CORE_MODULE) != nullptr &&
                    GetModuleHandleW(constants::FMOD_STUDIO_MODULE) != nullptr)
                {
                    return true;
                }
                Sleep(10);
            }
            return false;
        }

        /** @brief `<game root>/Mods`: the game runs from `<game root>/Bin/<platform>/`. */
        [[nodiscard]] std::filesystem::path mods_directory()
        {
            std::wstring exe(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
                if (length < exe.size())
                {
                    exe.resize(length);
                    break;
                }
                exe.resize(exe.size() * 2);
            }
            return std::filesystem::path(exe).parent_path().parent_path().parent_path() / L"Mods";
        }

        /** @brief The directory of this module (the framework DLL or ASI). */
        [[nodiscard]] std::filesystem::path module_directory()
        {
            HMODULE self = nullptr;
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&module_directory),
                    &self
                ) == 0)
            {
                return {};
            }
            std::wstring path(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
                if (length < path.size())
                {
                    path.resize(length);
                    break;
                }
                path.resize(path.size() * 2);
            }
            return std::filesystem::path(path).parent_path();
        }

        /**
         * @brief Loads the learned lip model in either mode, so an INI reload can switch Mode live; without it, lines
         *        keep the basic spectral mouth.
         */
        void load_lip_model()
        {
            const std::filesystem::path path = module_directory() / constants::LIP_MODEL_FILE_NAME;
            std::string error;
            std::optional<audio::LipModel> model = audio::LipModel::load(path, error);
            if (!model.has_value())
            {
                if (s_lip_sync_advanced)
                {
                    DMK::log()
                        .warning("LipSync: {} not loaded ({}); using the basic mouth", path.filename().string(), error);
                }
                else
                {
                    DMK::log().info("LipSync: BASIC mode; {} not loaded ({})", path.filename().string(), error);
                }
                // The worker still decodes lines and computes the basic mouth.
                game::speech::set_lip_model(nullptr);
                return;
            }
            DMK::log().info(
                "LipSync: {} mode, lip model with {} channel(s), {} parameter(s)",
                s_lip_sync_advanced ? "ADVANCED" : "BASIC",
                model->channels().size(),
                model->parameter_count()
            );
            game::lip_drive::set_pose_channels(model->channels());
            game::speech::set_lip_model(std::make_shared<const audio::LipModel>(std::move(*model)));
        }

        void register_config_items()
        {
            const DMK::config::SectionBinder settings = DMK::config::section("Settings");
            settings.bind_log_level("LogLevel", constants::DEFAULT_LOG_LEVEL);
            const DMK::config::SectionBinder debug = DMK::config::section("Debug");
            debug.bind_string("EventLog", "Event log filter", &audio::replacements::set_event_log_filter, "");
            debug.bind_string("SpeakLog", "Speak log filter", &game::speak_hook::set_log_filter, "");
            const DMK::config::SectionBinder lip_sync = DMK::config::section("LipSync");
            lip_sync.bind_bool(
                "Enabled",
                "Voice-driven mouth movement",
                [](bool on) { s_lip_sync_enabled.store(on); },
                true
            );
            lip_sync.bind_int("OffsetMs", "Mouth timing offset (ms)", &game::speech::set_lip_offset_ms, 0);
            lip_sync.bind_string(
                "Mode",
                "Lip sync mode (ADVANCED or BASIC)",
                [](std::string_view mode)
                {
                    std::string lowered(mode);
                    std::ranges::transform(
                        lowered,
                        lowered.begin(),
                        [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
                    );
                    s_lip_sync_advanced.store(lowered != "basic");
                    game::speech::set_lip_advanced(lowered != "basic");
                },
                "ADVANCED"
            );
        }
    } // namespace

    DMK::Result<void> init(DMK::Session &session, const WheelHostTable *wheel_host, std::span<std::uint64_t> slots)
    {
        (void)wheel_host;
        (void)slots;
        DMK::Logger &logger = session.log();
        logger.info("========================================");
        version::log_version_info();

        register_config_items();
        DMK::config::load(constants::get_config_filename());
        DMK::config::log_all();

        if (!wait_for_game_audio())
        {
            logger.error("Init: {} or the game's FMOD modules are not loaded", constants::GAME_MODULE);
            return std::unexpected(DMK::Error{DMK::ErrorCode::ProcessMismatch, "SoundFramework::init/modules"});
        }
        fmod::Api api;
        if (!fmod::resolve(api))
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "SoundFramework::init/fmod"});
        }
        fmod::publish(api);
        DMK_TRY_VOID(fmod::install_listener_hook(s_hooks));
        DMK_TRY_VOID(fmod::install_update_hook(s_hooks));
        DMK_TRY_VOID(fmod::install_event_hook(s_hooks));
        DMK_TRY_VOID(fmod::install_parameter_hook(s_hooks));
        if (!audio::start(mods_directory(), module_directory() / constants::MOODS_FILE_NAME))
        {
            return std::unexpected(DMK::Error{DMK::ErrorCode::Unknown, "SoundFramework::init/worker"});
        }

        // Every game-side feature runs from the main-thread tick, which the Commands gate covers. Without it the packs
        // still load and replace the game's sounds, and the commands, voice lines and faces stay off.
        const Anchors anchors = resolve_anchors(DMK::Region::module_named(constants::GAME_MODULE));
        if (anchors.commands)
        {
            game::commands::configure(anchors);
            game::entities::configure(anchors);
            game::speech::configure(anchors);
            game::game_clock::configure(anchors.voice_clock ? anchors.genv : 0, anchors.timer_vtable);
            DMK_TRY_VOID(game::speak_hook::install(anchors, s_hooks));
            DMK_TRY_VOID(game::save_load_hook::install(anchors, s_hooks));
            if (s_lip_sync_enabled)
            {
                load_lip_model();
                DMK_TRY_VOID(game::lip_drive::start(anchors, s_hooks));
            }
            else
            {
                logger.info("Init: [LipSync] Enabled = false; the face pose hook is not installed");
            }
            DMK_TRY_VOID(game::main_thread::start(anchors, s_hooks));
        }
        else
        {
            logger.error("Init: the main-thread tick is unavailable; commands, voice lines and faces stay off");
        }
        // Saving the INI applies it while playing; [LipSync] Enabled alone needs a restart.
        const DMK::config::AutoReloadStatus reload = DMK::config::enable_auto_reload(
            std::chrono::milliseconds{250},
            [](bool content_changed)
            {
                if (content_changed)
                {
                    DMK::log().info("INI auto-reload: settings applied");
                }
                else
                {
                    DMK::log().info("INI auto-reload: no content change");
                }
            }
        );
        if (reload == DMK::config::AutoReloadStatus::Started)
        {
            logger.info("Init: INI hot-reload watcher started (250 ms debounce)");
        }
        logger.info("Init: ready; waiting for the game's audio system");
        return {};
    }

    ShutdownResult shutdown() noexcept
    {
        // The console keeps raw pointers to the command functions, so they go first, on the main thread.
        if (!game::commands::remove_commands(constants::COMMAND_REMOVAL_DEADLINE))
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Shutdown: the main thread did not remove the console commands in time; the framework stays loaded"
            );
            return ShutdownResult{};
        }
        const DMK::hook::HookRetirement retirement = s_hooks.retire(constants::HOOK_RETIREMENT_DEADLINE);
        if (retirement.outcome != DMK::hook::RetirementOutcome::Retired || retirement.pinned != 0)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Shutdown: the hook did not retire ({}, {} pinned, {} call(s) in a detour); "
                "the framework keeps its state",
                DMK::to_string(retirement.refusal),
                retirement.pinned,
                retirement.in_flight
            );
            return ShutdownResult{
                .hooks = retirement,
            };
        }
        // The hooks no longer run; no INI reload may run its setters during teardown.
        DMK::config::disable_auto_reload();
        game::entities::clear();
        // The lip model worker is joined here, off the loader lock, before the module can unload.
        game::speech::shutdown();
        game::lip_drive::clear();
        // The engine joins its worker before it releases its sounds.
        audio::shutdown();
        DMK::config::clear();
        const bool workers_joined = DMK::diagnostics::module_pin_count(DMK::diagnostics::ModulePinReason::Worker) == 0;
        (void)DMK::log().try_log(
            DMK::LogLevel::Info,
            "Shutdown: commands removed; hooks retired ({} restored, {} vacated); sounds released; workers {}",
            retirement.restored,
            retirement.vacated,
            workers_joined ? "joined" : "did not join"
        );
        return ShutdownResult{
            .hooks = retirement,
            .complete = workers_joined,
        };
    }
} // namespace SoundFramework
