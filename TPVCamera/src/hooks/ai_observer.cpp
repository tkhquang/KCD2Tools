/**
 * @file hooks/ai_observer.cpp
 * @brief The camera-observer update detour (see ai_observer.hpp).
 */

#include "hooks/ai_observer.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "hooks/camera_hook.hpp"
#include "hooks/vtable_slots.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <atomic>
#include <optional>

namespace TPVCamera
{
    namespace
    {
        // wh::engine3d::C_CameraObserver update (vtable slot CAMERA_OBSERVER_VTABLE_UPDATE_SLOT):
        // bool __fastcall(observer /*rcx*/, Vec3 *position /*rdx*/, bool *position_valid /*r8*/, Vec3 *direction /*r9*/,
        // bool *direction_valid, float *fov). It copies the system view camera's translation, forward column and field
        // of view.
        using CameraObserverUpdateFunc = bool(__fastcall *)(uintptr_t observer, float *position, bool *position_valid,
                                                            float *direction, bool *direction_valid, float *fov);
        std::atomic<CameraObserverUpdateFunc> s_camera_observer_update_original{nullptr};

        /**
         * @brief Camera-observer update detour: keeps the AI's camera observer at the first-person eye in third person.
         * @details The engine's camera observer copies the system view camera, which carries the third-person offset.
         *          The AI selects which NPCs to update in full, and which to hide and pause, from that observer, so an
         *          NPC a few metres beyond its reach from the pulled-back camera (a sleeper in a house, someone inside a
         *          shop) vanishes in third person while first person, whose camera is the eye, shows it. The original
         *          runs first. While the offset renders, its position is replaced with the eye of the last camera
         *          frame (last_final()), so every consumer of the observer sees what it sees in first person. The direction and
         *          field of view stay as the original set them: the offset camera looks the same way as the eye.
         */
        bool __fastcall detour_camera_observer_update(uintptr_t observer, float *position, bool *position_valid,
                                                      float *direction, bool *direction_valid, float *fov) noexcept
        {
            const DetourScope in_flight;
            const CameraObserverUpdateFunc original = s_camera_observer_update_original.load(std::memory_order_acquire);
            const bool updated =
                original != nullptr && original(observer, position, position_valid, direction, direction_valid, fov);
            // The original has just written through this pointer, so writing it again is as safe as its own store.
            if (updated && position != nullptr && camera_offset_active())
            {
                if (const FrameRecord frame = last_final(); frame.valid)
                {
                    position[0] = frame.eye.x;
                    position[1] = frame.eye.y;
                    position[2] = frame.eye.z;
                }
            }
            return updated;
        }
    } // namespace

    void install_camera_observer_hook(uintptr_t module_base, size_t module_size, HookSet &hooks)
    {
        DMK::Logger &logger = DMK::log();
        const std::optional<std::uintptr_t> vtable = class_vtable(GameClass::CameraObserver);
        if (!vtable.has_value())
        {
            logger.warning("Camera: C_CameraObserver did not resolve; in third person the AI keeps judging NPCs from "
                           "the pulled-back camera");
            return;
        }
        const uintptr_t target = read_checked_vtable_slot(
            *vtable, Constants::CAMERA_OBSERVER_VTABLE_UPDATE_SLOT, Aob::k_cameraObserverUpdateBody,
            Aob::k_cameraObserverUpdateBodyWindow, module_base, module_size);
        if (target == 0)
        {
            logger.warning("Camera: C_CameraObserver slot {} is not an in-image update that reads the view camera; "
                           "camera-observer hook skipped",
                           Constants::CAMERA_OBSERVER_VTABLE_UPDATE_SLOT);
            return;
        }

        auto result = DMK::hook::inline_at(
            DMK::hook::InlineRequest{.name = "CameraObserverUpdate", .target = DMK::Address{target}},
            detour_camera_observer_update);
        if (!result.has_value())
        {
            logger.warning("Camera: camera-observer hook failed ({}); in third person the AI keeps judging NPCs from "
                           "the pulled-back camera",
                           result.error().message());
            return;
        }
        // Publish the trampoline before enable() arms the patch, so the detour never sees a null original.
        s_camera_observer_update_original.store(result->original<CameraObserverUpdateFunc>(),
                                                std::memory_order_release);
        const auto armed = hooks.push(std::move(*result)).enable();
        if (!armed.has_value())
        {
            logger.warning("Camera: camera-observer hook could not be armed ({}); in third person the AI keeps judging "
                           "NPCs from the pulled-back camera",
                           armed.error().message());
            return;
        }
        logger.info("Camera: the AI camera observer follows the first-person eye in third person (C_CameraObserver "
                    "slot {} at {})",
                    Constants::CAMERA_OBSERVER_VTABLE_UPDATE_SLOT, DMK::format::format_address(target));
    }
} // namespace TPVCamera
