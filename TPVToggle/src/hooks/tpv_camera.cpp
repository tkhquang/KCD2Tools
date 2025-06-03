// tpv_camera.cpp
#include "tpv_camera.hpp"
#include "tpv_utils.hpp" // For TpvUtils::GetHeadBoneWorldPosition and SafeRead
#include "global_state.hpp"
#include "config.hpp"
#include <DetourModKit.hpp> // Central header, includes DMK::Memory, DMK::String, DMK::Logger etc.
#include <windows.h>        // For GetAsyncKeyState (still needed)
#include <DirectXMath.h>    // For vector math

// Use DMK namespaces and DirectX for convenience
using namespace DetourModKit;
using namespace DetourModKit::String;
using namespace DirectX;

// Define the global TPV config instance
TpvCamera::TpvConfig TpvCamera::g_tpvConfig;

namespace TpvCamera
{
    // --- Placeholder RVAs --- (Assumed to be defined correctly in your project)
    namespace Rvas
    {
        const uintptr_t CameraUpdateFunction_RVA = 0x413E98;
    }

    // --- Function Typedefs ---
    typedef void(__fastcall *CameraUpdateFunc)(uintptr_t pViewSystem_or_CView, uintptr_t pSecondParam_If_Any);
    static CameraUpdateFunc fpCameraUpdateOriginal = nullptr;

    // --- Game Structure Offsets ---
    namespace Offsets
    {
        const ptrdiff_t ViewSystem_To_PuVar4_Container = 0x20;
        const ptrdiff_t ViewLink_To_ViewCamera_MemberOffset = 0x28; // 5 * sizeof(uintptr_t)

        const ptrdiff_t CameraStruct_PosX = 0x10C;
        const ptrdiff_t CameraStruct_PosY = 0x110; // Note: Re-verify if Ghidra (plVar5+0x22) implies byte or element offset. Assuming byte for now.
        const ptrdiff_t CameraStruct_PosZ = 0x114;
        // Based on pfVar1 = (float *)(plVar5 + 0x1d); if plVar5 is longlong*, byte offset 0x1D*8 = 0xE8
        const ptrdiff_t CameraStruct_OrientationMatrixBase = 0xE8;

        const ptrdiff_t CPlayer_To_PlayerViewData = 0x238;
        const ptrdiff_t PlayerViewData_To_ViewQuatFinal = 0x24;
    }

    static std::string g_cameraUpdateHookId;
    static bool g_tpvActive = false;
    static bool s_tpvPointersFullyInitialized = false;

