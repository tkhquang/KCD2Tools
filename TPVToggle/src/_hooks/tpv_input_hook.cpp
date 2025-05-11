#include "tpv_input_hook.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h"
#include "math_utils.h" // Include our math header
#include "MinHook.h"

#include <stdexcept>

// Define function pointer type based on Ghidra/RE analysis (__fastcall convention, RCX=this, RDX=eventPtr)
typedef void(__fastcall *TpvInputProcessFunc)(uintptr_t thisPtr, char *eventPtr);

// Trampoline pointer for original function
TpvInputProcessFunc fpTpvInputProcessOriginal = nullptr;
static BYTE *g_tpvInputHookAddress = nullptr;

// Detour function
void __fastcall Detour_TpvInputProcess(uintptr_t thisPtr, char *eventPtr) // thisPtr is C_CameraThirdPerson*
{
    Logger &logger = Logger::getInstance();
    bool original_called = false; // Track if original was successfully called

    // Call original function first to update the camera's internal state
    if (fpTpvInputProcessOriginal)
    {
        try
        {
            fpTpvInputProcessOriginal(thisPtr, eventPtr);
            original_called = true;
        }
        catch (...)
        {
            logger.log(LOG_ERROR, "TpvInputHook: Exception calling original TpvInputProcess func!");
            return; // Don't proceed if original crashed
        }
    }
    else
    {
        logger.log(LOG_ERROR, "TpvInputHook: Original TpvInputProcess func ptr is NULL! Cannot update camera state.");
        return; // Can't proceed without original
    }

    // Read updated camera state only if original executed successfully
    if (original_called)
    {
        try
        {
            // Check readability of the specific quaternion memory location
            if (isMemoryReadable((void *)(thisPtr + Constants::TPV_CAMERA_QUATERNION_OFFSET), sizeof(Quaternion)))
            {
                // Read quaternion using the specific constant for the camera object
                float *quatData = reinterpret_cast<float *>(thisPtr + Constants::TPV_CAMERA_QUATERNION_OFFSET);
                Quaternion currentCamRot(quatData[0], quatData[1], quatData[2], quatData[3]); // Assuming XYZW

                // Calculate world forward vector (Y-Forward for CryEngine)
                Vector3 worldForward = currentCamRot.Rotate(Vector3(0.0f, 1.0f, 0.0f));
                worldForward.Normalize(); // Ensure it's normalized

                // Store globally
                g_latestTpvCameraForward = worldForward;

                // Logging (Reduced Frequency)
                if (logger.isDebugEnabled())
                {
                    static int log_counter_tpv = 0;
                    if (++log_counter_tpv % 150 == 0)
                    { // Log less often
                        std::ostringstream qss;
                        qss << std::fixed << std::setprecision(4)
                            << "TpvInputHook: Read Quat(" << currentCamRot.x << ", " << currentCamRot.y << ", "
                            << currentCamRot.z << ", " << currentCamRot.w << ") ";
                        qss << "-> WorldFwd(" << worldForward.x << ", " << worldForward.y << ", " << worldForward.z << ")";
                        logger.log(LOG_DEBUG, qss.str());
                    }
                }
            }
            else
            {
                if (logger.isDebugEnabled())
                { // Reduce log spam
                    static int read_err_count = 0;
                    if (++read_err_count % 100 == 0)
                        logger.log(LOG_WARNING, "TpvInputHook: Cannot read TPV camera quaternion data at " + format_address(thisPtr + Constants::TPV_CAMERA_QUATERNION_OFFSET));
                }
            }
        }
        catch (const std::exception &e)
        {
            logger.log(LOG_ERROR, "TpvInputHook: Error processing camera state: " + std::string(e.what()));
        }
        catch (...)
        {
            logger.log(LOG_ERROR, "TpvInputHook: Unknown error processing camera state.");
        }
    }
}

bool initializeTpvInputHook(uintptr_t moduleBase, size_t moduleSize)
{
    Logger &logger = Logger::getInstance();
    std::vector<BYTE> pattern = parseAOB(Constants::TPV_INPUT_PROCESS_AOB_PATTERN);
    if (pattern.empty())
    {
        logger.log(LOG_ERROR, "TpvInputHook: Failed to parse TPV Input AOB pattern.");
        return false;
    }

    g_tpvInputHookAddress = FindPattern(reinterpret_cast<BYTE *>(moduleBase), moduleSize, pattern);
    if (!g_tpvInputHookAddress)
    {
        logger.log(LOG_ERROR, "TpvInputHook: TPV Input pattern not found.");
        return false;
    }

    logger.log(LOG_INFO, "TpvInputHook: Found TPV Input Process function at " + format_address(reinterpret_cast<uintptr_t>(g_tpvInputHookAddress)));

    MH_STATUS status = MH_CreateHook(g_tpvInputHookAddress, reinterpret_cast<LPVOID>(&Detour_TpvInputProcess), reinterpret_cast<LPVOID *>(&fpTpvInputProcessOriginal));
    if (status != MH_OK)
    {
        logger.log(LOG_ERROR, "TpvInputHook: MH_CreateHook failed: " + std::string(MH_StatusToString(status)));
        return false;
    }
    if (!fpTpvInputProcessOriginal)
    {
        logger.log(LOG_ERROR, "TpvInputHook: MH_CreateHook returned NULL trampoline.");
        MH_RemoveHook(g_tpvInputHookAddress); // Clean up hook if trampoline failed
        return false;
    }

    status = MH_EnableHook(g_tpvInputHookAddress);
    if (status != MH_OK)
    {
        logger.log(LOG_ERROR, "TpvInputHook: MH_EnableHook failed: " + std::string(MH_StatusToString(status)));
        MH_RemoveHook(g_tpvInputHookAddress); // Clean up hook
        return false;
    }

    logger.log(LOG_INFO, "TpvInputHook: Successfully installed.");
    return true;
}

void cleanupTpvInputHook()
{
    Logger &logger = Logger::getInstance();
    if (g_tpvInputHookAddress)
    {
        MH_DisableHook(g_tpvInputHookAddress);
        MH_RemoveHook(g_tpvInputHookAddress);
        g_tpvInputHookAddress = nullptr;
        fpTpvInputProcessOriginal = nullptr;
        logger.log(LOG_DEBUG, "TpvInputHook: Hook cleaned up.");
    }
}
