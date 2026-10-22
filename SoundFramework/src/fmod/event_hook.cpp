/**
 * @file fmod/event_hook.cpp
 * @brief The event start export hook.
 *
 * The game creates, positions and starts every sound through the FMOD Studio C++ API in fmodstudio.dll, so its public
 * `EventInstance::start` export sees each one, on every game build that ships this FMOD version, with no anchor inside
 * WHGame.dll.
 */

#include "fmod/event_hook.hpp"
#include "audio/replacements.hpp"
#include "constants.hpp"
#include "fmod/fmod_api.hpp"
#include "game/cutscene_voices.hpp"

#include <windows.h>

#include <atomic>

namespace SoundFramework::fmod
{
    namespace
    {
        using StartFn = Result(__fastcall *)(void *instance);

        StartFn s_original = nullptr;

        /**
         * @brief The instance start_own_event() is starting, null otherwise.
         * @details FMOD_Studio_EventInstance_Start is a bare `jmp` into the hooked C++ export, so a framework start
         *          runs the detour synchronously on the storing thread. Relaxed order is enough: no other thread can
         *          start this instance, so a read on any other thread never matches, whichever value it sees.
         */
        std::atomic<void *> s_own_start{nullptr};

        Result __fastcall start_detour(void *instance) noexcept
        {
            if (instance != s_own_start.load(std::memory_order_relaxed))
            {
                game::cutscene_voices::on_event_start(instance);
                audio::replacements::on_event_start(instance);
            }
            return s_original(instance);
        }
    } // namespace

    Result start_own_event(void *instance) noexcept
    {
        s_own_start.store(instance, std::memory_order_relaxed);
        const Result started = api().event_start(instance);
        s_own_start.store(nullptr, std::memory_order_relaxed);
        return started;
    }

    DMK::Result<void> install_event_hook(DMK::hook::HookStack &hooks)
    {
        const HMODULE studio = GetModuleHandleW(constants::FMOD_STUDIO_MODULE);
        const FARPROC target =
            studio == nullptr ? nullptr : GetProcAddress(studio, constants::STUDIO_EVENT_START_EXPORT);
        if (target == nullptr)
        {
            DMK::log().error("FMOD: Studio::EventInstance::start export not found");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "event_hook/export"});
        }
        DMK_TRY(
            created,
            DMK::hook::inline_at<&start_detour>(DMK::hook::InlineRequest{
                .name = "FmodStudioEventStart",
                .target = DMK::Address{reinterpret_cast<std::uintptr_t>(target)},
            })
        );
        s_original = created.original<StartFn>();
        // Stored before it is armed: the stack owns it through teardown even when the arm fails half-way.
        DMK::hook::Hook &stored = hooks.push(std::move(created));
        DMK_TRY_VOID(stored.enable());
        DMK::log().info(
            "FMOD: Studio::EventInstance::start hooked at {}",
            DMK::format::format_address(reinterpret_cast<std::uintptr_t>(target))
        );
        return {};
    }
} // namespace SoundFramework::fmod
