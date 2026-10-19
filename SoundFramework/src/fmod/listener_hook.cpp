/**
 * @file fmod/listener_hook.cpp
 * @brief The listener export hook.
 *
 * The game's audio layer sets the FMOD Studio listener every frame through the C++ API. The method's `this` is the
 * game's Studio::System, so hooking the export (a public FMOD symbol, identical on every game build that ships this
 * FMOD version) hands the framework the system and the listener without reading anything inside WHGame.dll.
 */

#include "fmod/listener_hook.hpp"
#include "constants.hpp"
#include "fmod/fmod_api.hpp"
#include "audio/engine.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

namespace SoundFramework::fmod
{
    namespace
    {
        using SetListenerFn =
            Result(__fastcall *)(void *system, int listener, const Attributes3d *attributes, const Vector *attenuation);

        SetListenerFn s_original = nullptr;

        Result __fastcall set_listener_detour(
            void *system,
            int listener,
            const Attributes3d *attributes,
            const Vector *attenuation
        ) noexcept
        {
            const Result result = s_original(system, listener, attributes, attenuation);
            if (result == OK && listener == 0 && attributes != nullptr && system != nullptr)
            {
                audio::on_listener(system, *attributes);
            }
            return result;
        }
    } // namespace

    DMK::Result<void> install_listener_hook(DMK::hook::HookStack &hooks)
    {
        const HMODULE studio = GetModuleHandleW(constants::FMOD_STUDIO_MODULE);
        const FARPROC target =
            studio == nullptr ? nullptr : GetProcAddress(studio, constants::STUDIO_SET_LISTENER_EXPORT);
        if (target == nullptr)
        {
            DMK::log().error("FMOD: Studio::System::setListenerAttributes export not found");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "listener_hook/export"});
        }
        DMK_TRY(
            created,
            DMK::hook::inline_at<&set_listener_detour>(DMK::hook::InlineRequest{
                .name = "FmodStudioSetListener",
                .target = DMK::Address{reinterpret_cast<std::uintptr_t>(target)},
            })
        );
        s_original = created.original<SetListenerFn>();
        // Stored before it is armed: the stack owns it through teardown even when the arm fails half-way.
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info(
            "FMOD: Studio::System::setListenerAttributes hooked at {}",
            DMK::format::format_address(reinterpret_cast<std::uintptr_t>(target))
        );
        return {};
    }
} // namespace SoundFramework::fmod
