/**
 * @file global_state.cpp
 * @brief Storage backing the cross-module shared-state accessors.
 */

#include "global_state.hpp"
#include "core/frame/channels.hpp"

// Stable unmangled symbol for the resolved game context pointer (see header).
extern "C"
{
    std::atomic<std::byte *> g_global_context_ptr_address{nullptr};
}

namespace TPVCamera
{
    ModuleInfo &module_info() noexcept
    {
        static ModuleInfo state;
        return state;
    }

    OverlayState &overlay_state() noexcept
    {
        static OverlayState state;
        return state;
    }

    CameraState &camera_state() noexcept
    {
        static CameraState state;
        return state;
    }

    namespace
    {
        // Thread: published by the frustum detour (main thread), read by the interaction, ranged and AI observer hooks.
        SeqlockSnapshot<FrameRecord> s_last_final{};
    } // namespace

    void publish_last_final(const FrameRecord &record) noexcept
    {
        s_last_final.publish(record);
    }

    FrameRecord last_final() noexcept
    {
        FrameRecord record{};
        if (!s_last_final.try_load(record))
        {
            return FrameRecord{};
        }
        return record;
    }

    bool last_final_pose(float &px, float &py, float &pz, float &dx, float &dy, float &dz) noexcept
    {
        const FrameRecord record = last_final();
        if (!record.valid)
        {
            return false;
        }
        px = record.position.x;
        py = record.position.y;
        pz = record.position.z;
        dx = record.forward.x;
        dy = record.forward.y;
        dz = record.forward.z;
        return true;
    }

    std::atomic<uint32_t> &game_state_mask() noexcept
    {
        static std::atomic<uint32_t> mask{0};
        return mask;
    }

    std::atomic<bool> &game_world_ready() noexcept
    {
        static std::atomic<bool> ready{false};
        return ready;
    }

} // namespace TPVCamera
