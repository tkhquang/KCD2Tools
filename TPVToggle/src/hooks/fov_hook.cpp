/**
 * @file hooks/fov_hook.cpp
 * @brief Implementation of TPV FOV hook functionality using DetourModKit.
 */
#define _USE_MATH_DEFINES
#include "fov_hook.h"
#include "constants.h"
#include "utils.h"
#include "game_interface.h"

#include <DetourModKit.hpp>

#include <stdexcept>
#include <math.h>

using DetourModKit::LogLevel;
using DMKFormat::format_address;

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
    DMKLogger &logger = DMKLogger::get_instance();

    // Call original function first
    if (Original_TpvFovCalculate)
    {
        Original_TpvFovCalculate(pViewStruct, deltaTime);
    }
    else
    {
        logger.log(LogLevel::Error, "FovHook: Original function pointer is NULL!");
        return;
    }

    // Get camera manager and check TPV state
    uintptr_t cameraManager = getCameraManagerInstance();
    if (cameraManager != 0)
    {
        // Check if we're in TPV mode (flag at offset 0x38 in TPV object)
        volatile std::byte *flagAddress = getResolvedTpvFlagAddress();
        if (flagAddress && DMKMemory::is_readable(const_cast<std::byte *>(flagAddress), sizeof(std::byte)))
        {
            std::byte flagValue = *flagAddress;
            if (flagValue == std::byte{1})
            { // TPV mode
                // Apply custom FOV (field at offset 0x30 in view structure)
                uintptr_t fovWriteAddress = reinterpret_cast<uintptr_t>(pViewStruct) + Constants::OFFSET_TpvFovWrite;
                if (DMKMemory::is_writable(reinterpret_cast<void *>(fovWriteAddress), sizeof(float)))
                {
                    *reinterpret_cast<float *>(fovWriteAddress) = g_desiredFovRadians;
                    logger.log(LogLevel::Trace, "FovHook: Applied FOV " + std::to_string(g_desiredFovRadians) + " radians");
                }
            }
        }
    }
}

bool initializeFovHook(uintptr_t module_base, size_t module_size, float desired_fov_degrees)
{
    DMKLogger &logger = DMKLogger::get_instance();

    if (desired_fov_degrees <= 0.0f)
    {
        logger.log(LogLevel::Info, "FovHook: FOV feature disabled (degrees <= 0)");
        return true; // Not an error condition
    }

    try
    {
        logger.log(LogLevel::Info, "FovHook: Initializing TPV FOV hook...");

        // Convert degrees to radians
        g_desiredFovRadians = desired_fov_degrees * (M_PI / 180.0f);
        logger.log(LogLevel::Info, "FovHook: Target FOV set to " + std::to_string(desired_fov_degrees) + " degrees (" + std::to_string(g_desiredFovRadians) + " radians)");

        // Use DMKHookManager to create hook via AOB scan
        DMKHookManager &hook_manager = DMKHookManager::get_instance();

        auto result = hook_manager.create_inline_hook_aob(
            "TpvFovCalculate",
            module_base,
            module_size,
            Constants::TPV_FOV_CALCULATE_AOB_PATTERN,
            0, // No offset from pattern
            reinterpret_cast<void *>(Detour_TpvFovCalculate),
            reinterpret_cast<void **>(&Original_TpvFovCalculate));

        if (!result.has_value())
        {
            throw std::runtime_error("Failed to create TPV FOV hook: " + std::string(DMK::Hook::error_to_string(result.error())));
        }
        g_fovHookId = result.value();

        // Get the target address for logging
        (void)hook_manager.with_inline_hook(g_fovHookId, [&](DMK::InlineHook &hook) {
            logger.log(LogLevel::Info, "FovHook: Found TPV FOV function at " +
                                     format_address(hook.get_target_address()));
            return true;
        });

        logger.log(LogLevel::Info, "FovHook: TPV FOV hook successfully installed");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LogLevel::Error, "FovHook: Initialization failed: " + std::string(e.what()));
        cleanupFovHook();
        return false;
    }
}

void cleanupFovHook()
{
    DMKLogger &logger = DMKLogger::get_instance();

    if (!g_fovHookId.empty())
    {
        DMKHookManager &hook_manager = DMKHookManager::get_instance();

        if (hook_manager.remove_hook(g_fovHookId))
        {
            logger.log(LogLevel::Info, "FovHook: Successfully removed");
        }
        else
        {
            logger.log(LogLevel::Warning, "FovHook: Failed to remove hook");
        }

        g_fovHookId.clear();
        Original_TpvFovCalculate = nullptr;
    }

    logger.log(LogLevel::Debug, "FovHook: Cleanup complete");
}

bool isFovHookActive()
{
    return !g_fovHookId.empty();
}
