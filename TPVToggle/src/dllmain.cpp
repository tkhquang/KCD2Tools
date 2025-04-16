/**
 * @file dllmain.cpp
 * @brief Main entry point and initialization logic for the TPVToggle mod.
 *
 * Handles DLL attach/detach, sets up logging, config loading, AOB scanning,
 * MinHook initialization, hook placement, and starts the background key/state
 * monitoring thread. Defines the global pointers shared with assembly hooks.
 */

#include "aob_scanner.h"
#include "toggle_thread.h" // Includes extern declarations of globals
#include "logger.h"
#include "config.h"
#include "utils.h"
#include "constants.h"
#include "version.h"
#include "MinHook.h"

#include <windows.h>
#include <psapi.h> // For GetModuleInformation
#include <string>
#include <vector>
#include <iomanip>
#include <thread>
#include <chrono> // For std::chrono::seconds

// --- Global Variable Definitions ---
// Pointers to allocated storage for captured register/address values
uintptr_t *g_r9_for_tpv_flag = nullptr;
uintptr_t *g_rbx_for_camera_distance = nullptr;

// Pointers to the original code trampoline addresses (set by MinHook)
void *fpTPV_OriginalCode = nullptr;
void *fpMenuOpen_OriginalCode = nullptr;
void *fpMenuClose_OriginalCode = nullptr;
void *fpCameraDistance_OriginalCode = nullptr;

// --- Global Hook State Variables (External Linkage) ---
bool g_isOverlayActive = false;     // The actual overlay state flag
bool *g_pIsOverlayActive = nullptr; // Pointer to the flag for assembly

// --- End Global Definitions ---

// Target addresses for hooks (local to this file's logic)
namespace // Use anonymous namespace for file-local globals NOT shared
{
    BYTE *g_tpvHookAddress = nullptr;
    BYTE *g_menuOpenHookAddress = nullptr;
    BYTE *g_menuCloseHookAddress = nullptr;
    BYTE *g_cameraDistanceHookAddress = nullptr;
}

// --- External Assembly Detour Declarations ---
extern "C"
{
    void TPV_CaptureR9_Detour();
    void MenuOpen_Detour();
    void MenuClose_Detour();
    void CameraDistance_CaptureRBX_Detour();
}

/**
 * @brief Helper to safely free allocated memory for captured register pointers.
 * @param ptr_addr Reference to the global pointer variable.
 * @param name Descriptive name for logging.
 * @param logger Reference to the logger instance.
 */
void SafeVirtualFree(uintptr_t *&ptr_addr, const std::string &name,
                     Logger &logger)
{
    if (ptr_addr)
    {
        logger.log(LOG_DEBUG, "Cleanup: Freeing " + name + " storage at " +
                                  format_address(reinterpret_cast<uintptr_t>(ptr_addr)));
        if (!VirtualFree(ptr_addr, 0, MEM_RELEASE))
        {
            logger.log(LOG_ERROR, "Cleanup: VirtualFree failed for " + name +
                                      " storage. Error: " + std::to_string(GetLastError()));
        }
        ptr_addr = nullptr;
    }
    else
    {
        logger.log(LOG_DEBUG, "Cleanup: " + name +
                                  " storage already freed or not allocated.");
    }
}

/**
 * @brief Cleans up resources (hooks, allocated memory) on DLL unload/failure.
 * Attempts disable/remove hooks, uninit MinHook, free allocated memory.
 * Safe to call even if initialization was partial.
 */
