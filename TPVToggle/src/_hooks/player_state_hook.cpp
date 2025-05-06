#include "player_state_hook.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h"
#include "game_interface.h" // For getViewState()
#include "math_utils.h"     // For Vector3, Quaternion
#include "MinHook.h"

#include <vector>    // Include vector for GetAsyncKeyState check maybe
#include <windows.h> // For GetAsyncKeyState

// Helper to log quaternion
std::string QuatToString(const Quaternion &q)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4)
        << "Q(" << q.x << ", " << q.y << ", " << q.z << ", " << q.w << ")";
    return oss.str();
}

// Define function pointer type based on Ghidra/RE analysis
// void FUN_18036059c(longlong param_1, undefined8 *param_2, undefined8 *param_3, longlong *param_4)
// RCX=playerComponent, RDX=destinationStatePtr, R8=sourceStatePtr, R9=physicsObjectPtr
typedef void(__fastcall *PlayerStateCopyFunc)(uintptr_t playerComponent, uintptr_t destinationStatePtr, uintptr_t sourceStatePtr, uintptr_t physicsObjectPtr);

// Trampoline pointer
PlayerStateCopyFunc fpPlayerStateCopyOriginal = nullptr;
static BYTE *g_playerStateHookAddress = nullptr;

// Detour function for Player State Copy (FUN_18036059c)
// Style: Post-Copy Override (Attempt 2 refinement)
void __fastcall Detour_PlayerStateCopy(uintptr_t playerComponent, uintptr_t destinationStatePtr, uintptr_t sourceStatePtr, uintptr_t physicsObjectPtr)
{
    Logger &logger = Logger::getInstance();

    // --- Original Function Pointer Check ---
    if (!fpPlayerStateCopyOriginal)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: Original function pointer is NULL! Cannot proceed.");
        return;
    }

    // --- Simple Rate Limiting for Logs ---
    static std::chrono::steady_clock::time_point last_log_time;
    bool enableDetailedLogging = false;
    auto now = std::chrono::steady_clock::now();
    if (logger.isDebugEnabled() && (now - last_log_time > std::chrono::milliseconds(100)))
    { // Log max ~10 times/sec in debug
        enableDetailedLogging = true;
        last_log_time = now;
    }

    // --- Argument Validation ---
    // Destination MUST be writable. Source only needs read (as we call original first).
    if (!isMemoryReadable((void *)sourceStatePtr, Constants::PLAYER_STATE_SIZE) ||
        !isMemoryWritable((void *)destinationStatePtr, Constants::PLAYER_STATE_SIZE))
    {
        // Log less frequently for memory errors
        static int mem_error_count = 0;
        if (++mem_error_count % 100 == 0)
        {
            logger.log(LOG_WARNING, "PlayerStateHook: Invalid src/dst memory. Calling original unmodified.");
        }
        fpPlayerStateCopyOriginal(playerComponent, destinationStatePtr, sourceStatePtr, physicsObjectPtr);
        return;
    }

    // --- Call Original Function FIRST ---
    // Let the game apply its calculated state (position + game's idea of rotation)
    try
    {
        fpPlayerStateCopyOriginal(playerComponent, destinationStatePtr, sourceStatePtr, physicsObjectPtr);
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: Exception calling original state copy func: " + std::string(e.what()));
        return; // Don't try to modify if original crashed
    }
    catch (...)
    {
        logger.log(LOG_ERROR, "PlayerStateHook: Unknown exception calling original state copy func.");
        return; // Don't try to modify if original crashed
    }

    // --- Get Current TPV State AFTER original call ---
    bool isTpv = (getViewState() == 1);

    // --- Apply Our Rotation Logic ONLY if in TPV ---
    if (isTpv)
    {
        if (enableDetailedLogging)
        {
            Quaternion originalDestRotation = *reinterpret_cast<Quaternion *>(destinationStatePtr + Constants::PLAYER_STATE_ROTATION_OFFSET);
            logger.log(LOG_DEBUG, "PlayerStateHook: TPV Active. Rotation AFTER original copy: " + QuatToString(originalDestRotation));
        }

        try
        {
            // Get camera forward vector (read from global updated by TPV Input hook)
            Vector3 camForward = g_latestTpvCameraForward; // Assuming this is normalized
            if (camForward.MagnitudeSquared() < 0.0001f)
            { // Safety check for zero vector
                if (enableDetailedLogging)
                    logger.log(LOG_WARNING, "PlayerStateHook: Camera forward vector is zero, cannot calculate rotation.");
                return; // Exit TPV logic if camForward is invalid
            }

            const Vector3 worldUp = {0.0f, 0.0f, 1.0f};
            Vector3 camRight = worldUp.Cross(camForward).Normalized(); // Z-up cross

            // Check movement input
            Vector3 targetMoveDir = {0.0f, 0.0f, 0.0f};
            bool isMoving = false;
            if (GetAsyncKeyState(0x57) & 0x8000)
            {
                targetMoveDir += camForward;
                isMoving = true;
            } // W
            if (GetAsyncKeyState(0x53) & 0x8000)
            {
                targetMoveDir -= camForward;
                isMoving = true;
            } // S
            if (GetAsyncKeyState(0x41) & 0x8000)
            {
                targetMoveDir -= camRight;
                isMoving = true;
            } // A
            if (GetAsyncKeyState(0x44) & 0x8000)
            {
                targetMoveDir += camRight;
                isMoving = true;
            } // D

            Quaternion rotationToApply;
            bool applyRotation = false; // Flag to determine if we overwrite

            if (isMoving && targetMoveDir.MagnitudeSquared() > 0.0001f)
            {
                // Player is pressing movement keys - align player to MOVEMENT direction
                targetMoveDir.Normalize();
                rotationToApply = Quaternion::LookRotation(targetMoveDir, worldUp);
                applyRotation = true;

                if (enableDetailedLogging)
                {
                    logger.log(LOG_DEBUG, "PlayerStateHook: MOVING. Applying MoveDir Rotation: " + QuatToString(rotationToApply));
                }
            }
            else
            {
                // Player is IDLE (no W/A/S/D pressed) - align player to CAMERA direction (for aiming)
                rotationToApply = Quaternion::LookRotation(camForward, worldUp);
                applyRotation = true; // Apply this alignment even when idle

                if (enableDetailedLogging)
                {
                    logger.log(LOG_DEBUG, "PlayerStateHook: IDLE. Applying CamFwd Rotation: " + QuatToString(rotationToApply));
                }
            }

            // Perform the overwrite if we decided to apply a rotation
            if (applyRotation)
            {
                // Get pointer to destination rotation
                Quaternion *destQuatPtr = reinterpret_cast<Quaternion *>(destinationStatePtr + Constants::PLAYER_STATE_ROTATION_OFFSET);

                // Write our calculated rotation (using default assignment operator)
                *destQuatPtr = rotationToApply;

                // Optional: Logging to verify the write immediately after
                if (enableDetailedLogging)
                {
                    Quaternion writtenQuat = *destQuatPtr; // Read back immediately
                    logger.log(LOG_DEBUG, "PlayerStateHook: Wrote rotation. Value in memory now: " + QuatToString(writtenQuat));
                    if (writtenQuat != rotationToApply)
                    { // Use the float-tolerant comparison
                        logger.log(LOG_WARNING, "PlayerStateHook: Post-write verification failed! Memory value differs from target rotation.");
                    }
                }
            }
            // No else needed - if !applyRotation, we simply leave the rotation written by the original function
        }
        catch (const std::exception &e)
        {
            logger.log(LOG_ERROR, "PlayerStateHook: Exception during TPV state override: " + std::string(e.what()));
        }
        catch (...)
        {
            logger.log(LOG_ERROR, "PlayerStateHook: Unknown exception during TPV state override.");
        }
    } // End if(isTpv)

    // Function finished
} // End Detour_PlayerStateCopy

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
