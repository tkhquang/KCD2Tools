/**
 * @file hooks/fov_hook.cpp
 * @brief Implementation of TPV FOV hook functionality using DetourModKit.
 */
#define _USE_MATH_DEFINES
#include "fov_hook.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "game_interface.h"

#include <DetourModKit.hpp>

#include <stdexcept>
#include <math.h>

using DMKString::format_address;

// Hook state
static TpvFovCalculateFunc Original_TpvFovCalculate = nullptr;
static std::string g_fovHookId;
static float g_desiredFovRadians = 0.0f;

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
        logger.log(LOG_ERROR, "FovHook: Original function pointer is NULL!");
        return;
    }

    // Get camera manager and check TPV state
    uintptr_t cameraManager = getCameraManagerInstance();
    if (cameraManager != 0)
    {
        // Check if we're in TPV mode (flag at offset 0x38 in TPV object)
        volatile std::byte *flagAddress = getResolvedTpvFlagAddress();
        if (flagAddress && DMKMemory::isMemoryReadable(flagAddress, sizeof(std::byte)))
        {
            std::byte flagValue = *flagAddress;
            if (flagValue == std::byte{1})
            { // TPV mode
                // Apply custom FOV (field at offset 0x30 in view structure)
                uintptr_t fovWriteAddress = reinterpret_cast<uintptr_t>(pViewStruct) + Constants::OFFSET_TpvFovWrite;
                if (DMKMemory::isMemoryWritable(reinterpret_cast<void *>(fovWriteAddress), sizeof(float)))
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
        g_desiredFovRadians = desired_fov_degrees * (M_PI / 180.0f);
        logger.log(LOG_INFO, "FovHook: Target FOV set to " + std::to_string(desired_fov_degrees) + " degrees (" + std::to_string(g_desiredFovRadians) + " radians)");

        // Use DMKHookManager to create hook via AOB scan
        DMKHookManager &hook_manager = DMKHookManager::getInstance();

        g_fovHookId = hook_manager.create_inline_hook_aob(
            "TpvFovCalculate",
            module_base,
            module_size,
            Constants::TPV_FOV_CALCULATE_AOB_PATTERN,
            0, // No offset from pattern
            reinterpret_cast<void *>(Detour_TpvFovCalculate),
            reinterpret_cast<void **>(&Original_TpvFovCalculate));

        if (g_fovHookId.empty())
        {
            throw std::runtime_error("Failed to create TPV FOV hook via AOB scan");
        }

        // Get the target address for logging
        DMK::InlineHook *hook = hook_manager.get_inline_hook(g_fovHookId);
        if (hook)
        {
            logger.log(LOG_INFO, "FovHook: Found TPV FOV function at " +
                                     format_address(hook->getTargetAddress()));
        }

        logger.log(LOG_INFO, "FovHook: TPV FOV hook successfully installed");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "FovHook: Initialization failed: " + std::string(e.what()));
        cleanupFovHook();
        return false;
    }
}

void cleanupFovHook()
{
    Logger &logger = Logger::getInstance();

    if (!g_fovHookId.empty())
    {
        DMKHookManager &hook_manager = DMKHookManager::getInstance();

        if (hook_manager.remove_hook(g_fovHookId))
        {
            logger.log(LOG_INFO, "FovHook: Successfully removed");
        }
        else
        {
            logger.log(LOG_WARNING, "FovHook: Failed to remove hook");
        }

        g_fovHookId.clear();
        Original_TpvFovCalculate = nullptr;
    }

    logger.log(LOG_DEBUG, "FovHook: Cleanup complete");
}

bool isFovHookActive()
{
    return !g_fovHookId.empty();
}
