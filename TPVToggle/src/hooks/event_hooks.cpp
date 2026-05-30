/**
 * @file hooks/event_hooks.cpp
 * @brief Implementation of event handling hooks for input filtering using DetourModKit.
 */

#include "event_hooks.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "config.hpp"

#include <DetourModKit.hpp>

#include <stdexcept>

using DMK::Format::format_address;

// External config reference
extern Config g_config;

namespace TPVToggle
{

// NOP pattern for disabling the scroll-accumulator write instruction. Typed as
// std::byte so it passes straight to DMK::Memory::write_bytes without a cast.
static constexpr std::byte NOP_PATTERN[Constants::ACCUMULATOR_WRITE_INSTR_LENGTH] = {
    std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}};

bool initializeEventHooks(uintptr_t module_base, size_t module_size)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        auto accumulator_pat = DMK::Scanner::parse_aob(Constants::ACCUMULATOR_WRITE_AOB_PATTERN);
        if (accumulator_pat.has_value())
        {
            const std::byte *accumulator_aob = DMK::Scanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *accumulator_pat);
            if (accumulator_aob)
            {
                TPVToggle::scroll_hook_state().writeAddress = const_cast<std::byte *>(accumulator_aob) + Constants::ACCUMULATOR_WRITE_HOOK_OFFSET;
                logger.info("EventHooks: Found accumulator write at {}", format_address(reinterpret_cast<uintptr_t>(TPVToggle::scroll_hook_state().writeAddress)));

                // Snapshot the original instruction bytes under one SEH frame so the
                // read cannot fault, and so there is no time-of-check window between a
                // readability probe and the copy.
                if (DMK::Memory::seh_read_bytes(reinterpret_cast<uintptr_t>(TPVToggle::scroll_hook_state().writeAddress),
                                                TPVToggle::scroll_hook_state().originalWriteBytes,
                                                Constants::ACCUMULATOR_WRITE_INSTR_LENGTH))
                {
                    // For hold-to-scroll feature - NOP it by default if enabled
                    if (!g_config.hold_scroll_keys.empty())
                    {
                        logger.info("EventHooks: Hold-to-scroll feature enabled, applying NOP by default");
                        if (DMK::Memory::write_bytes(TPVToggle::scroll_hook_state().writeAddress,
                                                   NOP_PATTERN,
                                                   Constants::ACCUMULATOR_WRITE_INSTR_LENGTH)
                                .has_value())
                        {
                            TPVToggle::scroll_hook_state().nopped.store(true);
                        }
                    }
                }
                else
                {
                    logger.warning("EventHooks: Cannot read original accumulator write bytes - NOP feature disabled");
                    TPVToggle::scroll_hook_state().writeAddress = nullptr;
                }
            }
            else
            {
                logger.warning("EventHooks: Accumulator write pattern not found - NOP feature disabled");
            }
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("EventHooks: Initialization failed: {}", e.what());
        cleanupEventHooks();
        return false;
    }
}

void cleanupEventHooks() noexcept
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    // Restore accumulator write if it was NOPed
    if (TPVToggle::scroll_hook_state().writeAddress != nullptr && TPVToggle::scroll_hook_state().nopped.load())
    {
        logger.info("EventHooks: Restoring original accumulator write bytes...");
        if (!DMK::Memory::write_bytes(TPVToggle::scroll_hook_state().writeAddress,
                                    TPVToggle::scroll_hook_state().originalWriteBytes,
                                    Constants::ACCUMULATOR_WRITE_INSTR_LENGTH)
                 .has_value())
        {
            logger.error("EventHooks: FAILED TO RESTORE ACCUMULATOR WRITE BYTES!");
        }
        TPVToggle::scroll_hook_state().nopped.store(false);
    }

    TPVToggle::scroll_hook_state().writeAddress = nullptr;
    logger.debug("EventHooks: Cleanup complete");
}

} // namespace TPVToggle
