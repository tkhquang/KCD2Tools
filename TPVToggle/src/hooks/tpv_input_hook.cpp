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

// Constants for known TPV mouse event IDs (based on your RE)
constexpr int MOUSE_EVENT_ID_TPV_YAW = 0x10A;   // Horizontal movement
constexpr int MOUSE_EVENT_ID_TPV_PITCH = 0x10B; // Vertical movement
constexpr int MOUSE_EVENT_ID_TPV_ZOOM = 0x10C;  // Mouse Wheel

// Constants for input event structure offsets (based on your RE)
// These are relative to 'inputEventPtr'
constexpr ptrdiff_t TPV_INPUT_EVENT_TYPE_CHECK_BYTE0_OFFSET = 0x00; // Expected value 0x01
constexpr ptrdiff_t TPV_INPUT_EVENT_TYPE_CHECK_INT4_OFFSET = 0x04;  // Expected value 0x08
constexpr ptrdiff_t TPV_INPUT_EVENT_ID_OFFSET = 0x10;               // Event ID (e.g., 0x10A, 0x10B)
constexpr ptrdiff_t TPV_INPUT_EVENT_DELTA_FLOAT_OFFSET = 0x18;      // Float delta for the event

/**
 * @brief Updates the global orbital camera rotation quaternion from the current yaw and pitch Euler angles.
 *        This function also applies pitch clamping based on configuration.
 */
void update_orbital_camera_rotation_from_euler()
{
    // Convert configured degree limits to radians for clamping
    Logger::getInstance().log(LOG_DEBUG, "UpdateEuler - PRE-CLAMP: g_orbitalCameraPitch=" + std::to_string(g_orbitalCameraPitch));
    float pitchMinRadians = g_config.orbit_pitch_min_degrees * (M_PI / 180.0f);
    float pitchMaxRadians = g_config.orbit_pitch_max_degrees * (M_PI / 180.0f);
    g_orbitalCameraPitch = std::max(pitchMinRadians, std::min(g_orbitalCameraPitch, pitchMaxRadians));
    Logger::getInstance().log(LOG_DEBUG, "UpdateEuler - POST-CLAMP: g_orbitalCameraPitch=" + std::to_string(g_orbitalCameraPitch) +
                                             " (MinRad=" + std::to_string(pitchMinRadians) + ", MaxRad=" + std::to_string(pitchMaxRadians) + ")");

    // Clamp the pitch
    g_orbitalCameraPitch = std::max(pitchMinRadians, std::min(g_orbitalCameraPitch, pitchMaxRadians));

    // Create quaternions for pitch and yaw
    // Pitch is around the local X-axis of the camera
    // Yaw is around the world Z-axis (assuming Z is up, adjust if KCD2 uses Y-up for world)
    DirectX::XMVECTOR qPitch = DirectX::XMQuaternionRotationNormal(DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), g_orbitalCameraPitch);
    DirectX::XMVECTOR qYaw = DirectX::XMQuaternionRotationNormal(DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), g_orbitalCameraYaw);

    // Combine rotations: Apply world yaw first, then local pitch
    // R_final = R_pitch * R_yaw
    // (Order can be experimented with: R_yaw * R_pitch would mean yawing a camera that's already pitched)
    DirectX::XMVECTOR finalRotationXm = DirectX::XMQuaternionMultiply(qPitch, qYaw);

    // Normalize and store
    g_orbitalCameraRotation = Quaternion::FromXMVector(DirectX::XMQuaternionNormalize(finalRotationXm));

    Logger::getInstance().log(LOG_DEBUG, "UpdateEuler - FINAL g_orbitalCameraRotation: " + QuatToString(g_orbitalCameraRotation));
}

/**
 * @brief Detour function for the TPV camera's input processing.
 *        If orbital camera mode is active, this function hijacks mouse input
 *        to control the orbital camera's yaw, pitch, and distance, preventing
 *        the game from processing these inputs for its default TPV behavior.
 *
 * @param thisPtr Pointer to the C_CameraThirdPerson instance (or similar TPV state object).
 * @param inputEventPtr Pointer to the raw input event structure.
 */

