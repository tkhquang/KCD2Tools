#include "player_state_hook.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "config.h"
#include "aob_scanner.h"
#include "global_state.h"
#include "game_interface.h" // For getViewState()
#include "math_utils.h"     // For Vector3, Quaternion
#include "MinHook.h"

#include <vector>    // Include vector for GetAsyncKeyState check maybe
#include <windows.h> // For GetAsyncKeyState

// External config reference
// extern Config g_config;

// Define function pointer type based on Ghidra/RE analysis
// void FUN_18036059c(longlong param_1, undefined8 *param_2, undefined8 *param_3, longlong *param_4)
// RCX=playerComponent, RDX=destinationStatePtr, R8=sourceStatePtr, R9=physicsObjectPtr
typedef void(__fastcall *PlayerStateCopyFunc)(uintptr_t playerComponent, uintptr_t destinationStatePtr, uintptr_t sourceStatePtr, uintptr_t physicsObjectPtr);

// Trampoline pointer
PlayerStateCopyFunc fpPlayerStateCopyOriginal = nullptr;
static BYTE *g_playerStateHookAddress = nullptr;

Vector3 g_playerWorldPositionTest(0.0f, 0.0f, 0.0f);
Quaternion g_playerWorldOrientationTest = Quaternion::Identity();

uintptr_t g_sourceStatePtr = 0;

/**
 * @brief Detour for the game's player state copy function.
 *        This hook reads the player's live world position and orientation from the
 *        source state structure *after* the original copy operation completes.
 *        It also includes logic to override the player's rotation if TPV mode is active,
 *        aligning the player with the orbital camera or movement direction.
 *
 * @param playerComponent   Typically RCX, context for the state copy (e.g., player's entity component).
 * @param destinationStatePtr RDX, pointer to the structure where state is being copied TO.
 * @param sourceStatePtr    R8, pointer to the structure where state is being copied FROM (this usually has the live data).
 * @param physicsObjectPtr  R9, possibly a related physics object pointer or other context.
 */
void __fastcall Detour_PlayerStateCopy(uintptr_t playerComponent, uintptr_t destinationStatePtr, uintptr_t sourceStatePtr, uintptr_t physicsObjectPtr)
{
    Logger &logger = Logger::getInstance();

    // --- Safety Check for Original Function Pointer ---
    if (!fpPlayerStateCopyOriginal)
    {
        // This should ideally not happen if initialization was successful
        logger.log(LOG_ERROR, "PlayerStateHook: Original function pointer is NULL. Detour aborted.");
        return;
    }

    // Call the original game function FIRST
    try
    {
        fpPlayerStateCopyOriginal(playerComponent, destinationStatePtr, sourceStatePtr, physicsObjectPtr);
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: Exception calling original PlayerStateCopy: " + std::string(e.what()));
        return;
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: Unknown exception calling original PlayerStateCopy.");
        return;
    }

    // NOW, read from destinationStatePtr
    // IMPORTANT: The offsets for position and rotation within destinationStatePtr
    // should be the same as they were for sourceStatePtr, because this function
    // appears to be a direct copy of that structure.
    if (destinationStatePtr != 0)
    {
        Logger &logger = Logger::getInstance();
        logger.log(LOG_DEBUG, "PlayerStateHook: ENTERING READ. DestPtr=" + format_address(destinationStatePtr));

        // Try to read POSITION using individual float reads to match CE layout exactly for verification
        if (isMemoryReadable(reinterpret_cast<void *>(destinationStatePtr), 0x0C + sizeof(float)))
        { // Need up to Z_pos + its size
            float pX = *reinterpret_cast<float *>(destinationStatePtr + 0x00);
            float pY = *reinterpret_cast<float *>(destinationStatePtr + 0x08); // Offset 8 for Y from CE dump
            float pZ = *reinterpret_cast<float *>(destinationStatePtr + 0x0C); // Offset 12 (0xC) for Z from CE dump
            g_playerWorldPosition = Vector3(pX, pY, pZ);
            logger.log(LOG_DEBUG, "  PLAYER_POS_FROM_CE_OFFSETS: X=" + std::to_string(pX) +
                                      " Y=" + std::to_string(pY) + " Z=" + std::to_string(pZ));
        }
        else
        {
            logger.log(LOG_WARNING, "PlayerStateHook: Position part of destinationStatePtr not readable with CE offsets.");
        }

        // Try to read ROTATION using individual float reads to match CE layout for XYZW
        if (isMemoryReadable(reinterpret_cast<void *>(destinationStatePtr + 0x10), sizeof(Quaternion)))
        {                                                                      // Rot starts at 0x10 from base
            float qX = *reinterpret_cast<float *>(destinationStatePtr + 0x10); // RotX
            float qY = *reinterpret_cast<float *>(destinationStatePtr + 0x14); // RotY
            float qZ = *reinterpret_cast<float *>(destinationStatePtr + 0x18); // RotZ
            float qW = *reinterpret_cast<float *>(destinationStatePtr + 0x1C); // RotW
            g_playerWorldOrientation = Quaternion(qX, qY, qZ, qW);
            logger.log(LOG_DEBUG, "  PLAYER_ROT_FROM_CE_OFFSETS: X=" + std::to_string(qX) +
                                      " Y=" + std::to_string(qY) + " Z=" + std::to_string(qZ) +
                                      " W=" + std::to_string(qW));
            logger.log(LOG_DEBUG, "  Constructed g_PWO from CE Offsets: " + QuatToString(g_playerWorldOrientation));

            if (!GetPlayerWorldTransform(g_playerWorldPositionTest, g_playerWorldOrientationTest))
            {
                // Log error if needed, or globals will just keep their old values
            }
        }
        else
        {
            logger.log(LOG_WARNING, "PlayerStateHook: Rotation part of destinationStatePtr not readable with CE offsets.");
        }
    }

    // The player rotation override logic previously here has been moved to CameraProfileThread,
    // which will call CEntity::SetWorldTM. This hook is now primarily for READING the latest player state.
}

