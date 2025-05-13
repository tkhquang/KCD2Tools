#include "camera_profile_thread.h"
#include "camera_profile.h"
#include "logger.h"
#include "utils.h"
#include "global_state.h"
#include "config.h"

#include <unordered_map>
#include <bitset>
#include <vector>
#include <algorithm>
#include <chrono>
#include <iomanip>

// External config reference
extern Config g_config;

/**
 * @brief Structure to efficiently map VK codes to bit positions for batch processing
 */
struct KeyBitMapInfo
{
    std::vector<int> allKeys;               // All registered keys in monitoring order
    std::unordered_map<int, size_t> keyMap; // Maps VK code -> bit position (0 to 63)
    size_t keyCount = 0;                    // Number of unique keys mapped

    // Bitmasks for different key groups
    uint64_t masterToggleMask = 0;
    uint64_t profileSaveMask = 0; // Create New
    uint64_t profileCycleMask = 0;
    uint64_t profileResetMask = 0;
    uint64_t profileUpdateMask = 0;
    uint64_t profileDeleteMask = 0;
    uint64_t offsetXIncMask = 0;
    uint64_t offsetXDecMask = 0;
    uint64_t offsetYIncMask = 0;
    uint64_t offsetYDecMask = 0;
    uint64_t offsetZIncMask = 0;
    uint64_t offsetZDecMask = 0;

    // Add more masks here if total unique keys > 64 bits supported by uint64_t
};

/**
 * @brief Create a bitmap mapping for all registered keys
 * @param config The global configuration with key settings
 * @return KeyBitMapInfo structure with mappings and masks
 */
KeyBitMapInfo createKeyBitMap(const Config &config)
{
    KeyBitMapInfo info;
    info.keyCount = 0; // Reset key count

    // Helper to register keys and update masks
    auto registerKeys = [&](const std::vector<int> &keys, uint64_t &mask)
    {
        for (int vk : keys)
        {
            if (vk != 0)
            {
                // Check if key already registered and if we have space in the 64-bit map
                if (info.keyMap.find(vk) == info.keyMap.end())
                {
                    if (info.keyCount >= 64)
                    {
                        // Log error or warning: exceeded 64 unique hotkeys
                        Logger::getInstance().log(LOG_ERROR, "CameraProfileThread: Exceeded maximum unique hotkeys (64). Key " + format_vkcode(vk) + " ignored.");
                        continue; // Skip this key
                    }
                    info.keyMap[vk] = info.keyCount; // Assign current bit position
                    info.allKeys.push_back(vk);
                    info.keyCount++;
                }
                // Set the corresponding bit in the specific action mask
                mask |= (1ULL << info.keyMap[vk]);
            }
        }
    };

    // Register all key groups from config, including new ones
    registerKeys(config.master_toggle_keys, info.masterToggleMask);
    registerKeys(config.profile_save_keys, info.profileSaveMask); // Create New
    registerKeys(config.profile_cycle_keys, info.profileCycleMask);
    registerKeys(config.profile_reset_keys, info.profileResetMask);
    registerKeys(config.profile_update_keys, info.profileUpdateMask);
    registerKeys(config.profile_delete_keys, info.profileDeleteMask);
    registerKeys(config.offset_x_inc_keys, info.offsetXIncMask);
    registerKeys(config.offset_x_dec_keys, info.offsetXDecMask);
    registerKeys(config.offset_y_inc_keys, info.offsetYIncMask);
    registerKeys(config.offset_y_dec_keys, info.offsetYDecMask);
    registerKeys(config.offset_z_inc_keys, info.offsetZIncMask);
    registerKeys(config.offset_z_dec_keys, info.offsetZDecMask);

    return info;
}

/**
 * @brief Get current state of all registered keys as a bitmap
 * @param keys Vector of VK codes to check
 * @param keyMap Mapping of VK codes to bit positions
 * @return 64-bit bitmap where each bit represents a key state (1=pressed)
 */
uint64_t getKeyStateBitmap(const std::vector<int> &keys, const std::unordered_map<int, size_t> &keyMap)
{
    uint64_t stateBitmap = 0;

    for (int vk : keys)
    {
        if (vk != 0)
        {
            auto it = keyMap.find(vk);
            if (it != keyMap.end())
            {
                // GetAsyncKeyState returns non-zero if pressed; highest bit indicates currently down.
                bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
                if (pressed)
                {
                    // Set the corresponding bit if key is down
                    stateBitmap |= (1ULL << it->second);
                }
            }
        }
    }

    return stateBitmap;
}

/**
 * @brief Compare current key state bitmap with previous to detect new key presses
 * @param currentState Current key state bitmap
 * @param previousState Previous key state bitmap
 * @param actionMask Bitmap mask for the specific key group
 * @return true if any key in the group was newly pressed
 */
bool isNewKeyPress(uint64_t currentState, uint64_t previousState, uint64_t actionMask)
{
    // A key is newly pressed if it's set in current state but not in previous state
    uint64_t newPresses = (currentState & ~previousState) & actionMask;
    return newPresses != 0;
}

/**
 * @brief Camera profile thread entry point - optimized with batch key checking
 */
