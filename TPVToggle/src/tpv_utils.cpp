#include "tpv_utils.hpp"
#include "global_state.hpp"
#include <DetourModKit.hpp> // Includes DMK::Memory, DMK::Logger, DMK::String
#include <windows.h>        // Still useful for some low-level checks if needed, though DMK likely wraps them.
#include <thread>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <DirectXMath.h>

using namespace DetourModKit;
using namespace DetourModKit::String;
using namespace DirectX;

namespace TpvUtils
{
    // Initialize RVAs
    namespace Rvas
    {
        const uintptr_t GetActualComponentFromManager_RVA = 0x201AB20;
    }

    // Static map for one-time logging (can be removed if DMKLogger has rate limiting)
    static std::unordered_map<std::string, bool> g_oneTimeLogMap;
    static std::mutex g_oneTimeLogMutex;

    void LogOnce(Logger &logger, LogLevel level, const std::string &key, const std::string &message)
    {
        std::lock_guard<std::mutex> lock(g_oneTimeLogMutex);
        if (g_oneTimeLogMap.find(key) == g_oneTimeLogMap.end())
        {
            logger.log(level, message);
            g_oneTimeLogMap[key] = true;
        }
    }
    void ResetLogOnce(const std::string &key)
    { // Call if condition becomes valid again
        std::lock_guard<std::mutex> lock(g_oneTimeLogMutex);
        g_oneTimeLogMap.erase(key);
    }

