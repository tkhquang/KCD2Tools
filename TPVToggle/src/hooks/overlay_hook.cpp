/**
 * @file hooks/overlay_hook.cpp
 * @brief Implementation of overlay detection hook functionality.
 */

#include "overlay_hook.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h"
#include "MinHook.h"

#include <stdexcept>

// Assembly detour imports
extern "C"
{
    void Overlay_CaptureRBX_Detour();
    extern void *fpOverlay_OriginalCode;
}

static BYTE *g_overlayHookAddress = nullptr;

bool initializeOverlayHook(uintptr_t module_base, size_t module_size)
{
    Logger &logger = Logger::getInstance();

    try
    {
        logger.log(LOG_INFO, "OverlayHook: Initializing overlay detection...");

        // Allocate storage for RBX capture
        g_rbx_for_overlay_flag = reinterpret_cast<uintptr_t *>(
            VirtualAlloc(NULL, sizeof(uintptr_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

        if (!g_rbx_for_overlay_flag)
        {
            throw std::runtime_error("Failed to allocate RBX storage: " + std::to_string(GetLastError()));
        }

        *g_rbx_for_overlay_flag = 0;
        logger.log(LOG_DEBUG, "OverlayHook: Allocated RBX storage at " + format_address(reinterpret_cast<uintptr_t>(g_rbx_for_overlay_flag)));

        // Scan for overlay check pattern
        std::vector<BYTE> overlay_pat = parseAOB(Constants::OVERLAY_CHECK_AOB_PATTERN);
        if (overlay_pat.empty())
        {
            throw std::runtime_error("Failed to parse overlay check AOB pattern");
        }

        BYTE *overlay_aob = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, overlay_pat);
        if (!overlay_aob)
        {
            throw std::runtime_error("Overlay check AOB pattern not found");
        }

        g_overlayHookAddress = overlay_aob + Constants::OVERLAY_HOOK_OFFSET;
        logger.log(LOG_INFO, "OverlayHook: Found overlay check at " + format_address(reinterpret_cast<uintptr_t>(g_overlayHookAddress)));

        // Create and enable the MinHook
        MH_STATUS status = MH_CreateHook(g_overlayHookAddress,
                                         reinterpret_cast<LPVOID>(Overlay_CaptureRBX_Detour),
                                         &fpOverlay_OriginalCode);

        if (status != MH_OK)
        {
            throw std::runtime_error("MH_CreateHook failed: " + std::string(MH_StatusToString(status)));
        }

        if (!fpOverlay_OriginalCode)
        {
            MH_RemoveHook(g_overlayHookAddress);
            throw std::runtime_error("MH_CreateHook returned NULL trampoline");
        }

        status = MH_EnableHook(g_overlayHookAddress);
        if (status != MH_OK)
        {
            MH_RemoveHook(g_overlayHookAddress);
            throw std::runtime_error("MH_EnableHook failed: " + std::string(MH_StatusToString(status)));
        }

        logger.log(LOG_INFO, "OverlayHook: Overlay detection hook successfully installed");
        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "OverlayHook: Initialization failed: " + std::string(e.what()));
        cleanupOverlayHook();
        return false;
    }
}

void cleanupOverlayHook()
{
    Logger &logger = Logger::getInstance();

    if (g_overlayHookAddress && fpOverlay_OriginalCode)
    {
        MH_DisableHook(g_overlayHookAddress);
        MH_RemoveHook(g_overlayHookAddress);
        fpOverlay_OriginalCode = nullptr;
        g_overlayHookAddress = nullptr;
    }

    if (g_rbx_for_overlay_flag)
    {
        VirtualFree(g_rbx_for_overlay_flag, 0, MEM_RELEASE);
        g_rbx_for_overlay_flag = nullptr;
    }

    logger.log(LOG_DEBUG, "OverlayHook: Cleanup complete");
}

bool isOverlayHookActive()
{
    return (g_overlayHookAddress != nullptr &&
            fpOverlay_OriginalCode != nullptr &&
            g_rbx_for_overlay_flag != nullptr);
}
