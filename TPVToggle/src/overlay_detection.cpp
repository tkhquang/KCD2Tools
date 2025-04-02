/**
 * @file overlay_detection.cpp
 * @brief Implementation of game overlay/menu state detection
 *
 * This file implements the system to detect when the game opens menus, dialogs,
 * or other overlays that should cause the game to switch to first-person view.
 * When the overlay closes, the system automatically restores the previous view state.
 */

#include "overlay_detection.h"
#include "aob_scanner.h"
#include "logger.h"
#include "utils.h"
#include "toggle_thread.h"
#include "constants.h"
#include <vector>
#include <string>
#include <sstream>
#include <psapi.h>

// Addresses and pointers for overlay detection
volatile BYTE *rbx_base_pointer = nullptr;      // RBX value captured during hook
volatile DWORD64 *overlay_flag_addr = nullptr;  // Computed address of overlay flag (RBX+0xD8)
volatile float *camera_distance_addr = nullptr; // Camera distance address

// Camera distance specific tracking
volatile BYTE *camera_rbx_pointer = nullptr;  // RBX value for camera distance calculations
BYTE original_camera_bytes[5];                // Original instruction bytes for camera hook
BYTE *camera_instr_addr = nullptr;            // Address of the camera distance instruction
DWORD original_camera_protection = 0;         // Original memory protection for camera hook
PVOID cameraExceptionHandlerHandle = nullptr; // Handle for camera exception handler

// Overlay detection tracking
BYTE original_overlay_bytes[7];                // Original instruction bytes at hook location
BYTE *overlay_instr_addr = nullptr;            // Address of the hooked instruction
DWORD original_overlay_protection = 0;         // Original memory protection at hook location
PVOID overlayExceptionHandlerHandle = nullptr; // Handle for the exception handler

// State tracking variables
volatile bool was_in_overlay = false;            // Track if we were in an overlay previously
volatile bool was_in_tpv_before_overlay = false; // Track view state before overlay opened
volatile float saved_camera_distance = 0.0f;     // Save camera distance when entering overlay

// Camera freezing variables
volatile bool extended_freeze_active = false;
volatile DWORD64 extended_freeze_end_time = 0;
volatile bool camera_distance_frozen = false;
volatile float frozen_camera_distance = 0.0f;
HANDLE cameraFreezeThread = NULL;

/**
 * Thread function that continues to protect the camera distance value
 * for a short time after returning to TPV. This helps prevent the game
 * from adjusting the distance during view transitions.
 */
DWORD WINAPI ExtendedProtectionThread(LPVOID)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "Extended camera protection started");

    // Keep protecting the camera distance for the specified duration
    while (GetTickCount64() < extended_freeze_end_time && extended_freeze_active)
    {
        // Write the frozen distance value
        SetCameraDistance(frozen_camera_distance);

        Sleep(8); // Write at ~120Hz for stronger protection
    }

    extended_freeze_active = false;
    logger.log(LOG_INFO, "Extended camera protection ended");
    return 0;
}

/**
 * Starts extended protection for the camera distance after returning to TPV.
 * This prevents the game from changing the distance during view transitions.
 *
 * @param duration_ms How long to protect the camera distance in milliseconds
 */
void StartExtendedProtection(int duration_ms = 500)
{
    // Don't start if already active
    if (extended_freeze_active)
    {
        // Just update the end time if already running
        extended_freeze_end_time = GetTickCount64() + duration_ms;
        return;
    }

    // Set up protection duration
    extended_freeze_active = true;
    extended_freeze_end_time = GetTickCount64() + duration_ms;

    // Start a thread that will continuously write the frozen distance value
    CreateThread(NULL, 0, ExtendedProtectionThread, NULL, 0, NULL);
}

/**
 * Stops camera freezing with a delayed, extended protection phase.
 * This approach:
 * 1. First stops the continuous freeze thread (which runs at 60Hz)
 * 2. Then starts a more aggressive protection thread (120Hz) for a short time
 * 3. This ensures the distance stays frozen even as game systems try to change it
 */
