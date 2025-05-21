/**
 * @file hooks/fov_hook.cpp
 * @brief Implementation of TPV FOV hook functionality.
 */
#define _USE_MATH_DEFINES
#include "fov_hook.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
// #include "aob_scanner.h" // No longer needed here if HookManager handles AOB
#include "game_interface.h"
#include "hook_manager.hpp" // Use HookManager
#include "global_state.h"   // For g_ModuleBase, g_ModuleSize

#include <stdexcept>
#include <math.h> // For M_PI

// Hook state
static TpvFovCalculateFunc Original_TpvFovCalculate = nullptr;
// static BYTE *g_fovHookAddress = nullptr; // Managed by HookManager
static float g_desiredFovRadians = 0.0f;
static std::string g_fovHookId = ""; // To store the ID from HookManager

/**
 * @brief Detour function for TPV FOV calculation.
 * @details Intercepts the TPV FOV update function and applies custom FOV when in TPV mode.
 */
static void __fastcall Detour_TpvFovCalculate(float *pViewStruct, float deltaTime)
{
    Logger &logger = Logger::getInstance();

    // Call original function first
    if (Original_TpvFovCalculate)
    {
        Original_TpvFovCalculate(pViewStruct, deltaTime);
    }
    else
    {
        // This case should ideally not happen if hook setup was successful
        logger.log(LOG_ERROR, "FovHook: Original_TpvFovCalculate (trampoline) is NULL!");
        return;
    }

    // Get camera manager and check TPV state
    uintptr_t cameraManager = getCameraManagerInstance();
    if (cameraManager != 0)
    {
        // Check if we're in TPV mode (flag at offset 0x38 in TPV object)
        volatile BYTE *flagAddress = getResolvedTpvFlagAddress();
        if (flagAddress && isMemoryReadable(flagAddress, sizeof(BYTE)))
        {
            BYTE flagValue = *flagAddress;
            if (flagValue == 1)
            { // TPV mode
                // Apply custom FOV (field at offset 0x30 in view structure)
                uintptr_t fovWriteAddress = reinterpret_cast<uintptr_t>(pViewStruct) + Constants::OFFSET_TpvFovWrite;
                if (isMemoryWritable(reinterpret_cast<void *>(fovWriteAddress), sizeof(float)))
                {
                    *reinterpret_cast<float *>(fovWriteAddress) = g_desiredFovRadians;
                    logger.log(LOG_TRACE, "FovHook: Applied FOV " + std::to_string(g_desiredFovRadians) + " radians");
                }
            }
        }
    }
}

bool initializeFovHook(uintptr_t module_base, size_t module_size, float desired_fov_degrees)
{
    Logger &logger = Logger::getInstance();

    if (desired_fov_degrees <= 0.0f)
    {
        logger.log(LOG_INFO, "FovHook: FOV feature disabled (degrees <= 0)");
        return true; // Not an error condition
    }

    try
    {
        logger.log(LOG_INFO, "FovHook: Initializing TPV FOV hook...");

        // Convert degrees to radians
        g_desiredFovRadians = desired_fov_degrees * (static_cast<float>(M_PI) / 180.0f);
        logger.log(LOG_INFO, "FovHook: Target FOV set to " + std::to_string(desired_fov_degrees) + " degrees (" + std::to_string(g_desiredFovRadians) + " radians)");

        HookManager &hookManager = HookManager::getInstance();
        g_fovHookId = hookManager.create_inline_hook_aob(
            "TpvFovCalculate",
            module_base,
            module_size,
            Constants::TPV_FOV_CALCULATE_AOB_PATTERN,
            0, // AOB_OFFSET for this hook
            reinterpret_cast<void *>(Detour_TpvFovCalculate),
            reinterpret_cast<void **>(&Original_TpvFovCalculate));

        if (g_fovHookId.empty() || Original_TpvFovCalculate == nullptr)
        {
            throw std::runtime_error("Failed to create TPV FOV hook via HookManager.");
        }

        logger.log(LOG_INFO, "FovHook: TPV FOV hook successfully installed with ID: " + g_fovHookId);
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "FovHook: Initialization failed: " + std::string(e.what()));
        cleanupFovHook(); // Attempt cleanup
        return false;
    }
}

void cleanupFovHook()
{
    Logger &logger = Logger::getInstance();

    if (!g_fovHookId.empty())
    {
        if (HookManager::getInstance().remove_hook(g_fovHookId))
        {
            logger.log(LOG_INFO, "FovHook: Hook '" + g_fovHookId + "' removed.");
        }
        else
        {
            logger.log(LOG_WARNING, "FovHook: Failed to remove hook '" + g_fovHookId + "' via HookManager.");
        }
        Original_TpvFovCalculate = nullptr; // Clear trampoline
        g_fovHookId = "";
    }
    logger.log(LOG_DEBUG, "FovHook: Cleanup complete");
}

bool isFovHookActive()
{
    // Check if the trampoline is not null and the hook ID is set
    return (Original_TpvFovCalculate != nullptr && !g_fovHookId.empty());
}