void CleanupResources()
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "Cleanup: Starting resource cleanup...");

    // Determine if MinHook likely needs uninitialization
    bool minhook_was_used = (g_tpvHookAddress || g_menuOpenHookAddress || g_menuCloseHookAddress ||
                             g_cameraDistanceHookAddress || fpTPV_OriginalCode ||
                             fpMenuOpen_OriginalCode || fpMenuClose_OriginalCode ||
                             fpCameraDistance_OriginalCode);

    // --- Disable & Remove MinHook Hooks ---
    // Use addresses stored in anonymous namespace
    if (g_tpvHookAddress)
        MH_RemoveHook(g_tpvHookAddress);
    if (g_menuOpenHookAddress)
        MH_RemoveHook(g_menuOpenHookAddress);
    if (g_menuCloseHookAddress)
        MH_RemoveHook(g_menuCloseHookAddress);
    if (g_cameraDistanceHookAddress)
        MH_RemoveHook(g_cameraDistanceHookAddress);
    logger.log(LOG_DEBUG, "Cleanup: Hooks removal attempted.");

    // --- Uninitialize MinHook ---
    if (minhook_was_used)
    {
        MH_STATUS mh_stat = MH_Uninitialize();
        if (mh_stat != MH_OK && mh_stat != MH_ERROR_NOT_INITIALIZED)
        {
            logger.log(LOG_ERROR, "Cleanup: MinHook uninitialize failed: " +
                                      std::string(MH_StatusToString(mh_stat)));
        }
        else
        {
            logger.log(LOG_INFO, "Cleanup: MinHook uninitialize finished (Status: " +
                                     std::string(MH_StatusToString(mh_stat)) + ")");
        }
    }
    else
    {
        logger.log(LOG_DEBUG, "Cleanup: Skipping MinHook uninitialize (likely not used).");
    }

    // --- Free Allocated Memory ---
    SafeVirtualFree(g_r9_for_tpv_flag, "R9 (TPV)", logger);
    SafeVirtualFree(g_rbx_for_camera_distance, "RBX (CameraDist)", logger);

    // --- Reset Global State Pointers ---
    fpTPV_OriginalCode = nullptr;
    fpMenuOpen_OriginalCode = nullptr;
    fpMenuClose_OriginalCode = nullptr;
    fpCameraDistance_OriginalCode = nullptr;
    g_isOverlayActive = false;    // Reset flag value
    g_pIsOverlayActive = nullptr; // Reset pointer

    // Reset internal target addresses
    g_tpvHookAddress = nullptr;
    g_menuOpenHookAddress = nullptr;
    g_menuCloseHookAddress = nullptr;
    g_cameraDistanceHookAddress = nullptr;

    logger.log(LOG_INFO, "Cleanup: Resource cleanup finished.");
}

/**
 * @brief Allocates memory for storing a captured register pointer or address.
 * @param ptr_addr Reference to the global pointer variable to assign.
 * @param name Descriptive name for logging.
 * @param logger Reference to the logger.
 * @return True on success, False on failure.
 */
bool AllocateRegisterStorage(uintptr_t *&ptr_addr, const std::string &name, Logger &logger)
{
    ptr_addr = reinterpret_cast<uintptr_t *>(
        VirtualAlloc(NULL, sizeof(uintptr_t), MEM_COMMIT | MEM_RESERVE,
                     PAGE_READWRITE));
    if (!ptr_addr)
    {
        logger.log(LOG_ERROR, "Fatal: VirtualAlloc failed for " + name +
                                  " storage. Err: " + std::to_string(GetLastError()));
        return false;
    }
    *ptr_addr = 0; // Initialize stored pointer/address value to NULL
    logger.log(LOG_DEBUG, "MainThread: Allocated " + name + " storage: " +
                              format_address(reinterpret_cast<uintptr_t>(ptr_addr)));
    return true;
}

/**
 * @brief Sets up a MinHook hook for a given target address.
 * @details This version takes the target address directly, useful when the
 *          address might be stored globally or locally.
 * @param target_addr The exact address to hook.
 * @param detour_func Pointer to the assembly detour function.
 * @param original_code_ptr Global pointer to store the trampoline continuation.
 * @param hook_name Name for logging.
 * @param logger Logger instance.
 * @return True on success, False on failure.
 */
