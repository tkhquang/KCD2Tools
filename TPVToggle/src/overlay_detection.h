/**
 * @file overlay_detection.h
 * @brief Header for detecting game overlay/menu state
 *
 * This file defines the structures and functions needed to detect when
 * the game opens menus, dialogs, or other overlays, and automatically
 * switch between first and third person views accordingly.
 */

#ifndef OVERLAY_DETECTION_H
#define OVERLAY_DETECTION_H

#include <windows.h>

// Exception handler for the INT3 breakpoint at overlay check instruction
LONG WINAPI OverlayExceptionHandler(EXCEPTION_POINTERS *ExceptionInfo);

// Exception handler for the INT3 breakpoint at camera distance instruction
LONG WINAPI CameraExceptionHandler(EXCEPTION_POINTERS *ExceptionInfo);

// Initialize the overlay detection system
bool InitializeOverlayDetection();

// Start the overlay state monitoring thread
void StartOverlayMonitoring();

// Clean up overlay detection resources
void CleanupOverlayDetection();

// Get the camera distance value (returns 0.0f if not available)
float GetCameraDistance();

// Set the camera distance to a specific value (returns false if failed)
bool SetCameraDistance(float distance);

// Functions for camera distance freezing
void StartCameraFreeze(float distance);
void StopCameraFreeze();

#endif // OVERLAY_DETECTION_H
