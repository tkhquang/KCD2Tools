/**
 * @file hooks/tpv_input_hook.cpp
 * @brief Implementation of TPV camera input processing hooks
 *
 * Intercepts third-person camera input events to provide customizable
 * camera control including sensitivity adjustment and vertical limits.
 */
#define _USE_MATH_DEFINES
#include "tpv_input_hook.h"
#include "logger.h"
#include "constants.h"
#include "game_structures.h"
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h"
#include "config.h"
#include "ui_menu_hooks.h" // Add include for UI menu hooks
#include "MinHook.h"

#include <algorithm>
#include <atomic>
#include <cmath>

// External config reference
extern Config g_config;

// Function typedef for TPV camera input processing
typedef void(__fastcall *TpvCameraInputFunc)(uintptr_t thisPtr, char *inputEventPtr);

// Hook state
static TpvCameraInputFunc fpTpvCameraInputOriginal = nullptr;
static BYTE *g_tpvInputHookAddress = nullptr;

// Camera control state
static std::atomic<float> g_currentPitch(0.0f);      // Current camera pitch in radians
static std::atomic<float> g_currentYaw(0.0f);        // Current camera yaw in radians
static std::atomic<bool> g_limitsInitialized(false); // Whether pitch limits are initialized

// Mouse event IDs for TPV camera control
constexpr int MOUSE_EVENT_ID_TPV_YAW = 0x10A;   // Horizontal rotation
constexpr int MOUSE_EVENT_ID_TPV_PITCH = 0x10B; // Vertical rotation
constexpr int MOUSE_EVENT_ID_TPV_ZOOM = 0x10C;  // Camera zoom

// Convert degrees to radians
inline float DegreesToRadians(float degrees)
{
    return degrees * (M_PI / 180.0f);
}

// Convert radians to degrees
inline float RadiansToDegrees(float radians)
{
    return radians * (180.0f / M_PI);
}

/**
 * @brief Detour function for TPV camera input processing
 * @details Intercepts mouse input events to apply custom sensitivity and limits
 * @param thisPtr Pointer to the camera object
 * @param inputEventPtr Pointer to input event data
 */
