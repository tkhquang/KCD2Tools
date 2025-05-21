/**
 * @file hooks/tpv_input_hook.cpp
 * @brief Implementation of TPV camera input processing hooks
 *
 * Intercepts third-person camera input events to provide customizable
 * camera control including sensitivity adjustment and vertical limits.
 */
#define _USE_MATH_DEFINES // For M_PI
#include "tpv_input_hook.h"
#include "logger.h"
#include "constants.h"
#include "game_structures.h"
#include "utils.h"
// #include "aob_scanner.h" // No longer needed here
#include "global_state.h"
#include "config.h"
#include "ui_menu_hooks.h"  // For isGameMenuOpen()
#include "hook_manager.hpp" // Use HookManager

#include <algorithm> // For std::clamp
#include <atomic>
#include <cmath>  // For M_PI, std::abs
#include <string> // For std::string

// External config reference
extern Config g_config;

// Function typedef for TPV camera input processing
typedef void(__fastcall *TpvCameraInputFunc)(uintptr_t thisPtr, char *inputEventPtr);

// Hook state
static TpvCameraInputFunc fpTpvCameraInputOriginal = nullptr;
// static BYTE *g_tpvInputHookAddress = nullptr; // Managed by HookManager
static std::string g_tpvInputHookId = ""; // To store the ID from HookManager

// Camera control state
static std::atomic<float> g_currentPitch(0.0f);      // Current camera pitch in degrees (game uses degrees for input delta)
static std::atomic<bool> g_limitsInitialized(false); // Whether pitch limits are initialized

// Mouse event IDs for TPV camera control
constexpr int MOUSE_EVENT_ID_TPV_YAW = 0x10A;   // Horizontal rotation
constexpr int MOUSE_EVENT_ID_TPV_PITCH = 0x10B; // Vertical rotation
constexpr int MOUSE_EVENT_ID_TPV_ZOOM = 0x10C;  // Camera zoom

/**
 * @brief Detour function for TPV camera input processing
 * @details Intercepts mouse input events to apply custom sensitivity and limits
 * @param thisPtr Pointer to the camera object
 * @param inputEventPtr Pointer to input event data
 */
void __fastcall Detour_TpvCameraInput(uintptr_t thisPtr, char *inputEventPtr)
{
    Logger &logger = Logger::getInstance();

    // Basic validation of inputEventPtr
    if (!isMemoryReadable(inputEventPtr, sizeof(GameStructures::InputEvent)))
    {
        logger.log(LOG_TRACE, "TPVInputHook: inputEventPtr not readable or too small.");
        if (fpTpvCameraInputOriginal)
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
        return;
    }

    // Cast to input event structure
    GameStructures::InputEvent *event = reinterpret_cast<GameStructures::InputEvent *>(inputEventPtr);

    // Skip camera input processing if in-game menu is open or overlay is active
    // to prevent camera moving behind menus.
    if (isGameMenuOpen() || g_isOverlayActive.load(std::memory_order_relaxed))
    {
        // Zero out delta to prevent movement, then pass to original.
        // Some games still process input for other things even if menu is open.
        if (event->eventByte0 == 0x01 && event->eventType == 0x08 &&
            (event->eventId == MOUSE_EVENT_ID_TPV_YAW || event->eventId == MOUSE_EVENT_ID_TPV_PITCH))
        {
            if (isMemoryWritable(&(event->deltaValue), sizeof(float)))
            {
                event->deltaValue = 0.0f;
            }
        }
        if (fpTpvCameraInputOriginal)
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
        return;
    }

    // Check if it's a mouse event for TPV camera control
    if (event->eventByte0 == 0x01 && event->eventType == 0x08) // Typically mouse input
    {
        bool modifiedInput = false;
        float originalDeltaForLog = event->deltaValue; // For logging clarity

        switch (event->eventId)
        {
        case MOUSE_EVENT_ID_TPV_YAW:
        {
            float sensitivity = g_config.tpv_yaw_sensitivity;
            if (sensitivity != 1.0f && std::abs(event->deltaValue) > 1e-5f)
            {
                if (isMemoryWritable(&(event->deltaValue), sizeof(float)))
                {
                    event->deltaValue *= sensitivity;
                    modifiedInput = true;
                }
            }
            // No specific yaw state to track for limits currently
            break;
        }

        case MOUSE_EVENT_ID_TPV_PITCH:
        {
            float sensitivity = g_config.tpv_pitch_sensitivity;
            float pitchMin = g_config.tpv_pitch_min; // Degrees
            float pitchMax = g_config.tpv_pitch_max; // Degrees

            if (std::abs(event->deltaValue) > 1e-5f && isMemoryWritable(&(event->deltaValue), sizeof(float)))
            {
                float inputDeltaDegrees = event->deltaValue; // Game input delta is often in degrees like units
                float adjustedDeltaDegrees = inputDeltaDegrees * sensitivity;

                if (g_config.tpv_pitch_limits_enabled)
                {
                    if (!g_limitsInitialized.load(std::memory_order_relaxed))
                    {
                        g_currentPitch.store(0.0f, std::memory_order_relaxed); // Initialize to neutral (looking forward)
                        g_limitsInitialized.store(true, std::memory_order_relaxed);
                        logger.log(LOG_INFO, "TPVInputHook: Pitch tracking initialized for limits.");
                    }

                    float currentPitchDegrees = g_currentPitch.load(std::memory_order_relaxed);
                    float proposedPitchDegrees = currentPitchDegrees + adjustedDeltaDegrees;
                    float clampedPitchDegrees = std::clamp(proposedPitchDegrees, pitchMin, pitchMax);

                    // The actual delta to apply is the difference between clamped and current
                    adjustedDeltaDegrees = clampedPitchDegrees - currentPitchDegrees;

                    g_currentPitch.store(clampedPitchDegrees, std::memory_order_relaxed);
                }

                event->deltaValue = adjustedDeltaDegrees;
                modifiedInput = (std::abs(inputDeltaDegrees - adjustedDeltaDegrees) > 1e-5f); // Considered modified if it changed
            }
            break;
        }

        case MOUSE_EVENT_ID_TPV_ZOOM:
        {
            // No modification for zoom events currently
            break;
        }

        default:
        {
            // Unknown mouse event for TPV, pass through
            break;
        }
        }

        if (modifiedInput && std::abs(originalDeltaForLog) > 1e-5f)
        {
            // Log only significant input and its modification
            logger.log(LOG_TRACE, "TPVInput MODIFIED: EventID=" + format_hex(event->eventId) +
                                      " OriginalDelta=" + std::to_string(originalDeltaForLog) +
                                      " FinalDelta=" + std::to_string(event->deltaValue) +
                                      ((event->eventId == MOUSE_EVENT_ID_TPV_PITCH && g_config.tpv_pitch_limits_enabled) ? " CurrentPitch=" + std::to_string(g_currentPitch.load(std::memory_order_relaxed)) + "deg" : ""));
        }
        else if (std::abs(originalDeltaForLog) > 1e-5f)
        {
            // Log significant raw input even if not modified by us (for debugging game behavior)
            // logger.log(LOG_TRACE, "TPVInput RAW: EventID=" + format_hex(event->eventId) +
            //                           " Delta=" + std::to_string(originalDeltaForLog));
        }
    }

    // Always call original function
    if (fpTpvCameraInputOriginal)
    {
        fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
    }
}

