/**
 * @file exception_handler.cpp
 * @brief Implementation of exception handling for code hooking
 *
 * This file implements the exception handler used to capture the r9 register
 * value when the INT3 breakpoint is triggered, allowing the mod to find
 * the dynamic address of the third-person view flag.
 */

#include "exception_handler.h"
#include "logger.h"
#include "utils.h"
#include "constants.h"

extern volatile BYTE *toggle_addr; // Address to toggle
extern BYTE original_bytes[4];     // Original instruction bytes
extern BYTE *instr_addr;           // Address of the hooked instruction
extern DWORD original_protection;  // Original memory protection

/**
 * Exception handler for the INT3 breakpoint.
 * When triggered at the correct location:
 * 1. Captures the r9 register value
 * 2. Calculates the toggle address as r9+0x38
 * 3. Restores the original instructions
 * 4. Resumes execution
 */
LONG WINAPI ExceptionHandler(EXCEPTION_POINTERS *ExceptionInfo)
{
    // Check if this is our INT3 breakpoint at the expected address
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        ExceptionInfo->ContextRecord->Rip == (DWORD64)instr_addr)
    {
        // Get r9 register value from the exception context
        BYTE *r9 = (BYTE *)ExceptionInfo->ContextRecord->R9;

        // Safety check to ensure r9 is not null
        if (r9 == nullptr)
        {
            Logger::getInstance().log(LOG_ERROR, "Exception: r9 register is NULL");
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Compute toggle address from r9+offset
        toggle_addr = r9 + Constants::TOGGLE_FLAG_OFFSET;

        // Log detailed information for debugging
        Logger::getInstance().log(LOG_DEBUG, "Exception: r9 = " + format_address((uintptr_t)r9));
        Logger::getInstance().log(LOG_INFO, "Toggle address found at " + std::to_string((DWORD64)toggle_addr));

        // Restore original instruction
        VirtualProtect(instr_addr, 4, PAGE_EXECUTE_READWRITE, &original_protection);
        memcpy(instr_addr, original_bytes, 4);
        VirtualProtect(instr_addr, 4, original_protection, &original_protection);

        // Resume execution at the restored instruction
        ExceptionInfo->ContextRecord->Rip = (DWORD64)instr_addr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Not our exception, let other handlers try
    return EXCEPTION_CONTINUE_SEARCH;
}