bool CreateAndEnableHookDirect(BYTE *target_addr, // Direct address
                               LPVOID detour_func,
                               void **original_code_ptr, // Double pointer to store result
                               const std::string &hook_name,
                               Logger &logger)
{
    if (!target_addr)
    {
        logger.log(LOG_ERROR, "Fatal: Cannot create " + hook_name +
                                  " hook, target address is NULL.");
        return false;
    }

    logger.log(LOG_INFO, "MainThread: Creating " + hook_name + " hook at " +
                             format_address(reinterpret_cast<uintptr_t>(target_addr)) + "...");

    MH_STATUS mh_stat = MH_CreateHook(target_addr, detour_func, original_code_ptr);
    if (mh_stat != MH_OK)
    {
        logger.log(LOG_ERROR, "Fatal: MH_CreateHook failed for " + hook_name +
                                  ": " + std::string(MH_StatusToString(mh_stat)));
        return false;
    }
    if (!(*original_code_ptr))
    {
        logger.log(LOG_ERROR, "Fatal: MH_CreateHook OK for " + hook_name +
                                  " but continuation address is NULL");
        MH_RemoveHook(target_addr);
        return false;
    }
    logger.log(LOG_DEBUG, "MainThread: " + hook_name + " hook created. "
                                                       "Continuation: " +
                              format_address(reinterpret_cast<uintptr_t>(*original_code_ptr)));

    logger.log(LOG_INFO, "MainThread: Enabling " + hook_name + " hook...");
    mh_stat = MH_EnableHook(target_addr);
    if (mh_stat != MH_OK)
    {
        logger.log(LOG_ERROR, "Fatal: MH_EnableHook failed for " + hook_name +
                                  ": " + std::string(MH_StatusToString(mh_stat)));
        MH_RemoveHook(target_addr);
        return false;
    }
    logger.log(LOG_INFO, "MainThread: " + hook_name + " hook enabled successfully.");
    return true;
}

/**
 * @brief Main initialization function, runs in a separate thread on DLL attach.
 * @param hModule_param Module handle (unused).
 * @return DWORD 0 on success, non-zero on fatal error preventing mod function.
 */
