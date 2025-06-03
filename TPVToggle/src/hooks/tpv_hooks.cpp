#include "tpv_hooks.hpp"
#include "global_state.hpp"
// #include "constants.hpp" // If RVA_CameraFirstPerson_UpdateView is there
#include <DetourModKit.hpp>
#include <DirectXMath.h>
#include <windows.h>

using namespace DetourModKit;
// ... other using namespaces ...

// Define the trampoline for the new hook
CameraFirstPerson_UpdateView_Func fpCameraFirstPerson_UpdateView_Original = nullptr;
static std::string g_cameraFirstPersonUpdateHookId; // New ID for this hook

// Offsets within the pSViewParams_out structure (Pose*)
constexpr ptrdiff_t POSE_POSITION_OFFSET = 0x0;  // Start of Position (Vec3)
constexpr ptrdiff_t POSE_ROTATION_OFFSET = 0x0C; // Start of Rotation (Quaternion XYZW)
constexpr size_t POSE_STRUCT_MIN_SIZE = 0x1C;    // Size of Vec3 (12) + Quaternion (16) = 28 bytes

void __fastcall Detour_CameraFirstPerson_UpdateView(uintptr_t pThisCamera, uintptr_t pSViewParams_out /* Treat as Pose* */)
{
    DMKLogger &logger = DMKLogger::getInstance();
    // Static bool for one-time logging of critical errors per session to avoid spam
    static bool s_loggedCriticalErrorThisSession = false;

    // 1. Call the original function FIRST.
    // This populates pSViewParams_out with the 1P camera data.
    if (fpCameraFirstPerson_UpdateView_Original)
    {
        fpCameraFirstPerson_UpdateView_Original(pThisCamera, pSViewParams_out);
    }
    else
    {
        if (!s_loggedCriticalErrorThisSession)
        {
            logger.log(DMKLogLevel::LOG_ERROR, "Detour_CFPV_UpdateView: Original function pointer is NULL! Hook is broken.");
            s_loggedCriticalErrorThisSession = true;
        }
        return;
    }

    // // 2. Check if TPV should be active
    // if (!GlobalState::g_tpvEnabled || GlobalState::g_tpvForceDisabled)
    // {
    //     return;
    // }

    // 3. Ensure essential global pointers are valid
    if (!GlobalState::g_localPlayerEntity)
    {
        // This can happen normally during loading screens, so keep log level low or remove for release
        // logger.log(DMKLogLevel::LOG_TRACE, "Detour_CFPV_UpdateView: TPV enabled but local player entity is null.");
        return;
    }

    if (!GlobalState::g_localPlayerCEntity)
    {
        // Attempt to cache CEntity pointer if not already done
        uintptr_t pPlayerThis = GlobalState::g_localPlayerEntity;                       // This is C_Player*
        GlobalState::g_localPlayerCEntity = *(uintptr_t *)((char *)pPlayerThis + 0x38); // CEntity* is at C_Player+0x38
        if (!GlobalState::g_localPlayerCEntity)
        {
            // logger.log(DMKLogLevel::LOG_WARNING, "Detour_CFPV_UpdateView: Failed to get CEntity from C_Player for TPV.");
            return; // Can't proceed without CEntity for anchor
        }
    }
    uintptr_t pCEntity = GlobalState::g_localPlayerCEntity;

    // 4. Validate and Read from pSViewParams_out (which we treat as a Pose structure)
    char *pPoseBase = reinterpret_cast<char *>(pSViewParams_out);
    if (!DMKMemory::isMemoryReadable(pPoseBase, POSE_STRUCT_MIN_SIZE))
    {
        // logger.log(DMKLogLevel::LOG_TRACE, "Detour_CFPV_UpdateView: pSViewParams_out (Pose data) not readable.");
        return;
    }

    DirectX::XMFLOAT3 originalFpvPos;
    DirectX::XMFLOAT4 originalFpvRotQuat;

    // Read Pos from offset POSE_POSITION_OFFSET (0x0)
    originalFpvPos.x = *(float *)(pPoseBase + POSE_POSITION_OFFSET + 0x0);
    originalFpvPos.y = *(float *)(pPoseBase + POSE_POSITION_OFFSET + 0x4);
    originalFpvPos.z = *(float *)(pPoseBase + POSE_POSITION_OFFSET + 0x8);

    // Read Quat from offset POSE_ROTATION_OFFSET (0xC)
    originalFpvRotQuat.x = *(float *)(pPoseBase + POSE_ROTATION_OFFSET + 0x0);
    originalFpvRotQuat.y = *(float *)(pPoseBase + POSE_ROTATION_OFFSET + 0x4);
    originalFpvRotQuat.z = *(float *)(pPoseBase + POSE_ROTATION_OFFSET + 0x8);
    originalFpvRotQuat.w = *(float *)(pPoseBase + POSE_ROTATION_OFFSET + 0xC);

    // 5. Convert originalFpvRotQuat to FPV's world space basis vectors
    DirectX::XMMATRIX mOriginalFpvRotation = DirectX::XMMatrixRotationQuaternion(XMLoadFloat4(&originalFpvRotQuat));
    DirectX::XMFLOAT3 fpvRight, fpvUp, fpvForward;
    // Assuming standard DirectX coordinate system: X=Right, Y=Up, Z=Forward (camera looks along +Z)
    XMStoreFloat3(&fpvRight, mOriginalFpvRotation.r[0]);
    XMStoreFloat3(&fpvUp, mOriginalFpvRotation.r[1]);
    XMStoreFloat3(&fpvForward, mOriginalFpvRotation.r[2]);

    // 6. Read Entity's World Transform for anchor position and entity's up vector
    float *tmFloats = reinterpret_cast<float *>((char *)pCEntity + 0x58); // CEntity+0x58, Row-Major
    DirectX::XMFLOAT3 entityPosition, entityUpVector;

    // Check enough memory for Pos (indices 3,7,11) and Up vector (indices 8,9,10)
    if (DMKMemory::isMemoryReadable(tmFloats, sizeof(float) * 12)) // Check all 12 floats of the matrix
    {
        entityPosition = DirectX::XMFLOAT3(tmFloats[3], tmFloats[7], tmFloats[11]); // Pos from RowMajor Mat
        entityUpVector = DirectX::XMFLOAT3(tmFloats[8], tmFloats[9], tmFloats[10]); // Entity's Z-axis (Up) from RowMajor Mat (Row 2)
    }
    else
    {
        // logger.log(DMKLogLevel::LOG_WARNING, "Detour_CFPV_UpdateView: CEntity matrix for TPV anchor not fully readable.");
        return;
    }

    // --- 7. Calculate TPV Camera Position ---
    DirectX::XMVECTOR xmf_entityWorldPos = XMLoadFloat3(&entityPosition);
    DirectX::XMVECTOR xmf_entityWorldUp = XMLoadFloat3(&entityUpVector); // Entity's own world Up vector

    DirectX::XMVECTOR xmf_fpvForwardVec = XMLoadFloat3(&fpvForward); // Use FPV's world forward for offset direction
    DirectX::XMVECTOR xmf_fpvUpVec = XMLoadFloat3(&fpvUp);           // Use FPV's world up for vertical offset relative to aim
    DirectX::XMVECTOR xmf_fpvRightVec = XMLoadFloat3(&fpvRight);     // Use FPV's world right for horizontal offset

    // Anchor point: Entity's root position, adjusted upwards along its own Up vector.
    DirectX::XMVECTOR xmf_tpvAnchorPos = DirectX::XMVectorAdd(xmf_entityWorldPos,
                                                              DirectX::XMVectorScale(xmf_entityWorldUp, GlobalState::g_config.tpv_anchor_height_adjust));

    // Calculate final TPV position by offsetting from the anchor using FPV AIMING vectors
    DirectX::XMVECTOR xmf_finalTpvCamPos = xmf_tpvAnchorPos;
    xmf_finalTpvCamPos = DirectX::XMVectorSubtract(xmf_finalTpvCamPos,
                                                   DirectX::XMVectorScale(xmf_fpvForwardVec, GlobalState::g_config.tpv_offset_behind));
    xmf_finalTpvCamPos = DirectX::XMVectorAdd(xmf_finalTpvCamPos,
                                              DirectX::XMVectorScale(xmf_fpvUpVec, GlobalState::g_config.tpv_offset_up));
    xmf_finalTpvCamPos = DirectX::XMVectorAdd(xmf_finalTpvCamPos,
                                              DirectX::XMVectorScale(xmf_fpvRightVec, GlobalState::g_config.tpv_offset_right));

    DirectX::XMFLOAT3 finalTpvPos;
    XMStoreFloat3(&finalTpvPos, xmf_finalTpvCamPos);

    // --- 8. Calculate TPV Camera Orientation (Aim at shifted FPV target point) ---
    DirectX::XMVECTOR xmf_originalFpvPos = XMLoadFloat3(&originalFpvPos); // FPV camera's original world position
    DirectX::XMVECTOR xmf_fpvTargetPoint = DirectX::XMVectorAdd(xmf_originalFpvPos,
                                                                DirectX::XMVectorScale(xmf_fpvForwardVec, GlobalState::g_config.tpv_aim_convergence_distance));

    DirectX::XMFLOAT4 finalCameraRotQuat; // This will store the new TPV orientation

    // Calculate the new forward direction for the TPV camera
    DirectX::XMVECTOR newTpvForwardDirVec = DirectX::XMVectorSubtract(xmf_fpvTargetPoint, xmf_finalTpvCamPos);

    // Check for potential issues (target point same as camera pos, or parallel vectors)
    if (DirectX::XMVector3LessOrEqual(DirectX::XMVector3LengthSq(newTpvForwardDirVec), DirectX::XMVectorReplicate(1e-6f)))
    {
        // Target is too close or at the camera position; fallback to original FPV orientation
        // logger.log(DMKLogLevel::LOG_DEBUG, "Detour_CFPV_UpdateView: TPV target point too close to camera. Using original FPV orientation.");
        finalCameraRotQuat = originalFpvRotQuat;
    }
    else
    {
        newTpvForwardDirVec = DirectX::XMVector3Normalize(newTpvForwardDirVec);

        // Use the ENTITY'S UP vector for LookAt stability. This helps prevent unwanted roll.
        DirectX::XMVECTOR xmf_lookAtUp = DirectX::XMVector3Normalize(xmf_entityWorldUp);
        // As a fallback if entity up is problematic (e.g. perfectly aligned with newTpvForwardDirVec), use world up.
        if (DirectX::XMVector3LessOrEqual(DirectX::XMVector3LengthSq(DirectX::XMVector3Cross(newTpvForwardDirVec, xmf_lookAtUp)), DirectX::XMVectorReplicate(1e-5f)))
        {
            // logger.log(DMKLogLevel::LOG_DEBUG, "Detour_CFPV_UpdateView: Entity Up parallel to new TPV Forward. Using world Z-Up for LookAt.");
            xmf_lookAtUp = DirectX::XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); // World Z-Up
                                                                         // Final check if still parallel (highly unlikely with world up unless looking straight up/down)
            if (DirectX::XMVector3LessOrEqual(DirectX::XMVector3LengthSq(DirectX::XMVector3Cross(newTpvForwardDirVec, xmf_lookAtUp)), DirectX::XMVectorReplicate(1e-5f)))
            {
                // logger.log(DMKLogLevel::LOG_WARNING, "Detour_CFPV_UpdateView: World Z-Up also parallel. Using original FPV orientation.");
                finalCameraRotQuat = originalFpvRotQuat; // Fallback
                goto WriteDataSection;                   // Skip further quaternion calculation
            }
        }

        DirectX::XMMATRIX viewMatrix = DirectX::XMMatrixLookAtRH(xmf_finalTpvCamPos, xmf_fpvTargetPoint, xmf_lookAtUp);
        DirectX::XMMATRIX newCameraWorldMatrix = DirectX::XMMatrixInverse(nullptr, viewMatrix); // Camera's world transform is inverse of view
        DirectX::XMVECTOR xmv_finalCameraRotQuat = DirectX::XMQuaternionRotationMatrix(newCameraWorldMatrix);

        XMStoreFloat4(&finalCameraRotQuat, xmv_finalCameraRotQuat);
    }

