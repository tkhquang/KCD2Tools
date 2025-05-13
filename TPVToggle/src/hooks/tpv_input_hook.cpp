#include "tpv_input_hook.h"
#include "logger.h"
#include "constants.h"    // For TPV_INPUT_PROCESS_AOB_PATTERN and input event offsets
#include "utils.h"        // For format_address, isMemoryReadable
#include "aob_scanner.h"  // For FindPattern, parseAOB
#include "global_state.h" // For g_orbitalModeActive, g_orbitalCameraYaw, etc.
#include "config.h"       // For g_config (sensitivities, inversion settings)
#include "math_utils.h"   // For Quaternion, Vector3, M_PIDIV2
#include "MinHook.h"      // For MinHook functions

#include <DirectXMath.h> // For XMVECTOR, quaternion math
#include <algorithm>     // For std::max, std::min
#include <math.h>

// Trampoline for the original TPV camera input function
static TpvCameraInputFunc fpTpvCameraInputOriginal = nullptr;
// Address of the hooked function
static BYTE *g_tpvInputHookAddress = nullptr;

// Reference to global configuration
extern Config g_config;

// Define mouse event IDs based on RE findings. These were in your snippet:
constexpr int MOUSE_EVENT_ID_TPV_YAW = 0x10A;   // Example value, adjust if different from global
constexpr int MOUSE_EVENT_ID_TPV_PITCH = 0x10B; // Example value
constexpr int MOUSE_EVENT_ID_TPV_ZOOM = 0x10C;  // Example value

// Input event structure offsets - these should match what FUN_183924908 expects
constexpr ptrdiff_t INPUT_EVENT_CHECK_BYTE0 = 0x00; // Expected: 0x01 for this event type
constexpr ptrdiff_t INPUT_EVENT_CHECK_INT4 = 0x04;  // Expected: 0x08 for this event type
constexpr ptrdiff_t INPUT_EVENT_ID_OFFSET = 0x10;
constexpr ptrdiff_t INPUT_EVENT_DELTA_OFFSET = 0x18;

// Helper function for atomic float addition
inline void atomic_add_float(std::atomic<float> &atom, float val)
{
    float old_val = atom.load(std::memory_order_relaxed);
    float new_val;
    do
    {
        new_val = old_val + val;
    } while (!atom.compare_exchange_weak(old_val, new_val, std::memory_order_release, std::memory_order_relaxed));
}

void update_orbital_camera_rotation_from_euler()
{
    std::lock_guard<std::mutex> lock(g_orbitalCameraMutex);

    Logger &logger = Logger::getInstance();

    float currentPitchRad = g_orbitalCameraPitch.load(std::memory_order_relaxed);
    float currentPitchDeg = currentPitchRad * (180.0f / static_cast<float>(M_PI));
    float pitchMinDeg_float = static_cast<float>(g_config.orbit_pitch_min_degrees);
    float pitchMaxDeg_float = static_cast<float>(g_config.orbit_pitch_max_degrees);

    float pitchClampedDeg = std::max(pitchMinDeg_float, std::min(currentPitchDeg, pitchMaxDeg_float));
    g_orbitalCameraPitch.store(pitchClampedDeg * (static_cast<float>(M_PI) / 180.0f), std::memory_order_relaxed);

    DirectX::XMVECTOR qYaw = DirectX::XMQuaternionRotationNormal(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), g_orbitalCameraYaw.load(std::memory_order_relaxed));
    DirectX::XMVECTOR qPitch = DirectX::XMQuaternionRotationNormal(DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), g_orbitalCameraPitch.load(std::memory_order_relaxed));

    DirectX::XMVECTOR finalRotationXm = DirectX::XMQuaternionMultiply(qPitch, qYaw);
    g_orbitalCameraRotation = Quaternion::FromXMVector(DirectX::XMQuaternionNormalize(finalRotationXm));

    // Conditional logging for debugging (reduce frequency)
    // static int log_count_euler = 0;
    // if (logger.isDebugEnabled() && (++log_count_euler % 60 == 0)) {
    //     logger.log(LOG_DEBUG, "UpdateEuler: YawRad=" + std::to_string(g_orbitalCameraYaw.load()) +
    //                           " PitchRadClamped=" + std::to_string(g_orbitalCameraPitch.load()) + " (~" + std::to_string(pitchClampedDeg) + " deg) " +
    //                           " FinalQuat=" + QuatToString(g_orbitalCameraRotation));
    // }
}