DWORD WINAPI MainThread(LPVOID hModule_param)
{
    (void)hModule_param; // Unused parameter

    // Phase 1: Basic Setup
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "--------------------");
    Version::logVersionInfo();
    Config config = loadConfig(Constants::getConfigFilename());

    LogLevel log_level = LOG_INFO; // Default
    if (config.log_level == "DEBUG")
        log_level = LOG_DEBUG;
    else if (config.log_level == "WARNING")
        log_level = LOG_WARNING;
    else if (config.log_level == "ERROR")
        log_level = LOG_ERROR;
    else if (config.log_level == "INFO")
        log_level = LOG_INFO;
    // Call simplified setLogLevel
    logger.setLogLevel(log_level);

    logger.log(LOG_INFO, "MainThread: Initializing mod...");
    logger.log(LOG_INFO, "Settings: ToggleKeys: " + format_vkcode_list(config.toggle_keys));
    logger.log(LOG_INFO, "Settings: FPVKeys: " + format_vkcode_list(config.fpv_keys));
    logger.log(LOG_INFO, "Settings: TPVKeys: " + format_vkcode_list(config.tpv_keys));
    // Log level is logged inside setLogLevel now
    if (logger.isDebugEnabled()) // Only log patterns if debug enabled
    {
        logger.log(LOG_DEBUG, "Settings: TPV AOBPattern: " + config.aob_pattern);
        // Use correct constant names
        logger.log(LOG_DEBUG, "Settings: Menu Open AOB: " + std::string(Constants::MENU_OPEN_AOB_PATTERN));
        logger.log(LOG_DEBUG, "Settings: Menu Close AOB: " + std::string(Constants::MENU_CLOSE_AOB_PATTERN));
        logger.log(LOG_DEBUG, "Settings: CamDist AOBPattern: " + std::string(Constants::CAMERA_DISTANCE_AOB_PATTERN));
    }
    // Use correct constant names
    logger.log(LOG_INFO, "Settings: Offsets: TPVHook(+" + std::to_string(Constants::TPV_HOOK_OFFSET) +
                             ") Flag(R9+" + format_hex(Constants::TOGGLE_FLAG_OFFSET) +
                             ") MenuOpenHook(+" + std::to_string(Constants::MENU_OPEN_HOOK_OFFSET) +
                             ") MenuCloseHook(+" + std::to_string(Constants::MENU_CLOSE_HOOK_OFFSET) +
                             ") CamHook(+" + std::to_string(Constants::CAMERA_HOOK_OFFSET) +
                             ") Dist(RBX+" + format_hex(Constants::CAMERA_DISTANCE_OFFSET) + ")");

    // Phase 2: Memory Allocation & Module Info
    // Initialize the overlay detection variables
    g_isOverlayActive = false;
    g_pIsOverlayActive = &g_isOverlayActive;
    logger.log(LOG_DEBUG, "MainThread: g_pIsOverlayActive points to: " + format_address(reinterpret_cast<uintptr_t>(g_pIsOverlayActive)));
    logger.log(LOG_DEBUG, "MainThread: g_isOverlayActive address: " + format_address(reinterpret_cast<uintptr_t>(&g_isOverlayActive)));

    // Allocate storage for other captured pointers
    if (!AllocateRegisterStorage(g_r9_for_tpv_flag, "R9 (TPV)", logger) ||
        !AllocateRegisterStorage(g_rbx_for_camera_distance, "RBX (CameraDist)", logger))
    {
        CleanupResources();
        return 1;
    }

    logger.log(LOG_INFO, "MainThread: Searching for module '" + std::string(Constants::MODULE_NAME) + "'...");
    HMODULE game_module = NULL;
    for (int i = 0; i < 30 && !game_module; ++i)
    {
        game_module = GetModuleHandleA(Constants::MODULE_NAME);
        if (!game_module)
        {
            if (i == 0)
                logger.log(LOG_WARNING, "Module not found yet, retrying...");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (!game_module)
    {
        logger.log(LOG_ERROR, "Fatal: Module '" + std::string(Constants::MODULE_NAME) + "' not found after timeout.");
        CleanupResources();
        return 1;
    }
    logger.log(LOG_INFO, "MainThread: Found module '" + std::string(Constants::MODULE_NAME) + "' at " +
                             format_address(reinterpret_cast<uintptr_t>(game_module)));

    MODULEINFO mod_info = {};
    if (!GetModuleInformation(GetCurrentProcess(), game_module, &mod_info, sizeof(mod_info)))
    {
        logger.log(LOG_ERROR, "Fatal: GetModuleInformation failed. Err: " + std::to_string(GetLastError()));
        CleanupResources();
        return 1;
    }
    BYTE *module_base = static_cast<BYTE *>(mod_info.lpBaseOfDll);
    size_t module_size = mod_info.SizeOfImage;
    logger.log(LOG_DEBUG, "MainThread: Module size: " + format_address(module_size));
    logger.log(LOG_DEBUG, "MainThread: === Pre-Scan Checkpoint ===");

    // Phase 3: AOB Scans and Hook Address Calculation
    bool scans_ok = true;
    BYTE *pattern_start = nullptr;

    logger.log(LOG_INFO, "MainThread: Scanning for TPV pattern...");
    std::vector<BYTE> tpv_pattern = parseAOB(config.aob_pattern);
    if (!tpv_pattern.empty())
        pattern_start = FindPattern(module_base, module_size, tpv_pattern);
    if (pattern_start)
    {
        g_tpvHookAddress = pattern_start + Constants::TPV_HOOK_OFFSET;
        logger.log(LOG_INFO, "MainThread: TPV target: " + format_address(reinterpret_cast<uintptr_t>(g_tpvHookAddress)));
    }
    else
    {
        logger.log(LOG_ERROR, "Fatal: TPV AOB pattern not found or parse failed.");
        scans_ok = false;
    }

    // Menu Open Scan
    pattern_start = nullptr;
    logger.log(LOG_INFO, "MainThread: Scanning for Menu Open pattern...");
    std::vector<BYTE> menu_open_pattern = parseAOB(Constants::MENU_OPEN_AOB_PATTERN);
    if (!menu_open_pattern.empty())
        pattern_start = FindPattern(module_base, module_size, menu_open_pattern);
    if (pattern_start)
    {
        g_menuOpenHookAddress = pattern_start + Constants::MENU_OPEN_HOOK_OFFSET;
        logger.log(LOG_INFO, "MainThread: Menu Open target: " + format_address(reinterpret_cast<uintptr_t>(g_menuOpenHookAddress)));

        // Verify the pattern by dumping the first few bytes
        if (logger.isDebugEnabled())
        {
            std::ostringstream bytes_ss;
            bytes_ss << "Bytes at hook: ";
            for (int i = 0; i < 8; i++)
            {
                bytes_ss << std::hex << std::setw(2) << std::setfill('0')
                         << static_cast<int>(g_menuOpenHookAddress[i]) << " ";
            }
            logger.log(LOG_DEBUG, bytes_ss.str());
        }
    }
    else
    {
        logger.log(LOG_ERROR, "Fatal: Menu Open AOB pattern not found or parse failed.");
        scans_ok = false;
    }

    // Menu Close Scan
    pattern_start = nullptr;
    logger.log(LOG_INFO, "MainThread: Scanning for Menu Close pattern...");
    std::vector<BYTE> menu_close_pattern = parseAOB(Constants::MENU_CLOSE_AOB_PATTERN);
    if (!menu_close_pattern.empty())
        pattern_start = FindPattern(module_base, module_size, menu_close_pattern);
    if (pattern_start)
    {
        g_menuCloseHookAddress = pattern_start + Constants::MENU_CLOSE_HOOK_OFFSET;
        logger.log(LOG_INFO, "MainThread: Menu Close target: " + format_address(reinterpret_cast<uintptr_t>(g_menuCloseHookAddress)));

        // Verify the pattern by dumping the first few bytes
        if (logger.isDebugEnabled())
        {
            std::ostringstream bytes_ss;
            bytes_ss << "Bytes at hook: ";
            for (int i = 0; i < 8; i++)
            {
                bytes_ss << std::hex << std::setw(2) << std::setfill('0')
                         << static_cast<int>(g_menuCloseHookAddress[i]) << " ";
            }
            logger.log(LOG_DEBUG, bytes_ss.str());
        }
    }
    else
    {
        logger.log(LOG_ERROR, "Fatal: Menu Close AOB pattern not found or parse failed.");
        scans_ok = false;
    }

    // Camera Distance Scan
    pattern_start = nullptr;
    logger.log(LOG_INFO, "MainThread: Scanning for Camera Distance pattern...");
    std::vector<BYTE> camera_pattern = parseAOB(Constants::CAMERA_DISTANCE_AOB_PATTERN);
    if (!camera_pattern.empty())
        pattern_start = FindPattern(module_base, module_size, camera_pattern);
    if (pattern_start)
    {
        g_cameraDistanceHookAddress = pattern_start + Constants::CAMERA_HOOK_OFFSET;
        logger.log(LOG_INFO, "MainThread: Camera Distance target: " + format_address(reinterpret_cast<uintptr_t>(g_cameraDistanceHookAddress)));
    }
    else
    {
        logger.log(LOG_ERROR, "Fatal: Camera Distance AOB pattern not found or parse failed.");
        scans_ok = false;
    }

    // Check scan results before proceeding to hooking
    if (!scans_ok)
    {
        logger.log(LOG_ERROR, "Fatal: One or more AOB scans failed. Cannot proceed.");
        CleanupResources();
        return 1;
    }

    // Phase 4: Initialize MinHook and Create/Enable Hooks
    logger.log(LOG_INFO, "MainThread: Initializing MinHook...");
    MH_STATUS mh_stat = MH_Initialize();
    if (mh_stat != MH_OK)
    {
        logger.log(LOG_ERROR, "Fatal: MH_Initialize failed: " + std::string(MH_StatusToString(mh_stat)));
        CleanupResources();
        return 1;
    }

    // Create and enable hooks using the target address variables
    bool hooks_ok = true;
    // Use CreateAndEnableHookDirect as target addresses are stored locally now
    hooks_ok &= CreateAndEnableHookDirect(g_tpvHookAddress, reinterpret_cast<LPVOID>(&TPV_CaptureR9_Detour),
                                          &fpTPV_OriginalCode, "TPV R9 Capture", logger);
    hooks_ok &= CreateAndEnableHookDirect(g_menuOpenHookAddress, reinterpret_cast<LPVOID>(&MenuOpen_Detour),
                                          &fpMenuOpen_OriginalCode, "Menu Open", logger);
    hooks_ok &= CreateAndEnableHookDirect(g_menuCloseHookAddress, reinterpret_cast<LPVOID>(&MenuClose_Detour),
                                          &fpMenuClose_OriginalCode, "Menu Close", logger);
    hooks_ok &= CreateAndEnableHookDirect(g_cameraDistanceHookAddress, reinterpret_cast<LPVOID>(&CameraDistance_CaptureRBX_Detour),
                                          &fpCameraDistance_OriginalCode, "CamDist RBX Capture", logger);

    if (!hooks_ok)
    {
        logger.log(LOG_ERROR, "Fatal: Failed to create or enable one or more hooks.");
        CleanupResources();
        return 1;
    }

    // Phase 5: Start Key/State Monitoring Thread
    logger.log(LOG_INFO, "MainThread: Starting monitor thread...");
    ToggleData *thread_data = new ToggleData{
        std::move(config.toggle_keys),
        std::move(config.fpv_keys),
        std::move(config.tpv_keys)};

    // Wait loop only for essential TPV R9 pointer
    logger.log(LOG_INFO, "MainThread: Waiting for essential game pointers (TPV R9)...");
    int wait_cycles = 0;
    while (!g_r9_for_tpv_flag || *g_r9_for_tpv_flag == 0)
    {
        // Waiting for R9(TPV) pointer...
        wait_cycles++;
        Sleep(200);
    }
    logger.log(LOG_INFO, "MainThread: TPV R9 pointer acquired. Other pointers checked at runtime.");
    logger.log(LOG_DEBUG, "MainThread: R9(TPV)=" + format_address(g_r9_for_tpv_flag ? *g_r9_for_tpv_flag : 0));

    HANDLE h_thread = CreateThread(NULL, 0, MonitorThread, thread_data, 0, NULL);
    if (!h_thread)
    {
        logger.log(LOG_ERROR, "Fatal: CreateThread failed for monitor thread. Error: " + std::to_string(GetLastError()));
        CleanupResources();
        delete thread_data;
        return 1;
    }
    CloseHandle(h_thread);
    logger.log(LOG_INFO, "MainThread: Initialization successful. Mod active.");

    return 0; // Success
}

/**
 * @brief Standard Windows DLL entry point. Initializes/cleans up the mod.
 * @param hModule Handle to this DLL module.
 * @param reason_for_call Attach/Detach reason code.
 * @param lpReserved Reserved.
 * @return BOOL TRUE on success, FALSE on attach failure.
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason_for_call, LPVOID lpReserved)
{
    (void)lpReserved; // Unused

    switch (reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE h_main = CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
        if (!h_main)
        {
            MessageBoxA(NULL, "FATAL: Failed create initialization thread!", Constants::MOD_NAME, MB_ICONERROR | MB_OK);
            return FALSE; // Fail DLL load
        }
        CloseHandle(h_main);
        break;
    }
    case DLL_PROCESS_DETACH:
        CleanupResources();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}