WriteDataSection:
    // --- 9. Write the new TPV position and orientation back to pSViewParams_out ---
    if (DMKMemory::isMemoryWritable(pPoseBase, POSE_STRUCT_MIN_SIZE))
    {
        // Write Position
        *(float *)(pPoseBase + POSE_POSITION_OFFSET + 0x0) = finalTpvPos.x;
        *(float *)(pPoseBase + POSE_POSITION_OFFSET + 0x4) = finalTpvPos.y;
        *(float *)(pPoseBase + POSE_POSITION_OFFSET + 0x8) = finalTpvPos.z;

        // Write Rotation Quaternion
        *(float *)(pPoseBase + POSE_ROTATION_OFFSET + 0x0) = finalCameraRotQuat.x;
        *(float *)(pPoseBase + POSE_ROTATION_OFFSET + 0x4) = finalCameraRotQuat.y;
        *(float *)(pPoseBase + POSE_ROTATION_OFFSET + 0x8) = finalCameraRotQuat.z;
        *(float *)(pPoseBase + POSE_ROTATION_OFFSET + 0xC) = finalCameraRotQuat.w;
    }
    else
    {
        // logger.log(DMKLogLevel::LOG_WARNING, "Detour_CFPV_UpdateView: pSViewParams_out (Pose data) not writable at end.");
    }
}