void StopCameraFreezeWithProtection()
{
    // Get the frozen distance value before stopping the freeze
    float distance_to_protect = frozen_camera_distance;

    // First stop the normal freezing thread
    StopCameraFreeze();

    // Use the previously saved distance for extended protection
    frozen_camera_distance = distance_to_protect;

    // Start extended protection phase (3000ms by default)
    StartExtendedProtection(3000);

    Logger::getInstance().log(LOG_DEBUG, "Camera distance freeze stopped, extended protection active");
}

/**
 * Thread function that continuously writes a frozen camera distance value
 * to the camera distance memory address while in FPV mode during an overlay.
 */
DWORD WINAPI CameraFreezeThread(LPVOID)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "Camera freeze thread started");

    std::stringstream ss;
    ss << "Freezing camera distance at " << frozen_camera_distance;
    logger.log(LOG_DEBUG, ss.str());

    // Keep writing the frozen value until we're told to stop
    while (camera_distance_frozen)
    {
        // Try to set the camera distance to the frozen value
        SetCameraDistance(frozen_camera_distance);

        // Check the current value and log if it's different from what we expect
        float current = GetCameraDistance();
        if (abs(current - frozen_camera_distance) > 0.001f)
        {
            std::stringstream ss2;
            ss2 << "Camera distance changed from " << frozen_camera_distance
                << " to " << current << ", resetting...";
            logger.log(LOG_DEBUG, ss2.str());
        }

        // Sleep a short time to reduce CPU usage
        Sleep(16); // Approximately 60Hz
    }

    logger.log(LOG_INFO, "Camera freeze thread stopped");
    return 0;
}

/**
 * Starts freezing the camera distance at the specified value.
 */
void StartCameraFreeze(float distance)
{
    if (camera_distance_frozen)
    {
        // Already frozen, just update the value
        frozen_camera_distance = distance;
        return;
    }

    // Save the current distance value
    frozen_camera_distance = distance;
    camera_distance_frozen = true;

    // Start a thread that will continuously write this value
    cameraFreezeThread = CreateThread(NULL, 0, CameraFreezeThread, NULL, 0, NULL);

    // Check if thread creation failed
    if (cameraFreezeThread == NULL)
    {
        camera_distance_frozen = false;
        Logger::getInstance().log(LOG_ERROR, "Failed to create camera freeze thread");
    }
}

/**
 * Stops freezing the camera distance.
 */
void StopCameraFreeze()
{
    // Signal the thread to stop
    camera_distance_frozen = false;

    // Wait for the thread to finish
    if (cameraFreezeThread != NULL)
    {
        WaitForSingleObject(cameraFreezeThread, 100); // Wait up to 100ms
        CloseHandle(cameraFreezeThread);
        cameraFreezeThread = NULL;
    }

    Logger::getInstance().log(LOG_DEBUG, "Camera distance unfrozen");
}

/**
 * Updates the camera distance address when camera toggle is detected.
 * This calculates the correct camera distance memory address based on
 * the toggle address that we know is valid.
 */
void UpdateCameraAddresses()
{
    Logger &logger = Logger::getInstance();

    // Get the toggle_addr pointer since it gives us the camera structure
    volatile BYTE *toggle_ptr = getToggleAddr();
    if (toggle_ptr == nullptr)
    {
        logger.log(LOG_DEBUG, "UpdateCameraAddresses: Toggle address not yet available");
        return;
    }

    // Calculate the base address by subtracting the offset
    // The toggle address is at r9+0x38, and the base address we want is r9
    volatile BYTE *cam_base = toggle_ptr - Constants::TOGGLE_FLAG_OFFSET;

    // Now we have the camera base pointer, we can get the accurate distance address
    camera_distance_addr = (volatile float *)(cam_base + Constants::CAMERA_DISTANCE_OFFSET);

    logger.log(LOG_INFO, "Camera distance address updated to " +
                             format_address(reinterpret_cast<uintptr_t>(camera_distance_addr)));
}

