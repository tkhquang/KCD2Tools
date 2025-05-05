/**
 * @file hooks/event_hooks.cpp
 * @brief Implementation of event handling hooks for input filtering.
 */

#include "event_hooks.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "aob_scanner.h"
#include "game_interface.h"
#include "global_state.h"
#include "MinHook.h"

#include <stdexcept>

// Function typedefs
typedef uint64_t(__fastcall *EventHandlerType)(uintptr_t listenerMgrPtr, char *inputEventPtr);

// Hook state
static EventHandlerType fpEventHandlerOriginal = nullptr;
static BYTE *g_eventHookAddress = nullptr;

// NOP pattern for disabling accumulator writes
static constexpr BYTE NOP_PATTERN[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {0x90, 0x90, 0x90, 0x90, 0x90};

/**
 * @brief Detour function for the event handler.
 * @details Intercepts input events and filters scroll wheel events when overlay is active.
 */
static uint64_t __fastcall EventHandlerDetour(uintptr_t listenerMgrPtr, char *inputEventPtr)
{
    Logger &logger = Logger::getInstance();

    // Early validation check - if failed, jump directly to function call
    constexpr size_t required_size = Constants::INPUT_EVENT_VALUE_OFFSET + sizeof(float);
    if (!isMemoryReadable(inputEventPtr, required_size))
    {
        if (logger.isDebugEnabled())
        {
            logger.log(LOG_DEBUG, "EventHandler: Input event pointer unreadable");
        }
        // Early exit pattern - no goto needed
        return fpEventHandlerOriginal ? fpEventHandlerOriginal(listenerMgrPtr, inputEventPtr) : 0;
    }

    // Check if it's a mouse event
    bool isLikelyMouseEvent = false;
    if (isMemoryReadable(inputEventPtr + Constants::INPUT_EVENT_TYPE_OFFSET, sizeof(int)) &&
        isMemoryReadable(inputEventPtr + Constants::INPUT_EVENT_BYTE0_OFFSET, sizeof(char)))
    {
        int inputType = *reinterpret_cast<int *>(inputEventPtr + Constants::INPUT_EVENT_TYPE_OFFSET);
        char byte0 = *reinterpret_cast<char *>(inputEventPtr + Constants::INPUT_EVENT_BYTE0_OFFSET);
        isLikelyMouseEvent = (inputType == Constants::MOUSE_INPUT_TYPE_ID && byte0 == Constants::INPUT_EVENT_BYTE0_EXPECTED);
    }

    // Check if it's a scroll wheel event that needs filtering
    if (isLikelyMouseEvent)
    {
        int eventID = *reinterpret_cast<int *>(inputEventPtr + Constants::INPUT_EVENT_ID_OFFSET);
        if (eventID == Constants::MOUSE_WHEEL_EVENT_ID)
        {
            // Check overlay state
            long long overlayState = getOverlayState();
            if (overlayState > 0)
            { // Overlay is active
                // Zero out the scroll delta in the original event
                volatile float *delta_ptr = reinterpret_cast<volatile float *>(inputEventPtr + Constants::INPUT_EVENT_VALUE_OFFSET);
                if (isMemoryWritable(delta_ptr, sizeof(float)))
                {
                    float original_delta = *delta_ptr;
                    if (original_delta != 0.0f)
                    { // Avoid unnecessary writes
                        *delta_ptr = 0.0f;
                        if (logger.isDebugEnabled())
                        {
                            logger.log(LOG_DEBUG, "EventHandler: Zeroed scroll delta (was " + std::to_string(original_delta) + ") in original event due to ACTIVE overlay");
                        }
                    }
                }
                else
                {
                    logger.log(LOG_ERROR, "EventHandler: Cannot write to zero event delta!");
                }
            }
        }
    }

    // Always call the original function using the trampoline
    if (fpEventHandlerOriginal)
    {
        return fpEventHandlerOriginal(listenerMgrPtr, inputEventPtr);
    }
    else
    {
        logger.log(LOG_ERROR, "EventHandler: CRITICAL - Trampoline is NULL!");
        return 0;
    }
}

bool initializeEventHooks(uintptr_t module_base, size_t module_size)
{
    Logger &logger = Logger::getInstance();

    try
    {
        logger.log(LOG_INFO, "EventHooks: Initializing event handler hook...");

        // Scan for event handler function
        std::vector<BYTE> event_pat = parseAOB(Constants::EVENT_HANDLER_AOB_PATTERN);
        if (event_pat.empty())
        {
            throw std::runtime_error("Failed to parse event handler AOB pattern");
        }

        BYTE *event_aob = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, event_pat);
        if (!event_aob)
        {
            throw std::runtime_error("Event handler AOB pattern not found");
        }

        g_eventHookAddress = event_aob + Constants::EVENT_HANDLER_HOOK_OFFSET;
        logger.log(LOG_INFO, "EventHooks: Found event handler at " + format_address(reinterpret_cast<uintptr_t>(g_eventHookAddress)));

        // Scan for accumulator write instruction
        std::vector<BYTE> accumulator_pat = parseAOB(Constants::ACCUMULATOR_WRITE_AOB_PATTERN);
        if (!accumulator_pat.empty())
        {
            BYTE *accumulator_aob = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, accumulator_pat);
            if (accumulator_aob)
            {
                g_accumulatorWriteAddress = accumulator_aob + Constants::ACCUMULATOR_WRITE_HOOK_OFFSET;
                logger.log(LOG_INFO, "EventHooks: Found accumulator write at " + format_address(reinterpret_cast<uintptr_t>(g_accumulatorWriteAddress)));

                // Save original bytes
                if (isMemoryReadable(g_accumulatorWriteAddress, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH))
                {
                    memcpy(g_originalAccumulatorWriteBytes, g_accumulatorWriteAddress, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH);
                    logger.log(LOG_DEBUG, "EventHooks: Saved original accumulator write bytes");
                }
                else
                {
                    logger.log(LOG_WARNING, "EventHooks: Cannot read original accumulator write bytes - NOP feature disabled");
                    g_accumulatorWriteAddress = nullptr;
                }
            }
            else
            {
                logger.log(LOG_WARNING, "EventHooks: Accumulator write pattern not found - NOP feature disabled");
            }
        }

        // // Create and enable the event handler hook
        // MH_STATUS status = MH_CreateHook(g_eventHookAddress,
        //                                  reinterpret_cast<LPVOID>(EventHandlerDetour),
        //                                  reinterpret_cast<LPVOID *>(&fpEventHandlerOriginal));

        // if (status != MH_OK)
        // {
        //     throw std::runtime_error("MH_CreateHook failed: " + std::string(MH_StatusToString(status)));
        // }

        // if (!fpEventHandlerOriginal)
        // {
        //     MH_RemoveHook(g_eventHookAddress);
        //     throw std::runtime_error("MH_CreateHook returned NULL trampoline");
        // }

        // status = MH_EnableHook(g_eventHookAddress);
        // if (status != MH_OK)
        // {
        //     MH_RemoveHook(g_eventHookAddress);
        //     throw std::runtime_error("MH_EnableHook failed: " + std::string(MH_StatusToString(status)));
        // }

        // logger.log(LOG_INFO, "EventHooks: Event handler hook successfully installed");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "EventHooks: Initialization failed: " + std::string(e.what()));
        cleanupEventHooks();
        return false;
    }
}

void cleanupEventHooks()
{
    Logger &logger = Logger::getInstance();

    // Restore accumulator write if it was NOPed
    if (g_accumulatorWriteAddress != nullptr && g_accumulatorWriteNOPped.load())
    {
        logger.log(LOG_INFO, "EventHooks: Restoring original accumulator write bytes...");
        if (!WriteBytes(g_accumulatorWriteAddress, g_originalAccumulatorWriteBytes, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
        {
            logger.log(LOG_ERROR, "EventHooks: FAILED TO RESTORE ACCUMULATOR WRITE BYTES!");
        }
        g_accumulatorWriteNOPped.store(false);
    }

    // Clean up event handler hook
    if (g_eventHookAddress && fpEventHandlerOriginal)
    {
        MH_DisableHook(g_eventHookAddress);
        MH_RemoveHook(g_eventHookAddress);
        fpEventHandlerOriginal = nullptr;
        g_eventHookAddress = nullptr;
    }

    g_accumulatorWriteAddress = nullptr;
    logger.log(LOG_DEBUG, "EventHooks: Cleanup complete");
}

bool areEventHooksActive()
{
    return (g_eventHookAddress != nullptr && fpEventHandlerOriginal != nullptr);
}
