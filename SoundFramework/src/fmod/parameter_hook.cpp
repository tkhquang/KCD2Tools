/**
 * @file fmod/parameter_hook.cpp
 * @brief The event parameter export hook.
 *
 * Some game events run through stages inside one instance: a bow shot starts at the draw and plays its release when
 * the game sets one of its parameters. setParameterByName is the only parameter setter WHGame.dll imports, so its
 * public export sees every such change, on every game build that ships this FMOD version, with no anchor inside
 * WHGame.dll.
 */

#include "fmod/parameter_hook.hpp"
#include "audio/replacements.hpp"
#include "constants.hpp"
#include "fmod/fmod_api.hpp"

#include <windows.h>

namespace SoundFramework::fmod
{
    namespace
    {
        using SetParameterFn = Result(__fastcall *)(void *instance, const char *name, float value, bool ignore_seek);

        SetParameterFn s_original = nullptr;

        Result __fastcall set_parameter_detour(void *instance, const char *name, float value, bool ignore_seek) noexcept
        {
            audio::replacements::on_event_parameter(instance, name, value);
            return s_original(instance, name, value, ignore_seek);
        }
    } // namespace

    DMK::Result<void> install_parameter_hook(DMK::hook::HookStack &hooks)
    {
        const HMODULE studio = GetModuleHandleW(constants::FMOD_STUDIO_MODULE);
        const FARPROC target =
            studio == nullptr ? nullptr : GetProcAddress(studio, constants::STUDIO_EVENT_SET_PARAMETER_EXPORT);
        if (target == nullptr)
        {
            DMK::log().warning(
                "FMOD: Studio::EventInstance::setParameterByName export not found; replacements with PlayAt never play"
            );
            return {};
        }
        DMK_TRY(
            created,
            DMK::hook::inline_at<&set_parameter_detour>(DMK::hook::InlineRequest{
                .name = "FmodStudioEventSetParameter",
                .target = DMK::Address{reinterpret_cast<std::uintptr_t>(target)},
            })
        );
        s_original = created.original<SetParameterFn>();
        // Stored before it is armed: the stack owns it through teardown even when the arm fails half-way.
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info(
            "FMOD: Studio::EventInstance::setParameterByName hooked at {}",
            DMK::format::format_address(reinterpret_cast<std::uintptr_t>(target))
        );
        return {};
    }
} // namespace SoundFramework::fmod