bool initializeTpvHooks(uintptr_t moduleBase, size_t moduleSize)
{
    DMKLogger &logger = Logger::getInstance();
    DMKHookManager &hookManager = HookManager::getInstance();

    logger.log(DMKLogLevel::LOG_INFO, "TpvHooks: Initializing...");

    // Remove the old camera update hook if it exists
    // (Assuming old hook ID was g_cameraUpdateHookId from previous example)
    // if (!g_cameraUpdateHookId.empty()) { // Use the actual old hook ID variable name
    //     hookManager.remove_hook(g_cameraUpdateHookId);
    //     g_cameraUpdateHookId.clear();
    // }

    try
    {
        uintptr_t cameraFPUpdateAddr = moduleBase + RVA_CameraFirstPerson_UpdateView;

        g_cameraFirstPersonUpdateHookId = hookManager.create_inline_hook(
            "CameraFirstPerson_UpdateView",
            cameraFPUpdateAddr,
            reinterpret_cast<void *>(&Detour_CameraFirstPerson_UpdateView),
            reinterpret_cast<void **>(&fpCameraFirstPerson_UpdateView_Original),
            DMKHookConfig{.autoEnable = true});

        if (g_cameraFirstPersonUpdateHookId.empty())
        {
            throw std::runtime_error("Failed to create CameraFirstPerson_UpdateView hook.");
        }
        if (!fpCameraFirstPerson_UpdateView_Original)
        {
            hookManager.remove_hook(g_cameraFirstPersonUpdateHookId);
            throw std::runtime_error("CameraFirstPerson_UpdateView hook creation returned NULL trampoline.");
        }

        logger.log(DMKLogLevel::LOG_INFO, "TpvHooks: CameraFirstPerson_UpdateView hook installed at " + DMK::String::format_address(cameraFPUpdateAddr));

        GlobalState::g_tpvEnabled = false;
        GlobalState::g_localPlayerCEntity = 0;

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(DMKLogLevel::LOG_ERROR, "TpvHooks: Initialization failed: " + std::string(e.what()));
        cleanupTpvHooks();
        return false;
    }
}

void cleanupTpvHooks()
{
    DMKLogger &logger = Logger::getInstance();
    DMKHookManager &hookManager = HookManager::getInstance();

    // Cleanup new hook
    if (!g_cameraFirstPersonUpdateHookId.empty())
    {
        bool removed = hookManager.remove_hook(g_cameraFirstPersonUpdateHookId);
        logger.log(DMKLogLevel::LOG_INFO, "TpvHooks: CameraFirstPerson_UpdateView hook " +
                                              std::string(removed ? "successfully removed." : "removal failed."));
        g_cameraFirstPersonUpdateHookId.clear();
        fpCameraFirstPerson_UpdateView_Original = nullptr;
    }
}
