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
    DMK::HookManager &hookManager = DMK::HookManager::getInstance();
    logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: Initializing...");

    // Resolve pGame pointer address
    GlobalState::g_pGame_ptr_address = moduleBase + Rvas::DAT_1854b2330_pGame_RVA;
    logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: pGame pointer storage (DAT_1854b2330_pGame) resolved to " + DMKString::format_address(GlobalState::g_pGame_ptr_address));

    // Optional: Check if the memory for g_pGame_ptr_address itself is readable, which it should be if moduleBase is correct.
    if (!DMKMemory::isMemoryReadable(reinterpret_cast<void *>(GlobalState::g_pGame_ptr_address), sizeof(uintptr_t)))
    {
        logger.log(DMKLogLevel::LOG_ERROR, "CoreHooks: pGame pointer storage at " + DMKString::format_address(GlobalState::g_pGame_ptr_address) + " is NOT readable. This is a critical issue.");
        return false; // Cannot proceed if we can't even read where pGame is stored.
    }

    // Check if the pointer at g_pGame_ptr_address is initially valid (optional early check)
    // This checks if the game has initialized pGame yet.
    if (*reinterpret_cast<uintptr_t *>(GlobalState::g_pGame_ptr_address) == 0)
    {
        logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: pGame pointer storage at " + DMKString::format_address(GlobalState::g_pGame_ptr_address) + " currently holds a NULL pGame pointer. Waiting for it to be populated by the game.");
    }

    // Loop indefinitely until CPlayer (LocalPlayerEntity) is found
    logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: Attempting to find local player entity (will retry indefinitely)...");
    const int delay_ms = 1000; // 1000ms (1 second) delay between attempts

    GlobalState::g_localPlayerEntity = 0; // Ensure it's initialized to 0

    while (GlobalState::g_localPlayerEntity == 0) // Loop as long as player is not found
    {
        GlobalState::g_localPlayerEntity = GetLocalPlayerEntity();
        if (GlobalState::g_localPlayerEntity != 0)
        {
            // g_logged_pPlayer_address is handled by GetLocalPlayerEntity itself upon first successful retrieval.
            // So, no explicit log here, GetLocalPlayerEntity will log it.
            break; // Exit loop once player is found
        }

        // Log only if player is still not found before sleeping
        logger.log(DMKLogLevel::LOG_INFO, "CoreHooks: Player entity not found yet. Waiting " + std::to_string(delay_ms) + "ms before retrying...");
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
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