DWORD WINAPI CameraProfileThread(LPVOID param)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "CameraProfileThread: Started");

    // Read thread parameters (adjustment step)
    CameraProfileThreadData *data = static_cast<CameraProfileThreadData *>(param);
    if (!data)
    {
        logger.log(LOG_ERROR, "CameraProfileThread: NULL data received");
        return 1;
    }
    float adjustmentStep = data->adjustmentStep;
    delete data; // Clean up thread data structure

    // Create key mappings based on loaded config
    KeyBitMapInfo keyInfo = createKeyBitMap(g_config);

    // Initialize key state tracker (all keys initially up)
    uint64_t previousKeyState = 0;

    logger.log(LOG_DEBUG, "CameraProfileThread: Registered " + std::to_string(keyInfo.keyCount) + " unique keys for monitoring.");

    // Main loop
    while (WaitForSingleObject(g_exitEvent, 16) != WAIT_OBJECT_0) // ~60 Hz check
    {
        try
        {
            // Get current state of all monitored keys
            uint64_t currentKeyState = getKeyStateBitmap(keyInfo.allKeys, keyInfo.keyMap);

            // --- Process Keys ---

            // Master toggle: Always check regardless of adjustment mode
            if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.masterToggleMask))
            {
                bool newMode = !g_cameraAdjustmentMode.load();
                g_cameraAdjustmentMode.store(newMode);
                logger.log(LOG_INFO, "CameraProfileThread: Adjustment mode " + std::string(newMode ? "ENABLED" : "DISABLED"));
            }

            // Check other keys only if adjustment mode is enabled
            if (g_cameraAdjustmentMode.load())
            {
                // 1. CREATE NEW Profile key (e.g., Numpad 1)
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileSaveMask))
                {
                    logger.log(LOG_DEBUG, "CameraProfileThread: Create New Profile key press detected.");
                    CameraProfileManager::getInstance().createNewProfileFromLiveState("General");
                }

                // 2. UPDATE ACTIVE Profile key (e.g., Numpad 7)
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileUpdateMask))
                {
                    logger.log(LOG_DEBUG, "CameraProfileThread: Update Active Profile key press detected.");
                    // This function internally checks if active is "Default" and logs a warning if so.
                    CameraProfileManager::getInstance().updateActiveProfileWithLiveState();
                }

                // 3. DELETE ACTIVE Profile key (e.g., Numpad 9)
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileDeleteMask))
                {
                    logger.log(LOG_DEBUG, "CameraProfileThread: Delete Active Profile key press detected.");
                    // This function internally checks if active is "Default" and prevents deletion if so.
                    CameraProfileManager::getInstance().deleteActiveProfile();
                }

                // 4. Cycle Profiles key (e.g., Numpad 3)
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileCycleMask))
                {
                    logger.log(LOG_DEBUG, "CameraProfileThread: Cycle Profiles key press detected.");
                    CameraProfileManager::getInstance().cycleToNextProfile();
                }

                // 5. Reset to Default key (e.g., Numpad 5)
                if (isNewKeyPress(currentKeyState, previousKeyState, keyInfo.profileResetMask))
                {
                    logger.log(LOG_DEBUG, "CameraProfileThread: Reset to Default key press detected.");
                    CameraProfileManager::getInstance().resetToDefault();
                }

                // --- Continuous Adjustment Handling (Check current state, not just new presses) ---
                // Check adjustment keys only if they exist in the map (non-zero mask)
                if (keyInfo.offsetXIncMask && (currentKeyState & keyInfo.offsetXIncMask))
                    CameraProfileManager::getInstance().adjustOffset(adjustmentStep, 0.0f, 0.0f);
                if (keyInfo.offsetXDecMask && (currentKeyState & keyInfo.offsetXDecMask))
                    CameraProfileManager::getInstance().adjustOffset(-adjustmentStep, 0.0f, 0.0f);
                if (keyInfo.offsetYIncMask && (currentKeyState & keyInfo.offsetYIncMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, adjustmentStep, 0.0f);
                if (keyInfo.offsetYDecMask && (currentKeyState & keyInfo.offsetYDecMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, -adjustmentStep, 0.0f);
                if (keyInfo.offsetZIncMask && (currentKeyState & keyInfo.offsetZIncMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, 0.0f, adjustmentStep);
                if (keyInfo.offsetZDecMask && (currentKeyState & keyInfo.offsetZDecMask))
                    CameraProfileManager::getInstance().adjustOffset(0.0f, 0.0f, -adjustmentStep);

            } // end if(adjustment mode active)

            // Remember current key state for the next loop iteration
            previousKeyState = currentKeyState;
        }
        catch (const std::exception &e)
        {
            logger.log(LOG_ERROR, "CameraProfileThread: Exception caught: " + std::string(e.what()));
            Sleep(1000); // Prevent spamming errors
        }
        catch (...)
        {
            logger.log(LOG_ERROR, "CameraProfileThread: Caught unknown exception!");
            Sleep(1000);
        }
    } // end while (main loop)

    logger.log(LOG_INFO, "CameraProfileThread: Exiting");
    return 0;
}
