/**
 * @file fmod/fmod_api.cpp
 * @brief Resolution of the FMOD C API from the game's FMOD modules.
 */

#include "fmod/fmod_api.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

namespace SoundFramework::fmod
{
    namespace
    {
        Api s_api{};

        template <class Fn> bool bind(HMODULE module, const char *name, Fn &out)
        {
            const FARPROC address = module == nullptr ? nullptr : GetProcAddress(module, name);
            if (address == nullptr)
            {
                DMK::log().error("FMOD: export {} not found", name);
                return false;
            }
            out = reinterpret_cast<Fn>(address);
            return true;
        }
    } // namespace

    bool resolve(Api &out)
    {
        const HMODULE core = GetModuleHandleW(constants::FMOD_CORE_MODULE);
        const HMODULE studio = GetModuleHandleW(constants::FMOD_STUDIO_MODULE);
        if (core == nullptr || studio == nullptr)
        {
            DMK::log().error("FMOD: the game's fmod.dll or fmodstudio.dll is not loaded");
            return false;
        }
        bool ok = true;
        ok = bind(studio, "FMOD_Studio_System_IsValid", out.studio_is_valid) && ok;
        ok = bind(studio, "FMOD_Studio_System_GetCoreSystem", out.studio_get_core_system) && ok;
        ok = bind(studio, "FMOD_Studio_System_GetBus", out.studio_get_bus) && ok;
        ok = bind(studio, "FMOD_Studio_Bus_LockChannelGroup", out.bus_lock_channel_group) && ok;
        ok = bind(studio, "FMOD_Studio_Bus_UnlockChannelGroup", out.bus_unlock_channel_group) && ok;
        ok = bind(studio, "FMOD_Studio_Bus_GetChannelGroup", out.bus_get_channel_group) && ok;
        ok = bind(studio, "FMOD_Studio_EventInstance_GetDescription", out.event_get_description) && ok;
        ok = bind(studio, "FMOD_Studio_EventInstance_Get3DAttributes", out.event_get_3d_attributes) && ok;
        ok = bind(studio, "FMOD_Studio_EventInstance_SetVolume", out.event_set_volume) && ok;
        ok = bind(studio, "FMOD_Studio_EventInstance_GetPlaybackState", out.event_get_playback_state) && ok;
        ok = bind(studio, "FMOD_Studio_EventDescription_GetPath", out.description_get_path) && ok;
        ok = bind(core, "FMOD_System_CreateSound", out.create_sound) && ok;
        ok = bind(core, "FMOD_System_PlaySound", out.play_sound) && ok;
        ok = bind(core, "FMOD_System_Set3DListenerAttributes", out.set_3d_listener) && ok;
        ok = bind(core, "FMOD_Channel_Set3DAttributes", out.channel_set_3d_attributes) && ok;
        ok = bind(core, "FMOD_Channel_Set3DMinMaxDistance", out.channel_set_3d_min_max) && ok;
        ok = bind(core, "FMOD_Channel_SetPaused", out.channel_set_paused) && ok;
        ok = bind(core, "FMOD_Channel_SetVolume", out.channel_set_volume) && ok;
        ok = bind(core, "FMOD_Channel_SetPitch", out.channel_set_pitch) && ok;
        ok = bind(core, "FMOD_Channel_Stop", out.channel_stop) && ok;
        ok = bind(core, "FMOD_Channel_IsPlaying", out.channel_is_playing) && ok;
        ok = bind(core, "FMOD_Sound_Release", out.sound_release) && ok;
        ok = bind(core, "FMOD_Sound_GetLength", out.sound_get_length) && ok;
        ok = bind(core, "FMOD_Sound_GetFormat", out.sound_get_format) && ok;
        ok = bind(core, "FMOD_Sound_GetDefaults", out.sound_get_defaults) && ok;
        ok = bind(core, "FMOD_Sound_ReadData", out.sound_read_data) && ok;
        return ok;
    }

    const Api &api() noexcept
    {
        return s_api;
    }

    void publish(const Api &resolved) noexcept
    {
        s_api = resolved;
    }
} // namespace SoundFramework::fmod
