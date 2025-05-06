#pragma once

#include <cstdint>

// Initialize TPV Camera Update hook
bool initializeTpvCameraHook(uintptr_t moduleBase, size_t moduleSize);

// Cleanup TPV Camera Update hook
void cleanupTpvCameraHook();
