/**
 * @file camera_hooks.cpp
 * @brief Implementation of third-person camera position offset hook
 *
 * Intercepts the FPV camera update function to apply custom position offsets,
 * enabling over-the-shoulder and other camera positioning options.
 */

#include <DetourModKit.hpp>

#include "camera_hooks.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "math_utils.hpp"
#include "config.hpp"

#include <DirectXMath.h>
#include <stdexcept>

using namespace DetourModKit::String;
using namespace DetourModKit::Memory;
using namespace DetourModKit::Scanner;
using namespace GlobalState;

bool initializeFpvCameraHook(uintptr_t moduleBase, size_t moduleSize);
bool initializeCombatFpvCameraHook(uintptr_t moduleBase, size_t moduleSize);
void cleanupFpvCameraHook();
void cleanupCombatFpvCameraHook();

// Function typedef for the FPV camera update function
// RCX: C_CameraThirdPerson object pointer
// RDX: Pointer to output pose structure (position + quaternion)
typedef void(__fastcall *FpvCameraUpdateFunc)(uintptr_t thisPtr, uintptr_t outputPosePtr);

// Hook state
static FpvCameraUpdateFunc fpFpvCameraUpdateOriginal = nullptr;
static std::string g_fpvCameraHookId;

// Typedef for the Combat FPV camera update function
typedef void(__fastcall *CombatFpvCameraUpdateFunc)(uintptr_t thisPtr, uintptr_t outputPosePtr);

// Assuming FUN_180718ab4 is the target
// param_1: (this from C_Player or related context)
// param_2: (some player-internal output/state buffer from C_Player context)
// param_3: (output buffer for camera pose, this is what movsd [r14] writes to)
// param_4: (pointer to some component)
typedef void(__fastcall *ActionCameraUpdateFunc)(
    uintptr_t pPlayerContext,       // Corresponds to param_1_ab4
    uintptr_t pPlayerInternalState, // Corresponds to param_2_ab4
    uintptr_t pOutputPose,          // Corresponds to param_3_ab4
    uintptr_t pSomeComponent        // Corresponds to param_4_ab4
);
static ActionCameraUpdateFunc fpOriginal_ActionCameraUpdate = nullptr;
static std::string g_actionCameraHookId;

// Hook state for Combat FPV
static CombatFpvCameraUpdateFunc fpCombatFpvCameraUpdateOriginal = nullptr;
static std::string g_combatFpvCameraHookId;

/**
 * @brief Gets the currently active camera offset
 * @details Determines which offset source to use based on configuration
 * @return Vector3 The local space offset to apply
 */
Vector3 GetActiveFpvOffset()
{
    return Vector3(g_config.tpv_offset_x, g_config.tpv_offset_y, g_config.tpv_offset_z);
}

bool getViewState()
{
    return 1;
}

/**
 * @brief Detour function for FPV camera update
 * @details Intercepts the camera position calculation to apply custom offsets
 *          based on configuration, camera profiles, or transitions.
 * @param thisPtr Pointer to the camera object
 * @param outputPosePtr Pointer to output structure containing position/rotation
 */
