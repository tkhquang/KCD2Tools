/**
 * @file hooks/event_hooks.cpp
 * @brief Implementation of event handling hooks for input filtering using DetourModKit.
 */

#include "event_hooks.hpp"
#include "constants.hpp"
#include "utils.hpp"
#include "game_interface.hpp"
#include "global_state.hpp"
#include "config.hpp"

#include <DetourModKit.hpp>

#include <stdexcept>

using DetourModKit::LogLevel;
using DMKFormat::format_address;

// External config reference
extern Config g_config;

// NOP pattern for disabling the scroll-accumulator write instruction.
static constexpr BYTE NOP_PATTERN[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {0x90, 0x90, 0x90, 0x90, 0x90};

bool initializeEventHooks(uintptr_t module_base, size_t module_size)
{
    DMKLogger &logger = DMKLogger::get_instance();

    try
    {
        logger.log(LogLevel::Info, "EventHooks: Initializing scroll-accumulator NOP feature...");

        auto accumulator_pat = DMKScanner::parse_aob(Constants::ACCUMULATOR_WRITE_AOB_PATTERN);
        if (accumulator_pat.has_value())
        {
            const std::byte *accumulator_aob = DMKScanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *accumulator_pat);
            if (accumulator_aob)
            {
                g_accumulatorWriteAddress = const_cast<std::byte *>(accumulator_aob) + Constants::ACCUMULATOR_WRITE_HOOK_OFFSET;
                logger.log(LogLevel::Info, "EventHooks: Found accumulator write at " + format_address(reinterpret_cast<uintptr_t>(g_accumulatorWriteAddress)));

                if (DMKMemory::is_readable(g_accumulatorWriteAddress, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH))
                {
                    memcpy(g_originalAccumulatorWriteBytes, g_accumulatorWriteAddress, Constants::ACCUMULATOR_WRITE_INSTR_LENGTH);
                    logger.log(LogLevel::Debug, "EventHooks: Saved original accumulator write bytes");

                    // For hold-to-scroll feature - NOP it by default if enabled
                    if (!g_config.hold_scroll_keys.empty())
                    {
                        logger.log(LogLevel::Info, "EventHooks: Hold-to-scroll feature enabled, applying NOP by default");
                        if (DMKMemory::write_bytes(g_accumulatorWriteAddress,
                                                   reinterpret_cast<const std::byte *>(NOP_PATTERN),
                                                   Constants::ACCUMULATOR_WRITE_INSTR_LENGTH)
                                .has_value())
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
                                    Constants::ACCUMULATOR_WRITE_INSTR_LENGTH)
                 .has_value())
        {
            logger.log(LogLevel::Error, "EventHooks: FAILED TO RESTORE ACCUMULATOR WRITE BYTES!");
        }
        g_accumulatorWriteNOPped.store(false);
    }

    g_accumulatorWriteAddress = nullptr;
    logger.log(LogLevel::Debug, "EventHooks: Cleanup complete");
}