/**
 * Gets the current camera distance value.
 * Returns 0.0f if the address is not yet initialized.
 * Uses local variables to avoid race conditions with volatile pointers.
 */
float GetCameraDistance()
{
    // Take a local copy of the volatile pointers to prevent race conditions
    volatile float *local_camera_distance_addr = camera_distance_addr;
    volatile BYTE *local_camera_rbx = camera_rbx_pointer;
    volatile BYTE *local_overlay_rbx = rbx_base_pointer;

    // Use direct camera distance address if available
    if (local_camera_distance_addr != nullptr)
    {
        try
        {
            return *local_camera_distance_addr;
        }
        catch (...)
        {
            Logger::getInstance().log(LOG_ERROR, "Failed to read camera distance");
            return 0.0f;
        }
    }
    // Fallback to using camera RBX pointer if available (from camera exception handler)
    else if (local_camera_rbx != nullptr)
    {
        try
        {
            return *(float *)(local_camera_rbx + Constants::CAMERA_DISTANCE_OFFSET);
        }
        catch (...)
        {
            Logger::getInstance().log(LOG_ERROR, "Failed to read camera distance using camera RBX");
            return 0.0f;
        }
    }
    // Last resort fallback to overlay RBX pointer
    else if (local_overlay_rbx != nullptr)
    {
        try
        {
            return *(float *)(local_overlay_rbx + Constants::CAMERA_DISTANCE_OFFSET);
        }
        catch (...)
        {
            Logger::getInstance().log(LOG_ERROR, "Failed to read camera distance using overlay RBX");
            return 0.0f;
        }
    }
    // No valid pointers available
    else
    {
        Logger::getInstance().log(LOG_DEBUG, "No camera address available yet");
        return 0.0f;
    }
}

/**
 * Sets the camera distance to a specific value.
 * Returns false if the operation fails.
 * Uses local variables to avoid race conditions with volatile pointers.
 */
bool SetCameraDistance(float distance)
{
    // Take a local copy of the volatile pointers to prevent race conditions
    volatile float *local_camera_distance_addr = camera_distance_addr;
    volatile BYTE *local_camera_rbx = camera_rbx_pointer;
    volatile BYTE *local_overlay_rbx = rbx_base_pointer;

    // Use direct camera distance address if available
    if (local_camera_distance_addr != nullptr)
    {
        try
        {
            *local_camera_distance_addr = distance;
            return true;
        }
        catch (...)
        {
            Logger::getInstance().log(LOG_ERROR, "Failed to set camera distance");
            return false;
        }
    }
    // Fallback to using camera RBX pointer if available (from camera exception handler)
    else if (local_camera_rbx != nullptr)
    {
        try
        {
            *(float *)(local_camera_rbx + Constants::CAMERA_DISTANCE_OFFSET) = distance;
            return true;
        }
        catch (...)
        {
            Logger::getInstance().log(LOG_ERROR, "Failed to set camera distance using camera RBX");
            return false;
        }
    }
    // Last resort fallback to overlay RBX pointer
    else if (local_overlay_rbx != nullptr)
    {
        try
        {
            *(float *)(local_overlay_rbx + Constants::CAMERA_DISTANCE_OFFSET) = distance;
            return true;
        }
        catch (...)
        {
            Logger::getInstance().log(LOG_ERROR, "Failed to set camera distance using overlay RBX");
            return false;
        }
    }
    // No valid pointers available
    else
    {
        Logger::getInstance().log(LOG_ERROR, "Cannot set camera distance: no valid address available");
        return false;
    }
}

/**
 * Exception handler for the INT3 breakpoint at camera distance instruction.
 * When triggered, it captures the RBX register value which is used to
 * compute the address of the camera distance.
 */
