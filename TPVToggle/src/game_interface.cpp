/**
 * @file game_interface.cpp
 * @brief Implementation of game interface functions.
 *
 * Provides safe access to game memory structures and state management.
 */

#include "game_interface.h"
#include "logger.h"
#include "constants.h"
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h"

#include <stdexcept>

static bool isValidated()
{
    return g_global_context_ptr_address != nullptr;
}

bool initializeGameInterface(uintptr_t module_base, size_t module_size)
{
    Logger &logger = Logger::getInstance();

    try
    {
        logger.log(LOG_INFO, "GameInterface: Initializing with dynamic AOB scanning...");

        // Scan for global context pointer access pattern
        std::vector<BYTE> ctx_pat = parseAOB(Constants::CONTEXT_PTR_LOAD_AOB_PATTERN);
        if (ctx_pat.empty())
        {
            throw std::runtime_error("Failed to parse context pointer AOB pattern");
        }

        BYTE *ctx_aob = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, ctx_pat);
        if (!ctx_aob)
        {
            throw std::runtime_error("Context pointer AOB pattern not found");
        }

        logger.log(LOG_DEBUG, "GameInterface: Found context AOB at " + format_address(reinterpret_cast<uintptr_t>(ctx_aob)));

        // Extract the RIP-relative address from the found instruction
        BYTE *ctx_mov = ctx_aob + 2; // Skip to the MOV instruction
        if (!isMemoryReadable(ctx_mov + 3, sizeof(int32_t)))
        {
            throw std::runtime_error("Cannot read offset from context MOV instruction");
        }

        int32_t ctx_offset = *reinterpret_cast<int32_t *>(ctx_mov + 3);
        BYTE *ctx_rip = ctx_mov + 7; // End of instruction
        uintptr_t ctx_target_addr = reinterpret_cast<uintptr_t>(ctx_rip) + ctx_offset;

        g_global_context_ptr_address = reinterpret_cast<BYTE *>(ctx_target_addr);

        logger.log(LOG_INFO, "GameInterface: Global context pointer storage at " + format_address(ctx_target_addr));

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LOG_ERROR, "GameInterface: Initialization failed: " + std::string(e.what()));
        return false;
    }
}

void cleanupGameInterface()
{
    g_global_context_ptr_address = nullptr;
}

/**
 * @brief Gets the resolved address of the TPV flag using the original working logic.
 */
volatile BYTE *getResolvedTpvFlagAddress()
{
    if (!g_global_context_ptr_address || !isMemoryReadable(g_global_context_ptr_address, sizeof(uintptr_t)))
        return nullptr;

    // Step 1: Read the global context pointer
    uintptr_t global_ctx_ptr = *reinterpret_cast<uintptr_t *>(g_global_context_ptr_address);
    if (global_ctx_ptr == 0)
        return nullptr;

    // Step 2: Read the camera manager pointer directly from global context
    uintptr_t cam_manager_ptr_addr_val = global_ctx_ptr + Constants::OFFSET_ManagerPtrStorage;
    if (!isMemoryReadable(reinterpret_cast<void *>(cam_manager_ptr_addr_val), sizeof(uintptr_t)))
        return nullptr;

    uintptr_t cam_manager_ptr = *reinterpret_cast<uintptr_t *>(cam_manager_ptr_addr_val);
    if (cam_manager_ptr == 0)
        return nullptr;

    // Step 3: Get the TPV flag address directly
    uintptr_t flag_address_val = cam_manager_ptr + Constants::OFFSET_TpvFlag;
    return reinterpret_cast<volatile BYTE *>(flag_address_val);
}

int getViewState()
{
    volatile BYTE *flag_addr = getResolvedTpvFlagAddress();
    if (!flag_addr)
        return -1;

    if (!isMemoryReadable(flag_addr, sizeof(BYTE)))
        return -1;

    BYTE val = *flag_addr;
    return (val == 0 || val == 1) ? static_cast<int>(val) : -1;
}

