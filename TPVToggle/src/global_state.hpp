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

    // Pointers for TPV (optional to store globally, could be retrieved on-demand)
    extern uintptr_t g_Player_CEntity;
    extern uintptr_t g_Player_CCharInstance;
    extern uintptr_t g_Player_CDefaultSkeleton;

    extern uintptr_t g_FuncAddr_GetBoneIndexByName;
    extern uintptr_t g_FuncAddr_GetBoneWorldTransform;

    // TPV Specific Global State
    extern bool g_tpvEnabled;
    extern bool g_tpvForceDisabled; // e.g. for cutscenes, menus if detectable

    // Cached player-derived pointers for TPV (to avoid repeated lookups in critical path)
    // We won't use skeleton directly now, but CEntity is useful
    extern uintptr_t g_localPlayerCEntity; // CEntity* for the local player

    // Camera related data (if needed globally, or can be passed around)
    // For now, most camera logic will be self-contained in the hook or TpvLogic class

} // namespace GlobalState
