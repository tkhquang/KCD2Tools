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

    uintptr_t g_Player_CEntity = 0;
    uintptr_t g_Player_CCharInstance = 0;
    uintptr_t g_Player_CDefaultSkeleton = 0;

    uintptr_t g_FuncAddr_GetBoneIndexByName = 0;
    uintptr_t g_FuncAddr_GetBoneWorldTransform = 0;

    // TPV Specific Global State
    bool g_tpvEnabled = false; // Start with TPV disabled
    bool g_tpvForceDisabled = false;

    // Cached player-derived pointers
    uintptr_t g_localPlayerCEntity = 0; // CEntity*

} // namespace GlobalState