void __fastcall Detour_FpvCameraUpdate(uintptr_t thisPtr, uintptr_t outputPosePtr)
{
    DMKLogger &logger = DMKLogger::getInstance();

    // Call original function first to get base camera position
    if (!fpFpvCameraUpdateOriginal)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "FpvCameraHook: Original function pointer is NULL");
        return;
    }

    try
    {
        fpFpvCameraUpdateOriginal(thisPtr, outputPosePtr);
    }
    catch (const std::exception &e)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "FpvCameraHook: Exception in original function: " + std::string(e.what()));
        return;
    }
    catch (...)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "FpvCameraHook: Unknown exception in original function");
        return;
    }

    // Validate parameters and check if we're in TPV mode
    if (outputPosePtr == 0 || getViewState() == 0)
    {
        return;
    }

    // Validate output buffer is accessible
    if (!isMemoryReadable(reinterpret_cast<void *>(outputPosePtr), Constants::OUTPUT_POSE_REQUIRED_SIZE))
    {
        logger.log(DMKLogLevel::LOG_DEBUG, "FpvCameraHook: Output pose buffer not readable");
        return;
    }

    try
    {
        Vector3 *pOutPosition = reinterpret_cast<Vector3 *>(outputPosePtr + Constants::OUTPUT_POSE_POSITION_OFFSET);
        Quaternion *pOutRotation = reinterpret_cast<Quaternion *>(outputPosePtr + Constants::OUTPUT_POSE_ROTATION_OFFSET);

        Vector3 playerEyePosition = *pOutPosition;    // Player's head/eye position from FPV
        Quaternion playerAimRotation = *pOutRotation; // Player's current FPV aiming rotation (world space)

        Vector3 finalCameraPosition;
        Quaternion finalCameraRotation;

        // Standard Over-the-Shoulder TPV Logic
        Vector3 localOffset = GetActiveFpvOffset();
        Vector3 worldOffset = playerAimRotation.Rotate(localOffset);
        finalCameraPosition = playerEyePosition + worldOffset;
        finalCameraRotation = playerAimRotation; // Camera aims where player aims

        // Write to game
        *pOutPosition = finalCameraPosition;
        *pOutRotation = finalCameraRotation;
    }
    catch (const std::exception &e)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "FpvCameraHook: Exception applying offset: " + std::string(e.what()));
    }
    catch (...)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "FpvCameraHook: Unknown exception applying offset");
    }
}

/**
 * @brief Detour function for Combat FPV camera update
 */
void __fastcall Detour_CombatFpvCameraUpdate(uintptr_t thisPtr, uintptr_t outputPosePtr)
{
    DMKLogger &logger = DMKLogger::getInstance();

    // Call original function first
    if (!fpCombatFpvCameraUpdateOriginal)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "CombatFpvCameraHook: Original function pointer is NULL");
        return;
    }

    try
    {
        fpCombatFpvCameraUpdateOriginal(thisPtr, outputPosePtr);
    }
    catch (const std::exception &e)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "CombatFpvCameraHook: Exception in original function: " + std::string(e.what()));
        return;
    }
    catch (...)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "CombatFpvCameraHook: Unknown exception in original function");
        return;
    }

    // Validate parameters and check if we're in TPV mode (0)
    if (outputPosePtr == 0 || getViewState() == 1)
    {
        return;
    }

    // Validate output buffer is accessible
    if (!isMemoryReadable(reinterpret_cast<void *>(outputPosePtr), Constants::OUTPUT_POSE_REQUIRED_SIZE))
    {
        logger.log(DMKLogLevel::LOG_DEBUG, "CombatFpvCameraHook: Output pose buffer not readable");
        return;
    }

    try
    {
        // Get pointers to position and rotation in the output structure
        Vector3 *positionPtr = reinterpret_cast<Vector3 *>(
            outputPosePtr + Constants::OUTPUT_POSE_POSITION_OFFSET);
        Quaternion *rotationPtr = reinterpret_cast<Quaternion *>(
            outputPosePtr + Constants::OUTPUT_POSE_ROTATION_OFFSET);

        Vector3 currentPosition = *positionPtr;
        Quaternion currentRotation = *rotationPtr;

        // Get the FPV offset
        Vector3 localOffset = GetActiveFpvOffset();

        if (localOffset.x == 0.0f && localOffset.y == 0.0f && localOffset.z == 0.0f)
        {
            return; // No offset to apply
        }

        Vector3 worldOffset = currentRotation.Rotate(localOffset);
        Vector3 newPosition = currentPosition + worldOffset;

        if (isMemoryWritable(positionPtr, sizeof(Vector3)))
        {
            *positionPtr = newPosition;
        }
        else
        {
            logger.log(DMKLogLevel::LOG_WARNING, "CombatFpvCameraHook: Cannot write to position buffer");
        }
    }
    catch (const std::exception &e)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "CombatFpvCameraHook: Exception applying offset: " + std::string(e.what()));
    }
    catch (...)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "CombatFpvCameraHook: Unknown exception applying offset");
    }
}

