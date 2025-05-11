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
extern Config g_config;

// Define function pointer type based on Ghidra/RE analysis
// void FUN_18036059c(longlong param_1, undefined8 *param_2, undefined8 *param_3, longlong *param_4)
// RCX=playerComponent, RDX=destinationStatePtr, R8=sourceStatePtr, R9=physicsObjectPtr
typedef void(__fastcall *PlayerStateCopyFunc)(uintptr_t playerComponent, uintptr_t destinationStatePtr, uintptr_t sourceStatePtr, uintptr_t physicsObjectPtr);

// Trampoline pointer
PlayerStateCopyFunc fpPlayerStateCopyOriginal = nullptr;
static BYTE *g_playerStateHookAddress = nullptr;

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
        logger.log(LOG_ERROR, "PlayerStateHook: Original function pointer (fpPlayerStateCopyOriginal) is NULL! Detour cannot proceed.");
        // Depending on the function's nature, you might want to do nothing or attempt to call what you think it is.
        // For a state copy, doing nothing if the original can't be called is safest.
        return;
    }

    // --- Call the Original Game Function FIRST ---
    // Let the game perform its standard state copy. We'll read from 'sourceStatePtr' or modify 'destinationStatePtr' afterwards.
    try
    {
        fpPlayerStateCopyOriginal(playerComponent, destinationStatePtr, sourceStatePtr, physicsObjectPtr);
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: Exception occurred while calling original PlayerStateCopy function: " + std::string(e.what()));
        return; // Do not proceed if the original function crashes
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: Unknown exception occurred while calling original PlayerStateCopy function.");
        return; // Do not proceed
    }

    // --- Read Live Player State from sourceStatePtr ---
    // We read from sourceStatePtr because it's more likely to represent the "true" current state
    // *before* any potential modifications our hook might make to destinationStatePtr.
    // The PLAYER_STATE_SIZE constant should cover both position and rotation reads.
    if (isMemoryReadable(reinterpret_cast<void *>(sourceStatePtr), Constants::PLAYER_STATE_SIZE))
    {
        // Read position
        g_playerWorldPosition = *reinterpret_cast<Vector3 *>(sourceStatePtr + Constants::PLAYER_STATE_POSITION_OFFSET);

        // Read orientation
        g_playerWorldOrientation = *reinterpret_cast<Quaternion *>(sourceStatePtr + Constants::PLAYER_STATE_ROTATION_OFFSET);

        // Optional: Log the read player state (can be very spammy)
        if (logger.isDebugEnabled())
        {
            static int player_state_log_count = 0;
            if (++player_state_log_count % 300 == 0)
            { // Log approx every 5 seconds if 60fps
              // logger.log(LOG_DEBUG, "PlayerStateHook: Updated g_playerWorldPosition: " + Vector3ToString(g_playerWorldPosition) +
              //                           ", g_playerWorldOrientation: " + QuatToString(g_playerWorldOrientation));
            }
        }
    }
    else
    {
        // Log warning if source state isn't readable, less frequently.
        static int read_err_count = 0;
        if (++read_err_count % 100 == 0 && logger.isDebugEnabled())
        {
            logger.log(LOG_WARNING, "PlayerStateHook: sourceStatePtr at " + format_address(sourceStatePtr) + " is not readable. Cannot update player globals.");
        }
        // If we can't read the source, we probably shouldn't try to modify the destination based on stale/invalid data.
        return;
    }

    if (g_sourceStatePtr == 0)
    {

        logger.log(LOG_INFO, "PlayerStateHook: sourceStatePtr at " + format_address(sourceStatePtr));
    }

    g_sourceStatePtr = sourceStatePtr;

    // --- Apply Custom Player Rotation Logic (Decoupling from Camera) ---
    // This part only runs if TPV is active, and aims to make the player character
    // face the movement direction (if moving) or the camera's view direction (if idle).
    bool isTpvCurrently = (getViewState() == 1);
    if (isTpvCurrently &&
        isMemoryWritable(reinterpret_cast<void *>(destinationStatePtr), Constants::PLAYER_STATE_SIZE)) // Ensure destination is writable for our override
    {
        Vector3 cameraViewDirectionForPlayerOrientation; // The direction player should face or use for movement calcs

        if (g_config.enable_orbital_camera_mode && g_orbitalModeActive.load())
        {
            cameraViewDirectionForPlayerOrientation = g_orbitalCameraRotation.Rotate(Vector3(0.0f, 1.0f, 0.0f));
            logger.log(LOG_DEBUG, "PlayerStateCopy (ORBITAL): Using g_orbitalCameraRotation. Fwd=" + Vector3ToString(cameraViewDirectionForPlayerOrientation));
        }
        else
        {
            cameraViewDirectionForPlayerOrientation = g_latestTpvCameraForward;
            logger.log(LOG_DEBUG, "PlayerStateCopy (STANDARD): Using g_latestTpvCameraForward. Fwd=" + Vector3ToString(cameraViewDirectionForPlayerOrientation));
        }

        // Ensure the calculated direction is valid before using it
        if (cameraViewDirectionForPlayerOrientation.MagnitudeSquared() < 0.0001f)
        {
            // Log warning if the reference camera direction is invalid.
            static int cam_dir_warn_count = 0;
            if (++cam_dir_warn_count % 100 == 0 && logger.isDebugEnabled())
            {
                logger.log(LOG_WARNING, "PlayerStateHook: Reference cameraViewDirection is near zero. Skipping player rotation override.");
            }
            return; // Can't orient player if camera direction is undefined
        }
        cameraViewDirectionForPlayerOrientation.Normalize(); // Normalize for safety in LookRotation

        // Define world UP vector (adjust if KCD2 uses Y-up)
        const Vector3 worldUp = {0.0f, 0.0f, 1.0f};

        // Determine player's desired movement direction based on WASD and camera's orientation
        Vector3 cameraRightForMovement = worldUp.Cross(cameraViewDirectionForPlayerOrientation).Normalized();
        Vector3 targetMoveDir = {0.0f, 0.0f, 0.0f};
        bool isPlayerAttemptingToMove = false;

        if (GetAsyncKeyState(0x57) & 0x8000)
        {
            targetMoveDir += cameraViewDirectionForPlayerOrientation;
            isPlayerAttemptingToMove = true;
        } // W - Forward
        if (GetAsyncKeyState(0x53) & 0x8000)
        {
            targetMoveDir -= cameraViewDirectionForPlayerOrientation;
            isPlayerAttemptingToMove = true;
        } // S - Backward
        if (GetAsyncKeyState(0x41) & 0x8000)
        {
            targetMoveDir -= cameraRightForMovement;
            isPlayerAttemptingToMove = true;
        } // A - Left Strafe
        if (GetAsyncKeyState(0x44) & 0x8000)
        {
            targetMoveDir += cameraRightForMovement;
            isPlayerAttemptingToMove = true;
        } // D - Right Strafe

        Quaternion playerRotationToApply;
        bool shouldOverridePlayerRotation = false;

        if (isPlayerAttemptingToMove && targetMoveDir.MagnitudeSquared() > 0.0001f)
        {
            // Player is pressing movement keys: Orient player to the net movement direction
            targetMoveDir.Normalize();
            playerRotationToApply = Quaternion::LookRotation(targetMoveDir, worldUp);
            shouldOverridePlayerRotation = true;

            // Optional: Log movement-based rotation
            // if (logger.isDebugEnabled()) { /* ... */ }
        }
        else // Player is IDLE (no W/A/S/D pressed, or net movement is zero)
        {
            // Player is IDLE: Orient player to face where the relevant camera is looking
            playerRotationToApply = Quaternion::LookRotation(cameraViewDirectionForPlayerOrientation, worldUp);
            shouldOverridePlayerRotation = true;

            // Optional: Log idle/camera-facing rotation
            // if (logger.isDebugEnabled()) { /* ... */ }
        }

        // If we decided to override, write the new rotation to the DESTINATION state buffer.
        // The game will then use this modified state from destinationStatePtr.
        // In Detour_PlayerStateCopy, after writing to destinationStatePtr:
        if (shouldOverridePlayerRotation)
        {
            // Quaternion *destPlayerRotPtr = reinterpret_cast<Quaternion *>(destinationStatePtr + Constants::PLAYER_STATE_ROTATION_OFFSET);
            // *destPlayerRotPtr = playerRotationToApply;

            // // AGGRESSIVE: Also write to sourceStatePtr if it's different and writable
            // // This is to try and stop other systems from using a stale game-calculated rotation
            // // from sourceStatePtr after our hook runs.
            // if (isMemoryWritable(reinterpret_cast<void *>(sourceStatePtr), Constants::PLAYER_STATE_SIZE))
            // {
            //     Quaternion *srcPlayerRotPtr = reinterpret_cast<Quaternion *>(sourceStatePtr + Constants::PLAYER_STATE_ROTATION_OFFSET);
            //     if (*srcPlayerRotPtr != playerRotationToApply)
            //     { // Use your float-tolerant !=
            //         *srcPlayerRotPtr = playerRotationToApply;
            //         logger.log(LOG_DEBUG, "PlayerStateHook: AGGRESSIVELY Overrode sourceStatePtr rotation.");
            //     }
            // }
            logger.log(LOG_DEBUG, "PlayerStateCopy (ORBITAL): Player rotation override SKIPPED for testing.");
        }
    } // End if (isTpvCurrently && isMemoryWritable)
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
