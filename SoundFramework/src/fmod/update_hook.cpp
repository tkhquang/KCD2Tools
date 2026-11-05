/**
 * @file fmod/update_hook.cpp
 * @brief The Studio system update export hook.
 *
 * The game calls setListenerAttributes only when the listener moves. After a start or a reload with a still camera,
 * the listener hook has not seen the game's Studio system yet, and voice lines cannot open their recordings. The game
 * updates its Studio system every audio frame, so this hook learns the system on the first frame instead.
 */

#include "fmod/update_hook.hpp"
#include "audio/engine.hpp"
#include "constants.hpp"
#include "fmod/fmod_api.hpp"
#include "game/cutscene_voices.hpp"

#include <windows.h>

#include <atomic>

namespace SoundFramework::fmod
{
    namespace
    {
        using UpdateFn = Result(__fastcall *)(void *system);

        std::atomic<UpdateFn> s_original{nullptr};

        Result __fastcall update_detour(void *system) noexcept
        {
            if (system != nullptr)
            {
                audio::on_system(system);
                game::cutscene_voices::on_studio_system(system);
            }
            return s_original.load(std::memory_order_acquire)(system);
        }
    } // namespace

    DMK::Result<void> install_update_hook(DMK::hook::HookStack &hooks)
    {
        const HMODULE studio = GetModuleHandleW(constants::FMOD_STUDIO_MODULE);
        const FARPROC target =
            studio == nullptr ? nullptr : GetProcAddress(studio, constants::STUDIO_SYSTEM_UPDATE_EXPORT);
        if (target == nullptr)
        {
            DMK::log().warning(
                "FMOD: Studio::System::update export not found; voice lines open their recordings once the listener "
                "first moves"
            );
            return {};
        }
        DMK_TRY_VOID(hooks.install<&update_detour>(DMK::hook::InlineInstallRequest{
            .name = "FmodStudioSystemUpdate",
            .target = DMK::Address{reinterpret_cast<std::uintptr_t>(target)},
            .original = &s_original,
        }));
        DMK::log().info(
            "FMOD: Studio::System::update hooked at {}",
            DMK::format::format_address(reinterpret_cast<std::uintptr_t>(target))
        );
        return {};
    }
} // namespace SoundFramework::fmod
