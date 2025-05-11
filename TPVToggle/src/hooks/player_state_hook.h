#pragma once

#include <cstdint>

// Initialize player state copy hook
bool initializePlayerStateHook(uintptr_t moduleBase, size_t moduleSize);

// Cleanup player state copy hook
void cleanupPlayerStateHook();
