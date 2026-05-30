/**
 * @file hooks/fov_hook.cpp
 * @brief Implementation of TPV FOV hook functionality using DetourModKit.
 */
#include "fov_hook.hpp"
#include "constants.hpp"
#include "game_interface.hpp"

#include <DetourModKit.hpp>

#include <numbers>
#include <stdexcept>

using DetourModKit::LogLevel;

namespace TPVToggle
{

// Hook state
static TpvFovCalculateFunc Original_TpvFovCalculate = nullptr;
static float g_desiredFovRadians = 0.0f;

/**
 * @brief Body of the TPV FOV calculation detour.
 * @details Intercepts the TPV FOV update function and applies custom FOV when in
 *          TPV mode. Kept separate from the SEH wrapper below so structured
 *          exception handling does not share a frame with C++ object unwinding.
 */
static void Detour_TpvFovCalculate_Impl(float *pViewStruct, float deltaTime)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!Original_TpvFovCalculate)
    {
        logger.error("FovHook: Original function pointer is NULL!");
        return;
    }

    // Let the engine compute the base view first.
    Original_TpvFovCalculate(pViewStruct, deltaTime);

    // Only override the FOV while the third-person view is active. Test the cheap
    // null pointer first; getViewState() walks the camera-manager chain under an
    // SEH frame, so it should run only after the trivial guard passes.
    if (!pViewStruct || getViewState() != 1)
        return;

    // pViewStruct is the view buffer the original just populated, so the FOV
    // field (Constants::OFFSET_TpvFovWrite) is live and writable; write directly
    // instead of gating an engine-handed pointer with is_writable.
    float *fovField = reinterpret_cast<float *>(
        reinterpret_cast<uintptr_t>(pViewStruct) + Constants::OFFSET_TpvFovWrite);
    *fovField = g_desiredFovRadians;
    logger.log(LogLevel::Trace, "FovHook: Applied FOV {} radians", g_desiredFovRadians);
}

/**
 * @brief SEH wrapper for the per-frame TPV FOV calculation detour.
 * @details Degrades to a no-op if a post-patch view-struct layout change faults,
 *          rather than crashing the engine. The body lives in the _Impl function
 *          because __try cannot share a frame with C++ destructor unwinding.
 */
static void __fastcall Detour_TpvFovCalculate(float *pViewStruct, float deltaTime)
{
    __try
    {
        Detour_TpvFovCalculate_Impl(pViewStruct, deltaTime);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Swallow: an outdated FOV field offset must never crash the game.
    }
}

bool initializeFovHook(uintptr_t module_base, size_t module_size, float desired_fov_degrees)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (desired_fov_degrees <= 0.0f)
    {
        logger.info("FovHook: FOV feature disabled (degrees <= 0)");
        return true; // Not an error condition
    }

    try
    {
        g_desiredFovRadians = desired_fov_degrees * (std::numbers::pi_v<float> / 180.0f);
        logger.info("FovHook: Target FOV set to {} degrees ({} radians)", desired_fov_degrees, g_desiredFovRadians);

        // Use DMK::HookManager to create hook via AOB scan
        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

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

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("FovHook: Initialization failed: {}", e.what());
        return false;
    }
}

} // namespace TPVToggle