LONG WINAPI CameraExceptionHandler(EXCEPTION_POINTERS *ExceptionInfo)
{
    // Check if this is our INT3 breakpoint at the expected address
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        ExceptionInfo->ContextRecord->Rip == (DWORD64)camera_instr_addr)
    {
        // Get rbx register value from the exception context
        BYTE *rbx = (BYTE *)ExceptionInfo->ContextRecord->Rbx;

        // Safety check to ensure rbx is not null
        if (rbx == nullptr)
        {
            Logger::getInstance().log(LOG_ERROR, "CameraException: rbx register is NULL");
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Save RBX value for use in camera distance calculations
        camera_rbx_pointer = rbx;

        // Compute camera distance address and REPLACE any existing address
        // This ensures we're using the address from the camera code, not overlay code
        camera_distance_addr = (volatile float *)(rbx + Constants::CAMERA_DISTANCE_OFFSET);

        // Log detailed information for debugging
        Logger::getInstance().log(LOG_DEBUG, "CameraException: rbx = " + format_address((uintptr_t)rbx));
        Logger::getInstance().log(LOG_INFO, "Camera distance address found at " + format_address((uintptr_t)camera_distance_addr));

        // Restore original instruction
        VirtualProtect(camera_instr_addr, 5, PAGE_EXECUTE_READWRITE, &original_camera_protection);
        memcpy(camera_instr_addr, original_camera_bytes, 5);
        VirtualProtect(camera_instr_addr, 5, original_camera_protection, &original_camera_protection);

        // Resume execution at the restored instruction
        ExceptionInfo->ContextRecord->Rip = (DWORD64)camera_instr_addr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Not our exception, let other handlers try
    return EXCEPTION_CONTINUE_SEARCH;
}

/**
 * Exception handler for the INT3 breakpoint at overlay check instruction.
 * When triggered, it captures the RBX register value which is used to
 * compute the address of the overlay flag.
 */
LONG WINAPI OverlayExceptionHandler(EXCEPTION_POINTERS *ExceptionInfo)
{
    // Check if this is our INT3 breakpoint at the expected address
    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT &&
        ExceptionInfo->ContextRecord->Rip == (DWORD64)overlay_instr_addr)
    {
        // Get rbx register value from the exception context
        BYTE *rbx = (BYTE *)ExceptionInfo->ContextRecord->Rbx;

        // Safety check to ensure rbx is not null
        if (rbx == nullptr)
        {
            Logger::getInstance().log(LOG_ERROR, "OverlayException: rbx register is NULL");
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Save RBX value for use in overlay monitoring
        rbx_base_pointer = rbx;

        // Compute overlay flag address
        overlay_flag_addr = (DWORD64 *)(rbx + Constants::OVERLAY_FLAG_OFFSET);

        // Note: We don't compute camera distance address here anymore - we use the dedicated camera hook

        // Log detailed information for debugging
        Logger::getInstance().log(LOG_DEBUG, "OverlayException: rbx = " + format_address((uintptr_t)rbx));
        Logger::getInstance().log(LOG_INFO, "Overlay flag address found at " + format_address((uintptr_t)overlay_flag_addr));

        // Restore original instruction
        VirtualProtect(overlay_instr_addr, 7, PAGE_EXECUTE_READWRITE, &original_overlay_protection);
        memcpy(overlay_instr_addr, original_overlay_bytes, 7);
        VirtualProtect(overlay_instr_addr, 7, original_overlay_protection, &original_overlay_protection);

        // Resume execution at the restored instruction
        ExceptionInfo->ContextRecord->Rip = (DWORD64)overlay_instr_addr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Not our exception, let other handlers try
    return EXCEPTION_CONTINUE_SEARCH;
}

/**
 * Thread function that monitors the overlay flag and automatically
 * switches view modes when needed.
 */
DWORD WINAPI OverlayMonitorThread(LPVOID)
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "Overlay monitor thread started");

    // Wait for overlay flag address to be initialized by exception handler
    while (overlay_flag_addr == nullptr || rbx_base_pointer == nullptr)
    {
        Sleep(50);
    }

    logger.log(LOG_INFO, "Overlay monitor: Overlay flag address initialized, waiting for camera toggle address...");

    // Wait for camera toggle address to be initialized too
    // This ensures we don't try to get/set view state before the camera system is ready
    while (getToggleAddr() == nullptr)
    {
        Sleep(100);
    }

    logger.log(LOG_INFO, "Overlay monitor: Camera toggle address ready, synchronization complete");

    // Check if we already have a camera distance address from the hook
    if (camera_distance_addr != nullptr)
    {
        float initialDistance = GetCameraDistance();
        std::stringstream ss;
        ss << "Initial camera distance: " << initialDistance;
        logger.log(LOG_INFO, ss.str());
    }
    else
    {
        logger.log(LOG_INFO, "Waiting for camera distance hook to trigger...");
    }

    // Initial check for current overlay state
    bool in_overlay = false;

    while (true)
    {
        // Check if we have a valid address before reading
        if (overlay_flag_addr != nullptr && rbx_base_pointer != nullptr)
        {
            try
            {
                // Read the overlay flag value (0 = no overlay, >0 = overlay active)
                DWORD64 overlay_value = *overlay_flag_addr;

                // Determine if we're in an overlay (any non-zero value means overlay is active)
                bool current_in_overlay = (overlay_value > 0);

                // If overlay state has changed
                if (current_in_overlay != in_overlay)
                {
                    // State transition from normal gameplay to overlay
                    if (current_in_overlay && !in_overlay)
                    {
                        // Only proceed if the camera toggle address is available
                        if (getToggleAddr() != nullptr)
                        {
                            // Save current view state before switching to FPV
                            BYTE current_view = getViewState();
                            was_in_tpv_before_overlay = (current_view == 1);

                            // If we're in TPV, start freezing the camera distance and switch to FPV
                            if (was_in_tpv_before_overlay)
                            {
                                // Get current camera distance and start freezing it
                                float current_distance = GetCameraDistance();
                                StartCameraFreeze(current_distance);

                                // Switch to FPV when overlay opens
                                logger.log(LOG_INFO, "Overlay: Menu opened, switching to FPV and freezing camera distance");
                                setFirstPersonView();
                            }
                        }
                        else
                        {
                            logger.log(LOG_DEBUG, "Overlay: Menu opened but camera toggle address not ready yet");
                        }
                    }
                    // State transition from overlay back to normal gameplay
                    else if (!current_in_overlay && in_overlay)
                    {
                        // Only proceed if the camera toggle address is available
                        if (getToggleAddr() != nullptr)
                        {
                            // Restore previous view state
                            if (was_in_tpv_before_overlay)
                            {
                                // First restore TPV
                                logger.log(LOG_INFO, "Overlay: Menu closed, restoring TPV");
                                setThirdPersonView();

                                // Stop freezing with extended protection to prevent distance changes
                                StopCameraFreezeWithProtection();
                            }
                        }
                        else
                        {
                            logger.log(LOG_DEBUG, "Overlay: Menu closed but camera toggle address not ready yet");
                        }
                    }

                    // Update overlay state
                    in_overlay = current_in_overlay;
                    std::stringstream ss;
                    ss << "Overlay state changed to: "
                       << (in_overlay ? "Active" : "Inactive")
                       << ", overlay_value=" << overlay_value;
                    logger.log(LOG_DEBUG, ss.str());
                }
            }
            catch (...)
            {
                // Handle any exceptions during memory access
                logger.log(LOG_ERROR, "Exception accessing overlay flag at " +
                                          format_address((uintptr_t)overlay_flag_addr));

                // Reset pointers to force re-hooking
                overlay_flag_addr = nullptr;
                rbx_base_pointer = nullptr;

                // Re-initialize overlay detection
                InitializeOverlayDetection();
            }
        }

        // Short sleep to reduce CPU usage
        Sleep(33); // Around 30 FPS check rate - fast enough for good response, low CPU impact
    }

    return 0;
}