bool initializePlayerStateHook(uintptr_t moduleBase, size_t moduleSize)
{
    Logger &logger = Logger::getInstance();
    std::vector<BYTE> pattern = parseAOB(Constants::PLAYER_STATE_COPY_AOB_PATTERN);
    if (pattern.empty())
    {
        logger.log(LOG_ERROR, "PlayerStateHook: Failed to parse Player State Copy AOB pattern.");
        return false;
    }

    g_playerStateHookAddress = FindPattern(reinterpret_cast<BYTE *>(moduleBase), moduleSize, pattern);
    if (!g_playerStateHookAddress)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: Player State Copy pattern not found.");
        return false;
    }

    logger.log(LOG_INFO, "PlayerStateHook: Found Player State Copy function at " + format_address(reinterpret_cast<uintptr_t>(g_playerStateHookAddress)));

    MH_STATUS status = MH_CreateHook(g_playerStateHookAddress, reinterpret_cast<LPVOID>(&Detour_PlayerStateCopy), reinterpret_cast<LPVOID *>(&fpPlayerStateCopyOriginal));
    if (status != MH_OK)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: MH_CreateHook failed: " + std::string(MH_StatusToString(status)));
        return false;
    }
    if (!fpPlayerStateCopyOriginal)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: MH_CreateHook returned NULL trampoline.");
        MH_RemoveHook(g_playerStateHookAddress); // Clean up hook if trampoline failed
        return false;
    }

    status = MH_EnableHook(g_playerStateHookAddress);
    if (status != MH_OK)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: MH_EnableHook failed: " + std::string(MH_StatusToString(status)));
        MH_RemoveHook(g_playerStateHookAddress); // Clean up hook
        return false;
    }

    logger.log(LOG_INFO, "PlayerStateHook: Successfully installed.");
    return true;
}

void cleanupPlayerStateHook()
{
    Logger &logger = Logger::getInstance();
    if (g_playerStateHookAddress)
    {
        MH_DisableHook(g_playerStateHookAddress);
        MH_RemoveHook(g_playerStateHookAddress);
        g_playerStateHookAddress = nullptr;
        fpPlayerStateCopyOriginal = nullptr;
        logger.log(LOG_DEBUG, "PlayerStateHook: Hook cleaned up.");
    }
}
