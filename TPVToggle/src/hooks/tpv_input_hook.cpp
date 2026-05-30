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
using DMK::Format::format_hex;

namespace TPVToggle
{

// Function pointer type for TPV camera input processing.
using TpvCameraInputFunc = void(__fastcall *)(uintptr_t thisPtr, char *inputEventPtr);

// Original function pointer (trampoline from SafetyHook).
static TpvCameraInputFunc s_fpTpvCameraInputOriginal = nullptr;

// Camera control state. Pitch is accumulated in degrees so it can be clamped to
// the configured limits; the game already reports input deltas in degrees.
static std::atomic<float> s_currentPitch{0.0f};
static std::atomic<bool> s_limitsInitialized{false};

// Mouse event IDs for TPV camera control
constexpr int MOUSE_EVENT_ID_TPV_YAW = 0x10A;   // Horizontal rotation
constexpr int MOUSE_EVENT_ID_TPV_PITCH = 0x10B; // Vertical rotation
constexpr int MOUSE_EVENT_ID_TPV_ZOOM = 0x10C;  // Camera zoom

/**
 * @brief Body of the TPV camera input detour.
 * @details Intercepts mouse input events to apply custom sensitivity and limits.
 *          Kept separate from the SEH wrapper below so structured exception
 *          handling does not share a frame with C++ object unwinding.
 * @param thisPtr Pointer to the camera object
 * @param inputEventPtr Pointer to input event data
 */
static void Detour_TpvCameraInput_Impl(uintptr_t thisPtr, char *inputEventPtr)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    // inputEventPtr is engine-handed and forwarded straight to the original, so
    // it is live by definition; screen it with a cheap arithmetic check (no
    // syscall, no region-cache lock) instead of an is_readable probe on this
    // per-event path.
    if (!DMK::Memory::plausible_userspace_ptr(reinterpret_cast<uintptr_t>(inputEventPtr)))
    {
        if (s_fpTpvCameraInputOriginal)
            s_fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
        return;
    }

    // Deliberately swallow camera input while a menu or overlay is up: forwarding
    // the event to the original would let the engine pan the third-person camera
    // behind the open UI. Do NOT call s_fpTpvCameraInputOriginal here.
    if (isGameMenuOpen() || overlay_state().active.load())
    {
        return;
    }

    GameStructures::InputEvent *event = reinterpret_cast<GameStructures::InputEvent *>(inputEventPtr);

    // eventByte0 == 0x01 and eventType == 0x08 identify a mouse-move event,
    // the only class that carries TPV camera yaw/pitch/zoom deltas.
    if (event->eventByte0 == 0x01 && event->eventType == 0x08)
    {
        bool modifiedInput = false;

        // Gate on the level before building the message: format_hex allocates a
        // std::string and is evaluated as a call argument before log() is entered.
        if (std::abs(event->deltaValue) > 1e-5f && logger.is_enabled(LogLevel::Trace))
        {
            logger.log(LogLevel::Trace, "TPVInput RAW: EventID={} Delta={}",
                       format_hex(event->eventId), event->deltaValue);
        }

        switch (event->eventId)
        {
        case MOUSE_EVENT_ID_TPV_YAW:
        {
            // Apply horizontal sensitivity if configured
            float sensitivity = settings().yawSensitivity.load();
            if (sensitivity != 1.0f && std::abs(event->deltaValue) > 1e-5f)
            {
                event->deltaValue *= sensitivity;
                modifiedInput = true;

                logger.log(LogLevel::Trace, "TPVInput: Yaw adjusted with sensitivity {}", sensitivity);
            }
            break;
        }

        case MOUSE_EVENT_ID_TPV_PITCH:
        {
            float sensitivity = settings().pitchSensitivity.load();
            float pitchMin = settings().pitchMin.load();
            float pitchMax = settings().pitchMax.load();

            if (std::abs(event->deltaValue) > 1e-5f)
            {
                float originalDelta = event->deltaValue;

                float adjustedDelta = event->deltaValue * sensitivity;

                if (settings().pitchLimitsEnabled.load())
                {
                    if (!s_limitsInitialized.load())
                    {
                        s_currentPitch.store(0.0f);
                        s_limitsInitialized.store(true);
                        logger.info("TPVInput: Initialized pitch tracking at 0 deg");
                    }

                    // Track accumulated pitch in degrees so the proposed angle can
                    // be clamped against the configured limits.
                    float currentPitch = s_currentPitch.load();
                    float proposedPitch = currentPitch + adjustedDelta;

                    float clampedPitch = std::clamp(proposedPitch, pitchMin, pitchMax);

                    // Feed the engine only the portion of the delta that survives
                    // clamping, so it never rotates past the limit.
                    adjustedDelta = clampedPitch - currentPitch;

                    s_currentPitch.store(clampedPitch);

                    logger.log(LogLevel::Trace,
                               "TPVInput PITCH: Original={} Sens={} AdjustedDelta={} Current={} deg "
                               "Proposed={} deg Clamped={} deg Limits=[{} deg, {} deg]",
                               originalDelta, sensitivity, adjustedDelta, currentPitch,
                               proposedPitch, clampedPitch, pitchMin, pitchMax);
                }
                else
                {
                    logger.log(LogLevel::Trace, "TPVInput PITCH: Original={} Sens={} Adjusted={} (No limits)",
                               originalDelta, sensitivity, adjustedDelta);
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

        // Gate on the level before building the message: format_hex allocates a
        // std::string and is evaluated as a call argument before log() is entered.
        if (modifiedInput && logger.is_enabled(LogLevel::Trace))
        {
            logger.log(LogLevel::Trace, "TPVInput MODIFIED: EventID={} FinalDelta={}",
                       format_hex(event->eventId), event->deltaValue);
        }
    }

    if (s_fpTpvCameraInputOriginal)
    {
        s_fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
    }
}

/**
 * @brief SEH wrapper for the per-event TPV camera input detour.
 * @details Degrades to a no-op if a post-patch input-event layout change faults,
 *          rather than crashing the engine. The body lives in the _Impl function
 *          because __try cannot share a frame with C++ destructor unwinding.
 */
static void __fastcall Detour_TpvCameraInput(uintptr_t thisPtr, char *inputEventPtr) noexcept
{
    __try
    {
        Detour_TpvCameraInput_Impl(thisPtr, inputEventPtr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Swallow: an outdated input-event offset must never crash the game.
    }
}

bool initializeTpvInputHook(uintptr_t moduleBase, size_t moduleSize)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        DMK::HookManager &hook_manager = DMK::HookManager::get_instance();

        auto result = hook_manager.create_inline_hook_aob(
            "TpvCameraInput",
            moduleBase,
            moduleSize,
            Constants::TPV_INPUT_PROCESS_AOB_PATTERN,
            0, // No offset from pattern
            reinterpret_cast<void *>(Detour_TpvCameraInput),
            reinterpret_cast<void **>(&s_fpTpvCameraInputOriginal));

        if (!result.has_value())
        {
            throw std::runtime_error("Failed to create TPV input hook: " + std::string(DMK::Hook::error_to_string(result.error())));
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("TPVInputHook: Initialization failed: {}", e.what());
        return false;
    }
}

void resetCameraAngles()
{
    s_currentPitch.store(0.0f);
    s_limitsInitialized.store(false);
}

} // namespace TPVToggle
