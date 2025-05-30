#include <windows.h>
#include "config.hpp"

namespace GlobalState
{
    LocalConfig g_config;

    uintptr_t g_ModuleBase = 0;
    size_t g_ModuleSize = 0;

    uintptr_t g_ISystem = 0;
    uintptr_t g_pGame_ptr_address = 0;
    uintptr_t g_localPlayerEntity = 0;

} // namespace GlobalState