void __fastcall Detour_TpvCameraInput(uintptr_t thisPtr, char *inputEventPtr)
{
    Logger &logger = Logger::getInstance();

    // Validate input event pointer
    if (!isMemoryReadable(inputEventPtr, sizeof(GameStructures::InputEvent)))
    {
        if (fpTpvCameraInputOriginal)
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
        return;
    }

    // Skip camera input processing if in-game menu is open
    if (isGameMenuOpen())
    {
        // When menu is open, just skip through
        return;
    }

    // Cast to input event structure
    GameStructures::InputEvent *event = reinterpret_cast<GameStructures::InputEvent *>(inputEventPtr);

    // Check if it's a mouse event for TPV camera
    if (event->eventByte0 == 0x01 && event->eventType == 0x08)
    {
        bool modifiedInput = false;

        // Debug logging of raw input
        if (std::abs(event->deltaValue) > 1e-5f)
        {
            logger.log(LOG_TRACE, "TPVInput RAW: EventID=" + format_hex(event->eventId) +
                                      " Delta=" + std::to_string(event->deltaValue));
        }

        switch (event->eventId)
        {
        case MOUSE_EVENT_ID_TPV_YAW:
        {
            // Apply horizontal sensitivity if configured
            float sensitivity = g_config.tpv_yaw_sensitivity;
            if (sensitivity != 1.0f && std::abs(event->deltaValue) > 1e-5f)
            {
                event->deltaValue *= sensitivity;
                modifiedInput = true;
            }

            // Update current yaw for tracking (game values already in degrees)
            g_currentYaw.store(g_currentYaw.load() + event->deltaValue);

            if (modifiedInput)
            {
                logger.log(LOG_TRACE, "TPVInput: Yaw adjusted with sensitivity " +
                                          std::to_string(sensitivity));
            }
            break;
        }

        case MOUSE_EVENT_ID_TPV_PITCH:
        {
            // Get current config values
            float sensitivity = g_config.tpv_pitch_sensitivity;
            float pitchMin = g_config.tpv_pitch_min;
            float pitchMax = g_config.tpv_pitch_max;

            if (std::abs(event->deltaValue) > 1e-5f)
            {
                float originalDelta = event->deltaValue;

                // Apply vertical sensitivity
                float adjustedDelta = event->deltaValue * sensitivity;

                // Apply pitch limits if enabled
                if (g_config.tpv_pitch_limits_enabled)
                {
                    // Initialize pitch if not already done
                    if (!g_limitsInitialized.load())
                    {
                        // Start at neutral position
                        g_currentPitch.store(0.0f);
                        g_limitsInitialized.store(true);
                        logger.log(LOG_INFO, "TPVInput: Initialized pitch tracking at 0°");
                    }

                    // Get current pitch and calculate new pitch (in degrees)
                    float currentPitch = g_currentPitch.load();
                    float proposedPitch = currentPitch + adjustedDelta;

                    // Clamp to limits
                    float clampedPitch = std::clamp(proposedPitch, pitchMin, pitchMax);

                    // Calculate actual delta after clamping
                    adjustedDelta = clampedPitch - currentPitch;

                    // Update stored pitch
                    g_currentPitch.store(clampedPitch);

                    logger.log(LOG_TRACE, "TPVInput PITCH: Original=" + std::to_string(originalDelta) +
                                              " Sens=" + std::to_string(sensitivity) +
                                              " AdjustedDelta=" + std::to_string(adjustedDelta) +
                                              " Current=" + std::to_string(currentPitch) + "°" +
                                              " Proposed=" + std::to_string(proposedPitch) + "°" +
                                              " Clamped=" + std::to_string(clampedPitch) + "°" +
                                              " Limits=[" + std::to_string(pitchMin) + "°, " +
                                              std::to_string(pitchMax) + "°]");
                }
                else
                {
                    logger.log(LOG_TRACE, "TPVInput PITCH: Original=" + std::to_string(originalDelta) +
                                              " Sens=" + std::to_string(sensitivity) +
                                              " Adjusted=" + std::to_string(adjustedDelta) +
                                              " (No limits)");
                }

                // Apply the adjusted delta
                event->deltaValue = adjustedDelta;
                modifiedInput = true;
            }
            break;
        }

        case MOUSE_EVENT_ID_TPV_ZOOM:
            // No modification for zoom events currently
            break;

        default:
            // Unknown event, pass through
            break;
        }

        // Log significant modifications
        if (modifiedInput)
        {
            logger.log(LOG_TRACE, "TPVInput MODIFIED: EventID=" + format_hex(event->eventId) +
                                      " FinalDelta=" + std::to_string(event->deltaValue));
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
    logger.log(LOG_INFO, "TPVInputHook: Initializing camera input processing hook...");

    try
    {
        // Parse AOB pattern
        std::vector<BYTE> pattern = parseAOB(Constants::TPV_INPUT_PROCESS_AOB_PATTERN);
        if (pattern.empty())
        {
            throw std::runtime_error("Failed to parse TPV input function AOB pattern");
        }

        // Find the function
        g_tpvInputHookAddress = FindPattern(reinterpret_cast<BYTE *>(moduleBase), moduleSize, pattern);
        if (!g_tpvInputHookAddress)
        {
            throw std::runtime_error("TPV input function pattern not found");
        }

        logger.log(LOG_INFO, "TPVInputHook: Found TPV input function at " +
                                 format_address(reinterpret_cast<uintptr_t>(g_tpvInputHookAddress)));

        // Create hook
        MH_STATUS status = MH_CreateHook(
            reinterpret_cast<LPVOID>(g_tpvInputHookAddress),
            reinterpret_cast<LPVOID>(&Detour_TpvCameraInput),
            reinterpret_cast<LPVOID *>(&fpTpvCameraInputOriginal));

        if (status != MH_OK)
        {
            throw std::runtime_error("MH_CreateHook failed: " + std::string(MH_StatusToString(status)));
        }

        if (!fpTpvCameraInputOriginal)
        {
            MH_RemoveHook(reinterpret_cast<LPVOID>(g_tpvInputHookAddress));
            throw std::runtime_error("MH_CreateHook returned NULL trampoline");
        }

        // Enable hook
        status = MH_EnableHook(reinterpret_cast<LPVOID>(g_tpvInputHookAddress));
        if (status != MH_OK)
        {
            MH_RemoveHook(reinterpret_cast<LPVOID>(g_tpvInputHookAddress));
            throw std::runtime_error("MH_EnableHook failed: " + std::string(MH_StatusToString(status)));
        }

        // Log configuration
        logger.log(LOG_INFO, "TPVInputHook: Successfully installed with config:");
        logger.log(LOG_INFO, "  - Yaw Sensitivity: " + std::to_string(g_config.tpv_yaw_sensitivity));
        logger.log(LOG_INFO, "  - Pitch Sensitivity: " + std::to_string(g_config.tpv_pitch_sensitivity));

        if (g_config.tpv_pitch_limits_enabled)
        {
            logger.log(LOG_INFO, "  - Pitch Limits: " +
                                     std::to_string(g_config.tpv_pitch_min) + "° to " +
                                     std::to_string(g_config.tpv_pitch_max) + "°");
        }
        else
        {
            logger.log(LOG_INFO, "  - Pitch Limits: Disabled");
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "TPVInputHook: Initialization failed: " + std::string(e.what()));
        cleanupTpvInputHook();
        return false;
    }
}

void cleanupTpvInputHook()
{
    Logger &logger = Logger::getInstance();

    if (g_tpvInputHookAddress != nullptr)
    {
        MH_STATUS disableStatus = MH_DisableHook(reinterpret_cast<LPVOID>(g_tpvInputHookAddress));
        MH_STATUS removeStatus = MH_RemoveHook(reinterpret_cast<LPVOID>(g_tpvInputHookAddress));

        if (disableStatus == MH_OK && removeStatus == MH_OK)
        {
            logger.log(LOG_INFO, "TPVInputHook: Successfully removed");
        }
        else
        {
            logger.log(LOG_WARNING, "TPVInputHook: Cleanup issues - Disable: " +
                                        std::string(MH_StatusToString(disableStatus)) +
                                        ", Remove: " + std::string(MH_StatusToString(removeStatus)));
        }

        g_tpvInputHookAddress = nullptr;
        fpTpvCameraInputOriginal = nullptr;
    }

    // Reset state
    g_currentPitch.store(0.0f);
    g_currentYaw.store(0.0f);
    g_limitsInitialized.store(false);
}

void resetCameraAngles()
{
    g_currentPitch.store(0.0f);
    g_currentYaw.store(0.0f);
    g_limitsInitialized.store(false);
}

void update_orbital_camera_rotation_from_euler()
{
    // This is a placeholder for orbital camera functionality
    // Currently the game handles camera rotation internally
    // This function would be used for custom orbital camera mode
    // which is not currently implemented in this version
}