    void __fastcall Detour_CameraUpdate(uintptr_t pViewSystem_param1, uintptr_t pSecondParam_If_Any)
    {
        DMKLogger &logger = DMKLogger::getInstance(); // Use DMK alias or direct
        static uint64_t s_frameCount = 0;
        s_frameCount++;
        static bool s_initFailLoggedThisSession = false; // For one-time logging of init failure within this hook
        static bool s_camErrorLoggedThisSession = false; // For one-time logging of camera update failure
        static bool s_headPosErrorLoggedThisSession = false;
        static bool s_quatErrorLoggedThisSession = false;
        static bool s_posWriteErrorLoggedThisSession = false;
        static bool s_rotWriteErrorLoggedThisSession = false;

        if (!s_tpvPointersFullyInitialized && GlobalState::g_localPlayerEntity != 0)
        {
            if (GlobalState::g_Player_CDefaultSkeleton != 0) // Check if TpvUtils::Initialize successfully set this
            {
                s_tpvPointersFullyInitialized = true;
                logger.log(LOG_INFO, "Detour_CameraUpdate: Verified TPV animation pointers are initialized.");
                s_initFailLoggedThisSession = false; // Reset if previously failed then succeeded
            }
            else
            {
                if (!s_initFailLoggedThisSession)
                {
                    logger.log(LOG_ERROR, "Detour_CameraUpdate: TPV Animation pointers are NOT initialized (init in core_hooks likely failed/timed out). TPV will not work this session.");
                    s_initFailLoggedThisSession = true;
                }
            }
        }

        // Toggle Logic
        static bool lastToggleKeyState = false;
        bool currentToggleKeyState = (GetAsyncKeyState(g_tpvConfig.toggleTpvKey) & 0x8000) != 0;
        if (currentToggleKeyState && !lastToggleKeyState)
        {
            g_tpvActive = !g_tpvActive;
            if (g_tpvConfig.enableTpv)
            {
                logger.log(LOG_INFO, "TPV Camera Toggled: " + std::string(g_tpvActive ? "ON" : "OFF") +
                                         (s_tpvPointersFullyInitialized ? "" : " (Warning: Anim Pointers Not Ready)"));
            }
        }
        lastToggleKeyState = currentToggleKeyState;

        // Call original first
        if (fpCameraUpdateOriginal)
        {
            fpCameraUpdateOriginal(pViewSystem_param1, pSecondParam_If_Any);
        }
        else
        {
            static bool nullOrigLogged = false;
            if (!nullOrigLogged)
            {
                logger.log(LOG_ERROR, "TpvCameraHook: Original CameraUpdate function pointer is NULL!");
                nullOrigLogged = true;
            }
            return;
        }

        if (!g_tpvConfig.enableTpv || !g_tpvActive || !s_tpvPointersFullyInitialized || GlobalState::g_localPlayerEntity == 0)
        {
            return; // TPV disabled, or pointers not ready, or player not found
        }

        // DERIVE pCameraBeingUpdated
        uintptr_t pCameraBeingUpdated = 0;
        uintptr_t puVar4_container_val = 0;
        uintptr_t pLocalRes18_0_val = 0;

        if (pViewSystem_param1)
        {
            // Step 1: Get param_1[4] (puVar4 container)
            if (!DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(pViewSystem_param1 + Offsets::ViewSystem_To_PuVar4_Container), sizeof(uintptr_t)))
            {
                if (!s_camErrorLoggedThisSession)
                    logger.log(LOG_WARNING, "Detour_CameraUpdate: ViewSystem_To_PuVar4_Container is unreadable.");
                s_camErrorLoggedThisSession = true;
                return;
            }
            puVar4_container_val = *(uintptr_t *)(pViewSystem_param1 + Offsets::ViewSystem_To_PuVar4_Container);

            // Step 2: Get *puVar4 (local_res18[0])
            if (puVar4_container_val && DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(puVar4_container_val), sizeof(uintptr_t)))
            {
                pLocalRes18_0_val = *(uintptr_t *)puVar4_container_val;
            }
            else
            {
                if (!s_camErrorLoggedThisSession)
                    logger.log(LOG_TRACE, "Detour_CameraUpdate: puVar4_container_val is null or unreadable.");
                s_camErrorLoggedThisSession = true;
                return;
            }

