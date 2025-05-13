#pragma once
#include <cstdint>

bool initializeEntityHooks(uintptr_t moduleBase, size_t moduleSize);
void cleanupEntityHooks();
