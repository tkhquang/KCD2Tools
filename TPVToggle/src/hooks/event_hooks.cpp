/**
 * @file hooks/event_hooks.cpp
 * @brief Implementation of event handling hooks for input filtering using DetourModKit.
 */

#include "event_hooks.h"
#include "constants.h"
#include "utils.h"
#include "game_interface.h"
#include "global_state.h"
#include "config.h"

#include <DetourModKit.hpp>

#include <stdexcept>

using DetourModKit::LogLevel;
using DMKFormat::format_address;

// External config reference
extern Config g_config;

// Function typedefs
typedef uint64_t(__fastcall *EventHandlerType)(uintptr_t listenerMgrPtr, char *inputEventPtr);

// Hook state
static EventHandlerType fpEventHandlerOriginal = nullptr;
static std::string g_eventHookId;

// NOP pattern for disabling accumulator writes
static constexpr BYTE NOP_PATTERN[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {0x90, 0x90, 0x90, 0x90, 0x90};

/**
 * @brief Detour function for the event handler.
 * @details Intercepts input events and filters scroll wheel events when overlay is active.
 */
static uint64_t __fastcall EventHandlerDetour(uintptr_t listenerMgrPtr, char *inputEventPtr)
{
    DMKLogger &logger = DMKLogger::get_instance();

    // Early validation check - if failed, jump directly to function call
    constexpr size_t required_size = Constants::INPUT_EVENT_VALUE_OFFSET + sizeof(float);
    if (!DMKMemory::is_readable(inputEventPtr, required_size))
    {
        logger.log(LogLevel::Debug, "EventHandler: Input event pointer unreadable");
        // Early exit pattern - no goto needed
        return fpEventHandlerOriginal ? fpEventHandlerOriginal(listenerMgrPtr, inputEventPtr) : 0;
    }

    // Check if it's a mouse event
    bool isLikelyMouseEvent = false;
    if (DMKMemory::is_readable(inputEventPtr + Constants::INPUT_EVENT_TYPE_OFFSET, sizeof(int)) &&
        DMKMemory::is_readable(inputEventPtr + Constants::INPUT_EVENT_BYTE0_OFFSET, sizeof(char)))
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
            // Check if hold-to-scroll is in use
            if (!g_config.hold_scroll_keys.keys.empty())
            {
                // Allow scrolling only when hold key is pressed
                bool holdKeyPressed = g_holdToScrollActive.load(std::memory_order_relaxed);

                if (!holdKeyPressed)
                {
                    // Zero out the scroll delta in the original event
                    volatile float *delta_ptr = reinterpret_cast<volatile float *>(inputEventPtr + Constants::INPUT_EVENT_VALUE_OFFSET);
                    if (DMKMemory::is_writable(const_cast<float *>(delta_ptr), sizeof(float)))
                    {
                        float original_delta = *delta_ptr;
                        if (original_delta != 0.0f)
                        { // Avoid unnecessary writes
                            *delta_ptr = 0.0f;
                            logger.log(LogLevel::Debug, "EventHandler: Zeroed scroll delta (was " + std::to_string(original_delta) + ") due to hold key not pressed");
                        }
                    }
                }
            }
            // Standard overlay check - applies when hold-to-scroll is not in use or when overlay is active
            else
            {
                // Check overlay state
                bool overlayState = g_isOverlayActive.load();
                if (overlayState)
                { // Overlay is active
                    // Zero out the scroll delta in the original event
                    volatile float *delta_ptr = reinterpret_cast<volatile float *>(inputEventPtr + Constants::INPUT_EVENT_VALUE_OFFSET);
                    if (DMKMemory::is_writable(const_cast<float *>(delta_ptr), sizeof(float)))
                    {
                        float original_delta = *delta_ptr;
                        if (original_delta != 0.0f)
                        { // Avoid unnecessary writes
                            *delta_ptr = 0.0f;
                            logger.log(LogLevel::Debug, "EventHandler: Zeroed scroll delta (was " + std::to_string(original_delta) + ") in original event due to ACTIVE overlay");
                        }
                    }
                    else
                    {
                        logger.log(LogLevel::Error, "EventHandler: Cannot write to zero event delta!");
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
        logger.log(LogLevel::Error, "EventHandler: CRITICAL - Trampoline is NULL!");
        return 0;
    }
}

bool initializeEventHooks(uintptr_t module_base, size_t module_size)
{
    DMKLogger &logger = DMKLogger::get_instance();

    try
    {
        logger.log(LogLevel::Info, "EventHooks: Initializing event handler hook...");

        // Scan for event handler function
        auto event_pat = DMKScanner::parse_aob(Constants::EVENT_HANDLER_AOB_PATTERN);
        if (!event_pat.has_value())
        {
            throw std::runtime_error("Failed to parse event handler AOB pattern");
        }

        const std::byte *event_aob = DMKScanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *event_pat);
        if (!event_aob)
        {
            throw std::runtime_error("Event handler AOB pattern not found");
        }

        const std::byte *eventHookAddress = event_aob + Constants::EVENT_HANDLER_HOOK_OFFSET;
        logger.log(LogLevel::Info, "EventHooks: Found event handler at " + format_address(reinterpret_cast<uintptr_t>(eventHookAddress)));

        // Scan for accumulator write instruction
        auto accumulator_pat = DMKScanner::parse_aob(Constants::ACCUMULATOR_WRITE_AOB_PATTERN);
        if (accumulator_pat.has_value())
        {
            const std::byte *accumulator_aob = DMKScanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *accumulator_pat);
            if (accumulator_aob)
            {
                g_accumulatorWriteAddress = const_cast<std::byte *>(accumulator_aob) + Constants::ACCUMULATOR_WRITE_HOOK_OFFSET;
                logger.log(LogLevel::Info, "EventHooks: Found accumulator write at " + format_address(reinterpret_cast<uintptr_t>(g_accumulatorWriteAddress)));

                // Save original bytes
                if (DMKMemory::is_readable(g_accumulatorWriteAddress, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH))
                {
                    memcpy(g_originalAccumulatorWriteBytes, g_accumulatorWriteAddress, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH);
                    logger.log(LogLevel::Debug, "EventHooks: Saved original accumulator write bytes");

                    // For hold-to-scroll feature - NOP it by default if enabled
                    if (!g_config.hold_scroll_keys.keys.empty())
                    {
                        logger.log(LogLevel::Info, "EventHooks: Hold-to-scroll feature enabled, applying NOP by default");
                        if (DMKMemory::write_bytes(g_accumulatorWriteAddress,
                                                   reinterpret_cast<const std::byte *>(NOP_PATTERN),
                                                   Constants::ACCUMULATOR_WRITE_INSTR_LENGTH,
                                                   DMKLogger::get_instance()).has_value())
                        {
                            g_accumulatorWriteNOPped.store(true);
                        }
                    }
                }
                else
                {
                    logger.log(LogLevel::Warning, "EventHooks: Cannot read original accumulator write bytes - NOP feature disabled");
                    g_accumulatorWriteAddress = nullptr;
                }
            }
            else
            {
                logger.log(LogLevel::Warning, "EventHooks: Accumulator write pattern not found - NOP feature disabled");
            }
        }

        // Note: Event handler hook is currently disabled in original code
        // To enable it, uncomment the following DMKHookManager code:
        /*
        DMKHookManager &hook_manager = DMKHookManager::get_instance();
        g_eventHookId = hook_manager.create_inline_hook(
            "EventHandler",
            reinterpret_cast<uintptr_t>(eventHookAddress),
            reinterpret_cast<void *>(EventHandlerDetour),
            reinterpret_cast<void **>(&fpEventHandlerOriginal));

        if (g_eventHookId.empty())
        {
            throw std::runtime_error("Failed to create event handler hook");
        }

        logger.log(LogLevel::Info, "EventHooks: Event handler hook successfully installed");
        */

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LogLevel::Error, "EventHooks: Initialization failed: " + std::string(e.what()));
        cleanupEventHooks();
        return false;
    }
}

