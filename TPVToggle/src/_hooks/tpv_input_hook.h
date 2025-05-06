#pragma once

#include <cstdint>

// Initialize TPV Input processing hook
bool initializeTpvInputHook(uintptr_t moduleBase, size_t moduleSize);

// Cleanup TPV Input processing hook
void cleanupTpvInputHook();
