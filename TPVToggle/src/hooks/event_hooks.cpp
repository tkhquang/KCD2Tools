/**
 * @file hooks/event_hooks.cpp
 * @brief Implementation of event handling hooks for input filtering.
 */

#include "event_hooks.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "game_interface.h"
#include "global_state.h"
#include "hook_manager.hpp" // Use HookManager
#include "config.h"

#include <stdexcept>
#include <string> // For std::string

// External config reference
extern Config g_config;

// Function typedefs
typedef uint64_t(__fastcall *EventHandlerType)(uintptr_t listenerMgrPtr, char *inputEventPtr);

// Hook state
static EventHandlerType fpEventHandlerOriginal = nullptr;
// static BYTE *g_eventHookAddress = nullptr; // Managed by HookManager
static std::string g_eventHookId = ""; // To store the ID from HookManager

// NOP pattern for disabling accumulator writes
static constexpr BYTE NOP_PATTERN[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {0x90, 0x90, 0x90, 0x90, 0x90};

/**
 * @brief Detour function for the event handler.
 * @details Intercepts input events and filters scroll wheel events when overlay is active.
 */
static uint64_t __fastcall EventHandlerDetour(uintptr_t listenerMgrPtr, char *inputEventPtr)
{
    Logger &logger = Logger::getInstance();

    // Early validation check
    constexpr size_t required_size = Constants::INPUT_EVENT_VALUE_OFFSET + sizeof(float);
    if (!isMemoryReadable(inputEventPtr, required_size))
    {
        logger.log(LOG_DEBUG, "EventHandlerDetour: Input event pointer unreadable or too small.");
        // Critical to call original if possible, even on error, to not break game input
        if (fpEventHandlerOriginal)
        {
            return fpEventHandlerOriginal(listenerMgrPtr, inputEventPtr);
        }
        return 0; // Should not happen if hook setup was successful
    }

    // Check if it's a mouse event
    bool isLikelyMouseEvent = false;
    // Ensure memory for type and byte0 is readable before dereferencing
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
        // Ensure eventId memory is readable
        if (isMemoryReadable(inputEventPtr + Constants::INPUT_EVENT_ID_OFFSET, sizeof(int)))
        {
            int eventID = *reinterpret_cast<int *>(inputEventPtr + Constants::INPUT_EVENT_ID_OFFSET);
            if (eventID == Constants::MOUSE_WHEEL_EVENT_ID)
            {
                // Logic for hold-to-scroll or overlay active
                bool blockScroll = false;
                if (!g_config.hold_scroll_keys.empty()) // Hold-to-scroll feature is configured
                {
                    if (!g_holdToScrollActive.load(std::memory_order_relaxed))
                    {
                        blockScroll = true; // Block if hold key not pressed
                    }
                }
                else if (g_isOverlayActive.load()) // Standard overlay check (if hold-to-scroll not configured)
                {
                    blockScroll = true; // Block if overlay is active
                }

                if (blockScroll)
                {
                    volatile float *delta_ptr = reinterpret_cast<volatile float *>(inputEventPtr + Constants::INPUT_EVENT_VALUE_OFFSET);
                    // Ensure delta_ptr is writable
                    if (isMemoryWritable(delta_ptr, sizeof(float)))
                    {
                        float original_delta = *delta_ptr;
                        if (original_delta != 0.0f) // Avoid unnecessary writes
                        {
                            *delta_ptr = 0.0f;
                            logger.log(LOG_DEBUG, "EventHandlerDetour: Zeroed scroll delta (was " + std::to_string(original_delta) + ") due to scroll blocking conditions.");
                        }
                    }
                    else
                    {
                        logger.log(LOG_WARNING, "EventHandlerDetour: Cannot write to zero event delta for scroll wheel!");
                    }
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
        logger.log(LOG_ERROR, "EventHandlerDetour: CRITICAL - fpEventHandlerOriginal (trampoline) is NULL!");
        return 0; // Default return if trampoline is somehow null
    }
}

bool initializeEventHooks(uintptr_t module_base, size_t module_size)
{
    Logger &logger = Logger::getInstance();

    // Event handler hook is OPTIONAL for now, primarily used for input filtering.
    // The core NOPping logic for accumulator can proceed even if this hook fails.
    bool eventHandlerHooked = false;

    try
    {
        logger.log(LOG_INFO, "EventHooks: Initializing event handler hook (optional)...");

        HookManager &hookManager = HookManager::getInstance();
        g_eventHookId = hookManager.create_inline_hook_aob(
            "EventHandler",
            module_base,
            module_size,
            Constants::EVENT_HANDLER_AOB_PATTERN,
            Constants::EVENT_HANDLER_HOOK_OFFSET,
            reinterpret_cast<void *>(EventHandlerDetour),
            reinterpret_cast<void **>(&fpEventHandlerOriginal));

        if (g_eventHookId.empty() || fpEventHandlerOriginal == nullptr)
        {
            // Not a fatal error for the whole mod, just log a warning
            logger.log(LOG_WARNING, "EventHooks: Failed to create EventHandler hook. Advanced scroll filtering might not work.");
            // Proceed without this specific hook.
        }
        else
        {
            logger.log(LOG_INFO, "EventHooks: EventHandler hook successfully installed with ID: " + g_eventHookId);
            eventHandlerHooked = true;
        }
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_WARNING, "EventHooks: Exception during EventHandler hook initialization: " + std::string(e.what()) + ". Continuing without it.");
    }

    // Initialize accumulator NOP logic (this is separate from the event handler hook itself)
    logger.log(LOG_INFO, "EventHooks: Initializing scroll accumulator NOP logic...");
    std::vector<BYTE> accumulator_pat = parseAOB(Constants::ACCUMULATOR_WRITE_AOB_PATTERN);
    if (!accumulator_pat.empty())
    {
        BYTE *accumulator_aob = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, accumulator_pat);
        if (accumulator_aob)
        {
            g_accumulatorWriteAddress = accumulator_aob + Constants::ACCUMULATOR_WRITE_HOOK_OFFSET;
            logger.log(LOG_INFO, "EventHooks: Found accumulator write instruction at " + format_address(reinterpret_cast<uintptr_t>(g_accumulatorWriteAddress)));

            if (isMemoryReadable(g_accumulatorWriteAddress, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH))
            {
                memcpy(g_originalAccumulatorWriteBytes, g_accumulatorWriteAddress, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH);
                logger.log(LOG_DEBUG, "EventHooks: Saved original accumulator write bytes.");

                // If hold-to-scroll feature is enabled, NOP the accumulator write by default.
                // It will be restored when the hold key is pressed.
                if (!g_config.hold_scroll_keys.empty())
                {
                    logger.log(LOG_INFO, "EventHooks: Hold-to-scroll enabled. NOPping accumulator write by default.");
                    if (WriteBytes(g_accumulatorWriteAddress, NOP_PATTERN, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
                    {
                        g_accumulatorWriteNOPped.store(true);
                        logger.log(LOG_DEBUG, "EventHooks: Accumulator write successfully NOPped for hold-to-scroll.");
                    }
                    else
                    {
                        logger.log(LOG_ERROR, "EventHooks: Failed to NOP accumulator write for hold-to-scroll.");
                    }
                }
            }
            else
            {
                logger.log(LOG_WARNING, "EventHooks: Cannot read original accumulator write bytes. NOP feature for scroll will be disabled.");
                g_accumulatorWriteAddress = nullptr; // Disable NOP feature if we can't read original
            }
        }
        else
        {
            logger.log(LOG_WARNING, "EventHooks: Accumulator write AOB pattern not found. NOP feature for scroll will be disabled.");
        }
    }
    else
    {
        logger.log(LOG_ERROR, "EventHooks: Failed to parse accumulator write AOB pattern. NOP feature for scroll will be disabled.");
    }

    // Return true if either the hook was made OR the accumulator logic setup part succeeded (or didn't critically fail)
    // For now, let's say overall success if no exceptions were thrown preventing further mod load.
    // The actual functionality depends on g_accumulatorWriteAddress and g_eventHookId.
    return true;
}

void cleanupEventHooks()
{
    Logger &logger = Logger::getInstance();

    // Restore accumulator write if it was NOPped
    if (g_accumulatorWriteAddress != nullptr && g_accumulatorWriteNOPped.load())
    {
        // Ensure original bytes were actually saved
        bool originalBytesValid = false;
        for (size_t i = 0; i < Constants::ACCUMULATOR_WRITE_INSTR_LENGTH; ++i)
        {
            if (g_originalAccumulatorWriteBytes[i] != 0x00)
            { // Simple check, assumes NOPs are all 0x90 and original isn't all 0x00
                originalBytesValid = true;
                break;
            }
        }
        if (!originalBytesValid && Constants::ACCUMULATOR_WRITE_INSTR_LENGTH > 0)
        {
            logger.log(LOG_WARNING, "EventHooks: Original accumulator write bytes appear to be all zeros or uninitialized. Skipping restore to avoid writing garbage.");
        }
        else if (originalBytesValid)
        {
            logger.log(LOG_INFO, "EventHooks: Restoring original accumulator write bytes...");
            if (!WriteBytes(g_accumulatorWriteAddress, g_originalAccumulatorWriteBytes, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH, logger))
            {
                logger.log(LOG_ERROR, "EventHooks: FAILED TO RESTORE ACCUMULATOR WRITE BYTES!");
            }
        }
        g_accumulatorWriteNOPped.store(false); // Attempted restore, so reset flag
    }
    g_accumulatorWriteAddress = nullptr; // Clear the address

    // Clean up event handler hook via HookManager
    if (!g_eventHookId.empty())
    {
        if (HookManager::getInstance().remove_hook(g_eventHookId))
        {
            logger.log(LOG_INFO, "EventHooks: Hook '" + g_eventHookId + "' removed.");
        }
        else
        {
            logger.log(LOG_WARNING, "EventHooks: Failed to remove hook '" + g_eventHookId + "' via HookManager.");
        }
        fpEventHandlerOriginal = nullptr; // Clear trampoline
        g_eventHookId = "";
    }

    logger.log(LOG_DEBUG, "EventHooks: Cleanup complete.");
}

bool areEventHooksActive()
{
    // The event handler hook itself (for event object modification)
    return (fpEventHandlerOriginal != nullptr && !g_eventHookId.empty());
}
