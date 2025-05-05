/**
 * @file hooks/fov_hook.cpp
 * @brief Implementation of TPV FOV hook functionality.
 */

#include "fov_hook.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "aob_scanner.h"
#include "game_interface.h"
#include "MinHook.h"

#include <stdexcept>
#include <math.h>

// Hook state
static TpvFovCalculateFunc Original_TpvFovCalculate = nullptr;
static BYTE *g_fovHookAddress = nullptr;
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
                    // Disbabled for now due to spammy
                    // if (logger.isDebugEnabled())
                    // {
                    //     logger.log(LOG_DEBUG, "FovHook: Applied FOV " + std::to_string(g_desiredFovRadians) + " radians");
                    // }
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

        // Scan for FOV calculation function
        std::vector<BYTE> fov_pat = parseAOB(Constants::TPV_FOV_CALCULATE_AOB_PATTERN);
        if (fov_pat.empty())
        {
            throw std::runtime_error("Failed to parse TPV FOV AOB pattern");
        }

        BYTE *fov_aob = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, fov_pat);
        if (!fov_aob)
        {
            throw std::runtime_error("TPV FOV function AOB pattern not found");
        }

        g_fovHookAddress = fov_aob;
        logger.log(LOG_INFO, "FovHook: Found TPV FOV function at " + format_address(reinterpret_cast<uintptr_t>(g_fovHookAddress)));

        // Create and enable the MinHook
        MH_STATUS status = MH_CreateHook(g_fovHookAddress,
                                         reinterpret_cast<LPVOID>(Detour_TpvFovCalculate),
                                         reinterpret_cast<LPVOID *>(&Original_TpvFovCalculate));

        if (status != MH_OK)
        {
            throw std::runtime_error("MH_CreateHook failed: " + std::string(MH_StatusToString(status)));
        }

        if (!Original_TpvFovCalculate)
        {
            MH_RemoveHook(g_fovHookAddress);
            throw std::runtime_error("MH_CreateHook returned NULL trampoline");
        }

        status = MH_EnableHook(g_fovHookAddress);
        if (status != MH_OK)
        {
            MH_RemoveHook(g_fovHookAddress);
            throw std::runtime_error("MH_EnableHook failed: " + std::string(MH_StatusToString(status)));
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

    if (g_fovHookAddress && Original_TpvFovCalculate)
    {
        MH_DisableHook(g_fovHookAddress);
        MH_RemoveHook(g_fovHookAddress);
        Original_TpvFovCalculate = nullptr;
        g_fovHookAddress = nullptr;
    }

    logger.log(LOG_DEBUG, "FovHook: Cleanup complete");
}

bool isFovHookActive()
{
    return (g_fovHookAddress != nullptr && Original_TpvFovCalculate != nullptr);
}