/**
 * Initializes the overlay detection system by finding the pattern in memory
 * and setting up the INT3 hook.
 */
bool InitializeOverlayDetection()
{
    Logger &logger = Logger::getInstance();
    logger.log(LOG_INFO, "Initializing overlay detection...");

    // Parse AOB pattern for overlay flag
    std::vector<BYTE> pattern = parseAOB(Constants::OVERLAY_AOB_PATTERN);
    if (pattern.empty())
    {
        logger.log(LOG_ERROR, "Overlay: AOB pattern is empty or invalid");
        return false;
    }

    // Parse AOB pattern for camera distance
    std::vector<BYTE> distance_pattern = parseAOB(Constants::CAMERA_DISTANCE_AOB_PATTERN);
    if (distance_pattern.empty())
    {
        logger.log(LOG_WARNING, "Camera distance: AOB pattern is empty or invalid");
        // We continue anyway as this is not critical
    }
    else
    {
        logger.log(LOG_DEBUG, "Camera distance: AOB pattern parsed successfully");
    }

    // Find game module
    HMODULE hModule = GetModuleHandleA(Constants::MODULE_NAME);
    if (!hModule)
    {
        logger.log(LOG_ERROR, "Overlay: Failed to find " + std::string(Constants::MODULE_NAME));
        return false;
    }

    // Get module information for scanning
    MODULEINFO moduleInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo)))
    {
        logger.log(LOG_ERROR, "Overlay: Failed to get " + std::string(Constants::MODULE_NAME) + " info");
        return false;
    }

    // Scan for overlay pattern
    BYTE *base = (BYTE *)hModule;
    size_t size = moduleInfo.SizeOfImage;
    BYTE *aob_addr = FindPattern(base, size, pattern);
    if (!aob_addr)
    {
        logger.log(LOG_ERROR, "Overlay: AOB pattern not found in " + std::string(Constants::MODULE_NAME));
        return false;
    }
    logger.log(LOG_INFO, "Overlay: AOB pattern found at " + format_address(reinterpret_cast<uintptr_t>(aob_addr)));

    // Scan for camera distance pattern if provided
    BYTE *camera_addr = nullptr;
    if (!distance_pattern.empty())
    {
        camera_addr = FindPattern(base, size, distance_pattern);
        if (camera_addr)
        {
            logger.log(LOG_INFO, "Camera distance: AOB pattern found at " +
                                     format_address(reinterpret_cast<uintptr_t>(camera_addr)));

            // Set up INT3 hook at the instruction that accesses camera distance
            camera_instr_addr = camera_addr; // Direct instruction that accesses camera distance
            memcpy(original_camera_bytes, camera_instr_addr, 5);

            if (!VirtualProtect(camera_instr_addr, 5, PAGE_EXECUTE_READWRITE, &original_camera_protection))
            {
                logger.log(LOG_ERROR, "Camera: Failed to change protection at " +
                                          format_address(reinterpret_cast<uintptr_t>(camera_instr_addr)));
                // Continue anyway, as this isn't critical
            }
            else
            {
                // Place INT3 breakpoint for camera distance
                memset(camera_instr_addr, 0xCC, 5);
                logger.log(LOG_INFO, "Camera: INT3 set at " +
                                         format_address(reinterpret_cast<uintptr_t>(camera_instr_addr)));

                // Add exception handler for camera distance
                cameraExceptionHandlerHandle = AddVectoredExceptionHandler(1, CameraExceptionHandler);
                if (cameraExceptionHandlerHandle == nullptr)
                {
                    logger.log(LOG_ERROR, "Camera: Failed to add exception handler");
                    // Continue anyway, as this isn't critical
                }
                else
                {
                    logger.log(LOG_INFO, "Camera distance hook initialized successfully");
                }
            }
        }
        else
        {
            logger.log(LOG_WARNING, "Camera distance: AOB pattern not found");
        }
    }

    // Set up INT3 hook at the instruction that reads the overlay flag
    overlay_instr_addr = aob_addr; // Direct instruction that checks overlay flag
    memcpy(original_overlay_bytes, overlay_instr_addr, 7);

    if (!VirtualProtect(overlay_instr_addr, 7, PAGE_EXECUTE_READWRITE, &original_overlay_protection))
    {
        logger.log(LOG_ERROR, "Overlay: Failed to change protection at " +
                                  format_address(reinterpret_cast<uintptr_t>(overlay_instr_addr)));
        return false;
    }

    // Place INT3 breakpoint - replace "cmp qword ptr [rbx+0D8h], 0" with INT3
    memset(overlay_instr_addr, 0xCC, 7);
    logger.log(LOG_INFO, "Overlay: INT3 set at " +
                             format_address(reinterpret_cast<uintptr_t>(overlay_instr_addr)));

    // Add exception handler to capture rbx register
    overlayExceptionHandlerHandle = AddVectoredExceptionHandler(1, OverlayExceptionHandler);
    if (overlayExceptionHandlerHandle == nullptr)
    {
        logger.log(LOG_ERROR, "Overlay: Failed to add exception handler");
        return false;
    }

    logger.log(LOG_INFO, "Overlay detection initialized successfully");
    return true;
}

