/**
 * @file dllmain.cpp
 * @brief Main entry point for the TPVToggle mod
 *
 * This file contains the DLL entry point and main thread that initializes
 * the mod, finds memory patterns, sets up hooks, and starts key monitoring.
 */

#include "aob_scanner.h"
#include "exception_handler.h"
#include "toggle_thread.h"
#include "logger.h"
#include "config.h"
#include "utils.h"
#include "constants.h"
#include "version.h"

#include <windows.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <iomanip>

volatile BYTE *toggle_addr = nullptr;
BYTE original_bytes[4];
BYTE *instr_addr = nullptr;
DWORD original_protection;
PVOID exceptionHandlerHandle = nullptr;

/**
 * Cleans up resources when the DLL is unloaded.
 * Restores original bytes if hook was applied.
 */
void CleanupResources()
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "DLL is being unloaded, cleaning up resources");

    // Restore original bytes if we hooked anything
    if (instr_addr != nullptr)
    {
        DWORD oldProtect;
        if (VirtualProtect(instr_addr, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            memcpy(instr_addr, original_bytes, 4);
            VirtualProtect(instr_addr, 4, oldProtect, &oldProtect);
            logger.log(LOG_INFO, "Cleanup: Restored original instruction at " + format_address(reinterpret_cast<uintptr_t>(instr_addr)));
        }
        else
        {
            logger.log(LOG_ERROR, "Cleanup: Failed to restore original instruction");
        }
    }

    // Remove exception handler if added
    if (exceptionHandlerHandle != nullptr)
    {
        RemoveVectoredExceptionHandler(exceptionHandlerHandle);
        logger.log(LOG_INFO, "Cleanup: Removed exception handler");
    }

    logger.log(LOG_INFO, "Cleanup complete");
}

DWORD WINAPI MainThread(LPVOID _param)
{
    Logger &logger = Logger::getInstance();
    Config config = loadConfig(Constants::getConfigFilename());

    // Set log level based on configuration
    if (config.log_level == "DEBUG")
        logger.setLogLevel(LOG_DEBUG);
    else if (config.log_level == "INFO")
        logger.setLogLevel(LOG_INFO);
    else if (config.log_level == "WARNING")
        logger.setLogLevel(LOG_WARNING);
    else if (config.log_level == "ERROR")
        logger.setLogLevel(LOG_ERROR);
    else
    {
        logger.log(LOG_WARNING, "Settings: Invalid LogLevel '" + config.log_level + "', defaulting to DEBUG");
        logger.setLogLevel(LOG_DEBUG);
    }

    // Log version information
    Version::logVersionInfo();

    // Log configured toggle keys
    std::string keys_str;
    for (int vk : config.toggle_keys)
    {
        if (!keys_str.empty())
            keys_str += ", ";
        std::ostringstream oss;
        oss << "0x" << std::hex << std::setw(2) << std::setfill('0') << vk;
        keys_str += oss.str();
    }
    logger.log(LOG_INFO, "Settings: ToggleKeys = " + keys_str);

    logger.log(LOG_INFO, "DLL loaded");

    // Parse AOB pattern
    std::vector<BYTE> pattern = parseAOB(config.aob_pattern);
    if (pattern.empty())
    {
        logger.log(LOG_ERROR, "Settings: AOBPattern is empty or invalid");
        return 1;
    }

    // Find game module
    HMODULE hModule = GetModuleHandleA(Constants::MODULE_NAME);
    if (!hModule)
    {
        logger.log(LOG_ERROR, "Module: Failed to find " + std::string(Constants::MODULE_NAME));
        return 1;
    }
    logger.log(LOG_INFO, "Module: " + std::string(Constants::MODULE_NAME) + " found at " + format_address(reinterpret_cast<uintptr_t>(hModule)));

    // Get module information for scanning
    MODULEINFO moduleInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo)))
    {
        logger.log(LOG_ERROR, "Module: Failed to get " + std::string(Constants::MODULE_NAME) + " info");
        return 1;
    }

    // Scan for pattern
    BYTE *base = (BYTE *)hModule;
    size_t size = moduleInfo.SizeOfImage;
    BYTE *aob_addr = FindPattern(base, size, pattern);
    if (!aob_addr)
    {
        logger.log(LOG_ERROR, "AOB: Pattern not found in " + std::string(Constants::MODULE_NAME));
        return 1;
    }
    logger.log(LOG_INFO, "AOB: Found at " + format_address(reinterpret_cast<uintptr_t>(aob_addr)));

    // Set up INT3 hook at the instruction that reads the flag
    instr_addr = aob_addr + 18; // Offset to the instruction that reads [r9+0x38]
    memcpy(original_bytes, instr_addr, 4);
    if (!VirtualProtect(instr_addr, 4, PAGE_EXECUTE_READWRITE, &original_protection))
    {
        logger.log(LOG_ERROR, "Memory: Failed to change protection at " + format_address(reinterpret_cast<uintptr_t>(instr_addr)));
        return 1;
    }

    // Place INT3 breakpoint
    memset(instr_addr, 0xCC, 4);
    logger.log(LOG_INFO, "Hook: INT3 set at " + format_address(reinterpret_cast<uintptr_t>(instr_addr)));

    // Add exception handler to capture r9 register
    exceptionHandlerHandle = AddVectoredExceptionHandler(1, ExceptionHandler);

    // Start key monitoring thread
    ToggleData *data = new ToggleData{config.toggle_keys};
    CreateThread(NULL, 0, ToggleThread, data, 0, NULL);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID _lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        CleanupResources();
    }
    return TRUE;
}
