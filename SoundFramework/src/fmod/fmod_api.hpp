/**
 * @file fmod/fmod_api.hpp
 * @brief The slice of FMOD's public C API the framework uses, resolved from the game's own fmod.dll and fmodstudio.dll.
 *
 * The game ships FMOD 2.02 as ordinary DLLs that export the documented C API, so these are stable, versioned
 * functions rather than addresses inside the game. The C API takes the same object pointers as the C++ API the game
 * uses, so a Studio::System* the game hands to a C++ method is a valid FMOD_STUDIO_SYSTEM* here.
 */
#ifndef SNDFW_FMOD_FMOD_API_HPP
#define SNDFW_FMOD_FMOD_API_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace SoundFramework::fmod
{
    using Result = int;
    inline constexpr Result OK = 0;
    /// FMOD_ERR_INVALID_HANDLE: a channel handle whose sound ended or was stolen.
    inline constexpr Result ERR_INVALID_HANDLE = 30;

    struct Vector
    {
        float x;
        float y;
        float z;
    };

    struct Attributes3d
    {
        Vector position;
        Vector velocity;
        Vector forward;
        Vector up;
    };

    /** @brief FMOD_GUID: the ID of a Studio event, stable across bank loads. */
    struct Guid
    {
        std::uint32_t data1 = 0;
        std::uint16_t data2 = 0;
        std::uint16_t data3 = 0;
        std::array<std::uint8_t, 8> data4{};

        [[nodiscard]] bool operator==(const Guid &) const noexcept = default;
    };
    static_assert(sizeof(Guid) == 16);

    // FMOD_STUDIO_PLAYBACK_STATE.
    inline constexpr int PLAYBACK_STOPPED = 2;

    // FMOD_MODE bits.
    inline constexpr std::uint32_t MODE_LOOP_OFF = 0x00000001;
    inline constexpr std::uint32_t MODE_LOOP_NORMAL = 0x00000002;
    inline constexpr std::uint32_t MODE_2D = 0x00000008;
    inline constexpr std::uint32_t MODE_3D = 0x00000010;
    inline constexpr std::uint32_t MODE_CREATESTREAM = 0x00000080;
    inline constexpr std::uint32_t MODE_CREATESAMPLE = 0x00000100;
    inline constexpr std::uint32_t MODE_OPENONLY = 0x00002000;
    inline constexpr std::uint32_t MODE_ACCURATETIME = 0x00004000;
    inline constexpr std::uint32_t MODE_3D_LINEARSQUAREROLLOFF = 0x00400000;
    /// FMOD_SOUND_FORMAT values readData can deliver.
    inline constexpr int FORMAT_PCM16 = 2;
    inline constexpr int FORMAT_PCMFLOAT = 5;
    /// FMOD_TIMEUNIT_MS.
    inline constexpr std::uint32_t TIMEUNIT_MS = 0x00000001;
    /// FMOD_TIMEUNIT_PCM: sample frames.
    inline constexpr std::uint32_t TIMEUNIT_PCM = 0x00000002;

    /**
     * @brief FMOD_CREATESOUNDEXINFO of FMOD 2.02 on x64, with only the fields the framework sets named.
     * @note createSound accepts any cbsize from 4 to 224 in steps of 4 and zero-fills the rest of its own copy.
     */
    struct alignas(8) CreateSoundExInfo
    {
        int cbsize = sizeof(CreateSoundExInfo);
        std::uint32_t length = 0;
        std::uint8_t unnamed_0[192] = {};
        /// Non-zero: open this sound with FMOD's own file system instead of the one the game installed (CryPak).
        int ignore_set_file_system = 0;
        std::uint8_t unnamed_1[20] = {};
    };
    static_assert(sizeof(CreateSoundExInfo) == 224);
    static_assert(offsetof(CreateSoundExInfo, ignore_set_file_system) == 0xC8);

    /** @brief A CryEngine world position (z up) as an FMOD vector: the game hands FMOD (x, z, y). */
    [[nodiscard]] constexpr Vector from_world(float x, float y, float z) noexcept
    {
        return Vector{x, z, y};
    }

    /** @brief Function pointers into the game's FMOD. All null until resolve() succeeds. */
    struct Api
    {
        // fmodstudio.dll
        int(__stdcall *studio_is_valid)(void *system) = nullptr;
        Result(__stdcall *studio_get_core_system)(void *system, void **core) = nullptr;
        Result(__stdcall *studio_get_bus)(void *system, const char *path, void **bus) = nullptr;
        Result(__stdcall *studio_lookup_id)(void *system, const char *path, Guid *id) = nullptr;
        Result(__stdcall *bus_lock_channel_group)(void *bus) = nullptr;
        Result(__stdcall *bus_unlock_channel_group)(void *bus) = nullptr;
        Result(__stdcall *bus_get_channel_group)(void *bus, void **group) = nullptr;
        Result(__stdcall *event_get_description)(void *instance, void **description) = nullptr;
        Result(__stdcall *event_get_3d_attributes)(void *instance, Attributes3d *attributes) = nullptr;
        Result(__stdcall *event_set_volume)(void *instance, float volume) = nullptr;
        Result(__stdcall *event_get_playback_state)(void *instance, int *state) = nullptr;
        Result(__stdcall *event_get_user_data)(void *instance, void **user_data) = nullptr;
        Result(__stdcall *description_get_path)(void *description, char *path, int size, int *retrieved) = nullptr;
        Result(__stdcall *description_get_id)(void *description, Guid *id) = nullptr;
        // fmod.dll
        Result(__stdcall *create_sound)(void *core, const char *name, std::uint32_t mode, void *info, void **sound) =
            nullptr;
        Result(__stdcall *play_sound)(void *core, void *sound, void *group, int paused, void **channel) = nullptr;
        Result(__stdcall *set_3d_listener)(
            void *core,
            int listener,
            const Vector *position,
            const Vector *velocity,
            const Vector *forward,
            const Vector *up
        ) = nullptr;
        Result(__stdcall *channel_set_3d_attributes)(void *channel, const Vector *position, const Vector *velocity) =
            nullptr;
        Result(__stdcall *channel_set_3d_min_max)(void *channel, float min_distance, float max_distance) = nullptr;
        Result(__stdcall *channel_set_paused)(void *channel, int paused) = nullptr;
        Result(__stdcall *channel_set_volume)(void *channel, float volume) = nullptr;
        Result(__stdcall *channel_set_pitch)(void *channel, float pitch) = nullptr;
        Result(__stdcall *channel_stop)(void *channel) = nullptr;
        Result(__stdcall *channel_is_playing)(void *channel, int *playing) = nullptr;
        Result(__stdcall *sound_release)(void *sound) = nullptr;
        Result(__stdcall *sound_get_length)(void *sound, std::uint32_t *length, std::uint32_t unit) = nullptr;
        Result(__stdcall *sound_get_format)(void *sound, int *type, int *format, int *channels, int *bits) = nullptr;
        Result(__stdcall *sound_get_defaults)(void *sound, float *frequency, int *priority) = nullptr;
        Result(__stdcall *sound_read_data)(void *sound, void *buffer, std::uint32_t length, std::uint32_t *read) =
            nullptr;
    };

    /**
     * @brief Resolves every function from the loaded modules.
     * @return True when all of them resolved.
     * @note Setup/control-plane only.
     */
    [[nodiscard]] bool resolve(Api &api);

    /** @brief The resolved table; valid after a successful resolve() in init. */
    [[nodiscard]] const Api &api() noexcept;

    /** @brief Stores @p resolved as the process table. */
    void publish(const Api &resolved) noexcept;
} // namespace SoundFramework::fmod

#endif // SNDFW_FMOD_FMOD_API_HPP
