#include <DetourModKit.hpp>

#include <windows.h>
#include <thread>
#include <chrono>

#include "global_state.hpp"

bool g_logged_pPlayer_address = false; // For logging pPlayer address once

namespace Rvas
{
    // RVA to the global pointer that holds the address of the 'Game' object/interface
    const uintptr_t DAT_1854b2330_pGame_RVA = 0x54b2330;
} // namespace Rvas

uintptr_t GetLocalPlayerEntity();

bool initializeCoreHooks(uintptr_t moduleBase, size_t moduleSize)
{
    DMKLogger &logger = DMKLogger::getInstance();
    logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: Initializing...");

    GlobalState::g_ModuleBase = moduleBase; // Store these early
    GlobalState::g_ModuleSize = moduleSize;

    // Resolve pGame pointer address
    GlobalState::g_pGame_ptr_address = moduleBase + Rvas::DAT_1854b2330_pGame_RVA;
    logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: pGame pointer storage (DAT_1854b2330_pGame) resolved to " + DMKString::format_address(GlobalState::g_pGame_ptr_address));

    // Wait for g_pGame_ptr_address to be populated if necessary
    const int pGame_poll_delay_ms = 200;
    int pGame_polls = 0;
    while (*reinterpret_cast<uintptr_t *>(GlobalState::g_pGame_ptr_address) == 0)
    {
        if (pGame_polls == 0)
        {
            logger.log(DMKLogLevel::LOG_TRACE, "CoreHooks: pGame pointer storage at " + DMKString::format_address(GlobalState::g_pGame_ptr_address) + " currently holds a NULL pGame pointer. Waiting for it to be populated by the game.");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pGame_poll_delay_ms));
        pGame_polls++;
    }
    if (*reinterpret_cast<uintptr_t *>(GlobalState::g_pGame_ptr_address) == 0)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "CoreHooks: pGame pointer not populated after timeout. Cannot find local player.");
        return false; // Critical failure
    }
    logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: pGame pointer is now valid: " + DMKString::format_address(*(uintptr_t *)GlobalState::g_pGame_ptr_address));

    logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: Attempting to find local player entity...");
    const int player_poll_delay_ms = 500;
    // Indefinite loop as before - or add a max retry for player entity too if preferred
    GlobalState::g_localPlayerEntity = 0;
    while (GlobalState::g_localPlayerEntity == 0)
    {
        GlobalState::g_localPlayerEntity = GetLocalPlayerEntity(); // This function already uses pGame, now valid
        if (GlobalState::g_localPlayerEntity != 0)
        {
            break;
        }
        logger.log(DMKLogLevel::LOG_TRACE, "CoreHooks: Player entity not found yet. Waiting " + std::to_string(player_poll_delay_ms) + "ms before retrying...");
        std::this_thread::sleep_for(std::chrono::milliseconds(player_poll_delay_ms));
    }

    // Once player is found, try to initialize animation pointers with retries
    if (GlobalState::g_localPlayerEntity != 0)
    {
    }
    else
    {
        logger.log(DMKLogLevel::LOG_ERROR, "CoreHooks: CRITICAL - Could not find local player entity. TPV cannot initialize.");
        return false; // Definitely fatal if player is needed
    }

    logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: Initialization sequence finished.");
    return true;
}

void cleanupCoreHooks()
{
    DMKLogger &logger = DMKLogger::getInstance();
    DMK::HookManager &hookManager = DMK::HookManager::getInstance();

    GlobalState::g_ISystem = 0;
    // g_pGame_ptr_address remains valid, it's just an offset, no need to clear unless the module unloads.
    logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: Cleanup complete.");
}

uintptr_t GetLocalPlayerEntity()
{
    DMKLogger &logger = DMKLogger::getInstance();

    if (!GlobalState::g_pGame_ptr_address || !*(uintptr_t *)GlobalState::g_pGame_ptr_address)
        return 0;
    uintptr_t pGame = *(uintptr_t *)GlobalState::g_pGame_ptr_address;
    if (!pGame)
        return 0;
    uintptr_t pClientActor = (*(uintptr_t(__fastcall **)(uintptr_t))(*(uintptr_t *)pGame + 0x80))(pGame);
    if (!pClientActor)
        return 0;
    uintptr_t pPlayer = (*(uintptr_t(__fastcall **)(uintptr_t))(*(uintptr_t *)pClientActor + 0x200))(pClientActor);

    if (!g_logged_pPlayer_address && pPlayer)
    {
        logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: GetLocalPlayerEntity: pPlayer is at " + DMKString::format_address(pPlayer));
        g_logged_pPlayer_address = true;
    }
    return pPlayer;
}