void __fastcall Detour_TpvCameraInput(uintptr_t thisPtr, char *inputEventPtr)
{
    Logger &logger = Logger::getInstance();
    bool orbitalCurrentlyActive = g_orbitalModeActive.load(std::memory_order_relaxed);

    if (g_nativeOrbitCVarsEnabled.load(std::memory_order_relaxed))
    { // Native CVar test mode
        if (fpTpvCameraInputOriginal)
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
        return;
    }

    if (!isMemoryReadable(inputEventPtr, INPUT_EVENT_DELTA_OFFSET + sizeof(float)))
    {
        if (fpTpvCameraInputOriginal)
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
        return;
    }

    unsigned char eventByte0 = *reinterpret_cast<unsigned char *>(inputEventPtr + INPUT_EVENT_CHECK_BYTE0);
    int eventInt4 = *reinterpret_cast<int *>(inputEventPtr + INPUT_EVENT_CHECK_INT4);
    int eventID = *reinterpret_cast<int *>(inputEventPtr + INPUT_EVENT_ID_OFFSET);
    float deltaFromEvent = *reinterpret_cast<float *>(inputEventPtr + INPUT_EVENT_DELTA_OFFSET);

    // --- Start: Combined Logging and Hijacking Logic for Orbital Mode ---
    if (g_config.enable_orbital_camera_mode && orbitalCurrentlyActive)
    {
        // Check if it's the specific TPV mouse event type
        if (eventByte0 == 0x01 && eventInt4 == 0x08) // VERIFY THIS PRE-FILTER FOR KCD2 MOUSE LOOK
        {
            bool inputWasHijacked = false;
            bool eulerAnglesNeedUpdate = false;

            // Log the event *before* deciding to hijack
            // static int pre_hijack_log_count = 0;
            // if (logger.isDebugEnabled() && (++pre_hijack_log_count % 10 == 0) ) { // Log less
            //     logger.log(LOG_DEBUG, "TPV_INPUT_ORBITAL_CHECK: EventID=" + format_hex(eventID) + " Delta=" + std::to_string(deltaFromEvent));
            // }

            switch (eventID)
            {
            case MOUSE_EVENT_ID_TPV_YAW: // Use your VERIFIED ID for KCD2
                // logger.log(LOG_DEBUG, "DETECTED KCD2 YAW EVENT: ID=" + format_hex(eventID) + " Delta=" + std::to_string(deltaFromEvent));

                if (std::abs(deltaFromEvent) > 1e-5f)
                {
                    atomic_add_float(g_orbitalCameraYaw, deltaFromEvent * g_config.orbit_sensitivity_yaw);
                    eulerAnglesNeedUpdate = true;
                }
                inputWasHijacked = true;
                break;
            case MOUSE_EVENT_ID_TPV_PITCH: // Use your VERIFIED ID for KCD2
                // logger.log(LOG_DEBUG, "DETECTED KCD2 PITCH EVENT: ID=" + format_hex(eventID) + " Delta=" + std::to_string(deltaFromEvent));
                if (std::abs(deltaFromEvent) > 1e-5f)
                {
                    float pitchAmount = deltaFromEvent * g_config.orbit_sensitivity_pitch;
                    if (g_config.orbit_invert_pitch)
                        pitchAmount = -pitchAmount;
                    atomic_add_float(g_orbitalCameraPitch, -pitchAmount);
                    eulerAnglesNeedUpdate = true;
                }
                inputWasHijacked = true;
                break;
            case MOUSE_EVENT_ID_TPV_ZOOM: // Use your VERIFIED ID for KCD2
                // logger.log(LOG_DEBUG, "DETECTED KCD2 ZOOM EVENT: ID=" + format_hex(eventID) + " Delta=" + std::to_string(deltaFromEvent));
                if (std::abs(deltaFromEvent) > 1e-5f)
                {
                    float currentDist = g_orbitalCameraDistance.load(std::memory_order_relaxed);
                    float newDist = currentDist - (deltaFromEvent * g_config.orbit_zoom_sensitivity);
                    newDist = std::max(static_cast<float>(g_config.orbit_min_distance), std::min(newDist, static_cast<float>(g_config.orbit_max_distance)));
                    g_orbitalCameraDistance.store(newDist, std::memory_order_relaxed);
                }
                inputWasHijacked = true;
                break;
            default:
                // Not a mouse look/zoom event we explicitly handle for orbital mode.
                // Let it fall through to call original.
                break;
            }

            if (inputWasHijacked)
            {
                if (eulerAnglesNeedUpdate)
                {
                    update_orbital_camera_rotation_from_euler();
                }
                // static int hijack_log_count = 0;
                // if (logger.isDebugEnabled() && (++hijack_log_count % 30 == 0) ) { // Log less
                //     logger.log(LOG_DEBUG, "TPVInputHook (Orbital Active): HIJACKED EventID=" + format_hex(eventID) + ". New Yaw=" + std::to_string(g_orbitalCameraYaw.load()) + " Pitch=" + std::to_string(g_orbitalCameraPitch.load()));
                // }
                // logger.log(LOG_INFO, "ORBITAL_INPUT_HIJACK: EventID=" + format_hex(eventID) + " WAS HIJACKED.");
                return; // CRITICAL: Suppress original game function call for these hijacked events.
            }
        }
        // If orbital mode is active, but the event was not one we hijack (e.g. controller input if it uses same path, or different eventByte0/Int4)
        // then we should still call the original to let the game handle those other inputs.
        if (fpTpvCameraInputOriginal)
        {
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
        }
        else
        {
            logger.log(LOG_ERROR, "TPVInputHook: Original function pointer NULL! Cannot process unhijacked event in orbital mode.");
        }
        // No g_latestTpvCameraForward update here if orbital is active, Detour_TpvCameraUpdate will set it based on orbital cam.
        return; // End of orbital mode specific path
    }
    // --- End: Orbital Mode Logic ---

    // --- Standard Mode (Orbital Disabled/Not Active) or Fallback Logging ---
    // If orbital mode is OFF, OR if it's ON but the event wasn't of the specific type we check (eventByte0/Int4):

    // General Logging for any TPV input when not in full orbital hijack
    // (This includes the very first logs you had, but now after orbital hijack attempt)
    // static int passthrough_log_count = 0;
    // if (logger.isDebugEnabled() && (++passthrough_log_count % (orbitalCurrentlyActive ? 200:30) == 0) ) { // Log less if orbital on but not hijacking this event
    //      logger.log(LOG_DEBUG, "TPV_INPUT_PASSTHROUGH (Orbital " + std::string(orbitalCurrentlyActive?"ON":"OFF") +
    //                           "): Byte0=0x" + format_hex(eventByte0) +
    //                           " Int4=0x" + format_hex(eventInt4) +
    //                           " EventID=" + format_hex(eventID) +
    //                           " Delta=" + std::to_string(deltaFromEvent) );
    // }

    if (fpTpvCameraInputOriginal)
    {
        fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
    }
    else
    {
        logger.log(LOG_ERROR, "TPVInputHook: Original function pointer NULL! Cannot process event in standard mode.");
        return;
    }

    // Update g_latestTpvCameraForward based on game's TPV camera state *only if orbital is NOT active*
    if (!orbitalCurrentlyActive)
    {
        if (thisPtr != 0 && isMemoryReadable(reinterpret_cast<void *>(thisPtr + Constants::TPV_CAMERA_QUATERNION_OFFSET), sizeof(Quaternion)))
        {
            Quaternion gameTpvCamRot = *reinterpret_cast<Quaternion *>(thisPtr + Constants::TPV_CAMERA_QUATERNION_OFFSET);
            // Assuming game's TPV camera Y-axis is forward, Z-axis is up for rotation basis.
            Vector3 forwardVec = gameTpvCamRot.Rotate(Vector3(0.0f, 1.0f, 0.0f));
            g_latestTpvCameraForward = forwardVec.Normalized();
        }
    }
}