bool initializeCombatFpvCameraHook(uintptr_t moduleBase, size_t moduleSize)
{
    DMKLogger &logger = DMKLogger::getInstance();
    DMKHookManager &hookManager = DMKHookManager::getInstance();

    // Check if FPV feature is disabled or all FPV offsets are zero
    if (g_config.tpv_offset_x == 0.0f &&
        g_config.tpv_offset_y == 0.0f &&
        g_config.tpv_offset_z == 0.0f)
    {
        logger.log(DMKLogLevel::LOG_INFO, "CombatFpvCameraHook: Feature disabled (no FPV offsets configured, skipping combat hook)");
        return true; // Don't hook if primary FPV offset is disabled
    }

    logger.log(DMKLogLevel::LOG_INFO, "CombatFpvCameraHook: Initializing combat FPV camera position offset hook...");

    try
    {
        // Parse AOB pattern
        std::vector<std::byte> pattern = parseAOB(Constants::COMBAT_FPV_CAMERA_UPDATE_AOB_PATTERN);
        if (pattern.empty())
        {
            throw std::runtime_error("Failed to parse Combat FPV camera update AOB pattern");
        }

        // Find the target function
        std::byte *hookAddress = FindPattern(
            reinterpret_cast<std::byte *>(moduleBase), moduleSize, pattern);
        if (!hookAddress)
        {
            throw std::runtime_error("Combat FPV camera update pattern not found (WHGame.DLL + 0x9E01A8)");
        }

        logger.log(DMKLogLevel::LOG_INFO, "CombatFpvCameraHook: Found Combat FPV camera update at " +
                                              format_address(reinterpret_cast<uintptr_t>(hookAddress)));

        // Create the hook using DetourModKit
        g_combatFpvCameraHookId = hookManager.create_inline_hook_aob(
            "CombatFpvCameraUpdate",
            moduleBase,
            moduleSize,
            Constants::COMBAT_FPV_CAMERA_UPDATE_AOB_PATTERN,
            0, // No offset needed as AOB points to function start
            reinterpret_cast<void *>(&Detour_CombatFpvCameraUpdate),
            reinterpret_cast<void **>(&fpCombatFpvCameraUpdateOriginal),
            DMKHookConfig{.autoEnable = true});

        if (g_combatFpvCameraHookId.empty())
        {
            throw std::runtime_error("Failed to create Combat FPV camera hook");
        }

        if (!fpCombatFpvCameraUpdateOriginal)
        {
            hookManager.remove_hook(g_combatFpvCameraHookId);
            throw std::runtime_error("Combat FPV hook creation returned NULL trampoline");
        }

        logger.log(DMKLogLevel::LOG_INFO, "CombatFpvCameraHook: Successfully installed.");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "CombatFpvCameraHook: Initialization failed: " + std::string(e.what()));
        cleanupCombatFpvCameraHook();
        return false;
    }
}

void cleanupCombatFpvCameraHook()
{
    DMKLogger &logger = DMKLogger::getInstance();
    DMKHookManager &hookManager = DMKHookManager::getInstance();

    if (!g_combatFpvCameraHookId.empty())
    {
        bool removed = hookManager.remove_hook(g_combatFpvCameraHookId);
        if (removed)
        {
            logger.log(DMKLogLevel::LOG_INFO, "CombatFpvCameraHook: Successfully removed");
        }
        else
        {
            logger.log(DMKLogLevel::LOG_WARNING, "CombatFpvCameraHook: Failed to remove hook");
        }
        g_combatFpvCameraHookId.clear();
        fpCombatFpvCameraUpdateOriginal = nullptr;
    }
}

