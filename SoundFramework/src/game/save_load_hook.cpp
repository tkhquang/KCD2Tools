/**
 * @file game/save_load_hook.cpp
 * @brief The SaveGameManager::LoadCryEngineData hook.
 *
 * A save load keeps the player entity, and with it the player's speak manager and the line it plays. A framework line
 * then plays on into the loaded game while its face clock stands still through the loading screen, which leaves the
 * mouth behind the voice, and a looping framework sound plays on as well. Every save load, same-level reloads
 * included, passes through LoadCryEngineData, so its entry stops the framework's lines and sounds before the game
 * reads the save.
 */

#include "game/save_load_hook.hpp"
#include "audio/engine.hpp"
#include "game/speech.hpp"

#include <atomic>

namespace SoundFramework::game::save_load_hook
{
    namespace
    {
        using LoadFn = bool(__fastcall *)(void *manager, void *chunk, void *save_path);

        std::atomic<LoadFn> s_original{nullptr};

        bool __fastcall load_detour(void *manager, void *chunk, void *save_path) noexcept
        {
            speech::on_save_load();
            try
            {
                audio::request_stop_all();
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "SaveLoad: the framework's sounds were not stopped"
                );
            }
            return s_original.load(std::memory_order_acquire)(manager, chunk, save_path);
        }
    } // namespace

    DMK::Result<void> install(const Anchors &anchors, DMK::hook::HookStack &hooks)
    {
        if (!anchors.load_stop)
        {
            DMK::log().warning(
                "SaveLoad: the save-load anchors did not resolve; lines and sounds go on across a save load"
            );
            return {};
        }
        DMK_TRY_VOID(hooks.install<&load_detour>(DMK::hook::InlineInstallRequest{
            .name = "SaveGameLoad",
            .target = DMK::Address{anchors.save_game_load},
            .original = &s_original,
        }));
        DMK::log().info(
            "SaveLoad: SaveGameManager::LoadCryEngineData hooked at {}",
            DMK::format::format_address(anchors.save_game_load)
        );
        return {};
    }
} // namespace SoundFramework::game::save_load_hook