/**
 * @brief Initializes the TPV camera input hook by finding the target function via AOB scan
 *        and then creating and enabling a MinHook detour.
 *
 * @param moduleBase Base address of the game's main module (e.g., WHGame.dll).
 * @param moduleSize Size of the game's main module.
 * @return true if initialization was successful, false otherwise.
 */
bool initializeTpvInputHook(uintptr_t moduleBase, size_t moduleSize)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "TPVInputHook: Initializing for TPV camera input processing...");

    if (!g_config.enable_orbital_camera_mode && !g_config.enable_camera_profiles)
    { // Or a more specific check if this hook is ONLY for orbital
        logger.log(LOG_INFO, "TPVInputHook: Orbital camera mode and profiles are disabled in config, skipping hook initialization.");
        return true; // Not an error, just not needed.
    }

    std::vector<BYTE> pattern = parseAOB(Constants::TPV_INPUT_PROCESS_AOB_PATTERN);
    if (pattern.empty())
    {
        logger.log(LOG_ERROR, "TPVInputHook: Failed to parse AOB pattern for TPV input function.");
        return false;
    }

    g_tpvInputHookAddress = FindPattern(reinterpret_cast<BYTE *>(moduleBase), moduleSize, pattern);
    if (!g_tpvInputHookAddress)
    {
        logger.log(LOG_ERROR, "TPVInputHook: AOB pattern for TPV input function (FUN_183924908) not found.");
        return false;
    }
    logger.log(LOG_INFO, "TPVInputHook: Found TPV input function at " + format_address(reinterpret_cast<uintptr_t>(g_tpvInputHookAddress)));

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<LPVOID>(g_tpvInputHookAddress),
        reinterpret_cast<LPVOID>(&Detour_TpvCameraInput),
        reinterpret_cast<LPVOID *>(&fpTpvCameraInputOriginal));

    if (status != MH_OK)
    {
        logger.log(LOG_ERROR, "TPVInputHook: MH_CreateHook failed: " + std::string(MH_StatusToString(status)));
        return false;
    }

    if (!fpTpvCameraInputOriginal)
    {
        logger.log(LOG_ERROR, "TPVInputHook: MH_CreateHook succeeded but trampoline (original function pointer) is NULL.");
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_tpvInputHookAddress)); // Clean up the failed hook
        return false;
    }

    status = MH_EnableHook(reinterpret_cast<LPVOID>(g_tpvInputHookAddress));
    if (status != MH_OK)
    {
        logger.log(LOG_ERROR, "TPVInputHook: MH_EnableHook failed: " + std::string(MH_StatusToString(status)));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_tpvInputHookAddress)); // Clean up
        return false;
    }

    logger.log(LOG_INFO, "TPVInputHook: TPV camera input hook successfully installed and enabled.");
    return true;
}

/**
 * @brief Cleans up by disabling and removing the TPV camera input hook.
 */
void cleanupTpvInputHook()
{
    Logger &logger = Logger::getInstance();
    if (g_tpvInputHookAddress != nullptr) // Check if it was ever initialized
    {
        MH_STATUS disableStatus = MH_DisableHook(reinterpret_cast<LPVOID>(g_tpvInputHookAddress));
        MH_STATUS removeStatus = MH_RemoveHook(reinterpret_cast<LPVOID>(g_tpvInputHookAddress));

        if (disableStatus == MH_OK && removeStatus == MH_OK)
        {
            logger.log(LOG_INFO, "TPVInputHook: Successfully disabled and removed.");
        }
        else
        {
            logger.log(LOG_WARNING, "TPVInputHook: Issues during cleanup. Disable: " + std::string(MH_StatusToString(disableStatus)) +
                                        ", Remove: " + std::string(MH_StatusToString(removeStatus)));
        }

        g_tpvInputHookAddress = nullptr;
        fpTpvCameraInputOriginal = nullptr;
    }
}
