/**
 * @file hooks/fov_hook.cpp
 * @brief Implementation of TPV FOV hook functionality using DetourModKit.
 */
#include "fov_hook.hpp"
#include "constants.hpp"
#include "utils.hpp"
#include "game_interface.hpp"

#include <DetourModKit.hpp>

#include <numbers>
#include <stdexcept>

using DetourModKit::LogLevel;

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

    if (!Original_TpvFovCalculate)
    {
        logger.log(LogLevel::Error, "FovHook: Original function pointer is NULL!");
        return;
    }

    // Let the engine compute the base view first.
    Original_TpvFovCalculate(pViewStruct, deltaTime);

    // Only override the FOV while the third-person view is active. getViewState()
    // resolves the camera-manager chain and reads the flag under one SEH frame,
    // so no separate is_readable gate is needed here.
    if (getViewState() != 1 || !pViewStruct)
        return;

    // pViewStruct is the view buffer the original just populated, so the FOV
    // field (Constants::OFFSET_TpvFovWrite) is live and writable; write directly
    // instead of gating an engine-handed pointer with is_writable.
    float *fovField = reinterpret_cast<float *>(
        reinterpret_cast<uintptr_t>(pViewStruct) + Constants::OFFSET_TpvFovWrite);
    *fovField = g_desiredFovRadians;
    logger.log(LogLevel::Trace, "FovHook: Applied FOV " + std::to_string(g_desiredFovRadians) + " radians");
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

        g_desiredFovRadians = desired_fov_degrees * (std::numbers::pi_v<float> / 180.0f);
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