bool initializeFpvCameraHook(uintptr_t moduleBase, size_t moduleSize)
{
    DMKLogger &logger = DMKLogger::getInstance();
    DMKHookManager &hookManager = DMKHookManager::getInstance();

    logger.log(DMKLogLevel::LOG_INFO, "FpvCameraHook: Initializing camera position offset hook...");

    try
    {
        // Parse AOB pattern
        std::vector<std::byte> pattern = parseAOB(Constants::FPV_CAMERA_UPDATE_AOB_PATTERN);
        if (pattern.empty())
        {
            throw std::runtime_error("Failed to parse FPV camera update AOB pattern");
        }

        // Find the target function
        std::byte *hookAddress = FindPattern(
            reinterpret_cast<std::byte *>(moduleBase), moduleSize, pattern);
        if (!hookAddress)
        {
            throw std::runtime_error("FPV camera update pattern not found");
        }

        logger.log(DMKLogLevel::LOG_INFO, "FpvCameraHook: Found FPV camera update at " +
                                              format_address(reinterpret_cast<uintptr_t>(hookAddress)));

        // Create the hook using DetourModKit
        g_fpvCameraHookId = hookManager.create_inline_hook_aob(
            "FpvCameraUpdate",
            moduleBase,
            moduleSize,
            Constants::FPV_CAMERA_UPDATE_AOB_PATTERN,
            0, // No offset needed as AOB points to function start
            reinterpret_cast<void *>(&Detour_FpvCameraUpdate),
            reinterpret_cast<void **>(&fpFpvCameraUpdateOriginal),
            DMKHookConfig{.autoEnable = true});

        if (g_fpvCameraHookId.empty())
        {
            throw std::runtime_error("Failed to create FPV camera hook");
        }

        if (!fpFpvCameraUpdateOriginal)
        {
            hookManager.remove_hook(g_fpvCameraHookId);
            throw std::runtime_error("FPV hook creation returned NULL trampoline");
        }

        // Log configuration
        logger.log(DMKLogLevel::LOG_INFO, "FpvCameraHook: Successfully installed with configuration:");
        logger.log(DMKLogLevel::LOG_INFO, "  - Static offset: X=" + std::to_string(g_config.tpv_offset_x) +
                                              " Y=" + std::to_string(g_config.tpv_offset_y) +
                                              " Z=" + std::to_string(g_config.tpv_offset_z));
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "FpvCameraHook: Initialization failed: " + std::string(e.what()));
        cleanupFpvCameraHook();
        return false;
    }
}

void cleanupFpvCameraHook()
{
    DMKLogger &logger = DMKLogger::getInstance();
    DMKHookManager &hookManager = DMKHookManager::getInstance();

    if (!g_fpvCameraHookId.empty())
    {
        bool removed = hookManager.remove_hook(g_fpvCameraHookId);
        if (removed)
        {
            logger.log(DMKLogLevel::LOG_INFO, "FpvCameraHook: Successfully removed");
        }
        else
        {
            logger.log(DMKLogLevel::LOG_WARNING, "FpvCameraHook: Failed to remove hook");
        }
        g_fpvCameraHookId.clear();
        fpFpvCameraUpdateOriginal = nullptr;
    }
}

bool initializeCameraHooks(uintptr_t moduleBase, size_t moduleSize)
{
    DMKLogger &logger = DMKLogger::getInstance();

    bool all_success = true;

    if (!initializeFpvCameraHook(moduleBase, moduleSize))
    {
        logger.log(DMKLogLevel::LOG_WARNING, "FPV Camera Offset Hook initialization failed - Offset feature disabled.");
        all_success = false;
    }

    if (!initializeCombatFpvCameraHook(moduleBase, moduleSize))
    {
        logger.log(DMKLogLevel::LOG_WARNING, "FPV Combat Camera Offset Hook initialization failed - Offset feature disabled.");
        all_success = false;
    }

    return all_success;
}

void cleanupCameraHooks()
{
    cleanupFpvCameraHook();
    cleanupCombatFpvCameraHook();
}