/**
 * Starts the thread that monitors overlay state changes.
 */
void StartOverlayMonitoring()
{
    CreateThread(NULL, 0, OverlayMonitorThread, NULL, 0, NULL);
}

/**
 * Cleans up resources used by the overlay detection system.
 */
void CleanupOverlayDetection()
{
    Logger &logger = Logger::getInstance();

    // Stop extended protection if it's running
    extended_freeze_active = false;

    // Stop camera freeze if it's running
    if (camera_distance_frozen)
    {
        StopCameraFreeze();
    }

    // Restore original bytes for overlay hook
    if (overlay_instr_addr != nullptr)
    {
        DWORD oldProtect;
        if (VirtualProtect(overlay_instr_addr, 7, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            memcpy(overlay_instr_addr, original_overlay_bytes, 7);
            VirtualProtect(overlay_instr_addr, 7, oldProtect, &oldProtect);
            logger.log(LOG_INFO, "Overlay: Restored original instruction at " +
                                     format_address(reinterpret_cast<uintptr_t>(overlay_instr_addr)));
        }
        else
        {
            logger.log(LOG_ERROR, "Overlay: Failed to restore original instruction");
        }
    }

    // Restore original bytes for camera hook
    if (camera_instr_addr != nullptr)
    {
        DWORD oldProtect;
        if (VirtualProtect(camera_instr_addr, 5, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            memcpy(camera_instr_addr, original_camera_bytes, 5);
            VirtualProtect(camera_instr_addr, 5, oldProtect, &oldProtect);
            logger.log(LOG_INFO, "Camera: Restored original instruction at " +
                                     format_address(reinterpret_cast<uintptr_t>(camera_instr_addr)));
        }
        else
        {
            logger.log(LOG_ERROR, "Camera: Failed to restore original instruction");
        }
    }

    // Remove overlay exception handler if added
    if (overlayExceptionHandlerHandle != nullptr)
    {
        RemoveVectoredExceptionHandler(overlayExceptionHandlerHandle);
        logger.log(LOG_INFO, "Overlay: Removed exception handler");
    }

    // Remove camera exception handler if added
    if (cameraExceptionHandlerHandle != nullptr)
    {
        RemoveVectoredExceptionHandler(cameraExceptionHandlerHandle);
        logger.log(LOG_INFO, "Camera: Removed exception handler");
    }

    logger.log(LOG_INFO, "Overlay detection cleanup complete");
}
