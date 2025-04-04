/**
 * @file toggle_thread.cpp
 * @brief Implements background thread for key/state monitoring and interaction.
 *
 * Contains the main polling loop (GetAsyncKeyState), key state debouncing,
 * overlay state tracking via global boolean updated by hooks, and view state
 * management using captured R9. Includes automatic view switching when
 * overlays open/close.
 */

#include "toggle_thread.h" // Includes extern global pointer/flag declarations
#include "logger.h"
#include "utils.h"
#include "constants.h"

#include <windows.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <sstream>
#include <cmath>
#include <iomanip>

// --- Helper Function: Memory Validation ---
/**
 * @brief Checks if a memory address is likely readable.
 */
bool isMemoryReadable(const volatile void *address, size_t size)
{
    if (!address || size == 0)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(const_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD READ_FLAGS = (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    if ((mbi.Protect & READ_FLAGS) == 0 || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD))
        return false;
    uintptr_t region_base_addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t region_end_addr = region_base_addr + mbi.RegionSize;
    uintptr_t requested_addr = reinterpret_cast<uintptr_t>(address);
    uintptr_t requested_end_addr = requested_addr + size;
    if (requested_end_addr < requested_addr)
        return false;
    return (requested_addr >= region_base_addr && requested_end_addr <= region_end_addr);
}

/**
 * @brief Checks if a memory address is likely writable.
 */
bool isMemoryWritable(volatile void *address, size_t size)
{
    if (!address || size == 0)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(const_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD WRITE_FLAGS = (PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    if ((mbi.Protect & WRITE_FLAGS) == 0 || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD))
        return false;
    uintptr_t region_base_addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t region_end_addr = region_base_addr + mbi.RegionSize;
    uintptr_t requested_addr = reinterpret_cast<uintptr_t>(address);
    uintptr_t requested_end_addr = requested_addr + size;
    if (requested_end_addr < requested_addr)
        return false;
    return (requested_addr >= region_base_addr && requested_end_addr <= region_end_addr);
}

// --- View State Management (Uses R9 from TPV Hook) ---
/**
 * @brief (Internal) Reads the TPV flag byte using captured R9.
 */
bool readViewStateInternal(BYTE &current_value)
{
    if (!g_r9_for_tpv_flag)
        return false;
    uintptr_t r9_base_value = *g_r9_for_tpv_flag;
    if (r9_base_value == 0)
        return false;
    volatile BYTE *flag_addr = reinterpret_cast<volatile BYTE *>(r9_base_value + Constants::TOGGLE_FLAG_OFFSET);
    if (!isMemoryReadable(flag_addr, sizeof(BYTE)))
        return false;
    current_value = *flag_addr;
    return true;
}

/**
 * @brief (Internal) Writes the TPV flag byte using captured R9.
 */
bool writeViewStateInternal(BYTE new_state)
{
    Logger &logger = Logger::getInstance();
    if (!g_r9_for_tpv_flag)
    {
        logger.log(LOG_ERROR, "WriteView: Global R9 storage ptr is NULL.");
        return false;
    }
    uintptr_t r9_base_value = *g_r9_for_tpv_flag;
    if (r9_base_value == 0)
    {
        logger.log(LOG_WARNING, "WriteView: Cannot write, captured R9 is 0x0.");
        return false;
    }
    volatile BYTE *flag_addr = reinterpret_cast<volatile BYTE *>(r9_base_value + Constants::TOGGLE_FLAG_OFFSET);
    if (!isMemoryWritable(flag_addr, sizeof(BYTE)))
    {
        logger.log(LOG_ERROR, "WriteView: Cannot write to flag address " + format_address(reinterpret_cast<uintptr_t>(flag_addr)));
        return false;
    }
    *flag_addr = new_state;
    return true;
}

/**
 * @brief Safely gets the current view state (FPV/TPV).
 */
int getViewState()
{
    BYTE current_value = 0;
    if (readViewStateInternal(current_value))
    {
        if (current_value == 0 || current_value == 1)
            return static_cast<int>(current_value);
        else
        {
            Logger::getInstance().log(LOG_WARNING, "GetView: Read unexpected TPV flag value: " + std::to_string(current_value));
            return -1;
        }
    }
    return -1;
}

/**
 * @brief Safely sets the view state (FPV/TPV).
 */
bool setViewState(BYTE new_state, int *key_pressed_vk = nullptr)
{
    Logger &logger = Logger::getInstance();
    BYTE current_value = 0xFF;
    std::string key_log_suffix = (key_pressed_vk) ? " (Key: " + format_vkcode(*key_pressed_vk) + ")" : " (Internal)";
    std::string action_desc = (new_state == 0) ? "Set FPV" : "Set TPV";
    if (readViewStateInternal(current_value))
    {
        if (current_value == new_state)
        {
            if (logger.isDebugEnabled())
                logger.log(LOG_DEBUG, action_desc + key_log_suffix + ": View already " + std::to_string(new_state) + ". No change.");
            return true;
        }
    }
    logger.log(LOG_INFO, action_desc + key_log_suffix + ": Attempting write -> " + std::to_string(new_state));
    if (writeViewStateInternal(new_state))
    {
        logger.log(LOG_INFO, action_desc + key_log_suffix + ": Set TPV Flag to " + std::to_string(new_state) + (new_state ? " (TPV)" : " (FPV)"));
        return true;
    }
    else
    {
        logger.log(LOG_ERROR, action_desc + key_log_suffix + ": Write FAILED.");
        return false;
    }
}

/**
 * @brief Safely toggles between FPV (0) and TPV (1).
 */
bool safeToggleViewState(int *key_pressed_vk)
{
    Logger &logger = Logger::getInstance();
    std::string key_log_suffix = (key_pressed_vk) ? " (Key: " + format_vkcode(*key_pressed_vk) + ")" : " (Internal)";
    int current_state = getViewState();
    if (current_state == 0)
    {
        logger.log(LOG_DEBUG, "Toggle" + key_log_suffix + ": Current=FPV(0), switching to TPV(1)...");
        return setViewState(1, key_pressed_vk);
    }
    else if (current_state == 1)
    {
        logger.log(LOG_DEBUG, "Toggle" + key_log_suffix + ": Current=TPV(1), switching to FPV(0)...");
        return setViewState(0, key_pressed_vk);
    }
    else
    {
        logger.log(LOG_ERROR, "Toggle" + key_log_suffix + ": Cannot toggle, failed get valid state (" + std::to_string(current_state) + ").");
        return false;
    }
}

/** @brief Sets view to First Person. Wrapper for setViewState(0). */
bool setFirstPersonView(int *key_pressed_vk) { return setViewState(0, key_pressed_vk); }

/** @brief Sets view to Third Person. Wrapper for setViewState(1). */
bool setThirdPersonView(int *key_pressed_vk) { return setViewState(1, key_pressed_vk); }

// --- Main Monitor Thread ---
DWORD WINAPI MonitorThread(LPVOID param)
{
    // --- Initialization ---
    ToggleData *data_ptr = static_cast<ToggleData *>(param);
    if (!data_ptr)
    {
        Logger::getInstance().log(LOG_ERROR, "MonitorThread: NULL data.");
        return 1;
    }
    std::vector<int> toggle_keys = std::move(data_ptr->toggle_keys);
    std::vector<int> fpv_keys = std::move(data_ptr->fpv_keys);
    std::vector<int> tpv_keys = std::move(data_ptr->tpv_keys);
    delete data_ptr;
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "MonitorThread: Monitoring thread started.");

    // --- Debounce Map ---
    std::unordered_map<int, bool> key_was_down;
    std::vector<const std::vector<int> *> all_key_lists = {&toggle_keys, &fpv_keys, &tpv_keys};
    for (const auto *key_list : all_key_lists)
    {
        for (int vk : *key_list)
        {
            key_was_down[vk] = false;
        }
    }
    bool noop_keys = toggle_keys.empty() && fpv_keys.empty() && tpv_keys.empty();

    // --- State Tracking ---
    // Initialize previous state by safely reading the global flag
    bool previous_overlay_state = g_isOverlayActive;
    bool was_tpv_before_overlay = false; // Track view state before overlay opened

    // Define sleep constants
    const DWORD NORMAL_SLEEP_MS = 30;
    const DWORD ACTIVE_SLEEP_MS = 20;

    if (noop_keys)
        logger.log(LOG_INFO, "MonitorThread: No user keys configured. Overlay state handler active.");
    else
        logger.log(LOG_INFO, "MonitorThread: Keys: Toggle=" + format_vkcode_list(toggle_keys) +
                                 ", FPV=" + format_vkcode_list(fpv_keys) +
                                 ", TPV=" + format_vkcode_list(tpv_keys));

    logger.log(LOG_INFO, "MonitorThread: Starting main loop...");

    // --- Main Polling Loop ---
    while (true)
    {
        try
        {
            bool key_action_this_cycle = false;
            bool verbose_debug = logger.isDebugEnabled();

            // *** Read current view state *before* processing changes ***
            int current_view_state_snapshot = getViewState(); // Snapshot at start of cycle

            // --- Process Key Input ---
            if (!noop_keys)
            {
                int vk_pressed = 0;
                for (int vk : toggle_keys)
                {
                    bool is_down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    if (is_down && !key_was_down[vk])
                    {
                        if (verbose_debug)
                            logger.log(LOG_DEBUG, "Input: Toggle Key " + format_vkcode(vk) + " pressed.");
                        vk_pressed = vk;
                        safeToggleViewState(&vk_pressed);
                        key_action_this_cycle = true;
                    }
                    key_was_down[vk] = is_down;
                }
                for (int vk : fpv_keys)
                {
                    bool is_down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    if (is_down && !key_was_down[vk])
                    {
                        if (verbose_debug)
                            logger.log(LOG_DEBUG, "Input: FPV Key " + format_vkcode(vk) + " pressed.");
                        vk_pressed = vk;
                        setFirstPersonView(&vk_pressed);
                        key_action_this_cycle = true;
                    }
                    key_was_down[vk] = is_down;
                }
                for (int vk : tpv_keys)
                {
                    bool is_down = (GetAsyncKeyState(vk) & 0x8000) != 0;
                    if (is_down && !key_was_down[vk])
                    {
                        if (verbose_debug)
                            logger.log(LOG_DEBUG, "Input: TPV Key " + format_vkcode(vk) + " pressed.");
                        vk_pressed = vk;
                        setThirdPersonView(&vk_pressed);
                        key_action_this_cycle = true;
                    }
                    key_was_down[vk] = is_down;
                }
            }

            // --- Process Overlay State Change (With Auto View Switch) ---
            // Directly read g_isOverlayActive flag
            bool current_overlay_state = g_isOverlayActive;
            if (current_overlay_state != previous_overlay_state)
            {
                // --- Overlay Just Opened ---
                if (current_overlay_state) // Became true
                {
                    logger.log(LOG_INFO, "Overlay: State transition detected -> ACTIVE");
                    // *** Use the snapshot taken at the start of the loop ***
                    if (current_view_state_snapshot == 1)
                    { // Was TPV *before* this cycle started
                        logger.log(LOG_INFO, "Overlay: Was TPV, switching to FPV.");
                        was_tpv_before_overlay = true; // Set flag to remember
                        // Switch to FPV (the game might do this anyway, but we ensure it)
                        setFirstPersonView(); // Internal call, no key press arg needed
                    }
                    else
                    { // Was FPV or error
                        if (verbose_debug)
                            logger.log(LOG_DEBUG, "Overlay: Was FPV/Unknown, no view change needed.");
                        was_tpv_before_overlay = false; // Ensure flag is false
                    }
                }
                // --- Overlay Just Closed ---
                else // Became false
                {
                    logger.log(LOG_INFO, "Overlay: State transition detected -> INACTIVE");
                    if (was_tpv_before_overlay)
                    { // If we were TPV before opening
                        logger.log(LOG_INFO, "Overlay: Was TPV before, restoring TPV state...");
                        // Switch back to TPV
                        setThirdPersonView(); // Internal call, no key press arg needed
                    }
                    else
                    {
                        if (verbose_debug)
                            logger.log(LOG_DEBUG, "Overlay: Was not TPV before, no view restore.");
                    }
                    // Reset the tracking flag *after* handling the close event
                    was_tpv_before_overlay = false;
                }
                previous_overlay_state = current_overlay_state; // Update tracked state for next cycle
            }

            // --- Adaptive Sleep ---
            DWORD sleep_duration = key_action_this_cycle ? ACTIVE_SLEEP_MS : NORMAL_SLEEP_MS;
            Sleep(sleep_duration);
        }
        // Exception Handling
        catch (const std::exception &e)
        {
            logger.log(LOG_ERROR, "MonitorThread: Exception: " + std::string(e.what()));
            Sleep(1000); // Longer sleep on exception
        }
        catch (...)
        {
            logger.log(LOG_ERROR, "MonitorThread: Unknown exception.");
            Sleep(1000); // Longer sleep on exception
        }
    } // End while(true)
}