void __fastcall Detour_TpvCameraInput(uintptr_t thisPtr, char *inputEventPtr)
{
    Logger &logger = Logger::getInstance();

    if (g_nativeOrbitCVarsEnabled.load())
    { // If game's native orbit is supposed to be active
        if (fpTpvCameraInputOriginal)
        {
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr); // Let game handle ALL TPV input
        }
        return; // Do nothing else in this hook
    }

    if (!g_config.enable_camera_profiles && !g_config.enable_orbital_camera_mode)
    { // Early exit if neither feature needs this hook
        if (fpTpvCameraInputOriginal)
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
        return;
    }

    // Orbital Mode Logic
    if (g_config.enable_orbital_camera_mode && g_orbitalModeActive.load())
    {
        if (!isMemoryReadable(inputEventPtr, TPV_INPUT_EVENT_DELTA_FLOAT_OFFSET + sizeof(float)))
        {
            if (fpTpvCameraInputOriginal)
                fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
            return;
        }

        unsigned char eventByte0 = *reinterpret_cast<unsigned char *>(inputEventPtr + TPV_INPUT_EVENT_TYPE_CHECK_BYTE0_OFFSET);
        int eventInt4 = *reinterpret_cast<int *>(inputEventPtr + TPV_INPUT_EVENT_TYPE_CHECK_INT4_OFFSET);

        if (eventByte0 == 0x01 && eventInt4 == 0x08)
        {
            int eventID = *reinterpret_cast<int *>(inputEventPtr + TPV_INPUT_EVENT_ID_OFFSET);
            float deltaFromEvent = *reinterpret_cast<float *>(inputEventPtr + TPV_INPUT_EVENT_DELTA_FLOAT_OFFSET);
            bool inputHijacked = false;
            bool eulerAngleChanged = false; // Flag to see if we need to update combined quat

            // Log BEFORE switch
            logger.log(LOG_DEBUG, "Detour_TpvInputProcess (Orbital Active): Received EventID=" + format_hex(eventID, 4) + " Delta=" + std::to_string(deltaFromEvent));

            switch (eventID)
            {
            case MOUSE_EVENT_ID_TPV_YAW:
                if (std::abs(deltaFromEvent) > 0.0001f)
                { // Only update if delta is significant
                    g_orbitalCameraYaw += deltaFromEvent * g_config.orbit_sensitivity_yaw;
                    eulerAngleChanged = true;
                }
                inputHijacked = true;
                break;
            case MOUSE_EVENT_ID_TPV_PITCH:
                if (std::abs(deltaFromEvent) > 0.0001f)
                { // Only update if delta is significant
                    float pitchDelta = deltaFromEvent;
                    if (g_config.orbit_invert_pitch)
                        pitchDelta = -pitchDelta;
                    g_orbitalCameraPitch -= pitchDelta * g_config.orbit_sensitivity_pitch;
                    eulerAngleChanged = true;
                }
                inputHijacked = true;
                break;
            case MOUSE_EVENT_ID_TPV_ZOOM: // 0x10C
                g_orbitalCameraDistance -= deltaFromEvent * g_config.orbit_zoom_sensitivity;
                g_orbitalCameraDistance = std::max(g_config.orbit_min_distance, std::min(g_orbitalCameraDistance, g_config.orbit_max_distance));
                inputHijacked = true;
                // No eulerAngleChanged = true; here
                break;
            default:
                // Not an event ID we handle for orbital controls
                break;
            }

            if (inputHijacked)
            {
                if (eulerAngleChanged)
                {                                                // Only update if yaw or pitch changed
                    update_orbital_camera_rotation_from_euler(); // Called ONCE if needed
                }
                logger.log(LOG_DEBUG, "Detour_TpvInputProcess: HIJACKED Event ID " + format_hex(eventID, 4) + ". Suppressing original.");
                return; // Suppress original
            }
        }
        // If not a hijacked mouse event but orbital mode is on, still call original for other inputs.
        if (fpTpvCameraInputOriginal)
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr);
        // And then (importantly), after original runs (even if it did nothing due to no mouse input),
        // update g_latestTpvCameraForward for NON-ORBITAL mode's use of player state.
        // This part is tricky: if orbital is on, g_latestTpvCameraForward shouldn't be from the (stale) game camera.
        // Let's skip updating g_latestTpvCameraForward if orbital is on and input was NOT hijacked,
        // as the Detour_PlayerStateCopy will use g_orbitalCameraRotation's forward instead.
    }
    else // Orbital Mode is OFF (or feature disabled) - Standard TPV behaviour
    {
        if (fpTpvCameraInputOriginal)
        {
            fpTpvCameraInputOriginal(thisPtr, inputEventPtr); // Let game process input normally for its TPV cam
        }
        else
        {
            logger.log(LOG_ERROR, "Detour_TpvInputProcess: Original function pointer NULL!");
            return;
        }

        // After original game function has updated its TPV camera state (thisPtr+0x10),
        // read it to update g_latestTpvCameraForward.
        // This is for Detour_PlayerStateCopy when orbital is OFF.
        if (isMemoryReadable((void *)(thisPtr + Constants::TPV_CAMERA_QUATERNION_OFFSET), sizeof(Quaternion)))
        {
            float *quatData = reinterpret_cast<float *>(thisPtr + Constants::TPV_CAMERA_QUATERNION_OFFSET);
            Quaternion gameTpvCamRot(quatData[0], quatData[1], quatData[2], quatData[3]);
            g_latestTpvCameraForward = gameTpvCamRot.Rotate(Vector3(0.0f, 1.0f, 0.0f)); // Y-forward
            g_latestTpvCameraForward.Normalize();
        }
        else
        {
            // logger.log(LOG_WARNING, "Detour_TpvInputProcess (Non-Orbital): Cannot read game TPV quat for g_latestTpvCameraForward.");
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