bool setViewState(BYTE new_state, int *key_pressed_vk)
{
    Logger &logger = Logger::getInstance();

    if (new_state != 0 && new_state != 1)
        return false;

    std::string trigger = key_pressed_vk ? (" (K:" + format_vkcode(*key_pressed_vk) + ")") : "(I)";
    std::string desc = (new_state == 0) ? "FPV" : "TPV";

    volatile BYTE *flag_addr = getResolvedTpvFlagAddress();
    if (!flag_addr)
    {
        logger.log(LOG_ERROR, "Set" + desc + trigger + ": Failed to resolve address");
        return false;
    }

    int current = getViewState();
    if (current == static_cast<int>(new_state))
    {
        return true; // Already in desired state
    }

    if (!isMemoryWritable(flag_addr, sizeof(BYTE)))
    {
        logger.log(LOG_ERROR, "Set" + desc + trigger + ": No write permission at " + format_address(reinterpret_cast<uintptr_t>(flag_addr)));
        return false;
    }

    logger.log(LOG_DEBUG, "Set" + desc + trigger + ": Writing " + std::to_string(new_state) + " at " + format_address(reinterpret_cast<uintptr_t>(flag_addr)));
    *flag_addr = new_state;

    Sleep(1); // Small delay for stability

    int after = getViewState();
    if (after == static_cast<int>(new_state))
    {
        logger.log(LOG_INFO, "Set" + desc + trigger + ": Success");
        return true;
    }
    else
    {
        logger.log(LOG_ERROR, "Set" + desc + trigger + ": Failed (State=" + std::to_string(after) + ")");
        return false;
    }
}

bool safeToggleViewState(int *key_pressed_vk)
{
    Logger &logger = Logger::getInstance();
    std::string trigger = key_pressed_vk ? "(K:" + format_vkcode(*key_pressed_vk) + ")" : "(I)";

    int current = getViewState();
    if (current == 0)
    {
        logger.log(LOG_INFO, "Toggle" + trigger + ": FPV->TPV");
        return setViewState(1, key_pressed_vk);
    }
    else if (current == 1)
    {
        logger.log(LOG_INFO, "Toggle" + trigger + ": TPV->FPV");
        return setViewState(0, key_pressed_vk);
    }
    else
    {
        logger.log(LOG_ERROR, "Toggle" + trigger + ": Invalid state " + std::to_string(current));
        return false;
    }
}

long long getOverlayState()
{
    if (!g_rbx_for_overlay_flag || *g_rbx_for_overlay_flag == 0)
        return -1;

    uintptr_t rbx = *g_rbx_for_overlay_flag;
    volatile uint64_t *addr = reinterpret_cast<volatile uint64_t *>(rbx + Constants::OVERLAY_FLAG_OFFSET);

    if (!isMemoryReadable(addr, sizeof(uint64_t)))
        return -1;

    return static_cast<long long>(*addr);
}

uintptr_t getCameraManagerInstance()
{
    if (!isValidated())
        return 0;

    // Step 1: Read the global context pointer
    if (!isMemoryReadable(g_global_context_ptr_address, sizeof(uintptr_t)))
        return 0;
    uintptr_t global_ctx_ptr = *reinterpret_cast<uintptr_t *>(g_global_context_ptr_address);
    if (global_ctx_ptr == 0)
        return 0;

    // Step 2: Read the camera manager pointer
    uintptr_t cam_manager_ptr_addr = global_ctx_ptr + Constants::OFFSET_ManagerPtrStorage;
    if (!isMemoryReadable(reinterpret_cast<void *>(cam_manager_ptr_addr), sizeof(uintptr_t)))
        return 0;
    uintptr_t cam_manager_ptr = *reinterpret_cast<uintptr_t *>(cam_manager_ptr_addr);

    return cam_manager_ptr;
}
