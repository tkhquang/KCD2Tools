/**
 * @file tpv_camera_hook.cpp
 * @brief Implementation of third-person camera position offset hook using DetourModKit.
 *
 * Intercepts the TPV camera update function to apply custom position offsets,
 * enabling over-the-shoulder and other camera positioning options.
 */

#include "tpv_camera_hook.hpp"
#include "constants.hpp"
#include "game_structures.hpp"
#include "utils.hpp"
#include "global_state.hpp"
#include "game_interface.hpp"
#include "math_utils.hpp"
#include "config.hpp"
#include "transition_manager.hpp"

#include <DetourModKit.hpp>

#include <DirectXMath.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

using DetourModKit::LogLevel;

namespace TPVToggle
{

// Function pointer type for the TPV camera update function.
//   RCX: C_CameraThirdPerson object pointer
//   RDX: pointer to the output pose structure (position + quaternion)
using TpvCameraUpdateFunc = void(__fastcall *)(uintptr_t thisPtr, uintptr_t outputPosePtr);

// Original function pointer (trampoline from SafetyHook)
static TpvCameraUpdateFunc fpTpvCameraUpdateOriginal = nullptr;

/**
 * @brief Gets the currently active camera offset
 * @details Determines which offset source to use based on configuration
 * @return Vector3 The local space offset to apply
 */
static Vector3 GetActiveOffset()
{
    // Priority 1: Active transition
    if (settings().enableCameraProfiles.load())
    {
        Vector3 transitionPosition;
        Quaternion transitionRotation;

        // Advance the transition by the measured frame delta so it runs at
        // wall-clock speed at any frame rate (a fixed 0.016f would tie the
        // transition duration to ~62.5 fps). GetActiveOffset() is called once per
        // detour invocation, so one static timestamp per call is correct.
        static std::chrono::steady_clock::time_point s_lastTime = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(now - s_lastTime).count();
        s_lastTime = now;
        // Clamp so the first frame and any frame hitch cannot jump (or, at zero,
        // stall) the transition integrator.
        deltaTime = std::clamp(deltaTime, 0.0001f, 0.1f);

        if (TransitionManager::getInstance().updateTransition(deltaTime, transitionPosition, transitionRotation))
        {
            return transitionPosition;
        }

        // Priority 2: Camera profile system. load() takes a lock-free, tear-free
        // snapshot so this per-frame read never blocks on the profile writers.
        return camera_state().offset.load();
    }

    // Priority 3: Static configuration offsets
    return Vector3(settings().tpvOffsetX.load(), settings().tpvOffsetY.load(), settings().tpvOffsetZ.load());
}

/**
 * @brief Body of the TPV camera update detour.
 * @details Intercepts the camera position calculation to apply custom offsets
 *          based on configuration, camera profiles, or transitions. Lives in a
 *          dedicated function so the SEH wrapper below stays free of C++ object
 *          unwinding (MSVC forbids __try in a frame that requires it).
 * @param thisPtr Pointer to the camera object
 * @param outputPosePtr Pointer to output structure containing position/rotation
 */
static void Detour_TpvCameraUpdate_Impl(uintptr_t thisPtr, uintptr_t outputPosePtr)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!fpTpvCameraUpdateOriginal)
    {
        logger.error("TpvCameraHook: Original function pointer is NULL");
        return;
    }

    // Let the engine compute the base camera pose first.
    fpTpvCameraUpdateOriginal(thisPtr, outputPosePtr);

    // Only adjust the pose while the third-person view is active.
    if (outputPosePtr == 0 || getViewState() != 1)
        return;

    // outputPosePtr is the buffer the original function just populated, so it is
    // live and writable by definition (hot-path guide: do not gate an
    // engine-handed pointer with is_readable/is_writable). A cheap arithmetic
    // screen rejects an obviously bad pointer without a syscall or a region-cache
    // lock.
    if (!DMK::Memory::plausible_userspace_ptr(outputPosePtr))
        return;

    Vector3 *positionPtr = reinterpret_cast<Vector3 *>(
        outputPosePtr + Constants::TPV_OUTPUT_POSE_POSITION_OFFSET);
    Quaternion *rotationPtr = reinterpret_cast<Quaternion *>(
        outputPosePtr + Constants::TPV_OUTPUT_POSE_ROTATION_OFFSET);

    const Vector3 currentPosition = *positionPtr;
    const Quaternion currentRotation = *rotationPtr;

    // Priority order: active transition > camera profile > static config.
    const Vector3 localOffset = GetActiveOffset();
    if (localOffset.x == 0.0f && localOffset.y == 0.0f && localOffset.z == 0.0f)
        return;

    // Rotate the local-space offset into world space and apply it on top of the
    // engine-computed position.
    const Vector3 worldOffset = currentRotation.Rotate(localOffset);
    *positionPtr = currentPosition + worldOffset;

    // Build the offset strings only when trace logging is active; Vector3ToString
    // allocates, and the arguments are evaluated before log() is entered.
    if (logger.is_enabled(LogLevel::Trace))
    {
        logger.log(LogLevel::Trace, "TpvCameraHook: Applied offset - Local: {} World: {}",
                   Vector3ToString(localOffset), Vector3ToString(worldOffset));
    }
}

/**
 * @brief SEH wrapper for the per-frame TPV camera update detour.
 * @details A post-patch register or pose-layout change must degrade this hot-path
 *          detour to a no-op rather than fault the engine. Structured exception
 *          handling cannot share a stack frame with C++ destructor unwinding, so
 *          the actual body runs in Detour_TpvCameraUpdate_Impl and this frame
 *          holds nothing that needs unwinding.
 */
static void __fastcall Detour_TpvCameraUpdate(uintptr_t thisPtr, uintptr_t outputPosePtr)
{
    __try
    {
        Detour_TpvCameraUpdate_Impl(thisPtr, outputPosePtr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Swallow: an outdated offset must never crash the game.
    }
}

bool initializeTpvCameraHook(uintptr_t moduleBase, size_t moduleSize)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!settings().enableCameraProfiles.load() &&
        settings().tpvOffsetX.load() == 0.0f &&
        settings().tpvOffsetY.load() == 0.0f &&
        settings().tpvOffsetZ.load() == 0.0f)
    {
        logger.info("TpvCameraHook: Feature disabled (no offsets configured)");
        return true;
    }

    try
    {
        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

        auto result = hook_manager.create_inline_hook_aob(
            "TpvCameraUpdate",
            moduleBase,
            moduleSize,
            Constants::TPV_CAMERA_UPDATE_AOB_PATTERN,
            0, // No offset from pattern
            reinterpret_cast<void *>(Detour_TpvCameraUpdate),
            reinterpret_cast<void **>(&fpTpvCameraUpdateOriginal));

        if (!result.has_value())
        {
            throw std::runtime_error("Failed to create TPV camera update hook: " + std::string(DMK::Hook::error_to_string(result.error())));
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("TpvCameraHook: Initialization failed: {}", e.what());
        return false;
    }
}

} // namespace TPVToggle