void cleanupEventHooks()
{
    DMKLogger &logger = DMKLogger::get_instance();

    // Restore accumulator write if it was NOPed
    if (g_accumulatorWriteAddress != nullptr && g_accumulatorWriteNOPped.load())
    {
        logger.log(LogLevel::Info, "EventHooks: Restoring original accumulator write bytes...");
        if (!DMKMemory::write_bytes(g_accumulatorWriteAddress,
                                    g_originalAccumulatorWriteBytes,
                                    Constants::ACCUMULATOR_WRITE_INSTR_LENGTH,
                                    DMKLogger::get_instance()).has_value())
        {
            logger.log(LogLevel::Error, "EventHooks: FAILED TO RESTORE ACCUMULATOR WRITE BYTES!");
        }
        g_accumulatorWriteNOPped.store(false);
    }

    // Clean up event handler hook
    if (!g_eventHookId.empty())
    {
        (void)DMKHookManager::get_instance().remove_hook(g_eventHookId);
        g_eventHookId.clear();
        fpEventHandlerOriginal = nullptr;
    }

    g_accumulatorWriteAddress = nullptr;
    logger.log(LogLevel::Debug, "EventHooks: Cleanup complete");
}

bool areEventHooksActive()
{
    return !g_eventHookId.empty();
}
