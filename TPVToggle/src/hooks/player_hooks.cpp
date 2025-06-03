/**
 * @file player_hooks.cpp
 * @brief Implementation of player head visibility hook.
 *
 * Intercepts the SetHeadVisibility function to force head visibility in First-Person View (FPV).
 */

#include <DetourModKit.hpp>
#include "player_hooks.hpp"
#include "constants.hpp"

#include <stdexcept>
#include <vector>

using namespace DetourModKit;
using namespace DetourModKit::String;
using namespace DetourModKit::Memory;
using namespace DetourModKit::Scanner;

// Forward declarations
bool initializeHeadVisibilityHook(uintptr_t moduleBase, size_t moduleSize);
void cleanupHeadVisibilityHook();

// Global state for the hook
// Function typedef for SetHeadVisibility (FUN_180aacf24)
typedef void(__fastcall *SetHeadVisibilityFunc)(uintptr_t entityPtr, bool showHead, int unknownFlags);
static SetHeadVisibilityFunc fpSetHeadVisibilityOriginal = nullptr;
static std::string g_setHeadVisibilityHookId;
static bool is_logged = false;

/**
 * @brief Detour for the SetHeadVisibility function (FUN_180aacf24).
 * Captures the original showHead flag and forces the head to be visible
 * when in First-Person View (FPV).
 *
 * @param entityPtr Pointer to the player entity or related structure (RCX).
 * @param originalShowHeadFlag The game's intended head visibility state (DL).
 * @param unknownFlags Additional flags passed to the original function (R8B).
 */
void __fastcall Detour_SetHeadVisibility(uintptr_t entityPtr, bool originalShowHeadFlag, int unknownFlags)
{
    DMKLogger &logger = Logger::getInstance();

    // Determine final head visibility flag
    bool finalShowHeadFlag = originalShowHeadFlag;

    if (finalShowHeadFlag)
    {
        logger.log(DMKLogLevel::LOG_TRACE, "SetHeadVisibilityHook: In FPV, game wanted to hide head. Forcing visible.");
    }
    finalShowHeadFlag = false; // Force head visibility

    // Call the original function with the modified flag
    if (fpSetHeadVisibilityOriginal)
    {
        logger.log(DMKLogLevel::LOG_TRACE, "Calling original with flag: " + std::to_string(finalShowHeadFlag));
        fpSetHeadVisibilityOriginal(entityPtr, finalShowHeadFlag, unknownFlags);

        // Check the flag value after the call
        if (isMemoryReadable(reinterpret_cast<void *>(entityPtr + 0xA38), sizeof(std::byte)))
        {
            uintptr_t flagAddress = entityPtr + 0xA38;

            if (!is_logged)
            {
                logger.log(DMKLogLevel::LOG_INFO, "Hide Head Flag address at " + format_address(flagAddress));
                is_logged = true;
            }

            std::byte flagValueAfterCall = *reinterpret_cast<std::byte *>(flagAddress);
            logger.log(DMKLogLevel::LOG_TRACE, "Flag value at " + format_address(flagAddress) +
                                                   " *after* original call: " + std::to_string(static_cast<int>(flagValueAfterCall)));
        }
    }
    else
    {
        logger.log(DMKLogLevel::LOG_ERROR, "SetHeadVisibilityHook: Original function pointer is NULL! Cannot call original.");
    }
}

/**
 * @brief Initializes the hook for the head visibility function.
 * @param moduleBase Base address of the game module.
 * @param moduleSize Size of the game module.
 * @return true if successful, false otherwise.
 */
bool initializeHeadVisibilityHook(uintptr_t moduleBase, size_t moduleSize)
{
    DMKLogger &logger = Logger::getInstance();
    DMKHookManager &hookManager = HookManager::getInstance();

    logger.log(DMKLogLevel::LOG_INFO, "HeadVisibilityHook: Initializing...");

    try
    {
        // Parse AOB pattern
        std::vector<std::byte> pattern = parseAOB(Constants::SET_HEAD_VISIBILITY_AOB_PATTERN);
        if (pattern.empty())
        {
            throw std::runtime_error("Failed to parse SetHeadVisibility AOB pattern.");
        }

        // Create the hook using DetourModKit
        g_setHeadVisibilityHookId = hookManager.create_inline_hook_aob(
            "SetHeadVisibility",
            moduleBase,
            moduleSize,
            Constants::SET_HEAD_VISIBILITY_AOB_PATTERN,
            0, // No offset needed as AOB points to function start
            reinterpret_cast<void *>(&Detour_SetHeadVisibility),
            reinterpret_cast<void **>(&fpSetHeadVisibilityOriginal),
            DMKHookConfig{.autoEnable = true});

        if (g_setHeadVisibilityHookId.empty())
        {
            throw std::runtime_error("Failed to create SetHeadVisibility hook.");
        }

        if (!fpSetHeadVisibilityOriginal)
        {
            hookManager.remove_hook(g_setHeadVisibilityHookId);
            throw std::runtime_error("SetHeadVisibility hook creation returned NULL trampoline.");
        }

        logger.log(DMKLogLevel::LOG_INFO, "HeadVisibilityHook: Successfully installed and enabled.");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "HeadVisibilityHook: Initialization failed: " + std::string(e.what()));
        cleanupHeadVisibilityHook();
        return false;
    }
}

/**
 * @brief Cleans up (removes) the head visibility hook.
 */
void cleanupHeadVisibilityHook()
{
    DMKLogger &logger = Logger::getInstance();
    DMKHookManager &hookManager = HookManager::getInstance();

    if (!g_setHeadVisibilityHookId.empty())
    {
        bool removed = hookManager.remove_hook(g_setHeadVisibilityHookId);
        if (removed)
        {
            logger.log(DMKLogLevel::LOG_INFO, "HeadVisibilityHook: Successfully removed.");
        }
        else
        {
            logger.log(DMKLogLevel::LOG_WARNING, "HeadVisibilityHook: Failed to remove hook.");
        }
        g_setHeadVisibilityHookId.clear();
        fpSetHeadVisibilityOriginal = nullptr;
        is_logged = false;
    }
}

/**
 * @brief Initializes all player-related hooks.
 * @param moduleBase Base address of the game module.
 * @param moduleSize Size of the game module.
 * @return true if all hooks are successfully initialized, false otherwise.
 */
bool initializePlayerHooks(uintptr_t moduleBase, size_t moduleSize)
{
    DMKLogger &logger = Logger::getInstance();
    bool all_success = true;

    if (!initializeHeadVisibilityHook(moduleBase, moduleSize))
    {
        logger.log(DMKLogLevel::LOG_WARNING, "Player Head Visibility Hook initialization failed - Show head feature disabled.");
        all_success = false;
    }

    return all_success;
}

/**
 * @brief Cleans up all player-related hooks.
 */
void cleanupPlayerHooks()
{
    cleanupHeadVisibilityHook();
}
