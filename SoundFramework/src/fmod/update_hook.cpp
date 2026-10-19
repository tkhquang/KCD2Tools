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

#include <windows.h>

namespace SoundFramework::fmod
{
    namespace
    {
        using UpdateFn = Result(__fastcall *)(void *system);

        UpdateFn s_original = nullptr;

        Result __fastcall update_detour(void *system) noexcept
        {
            if (system != nullptr)
            {
                audio::on_system(system);
            }
            return s_original(system);
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
        DMK_TRY(
            created,
            DMK::hook::inline_at<&update_detour>(DMK::hook::InlineRequest{
                .name = "FmodStudioSystemUpdate",
                .target = DMK::Address{reinterpret_cast<std::uintptr_t>(target)},
            })
        );
        s_original = created.original<UpdateFn>();
        // Stored before it is armed: the stack owns it through teardown even when the arm fails half-way.
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info(
            "FMOD: Studio::System::update hooked at {}",
            DMK::format::format_address(reinterpret_cast<std::uintptr_t>(target))
        );
        return {};
    }
} // namespace SoundFramework::fmod
