/**
 * @file indoor_state.cpp
 * @brief The Indoors query: the engine's clip-volume point lookup, guarded.
 */
#include "indoor_state.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace TPVCamera
{
    namespace
    {
        // CClipVolumeManager::GetClipVolumeByPos(this, const Vec3* pos, const IClipVolume* ignore) -> CClipVolume*.
        using GetClipVolumeByPosFn = void *(__fastcall *)(void *manager, const float *pos, const void *ignore);

        // Written once at setup, before the camera hook is armed, and read on the main thread.
        std::uintptr_t s_manager_slot = 0;
        GetClipVolumeByPosFn s_get_clip_volume_by_pos = nullptr;

        // The volume the previous query found, compared to name a change on the Trace line and never read through.
        // Main thread only.
        std::uintptr_t s_traced_volume = 0;

        /// The volume holding @p pos, or 0 for none or on a fault. No C++ objects, so the SEH frame needs no unwinding.
        std::uintptr_t query_volume(void *manager, const float *pos) noexcept
        {
            __try
            {
                return reinterpret_cast<std::uintptr_t>(s_get_clip_volume_by_pos(manager, pos, nullptr));
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /// The Trace line for a change of the volume holding @p point: its name, flags and world bounds.
        void trace_volume(std::uintptr_t volume, const Vector3 &point)
        {
            if (volume == 0)
            {
                (void)DMK::log().try_log(DMK::LogLevel::Trace, "IndoorState: indoors=0 probe=({}, {}, {})", point.x,
                                         point.y, point.z);
                return;
            }
            std::array<char, Constants::CLIP_VOLUME_NAME_CAPACITY> name{};
            if (const auto read = DMK::memory::read<std::array<char, Constants::CLIP_VOLUME_NAME_CAPACITY>>(
                    DMK::Address{volume + Constants::CLIP_VOLUME_NAME_OFFSET}))
            {
                name = *read;
            }
            const auto flags =
                DMK::memory::read<std::uint32_t>(DMK::Address{volume + Constants::CLIP_VOLUME_FLAGS_OFFSET});
            const auto box =
                DMK::memory::read<std::array<float, 6>>(DMK::Address{volume + Constants::CLIP_VOLUME_BBOX_OFFSET});
            const std::array<float, 6> b = box ? *box : std::array<float, 6>{};
            (void)DMK::log().try_log(
                DMK::LogLevel::Trace,
                "IndoorState: indoors=1 volume={:#x} \"{}\" flags={:#x} box=({}, {}, {})-({}, {}, {}) "
                "probe=({}, {}, {})",
                volume, std::string_view(name.data(), strnlen(name.data(), name.size())), flags ? *flags : 0u, b[0],
                b[1], b[2], b[3], b[4], b[5], point.x, point.y, point.z);
        }
    } // namespace

    void initialize_indoor_state()
    {
        DMK::Logger &logger = DMK::log();
        const std::uintptr_t slot = gated_anchor_address(Feature::IndoorState, AnchorId::ClipVolumeManager);
        const std::uintptr_t fn = gated_anchor_address(Feature::IndoorState, AnchorId::GetClipVolumeByPos);
        if (slot == 0 || fn == 0)
        {
            logger.warning("IndoorState: the clip-volume manager or its point query did not resolve (game patched?); "
                           "presets bound to Indoors never apply");
            return;
        }
        s_manager_slot = slot;
        s_get_clip_volume_by_pos = reinterpret_cast<GetClipVolumeByPosFn>(fn);
        logger.info("IndoorState: GetClipVolumeByPos at {}, manager slot at {}", DMK::format::format_address(fn),
                    DMK::format::format_address(slot));
    }

    bool point_indoors(const Vector3 &point) noexcept
    {
        if (s_get_clip_volume_by_pos == nullptr)
        {
            return false;
        }
        // The manager exists only while a level is loaded, and is proven by its vtable before its function is called.
        const auto manager = DMK::memory::read<std::uintptr_t>(DMK::Address{s_manager_slot});
        if (!manager || !DMK::memory::is_plausible_ptr(DMK::Address{*manager}))
        {
            return false;
        }
        const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{*manager});
        if (!vtable || !vtable_is(GameClass::ClipVolumeManager, *vtable))
        {
            return false;
        }
        DMK_PROFILE_SCOPE("indoor.query");
        const float pos[3] = {point.x, point.y, point.z};
        const std::uintptr_t volume = query_volume(reinterpret_cast<void *>(*manager), pos);
        if (volume != s_traced_volume)
        {
            s_traced_volume = volume;
            if (DMK::log().is_enabled(DMK::LogLevel::Trace))
            {
                trace_volume(volume, point);
            }
        }
        return volume != 0;
    }
} // namespace TPVCamera