            // Step 3: Get local_res18[0][5] (pCameraBeingUpdated)
            if (pLocalRes18_0_val && DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(pLocalRes18_0_val + Offsets::ViewLink_To_ViewCamera_MemberOffset), sizeof(uintptr_t)))
            {
                pCameraBeingUpdated = *(uintptr_t *)(pLocalRes18_0_val + Offsets::ViewLink_To_ViewCamera_MemberOffset);
            }
            else
            {
                if (!s_camErrorLoggedThisSession)
                    logger.log(LOG_TRACE, "Detour_CameraUpdate: pLocalRes18_0_val is null or unreadable for camera struct.");
                s_camErrorLoggedThisSession = true;
                return;
            }
        }

        if (!pCameraBeingUpdated || !DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(pCameraBeingUpdated), 0x120)) // Increased check size
        {
            if (!s_camErrorLoggedThisSession)
            {
                logger.log(LOG_WARNING, "Detour_CameraUpdate: pCameraBeingUpdated derived as NULL or invalid memory. TPV cannot update. pViewSys: " + format_address(pViewSystem_param1) + ", pCamUpd: " + format_address(pCameraBeingUpdated));
                s_camErrorLoggedThisSession = true;
            }
            return;
        }
        if (s_camErrorLoggedThisSession)
            s_camErrorLoggedThisSession = false; // Reset if valid

        XMFLOAT3 headPosition;
        if (!TpvUtils::GetHeadBoneWorldPosition(headPosition)) // This now uses GlobalState::g_Player_CDefaultSkeleton internally
        {
            if (!s_headPosErrorLoggedThisSession)
            { /*logger.log(LOG_TRACE, "Detour_CameraUpdate: Failed to get head position for TPV.");*/
                s_headPosErrorLoggedThisSession = true;
            }
            return;
        }
        if (s_headPosErrorLoggedThisSession)
            s_headPosErrorLoggedThisSession = false;

        uintptr_t pPlayer = GlobalState::g_localPlayerEntity;
        uintptr_t pPlayerViewData = pPlayer + Offsets::CPlayer_To_PlayerViewData;
        if (!pPlayerViewData)
        {
            if (!s_quatErrorLoggedThisSession)
            {
                logger.log(LOG_TRACE, "Detour_CameraUpdate: pPlayerViewData is null.");
                s_quatErrorLoggedThisSession = true;
            }
            return;
        }

        XMFLOAT4 playerAimQuat;
        float *pQuatFloats = reinterpret_cast<float *>(pPlayerViewData + Offsets::PlayerViewData_To_ViewQuatFinal);
        if (!DMK::Memory::isMemoryReadable(pQuatFloats, sizeof(XMFLOAT4)))
        {
            if (!s_quatErrorLoggedThisSession)
            {
                logger.log(LOG_TRACE, "Detour_CameraUpdate: Aim Quaternion pointer is unreadable.");
                s_quatErrorLoggedThisSession = true;
            }
            return;
        }
        playerAimQuat = *reinterpret_cast<XMFLOAT4 *>(pQuatFloats);
        if (playerAimQuat.w == 0.0f && playerAimQuat.x == 0.0f && playerAimQuat.y == 0.0f && playerAimQuat.z == 0.0f)
        {
            playerAimQuat.w = 1.0f;
        }
        if (s_quatErrorLoggedThisSession)
            s_quatErrorLoggedThisSession = false;

        // Calculate TPV Camera Transform
        XMMATRIX viewRotationMatrix = XMMatrixRotationQuaternion(XMLoadFloat4(&playerAimQuat));
        XMVECTOR worldYForward = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMVECTOR worldZUp = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        XMVECTOR camForward = XMVector3Normalize(XMVector3TransformNormal(worldYForward, viewRotationMatrix));
        XMVECTOR camRight = XMVector3Normalize(XMVector3Cross(worldZUp, camForward));
        XMVECTOR camUp = XMVector3Normalize(XMVector3Cross(camForward, camRight));

        XMVECTOR headPosVec = XMLoadFloat3(&headPosition);
        XMVECTOR desiredOffsetRelative = XMVectorScale(camForward, -g_tpvConfig.cameraDistance);
        desiredOffsetRelative = XMVectorAdd(desiredOffsetRelative, XMVectorScale(camUp, g_tpvConfig.cameraHeightOffset));
        desiredOffsetRelative = XMVectorAdd(desiredOffsetRelative, XMVectorScale(camRight, g_tpvConfig.cameraRightOffset));
        XMVECTOR finalCamPosVec = XMVectorAdd(headPosVec, desiredOffsetRelative);

        XMFLOAT3 finalCamPos;
        XMStoreFloat3(&finalCamPos, finalCamPosVec);

        // Write new position to Game's Camera Structure
        if (!DMK::Memory::isMemoryWritable(reinterpret_cast<void *>(pCameraBeingUpdated + Offsets::CameraStruct_PosX), sizeof(float)) ||
            !DMK::Memory::isMemoryWritable(reinterpret_cast<void *>(pCameraBeingUpdated + Offsets::CameraStruct_PosY), sizeof(float)) ||
            !DMK::Memory::isMemoryWritable(reinterpret_cast<void *>(pCameraBeingUpdated + Offsets::CameraStruct_PosZ), sizeof(float)))
        {
            if (!s_posWriteErrorLoggedThisSession)
            {
                logger.log(LOG_ERROR, "Detour_CameraUpdate: Camera position members are not writable!");
                s_posWriteErrorLoggedThisSession = true;
            }
            return;
        }
        s_posWriteErrorLoggedThisSession = false; // Reset if writable

        *(float *)(pCameraBeingUpdated + Offsets::CameraStruct_PosX) = finalCamPos.x;
        *(float *)(pCameraBeingUpdated + Offsets::CameraStruct_PosY) = finalCamPos.y;
        *(float *)(pCameraBeingUpdated + Offsets::CameraStruct_PosZ) = finalCamPos.z;

        // Write Orientation Matrix
        uintptr_t pMatrixWriteBase = pCameraBeingUpdated + Offsets::CameraStruct_OrientationMatrixBase;
        if (!DMK::Memory::isMemoryWritable(reinterpret_cast<void *>(pMatrixWriteBase), sizeof(float) * 9))
        { // Check 3x3 part
            if (!s_rotWriteErrorLoggedThisSession)
            {
                logger.log(LOG_ERROR, "Detour_CameraUpdate: Camera rotation matrix base is not writable!");
                s_rotWriteErrorLoggedThisSession = true;
            }
            return;
        }
        s_rotWriteErrorLoggedThisSession = false; // Reset

        // Write 3x3 Rotation (Column Major: Right, Forward, Up for CryEngine camera space relative to world)
        // Col 0 (Right - Game's X-Basis for Camera View)
        ((float *)pMatrixWriteBase)[0] = XMVectorGetX(camRight);
        ((float *)pMatrixWriteBase)[1] = XMVectorGetY(camRight);
        ((float *)pMatrixWriteBase)[2] = XMVectorGetZ(camRight);

        // Col 1 (Forward - Game's Y-Basis for Camera View)
        ((float *)pMatrixWriteBase)[3] = XMVectorGetX(camForward); // If these are indeed distinct columns in a Matrix33 part of 34
        ((float *)pMatrixWriteBase)[4] = XMVectorGetY(camForward);
        ((float *)pMatrixWriteBase)[5] = XMVectorGetZ(camForward);

        // Col 2 (Up - Game's Z-Basis for Camera View)
        ((float *)pMatrixWriteBase)[6] = XMVectorGetX(camUp);
        ((float *)pMatrixWriteBase)[7] = XMVectorGetY(camUp);
        ((float *)pMatrixWriteBase)[8] = XMVectorGetZ(camUp);
    }

    // ... (initializeTpvCameraHooks and cleanupTpvCameraHooks - ensure RVAs are defined for them to use) ...
    // Ensure CameraUpdateFunction_RVA in this file's Rvas namespace is correctly used in initializeTpvCameraHooks
    bool initializeTpvCameraHooks(uintptr_t moduleBase, size_t moduleSize)
    {
        Logger &logger = Logger::getInstance();
        DMKHookManager &hookManager = HookManager::getInstance();
        logger.log(LOG_INFO, "TpvCamera: Initializing TPV camera hooks...");

        if (GlobalState::g_Player_CDefaultSkeleton == 0 && s_tpvPointersFullyInitialized)
        { // Check if init failed
            logger.log(LOG_WARNING, "TpvCamera: CDefaultSkeleton not initialized during prior phase. TPV functionality may be impaired.");
            // s_tpvPointersFullyInitialized will likely remain false preventing TPV logic from running.
        }

        uintptr_t cameraUpdateTarget = moduleBase + Rvas::CameraUpdateFunction_RVA;
        if (Rvas::CameraUpdateFunction_RVA == 0x0 || !DMK::Memory::isMemoryReadable(reinterpret_cast<void *>(cameraUpdateTarget), 1))
        {
            logger.log(LOG_ERROR, "TpvCamera: CameraUpdateFunction_RVA (for FUN_180413e98) is invalid or points to unreadable memory. RVA: " + format_hex(Rvas::CameraUpdateFunction_RVA));
            return false;
        }

        g_cameraUpdateHookId = hookManager.create_inline_hook(
            "TpvCameraUpdateHook",
            cameraUpdateTarget,
            reinterpret_cast<void *>(&Detour_CameraUpdate),
            reinterpret_cast<void **>(&fpCameraUpdateOriginal));

        if (g_cameraUpdateHookId.empty() || !fpCameraUpdateOriginal)
        {
            logger.log(LOG_ERROR, "TpvCamera: Failed to create CameraUpdate hook at " + format_address(cameraUpdateTarget));
            return false;
        }
        logger.log(LOG_INFO, "TpvCamera: CameraUpdate hook installed at " + format_address(cameraUpdateTarget));
        s_tpvPointersFullyInitialized = false;
        return true;
    }

    void cleanupTpvCameraHooks()
    {
        DMKLogger &logger = Logger::getInstance();
        DMKHookManager &hookManager = HookManager::getInstance();
        if (!g_cameraUpdateHookId.empty())
        {
            if (hookManager.remove_hook(g_cameraUpdateHookId))
            {
                logger.log(LOG_INFO, "TpvCamera: Hook '" + g_cameraUpdateHookId + "' removed successfully.");
            }
            else
            {
                logger.log(LOG_WARNING, "TpvCamera: Failed to remove hook '" + g_cameraUpdateHookId + "'.");
            }
            g_cameraUpdateHookId.clear();
            fpCameraUpdateOriginal = nullptr;
            s_tpvPointersFullyInitialized = false; // Reset state
        }
        else
        {
            logger.log(LOG_INFO, "TpvCamera: No TPV camera hook was active to cleanup.");
        }
    }

    void updateTpvSettings()
    {
        // This would be called if, for example, you implemented runtime changes to g_tpvConfig via an overlay or console command.
        // Currently, g_tpvConfig is read directly each frame in Detour_CameraUpdate.
        DMKLogger::getInstance().log(LOG_INFO, "TpvCamera::updateTpvSettings called (e.g., after config reload/hotkey).");
    }

} // namespace TpvCamera