    // --- InitializeAndLogPlayerAnimationPointers ---
    bool InitializeAndLogPlayerAnimationPointers(int maxRetries, int retryDelayMs)
    {
        Logger &logger = Logger::getInstance();
        logger.log(LOG_INFO, "TpvUtils: Beginning initialization of player animation pointers (Max Retries: " + std::to_string(maxRetries) + ")");

        static bool s_playerEntityLogged = false;
        // ... other static bools for one-time INFO logging ...
        static bool s_playerCEntityLogged = false;
        static bool s_compMgrLogged = false;
        static bool s_charInstLogged = false;
        static bool s_defSkelDirectLogged = false;

        int currentRetry = 0;
        while (currentRetry < maxRetries)
        {
            currentRetry++;

            if (GlobalState::g_localPlayerEntity == 0)
            {
                logger.log(LOG_DEBUG, "TpvUtils: LocalPlayerEntity is not available (Attempt " + std::to_string(currentRetry) + "). Retrying...");
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            if (!s_playerEntityLogged)
            {
                logger.log(LOG_INFO, "TpvUtils: P_Player_this = " + format_address(GlobalState::g_localPlayerEntity));
                s_playerEntityLogged = true;
            }

            if (!DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(GlobalState::g_localPlayerEntity + Offsets::CPlayer_To_CEntity), sizeof(uintptr_t)))
            {
                LogOnce(logger, LOG_WARNING, "PPlayerCEntityReadFail", "TpvUtils: Memory for P_Player_Entity is not readable.");
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            uintptr_t pPlayerEntity = *(uintptr_t *)(GlobalState::g_localPlayerEntity + Offsets::CPlayer_To_CEntity);
            GlobalState::g_Player_CEntity = pPlayerEntity; // Store this intermediate step

            if (pPlayerEntity == 0)
            {
                LogOnce(logger, LOG_WARNING, "PPlayerCEntityNull", "TpvUtils: P_Player_Entity is NULL.");
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            if (!s_playerCEntityLogged)
            {
                logger.log(LOG_INFO, "TpvUtils: P_Player_Entity = " + format_address(pPlayerEntity));
                s_playerCEntityLogged = true;
                ResetLogOnce("PPlayerCEntityNull"); // Reset error if now valid
            }

            if (!DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(pPlayerEntity), sizeof(uintptr_t)))
            {
                LogOnce(logger, LOG_WARNING, "CEntityVTableReadFail", "TpvUtils: Memory for CEntity VTable ptr is not readable.");
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            uintptr_t vtable_CEntity = *(uintptr_t *)pPlayerEntity;
            if (vtable_CEntity == 0 || vtable_CEntity != (GlobalState::g_ModuleBase + 0x40508D8))
            { // CEntity vftable RVA
                logger.log(LOG_DEBUG, "TpvUtils: CEntity vtable invalid or mismatch (Attempt " + std::to_string(currentRetry) + "). Got: " + format_address(vtable_CEntity));
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            ResetLogOnce("CEntityVTableReadFail");

            if (!DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(vtable_CEntity + 74 * sizeof(uintptr_t)), sizeof(uintptr_t)))
            {
                LogOnce(logger, LOG_WARNING, "VTableFunc74ReadFail", "TpvUtils: Memory for CEntity::vtable[74] func ptr is not readable.");
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            uintptr_t pFunc_v74 = *(uintptr_t *)(vtable_CEntity + 74 * sizeof(uintptr_t));
            if (pFunc_v74 == 0)
            {
                LogOnce(logger, LOG_WARNING, "VTableFunc74Null", "TpvUtils: CEntity::vtable[74] (GetProxyManager func ptr) is NULL.");
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            CEntity_VTableFunc74_Type func_CEntity_GetProxyManager = reinterpret_cast<CEntity_VTableFunc74_Type>(pFunc_v74);
            ResetLogOnce("VTableFunc74ReadFail");
            ResetLogOnce("VTableFunc74Null");

            uintptr_t compMgrOrProxyListBase_Addr = func_CEntity_GetProxyManager(pPlayerEntity, 0);
            if (compMgrOrProxyListBase_Addr == 0)
            {
                logger.log(LOG_DEBUG, "TpvUtils: CEntity::GetProxyManager call returned 0 (Attempt " + std::to_string(currentRetry) + ").");
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            if (!s_compMgrLogged)
            {
                logger.log(LOG_INFO, "TpvUtils: CompMgrOrProxyListBase_Addr = " + format_address(compMgrOrProxyListBase_Addr));
                s_compMgrLogged = true;
            }

            GetActualComponentFromManager_Type func_getActualComponent =
                reinterpret_cast<GetActualComponentFromManager_Type>(GlobalState::g_ModuleBase + Rvas::GetActualComponentFromManager_RVA);
            if (!DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(func_getActualComponent), 1))
            { // Check if code is readable
                logger.log(LOG_ERROR, "TpvUtils: GetActualComponentFromManager_RVA points to invalid/unreadable code.");
                return false;
            }

            uintptr_t param1_for_getActual = compMgrOrProxyListBase_Addr - 0x50;
            const unsigned int PROXY_ID_CHARACTER = 0;

            uintptr_t pCharInst = func_getActualComponent(param1_for_getActual, PROXY_ID_CHARACTER);
            GlobalState::g_Player_CCharInstance = pCharInst;
            if (pCharInst == 0)
            {
                logger.log(LOG_DEBUG, "TpvUtils: GetActualComponent (for CCharInstance) returned 0 (Attempt " + std::to_string(currentRetry) + ").");
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            if (!s_charInstLogged)
            {
                logger.log(LOG_INFO, "TpvUtils: P_CharInst_this = " + format_address(pCharInst));
                // VTable verification for CCharInstance (Optional but good)
                s_charInstLogged = true;
            }

            if (!DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(pCharInst + Offsets::CCharInstance_To_CDefaultSkeleton), sizeof(uintptr_t)))
            {
                LogOnce(logger, LOG_WARNING, "CIDefSkelReadFail", "TpvUtils: Memory for P_DefSkel_this (from CI+A70) ptr is not readable.");
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            uintptr_t potential_P_DefSkel_this = *(uintptr_t *)(pCharInst + Offsets::CCharInstance_To_CDefaultSkeleton);

            if (potential_P_DefSkel_this == 0)
            {
                logger.log(LOG_DEBUG, "TpvUtils: P_DefSkel_this from CI+A70 is NULL (Attempt " + std::to_string(currentRetry) + "). Retrying...");
                GlobalState::g_Player_CDefaultSkeleton = 0;
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            ResetLogOnce("CIDefSkelReadFail"); // Reached here, so address itself was readable.
            if (!s_defSkelDirectLogged)
            {
                logger.log(LOG_INFO, "TpvUtils: P_DefSkel_this (from CI+A70) = " + format_address(potential_P_DefSkel_this));
                s_defSkelDirectLogged = true;
            }

            if (!DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(potential_P_DefSkel_this), sizeof(uintptr_t)))
            {
                LogOnce(logger, LOG_WARNING, "DefSkelVTableReadFail", "TpvUtils: Memory for CDefaultSkeleton VTable is not readable from P_DefSkel_this.");
                GlobalState::g_Player_CDefaultSkeleton = 0;
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
            uintptr_t vtable_CDefSkel = *(uintptr_t *)potential_P_DefSkel_this;

            uintptr_t expectedVTable_Concrete = GlobalState::g_ModuleBase + 0x3A86390;
            // The previous log confirmed 0x3A86390 was the correct vtable RVA

            if (vtable_CDefSkel == expectedVTable_Concrete)
            {
                GlobalState::g_Player_CDefaultSkeleton = potential_P_DefSkel_this;
                logger.log(LOG_INFO, "TpvUtils: SUCCESS! P_DefSkel_this confirmed: " + format_address(GlobalState::g_Player_CDefaultSkeleton) + " (VTable: " + format_address(vtable_CDefSkel) + ")");

                // --- NEW: CACHE FUNCTION POINTERS ---
                if (!DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(vtable_CDefSkel + 7 * sizeof(uintptr_t)), sizeof(uintptr_t)) ||
                    !DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(vtable_CDefSkel + 13 * sizeof(uintptr_t)), sizeof(uintptr_t)))
                {
                    logger.log(LOG_ERROR, "TpvUtils: Failed to read function pointers from CDefaultSkeleton vtable even though vtable matched!");
                    GlobalState::g_Player_CDefaultSkeleton = 0; // Invalidate if we can't get functions
                    if (currentRetry < maxRetries)
                        std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                    continue;
                }

                GlobalState::g_FuncAddr_GetBoneIndexByName = *(uintptr_t *)(vtable_CDefSkel + 7 * sizeof(uintptr_t));
                GlobalState::g_FuncAddr_GetBoneWorldTransform = *(uintptr_t *)(vtable_CDefSkel + 13 * sizeof(uintptr_t));

                if (GlobalState::g_FuncAddr_GetBoneIndexByName == 0 || GlobalState::g_FuncAddr_GetBoneWorldTransform == 0 ||
                    !DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(GlobalState::g_FuncAddr_GetBoneIndexByName), 1) || // Check code readability
                    !DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(GlobalState::g_FuncAddr_GetBoneWorldTransform), 1))
                {
                    logger.log(LOG_ERROR, "TpvUtils: Cached function pointers for bone operations are NULL or invalid.");
                    logger.log(LOG_DEBUG, "  g_FuncAddr_GetBoneIndexByName: " + format_address(GlobalState::g_FuncAddr_GetBoneIndexByName));
                    logger.log(LOG_DEBUG, "  g_FuncAddr_GetBoneWorldTransform: " + format_address(GlobalState::g_FuncAddr_GetBoneWorldTransform));
                    GlobalState::g_Player_CDefaultSkeleton = 0; // Invalidate if functions are bad
                    GlobalState::g_FuncAddr_GetBoneIndexByName = 0;
                    GlobalState::g_FuncAddr_GetBoneWorldTransform = 0;
                    if (currentRetry < maxRetries)
                        std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                    continue;
                }

                logger.log(LOG_INFO, "TpvUtils: GetBoneIndexByName Addr CACHED: " + format_address(GlobalState::g_FuncAddr_GetBoneIndexByName));
                logger.log(LOG_INFO, "TpvUtils: GetBoneWorldTransform Addr CACHED: " + format_address(GlobalState::g_FuncAddr_GetBoneWorldTransform));
                logger.log(LOG_INFO, "TpvUtils: Player animation pointers chain successfully resolved & functions cached!");
                return true;
            }
            // ... (else if for IDefaultSkeleton or failure condition and retry remains the same) ...
            else
            { // Neither CDefaultSkeleton concrete vtable nor IDefaultSkeleton vtable was found (or other conditions)
                logger.log(LOG_WARNING, "TpvUtils: P_DefSkel_this (" + format_address(potential_P_DefSkel_this) + ") VTable mismatch. Got " + format_address(vtable_CDefSkel) +
                                            // ... update this log with current expected vtables
                                            " (Attempt " + std::to_string(currentRetry) + "). Retrying...");
                GlobalState::g_Player_CDefaultSkeleton = 0;
                if (currentRetry < maxRetries)
                    std::this_thread::sleep_for(std::chrono::milliseconds(retryDelayMs));
                continue;
            }
        } // End of retry loop

        logger.log(LOG_ERROR, "TpvUtils: Failed to resolve & cache CDefaultSkeleton functions after " + std::to_string(maxRetries) + " retries.");
        GlobalState::g_Player_CDefaultSkeleton = 0;
        GlobalState::g_FuncAddr_GetBoneIndexByName = 0;
        GlobalState::g_FuncAddr_GetBoneWorldTransform = 0;
        return false;
    }

    bool GetHeadBoneWorldPosition(DirectX::XMFLOAT3 &outPosition)
    {
        Logger &logger = Logger::getInstance();
        // Static bools for one-time logging of persistent errors within this frame function
        static bool s_ghbwp_defSkelNullLogged = false;
        static bool s_ghbwp_boneFuncNullLogged = false;
        static bool s_ghbwp_boneNotFoundLogged = false;
        static bool s_ghbwp_transformNullLogged = false;

        // Use cached function pointers
        if (GlobalState::g_Player_CDefaultSkeleton == 0 ||
            GlobalState::g_FuncAddr_GetBoneIndexByName == 0 ||
            GlobalState::g_FuncAddr_GetBoneWorldTransform == 0)
        {
            if (!s_ghbwp_defSkelNullLogged && GlobalState::g_Player_CDefaultSkeleton == 0)
            {
                logger.log(LOG_TRACE, "GetHeadBoneWorldPosition: CDefaultSkeleton pointer (GlobalState) is null.");
                s_ghbwp_defSkelNullLogged = true;
            }
            if (!s_ghbwp_boneFuncNullLogged && (GlobalState::g_FuncAddr_GetBoneIndexByName == 0 || GlobalState::g_FuncAddr_GetBoneWorldTransform == 0))
            {
                logger.log(LOG_TRACE, "GetHeadBoneWorldPosition: Cached bone function pointers are null.");
                s_ghbwp_boneFuncNullLogged = true;
            }
            return false;
        }
        if (s_ghbwp_defSkelNullLogged)
            s_ghbwp_defSkelNullLogged = false;
        if (s_ghbwp_boneFuncNullLogged)
            s_ghbwp_boneFuncNullLogged = false;

        GetBoneIndexByName_Type func_getIndex = reinterpret_cast<GetBoneIndexByName_Type>(GlobalState::g_FuncAddr_GetBoneIndexByName);
        GetBoneWorldTransform_Type func_getTransform = reinterpret_cast<GetBoneWorldTransform_Type>(GlobalState::g_FuncAddr_GetBoneWorldTransform);

        logger.log(LOG_DEBUG, "GetHeadBoneWorldPosition: g_FuncAddr_GetBoneWorldTransform=" + format_address(GlobalState::g_FuncAddr_GetBoneWorldTransform));

        logger.log(LOG_DEBUG, "GetHeadBoneWorldPosition: g_FuncAddr_GetBoneIndexByName=" + format_address(GlobalState::g_FuncAddr_GetBoneIndexByName));

        const char *headBoneName = "head";
        int boneIndex = func_getIndex(GlobalState::g_Player_CDefaultSkeleton, headBoneName);

        if (boneIndex < 0 || boneIndex == 0xFFFF)
        {
            if (!s_ghbwp_boneNotFoundLogged)
            {
                logger.log(LOG_WARNING, "GetHeadBoneWorldPosition: Bone '" + std::string(headBoneName) + "' not found (index: " + std::to_string(boneIndex) + ").");
                s_ghbwp_boneNotFoundLogged = true;
            }
            return false;
        }
        if (s_ghbwp_boneNotFoundLogged)
            s_ghbwp_boneNotFoundLogged = false;

        logger.log(LOG_DEBUG, "GetHeadBoneWorldPosition: BoneIndex=" + boneIndex);

        // uintptr_t pTransformDataRaw = func_getTransform(GlobalState::g_Player_CDefaultSkeleton, boneIndex);

        // logger.log(LOG_DEBUG, "GetHeadBoneWorldPosition: pTransformDataRaw (" + format_address(pTransformDataRaw) + ")");

        // if (pTransformDataRaw == 0 || pTransformDataRaw < 0x10000 || !DMK::Memory::isMemoryReadable(reinterpret_cast<const void *>(pTransformDataRaw), sizeof(Matrix34_placeholder)))
        // {
        //     if (!s_ghbwp_transformNullLogged)
        //     {
        //         logger.log(LOG_WARNING, "GetHeadBoneWorldPosition: GetBoneTransform returned invalid pointer (" + format_address(pTransformDataRaw) + ") for bone '" + std::string(headBoneName) + "'.");
        //         s_ghbwp_transformNullLogged = true;
        //     }
        //     return false;
        // }
        // if (s_ghbwp_transformNullLogged)
        //     s_ghbwp_transformNullLogged = false;

        // Matrix34_placeholder *transformMatrix = reinterpret_cast<Matrix34_placeholder *>(pTransformDataRaw);

        // // !!! CRITICAL: VALIDATE THIS MATRIX LAYOUT AND POSITION EXTRACTION !!!
        // // Using Column-Major standard: Position is the 4th column (indices 9, 10, 11 in a flat float[12])
        // outPosition.x = transformMatrix->m[9];
        // outPosition.y = transformMatrix->m[10];
        // outPosition.z = transformMatrix->m[11];

        return true;
    }
} // namespace TpvUtils