bool initializeTpvInputHook(uintptr_t moduleBase, size_t moduleSize)
{
    Logger &logger = Logger::getInstance();

    // Check if any relevant feature is enabled
    if (g_config.tpv_pitch_sensitivity == 1.0f &&
        g_config.tpv_yaw_sensitivity == 1.0f &&
        !g_config.tpv_pitch_limits_enabled &&
        !g_config.enable_overlay_feature) // Overlay check is for menu open scenario
    {
        logger.log(LOG_INFO, "TPVInputHook: No TPV input modifications configured, hook skipped.");
        return true; // Not an error
    }

    logger.log(LOG_INFO, "TPVInputHook: Initializing TPV camera input processing hook...");

    try
    {
        HookManager &hookManager = HookManager::getInstance();
        g_tpvInputHookId = hookManager.create_inline_hook_aob(
            "TpvCameraInput",
            moduleBase,
            moduleSize,
            Constants::TPV_INPUT_PROCESS_AOB_PATTERN,
            0, // AOB_OFFSET
            reinterpret_cast<void *>(Detour_TpvCameraInput),
            reinterpret_cast<void **>(&fpTpvCameraInputOriginal));

        if (g_tpvInputHookId.empty() || fpTpvCameraInputOriginal == nullptr)
        {
            throw std::runtime_error("Failed to create TPV Camera Input hook via HookManager.");
        }

        logger.log(LOG_INFO, "TPVInputHook: TPV Camera Input hook successfully installed with ID: " + g_tpvInputHookId);
        logger.log(LOG_INFO, "  - Yaw Sensitivity: " + std::to_string(g_config.tpv_yaw_sensitivity));
        logger.log(LOG_INFO, "  - Pitch Sensitivity: " + std::to_string(g_config.tpv_pitch_sensitivity));
        if (g_config.tpv_pitch_limits_enabled)
        {
            logger.log(LOG_INFO, "  - Pitch Limits: ENABLED (" +
                                     std::to_string(g_config.tpv_pitch_min) + "° to " +
                                     std::to_string(g_config.tpv_pitch_max) + "°)");
        }
        else
        {
            logger.log(LOG_INFO, "  - Pitch Limits: DISABLED");
        }
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "TPVInputHook: Initialization failed: " + std::string(e.what()));
        cleanupTpvInputHook(); // Attempt cleanup
        return false;
    }
}

void cleanupTpvInputHook()
{
    Logger &logger = Logger::getInstance();

    if (!g_tpvInputHookId.empty())
    {
        if (HookManager::getInstance().remove_hook(g_tpvInputHookId))
        {
            logger.log(LOG_INFO, "TPVInputHook: Hook '" + g_tpvInputHookId + "' removed.");
        }
        else
        {
            logger.log(LOG_WARNING, "TPVInputHook: Failed to remove hook '" + g_tpvInputHookId + "' via HookManager.");
        }
        fpTpvCameraInputOriginal = nullptr; // Clear trampoline
        g_tpvInputHookId = "";
    }

    // Reset state
    resetCameraAngles(); // Calls the function below
    logger.log(LOG_DEBUG, "TPVInputHook: Cleanup complete.");
}

void resetCameraAngles()
{
    Logger::getInstance().log(LOG_DEBUG, "TPVInputHook: Resetting camera angles and pitch limit initialization state.");
    g_currentPitch.store(0.0f);
    // g_currentYaw is not strictly needed to be reset unless you implement yaw limits
    g_limitsInitialized.store(false);
}

bool isTpvInputHookActive()
{
    return (fpTpvCameraInputOriginal != nullptr && !g_tpvInputHookId.empty());
}
