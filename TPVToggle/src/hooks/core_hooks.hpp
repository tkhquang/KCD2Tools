#include <windows.h>

bool initializeCoreHooks(uintptr_t moduleBase, size_t moduleSize);
void cleanupCoreHooks();
