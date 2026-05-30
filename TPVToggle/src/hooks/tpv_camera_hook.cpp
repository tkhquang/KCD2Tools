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
#include <stdexcept>

using DetourModKit::LogLevel;

// External configuration reference
extern Config g_config;

// Hook state
static std::string g_tpvCameraHookId;

// Function typedef for the TPV camera update function
// RCX: C_CameraThirdPerson object pointer
// RDX: Pointer to output pose structure (position + quaternion)
typedef void(__fastcall *TpvCameraUpdateFunc)(uintptr_t thisPtr, uintptr_t outputPosePtr);

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
    if (g_config.enable_camera_profiles)
    {
        Vector3 transitionPosition;
        Quaternion transitionRotation;

        if (TransitionManager::getInstance().updateTransition(0.016f, transitionPosition, transitionRotation))
        {
            return transitionPosition;
        }

        // Priority 2: Camera profile system. load() takes a lock-free, tear-free
        // snapshot so this per-frame read never blocks on the profile writers.
        return g_currentCameraOffset.load();
    }

    // Priority 3: Static configuration offsets
    return Vector3(g_config.tpv_offset_x, g_config.tpv_offset_y, g_config.tpv_offset_z);
}

/**
 * @brief Detour function for TPV camera update
 * @details Intercepts the camera position calculation to apply custom offsets
 *          based on configuration, camera profiles, or transitions.
 * @param thisPtr Pointer to the camera object
 * @param outputPosePtr Pointer to output structure containing position/rotation
 */
static void __fastcall Detour_TpvCameraUpdate(uintptr_t thisPtr, uintptr_t outputPosePtr)
{
    DMKLogger &logger = DMKLogger::get_instance();

    if (!fpTpvCameraUpdateOriginal)
    {
        logger.log(LogLevel::Error, "TpvCameraHook: Original function pointer is NULL");
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
    if (!DMKMemory::plausible_userspace_ptr(outputPosePtr))
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

    logger.log(LogLevel::Trace, "TpvCameraHook: Applied offset - Local: " +
                              Vector3ToString(localOffset) + " World: " + Vector3ToString(worldOffset));
}

bool initializeTpvCameraHook(uintptr_t moduleBase, size_t moduleSize)
{
    DMKLogger &logger = DMKLogger::get_instance();

    if (!g_config.enable_camera_profiles &&
        g_config.tpv_offset_x == 0.0f &&
        g_config.tpv_offset_y == 0.0f &&
        g_config.tpv_offset_z == 0.0f)
    {
        logger.log(LogLevel::Info, "TpvCameraHook: Feature disabled (no offsets configured)");
        return true;
    }

    logger.log(LogLevel::Info, "TpvCameraHook: Initializing camera position offset hook...");

    try
    {
        DMKHookManager &hook_manager = DMKHookManager::get_instance();

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
        g_tpvCameraHookId = result.value();

        logger.log(LogLevel::Info, "TpvCameraHook: Successfully installed with configuration:");

        if (g_config.enable_camera_profiles)
        {
            logger.log(LogLevel::Info, "  - Camera profiles: ENABLED");
        }
        else
        {
            logger.log(LogLevel::Info, "  - Static offset: X=" + std::to_string(g_config.tpv_offset_x) +
                                     " Y=" + std::to_string(g_config.tpv_offset_y) +
                                     " Z=" + std::to_string(g_config.tpv_offset_z));
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LogLevel::Error, "TpvCameraHook: Initialization failed: " + std::string(e.what()));
        cleanupTpvCameraHook();
        return false;
    }
}

void cleanupTpvCameraHook()
{
    DMKLogger &logger = DMKLogger::get_instance();

    if (!g_tpvCameraHookId.empty())
    {
        DMKHookManager &hook_manager = DMKHookManager::get_instance();

        if (hook_manager.remove_hook(g_tpvCameraHookId))
        {
            logger.log(LogLevel::Info, "TpvCameraHook: Successfully removed");
        }
        else
        {
            logger.log(LogLevel::Warning, "TpvCameraHook: Failed to remove hook");
        }

        g_tpvCameraHookId.clear();
        fpTpvCameraUpdateOriginal = nullptr;
    }
}
