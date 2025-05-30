#include <windows.h>

#include "config.hpp"

namespace GlobalState
{
    // Global module information
    extern uintptr_t g_ModuleBase;
    extern size_t g_ModuleSize;

    extern LocalConfig g_config;

    extern uintptr_t g_ISystem;
    extern uintptr_t g_pGame_ptr_address;
    extern uintptr_t g_localPlayerEntity;

} // namespace GlobalState
