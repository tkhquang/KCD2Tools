/**
 * @file hooks/tpv_input_hook.cpp
 * @brief Implementation of TPV camera input processing hooks using DetourModKit.
 *
 * Intercepts third-person camera input events to provide customizable
 * camera control including sensitivity adjustment and vertical limits.
 */
#include "tpv_input_hook.hpp"
#include "constants.hpp"
#include "game_structures.hpp"
#include "utils.hpp"
#include "global_state.hpp"
#include "config.hpp"
#include "ui_menu_hooks.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>

using DetourModKit::LogLevel;
using DMKFormat::format_hex;

// External config reference
extern Config g_config;

// Function typedef for TPV camera input processing
typedef void(__fastcall *TpvCameraInputFunc)(uintptr_t thisPtr, char *inputEventPtr);

// Hook state
static std::string g_tpvInputHookId;

// Original function pointer (trampoline from SafetyHook)
static TpvCameraInputFunc fpTpvCameraInputOriginal = nullptr;

// Camera control state. Pitch is accumulated in degrees so it can be clamped to
// the configured limits; the game already reports input deltas in degrees.
static std::atomic<float> g_currentPitch(0.0f);
static std::atomic<bool> g_limitsInitialized(false);

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
static void __fastcall Detour_TpvCameraInput(uintptr_t thisPtr, char *inputEventPtr)
{
    DMKLogger &logger = DMKLogger::get_instance();

    // inputEventPtr is engine-handed and forwarded straight to the original, so
    // it is live by definition; screen it with a cheap arithmetic check (no
    // syscall, no region-cache lock) instead of an is_readable probe on this
    // per-event path.
    if (!DMKMemory::plausible_userspace_ptr(reinterpret_cast<uintptr_t>(inputEventPtr)))
    {
        if (fpTpvCameraInputOriginal)
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
        return;
    }

    // Skip camera input processing if in-game menu is open
    if (isGameMenuOpen() || g_isOverlayActive.load())
    {
        return;
    }

    GameStructures::InputEvent *event = reinterpret_cast<GameStructures::InputEvent *>(inputEventPtr);

    // eventByte0 == 0x01 and eventType == 0x08 identify a mouse-move event,
    // the only class that carries TPV camera yaw/pitch/zoom deltas.
    if (event->eventByte0 == 0x01 && event->eventType == 0x08)
    {
        bool modifiedInput = false;

        if (std::abs(event->deltaValue) > 1e-5f)
        {
            logger.log(LogLevel::Trace, "TPVInput RAW: EventID=" + format_hex(event->eventId) +
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

                logger.log(LogLevel::Trace, "TPVInput: Yaw adjusted with sensitivity " +
                                          std::to_string(sensitivity));
            }
            break;
        }

        case MOUSE_EVENT_ID_TPV_PITCH:
        {
            float sensitivity = g_config.tpv_pitch_sensitivity;
            float pitchMin = g_config.tpv_pitch_min;
            float pitchMax = g_config.tpv_pitch_max;

            if (std::abs(event->deltaValue) > 1e-5f)
            {
                float originalDelta = event->deltaValue;

                float adjustedDelta = event->deltaValue * sensitivity;

                if (g_config.tpv_pitch_limits_enabled)
                {
                    if (!g_limitsInitialized.load())
                    {
                        g_currentPitch.store(0.0f);
                        g_limitsInitialized.store(true);
                        logger.log(LogLevel::Info, "TPVInput: Initialized pitch tracking at 0 deg");
                    }

                    // Track accumulated pitch in degrees so the proposed angle can
                    // be clamped against the configured limits.
                    float currentPitch = g_currentPitch.load();
                    float proposedPitch = currentPitch + adjustedDelta;

                    float clampedPitch = std::clamp(proposedPitch, pitchMin, pitchMax);

                    // Feed the engine only the portion of the delta that survives
                    // clamping, so it never rotates past the limit.
                    adjustedDelta = clampedPitch - currentPitch;

                    g_currentPitch.store(clampedPitch);

                    logger.log(LogLevel::Trace, "TPVInput PITCH: Original=" + std::to_string(originalDelta) +
                                              " Sens=" + std::to_string(sensitivity) +
                                              " AdjustedDelta=" + std::to_string(adjustedDelta) +
                                              " Current=" + std::to_string(currentPitch) + " deg" +
                                              " Proposed=" + std::to_string(proposedPitch) + " deg" +
                                              " Clamped=" + std::to_string(clampedPitch) + " deg" +
                                              " Limits=[" + std::to_string(pitchMin) + " deg, " +
                                              std::to_string(pitchMax) + " deg]");
                }
                else
                {
                    logger.log(LogLevel::Trace, "TPVInput PITCH: Original=" + std::to_string(originalDelta) +
                                              " Sens=" + std::to_string(sensitivity) +
                                              " Adjusted=" + std::to_string(adjustedDelta) +
                                              " (No limits)");
                }

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

        if (modifiedInput)
        {
            logger.log(LogLevel::Trace, "TPVInput MODIFIED: EventID=" + format_hex(event->eventId) +
                                      " FinalDelta=" + std::to_string(event->deltaValue));
        }
    }

    if (fpTpvCameraInputOriginal)
    {
        fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
    }
}

bool initializeTpvInputHook(uintptr_t moduleBase, size_t moduleSize)
{
    DMKLogger &logger = DMKLogger::get_instance();
    logger.log(LogLevel::Info, "TPVInputHook: Initializing camera input processing hook...");

    try
    {
        DMKHookManager &hook_manager = DMKHookManager::get_instance();

        auto result = hook_manager.create_inline_hook_aob(
            "TpvCameraInput",
            moduleBase,
            moduleSize,
            Constants::TPV_INPUT_PROCESS_AOB_PATTERN,
            0, // No offset from pattern
            reinterpret_cast<void *>(Detour_TpvCameraInput),
            reinterpret_cast<void **>(&fpTpvCameraInputOriginal));

        if (!result.has_value())
        {
            throw std::runtime_error("Failed to create TPV input hook: " + std::string(DMK::Hook::error_to_string(result.error())));
        }
        g_tpvInputHookId = result.value();

        logger.log(LogLevel::Info, "TPVInputHook: Successfully installed with config:");
        logger.log(LogLevel::Info, "  - Yaw Sensitivity: " + std::to_string(g_config.tpv_yaw_sensitivity));
        logger.log(LogLevel::Info, "  - Pitch Sensitivity: " + std::to_string(g_config.tpv_pitch_sensitivity));

        if (g_config.tpv_pitch_limits_enabled)
        {
            logger.log(LogLevel::Info, "  - Pitch Limits: " +
                                     std::to_string(g_config.tpv_pitch_min) + " deg to " +
                                     std::to_string(g_config.tpv_pitch_max) + " deg");
        }
        else
        {
            logger.log(LogLevel::Info, "  - Pitch Limits: Disabled");
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LogLevel::Error, "TPVInputHook: Initialization failed: " + std::string(e.what()));
        cleanupTpvInputHook();
        return false;
    }
}

void cleanupTpvInputHook()
{
    DMKLogger &logger = DMKLogger::get_instance();

    if (!g_tpvInputHookId.empty())
    {
        DMKHookManager &hook_manager = DMKHookManager::get_instance();

        if (hook_manager.remove_hook(g_tpvInputHookId))
        {
            logger.log(LogLevel::Info, "TPVInputHook: Successfully removed");
        }
        else
        {
            logger.log(LogLevel::Warning, "TPVInputHook: Failed to remove hook");
        }

        g_tpvInputHookId.clear();
        fpTpvCameraInputOriginal = nullptr;
    }

    g_currentPitch.store(0.0f);
    g_limitsInitialized.store(false);
}

void resetCameraAngles()
{
    g_currentPitch.store(0.0f);
    g_limitsInitialized.store(false);
}
