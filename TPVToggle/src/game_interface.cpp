/**
 * @file game_interface.cpp
 * @brief Implementation of game interface functions.
 *
 * Provides safe access to game memory structures and state management.
 */

#include "game_interface.h"
#include "logger.h"
#include "constants.h"
#include "game_structures.h"
#include "utils.h"
#include "aob_scanner.h"
#include "global_state.h"

#include <stdexcept>

static bool isValidated()
{
    return g_global_context_ptr_address != nullptr;
}

/**
 * @brief Locates the scroll accumulator in memory using pattern scanning and direct RVA
 * @param module_base Base address of the target game module
 * @param module_size Size of the target game module in bytes
 * @return true if successfully located the accumulator, false otherwise
 */
bool findScrollAccumulator(uintptr_t module_base, size_t module_size)
{
    Logger &logger = Logger::getInstance();

    logger.log(LOG_INFO, "Attempting to find Scroll Accumulator address via AOB scan...");

    if (module_base == 0 || module_size == 0)
    {
        logger.log(LOG_ERROR, "findScrollAccumulator: Invalid module base/size provided.");
        return false;
    }

    // Use the AOB pattern provided in constants.h
    std::vector<BYTE> scroll_pat = parseAOB(Constants::SCROLL_STATE_BASE_AOB_PATTERN);
    if (scroll_pat.empty())
    {
        logger.log(LOG_ERROR, "Failed to parse scroll state AOB pattern from constants.");
        return false; // Cannot proceed without pattern
    }

    BYTE *scroll_aob_result = FindPattern(reinterpret_cast<BYTE *>(module_base), module_size, scroll_pat);

    if (!scroll_aob_result)
    {
        logger.log(LOG_ERROR, "Scroll state AOB pattern not found in module. Cannot locate accumulator.");
        return false; // Pattern not found, cannot proceed
    }

    // Found the pattern, now extract the RIP-relative address
    logger.log(LOG_INFO, "Found scroll state AOB pattern at: " + format_address(reinterpret_cast<uintptr_t>(scroll_aob_result)));

    // The instruction we targeted is '48 8B 15 offset' (7 bytes total)
    // mov rdx, [rip + offset]
    BYTE *instruction_address = scroll_aob_result; // Assuming AOB starts exactly at the MOV instruction

    // Extract the 32-bit relative offset (starts at byte 3 of the instruction)
    if (!isMemoryReadable(instruction_address + 3, sizeof(int32_t)))
    {
        logger.log(LOG_ERROR, "Cannot read relative offset from instruction at: " + format_address(reinterpret_cast<uintptr_t>(instruction_address + 3)));
        return false;
    }
    int32_t relative_offset = *reinterpret_cast<int32_t *>(instruction_address + 3);

    // Calculate the RIP value (address of the instruction *after* the MOV)
    BYTE *rip_value = instruction_address + 7; // Instruction is 7 bytes long

    // Calculate the absolute address where the pointer is stored
    uintptr_t scroll_ptr_storage_addr_val = reinterpret_cast<uintptr_t>(rip_value) + relative_offset;
    g_scrollPtrStorageAddress = reinterpret_cast<volatile uintptr_t *>(scroll_ptr_storage_addr_val);
    logger.log(LOG_INFO, "Calculated scroll state pointer storage address: " + format_address(scroll_ptr_storage_addr_val));

    return true;
}

volatile uintptr_t *getResolvedScrollAccumulatorAddress()
{
    Logger &logger = Logger::getInstance();
    // Read the pointer value from this storage address safely
    if (!isMemoryReadable(g_scrollPtrStorageAddress, sizeof(uintptr_t)))
    {
        logger.log(LOG_ERROR, "Cannot read scroll state base pointer from storage address!");
        return nullptr;
    }
    uintptr_t scroll_state_base_ptr = *g_scrollPtrStorageAddress; // Read the pointer value

    // Check if the base pointer is valid
    if (scroll_state_base_ptr == 0)
    {
        logger.log(LOG_ERROR, "Scroll state base pointer read from storage is NULL.");
        return nullptr; // Cannot proceed with NULL base pointer
    }
    logger.log(LOG_DEBUG, "Scroll state base structure located at: " + format_address(scroll_state_base_ptr));

    // Calculate address of the accumulator float using the known offset
    uintptr_t final_accum_addr_val = scroll_state_base_ptr + Constants::OFFSET_ScrollAccumulatorFloat; // +0x1C
    volatile uintptr_t *final_accum_addr = reinterpret_cast<volatile uintptr_t *>(final_accum_addr_val);
    logger.log(LOG_DEBUG, "Calculated final accumulator address: " + format_address(final_accum_addr_val));

    // Final validation: Check if the target address is readable/writable
    if (!isMemoryReadable(final_accum_addr, sizeof(float)))
    {
        logger.log(LOG_ERROR, "Final accumulator address is not readable!");
        return nullptr;
    }
    if (!isMemoryWritable(final_accum_addr, sizeof(float)))
    {
        logger.log(LOG_ERROR, "Final accumulator address is not writable!");
        return nullptr;
    }

    // Store the final, validated address globally
    g_scrollAccumulatorAddress = final_accum_addr; // Use the correct global name

    float currentValue = *g_scrollAccumulatorAddress; // Read current value for logging
    logger.log(LOG_INFO, "Successfully located scroll accumulator via AOB at " +
                             format_address(reinterpret_cast<uintptr_t>(g_scrollAccumulatorAddress)) +
                             ", current value: " + std::to_string(currentValue));

    return g_scrollAccumulatorAddress;
}

/**
 * @brief Safely resets the scroll accumulator to zero
 * @param logReset Whether to log successful resets (to avoid log spam)
 * @return true if successfully reset, false if pointer invalid or memory not writable
 */
bool resetScrollAccumulator(bool logReset)
{
    Logger &logger = Logger::getInstance();

    if (g_scrollAccumulatorAddress == nullptr)
    {
        g_scrollAccumulatorAddress = getResolvedScrollAccumulatorAddress();
    }

    if (g_scrollAccumulatorAddress != nullptr && isMemoryWritable(g_scrollAccumulatorAddress, sizeof(uintptr_t)))
    {
        float currentValue = *g_scrollAccumulatorAddress;
        if (currentValue != 0.0f)
        {
            *g_scrollAccumulatorAddress = 0.0f;
            if (logReset)
            {
                Logger::getInstance().log(LOG_DEBUG, "resetScrollAccumulator: Reset value from " +
                                                         std::to_string(currentValue) + " to 0.0");
            }
            return true;
        }
    }
    return false;
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

        if (findScrollAccumulator(module_base, module_size))
        {
            logger.log(LOG_INFO, "Scroll accumulator locator initialized successfully");
        }
        else
        {
            logger.log(LOG_WARNING, "Could not locate scroll accumulator - hold-to-scroll feature may not work correctly");
        }

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
    if (g_tpvFlagAddress != nullptr)
    {
        return g_tpvFlagAddress;
    }

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

extern "C" uintptr_t __cdecl getCameraManagerInstance()
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

bool GetPlayerWorldTransform(::Vector3 &outPosition, ::Quaternion &outOrientation)
{
    Logger &logger = Logger::getInstance();

    if (!g_thePlayerEntity)
    {
        logger.log(LOG_DEBUG, "GetPlayerWorldTransform: Called but g_thePlayerEntity is currently NULL.");
        return false;
    }

    uintptr_t matrix_address = reinterpret_cast<uintptr_t>(g_thePlayerEntity) + Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER;

    if (!isMemoryReadable(reinterpret_cast<void *>(matrix_address), sizeof(GameStructures::Matrix34f)))
    {
        logger.log(LOG_WARNING, "GetPlayerWorldTransform: Cannot read CEntity's m_worldTransform at " +
                                    format_address(matrix_address) + " for entity " +
                                    format_address(reinterpret_cast<uintptr_t>(g_thePlayerEntity)));
        return false;
    }

    const GameStructures::Matrix34f &playerMatrix =
        *reinterpret_cast<const GameStructures::Matrix34f *>(matrix_address);

    // Extract position
    outPosition.x = playerMatrix.m[0][3];
    outPosition.y = playerMatrix.m[1][3];
    outPosition.z = playerMatrix.m[2][3];

    // Extract rotation from the 3x3 part and convert to Quaternion
    DirectX::XMMATRIX dxRotMatrix = DirectX::XMMatrixSet(
        playerMatrix.m[0][0], playerMatrix.m[0][1], playerMatrix.m[0][2], 0.0f,
        playerMatrix.m[1][0], playerMatrix.m[1][1], playerMatrix.m[1][2], 0.0f,
        playerMatrix.m[2][0], playerMatrix.m[2][1], playerMatrix.m[2][2], 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);
    // Note: CRYENGINE matrices (Matrix34_tpl specifically) store basis vectors as ROWS.
    // m00, m01, m02 is the X-basis vector (Right).
    // m10, m11, m12 is the Y-basis vector (Forward for CryEngine).
    // m20, m21, m22 is the Z-basis vector (Up for CryEngine).
    // The DirectX::XMMatrixSet function takes arguments row by row.
    // So, the current mapping directly forms a matrix whose rows are these basis vectors.
    // This is standard for creating a rotation matrix for DirectXMath.
    outOrientation = ::Quaternion::FromXMVector(DirectX::XMQuaternionRotationMatrix(dxRotMatrix));

    std::ostringstream matrix_dump;
    matrix_dump << std::fixed << std::setprecision(4);
    matrix_dump << "\n  Matrix Read from Entity " << format_address(reinterpret_cast<uintptr_t>(g_thePlayerEntity)) << " @ offset " << format_hex(Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER) << " (Addr: " << format_address(matrix_address) << "):";
    matrix_dump << "\n    R0: [" << playerMatrix.m[0][0] << ", " << playerMatrix.m[0][1] << ", " << playerMatrix.m[0][2] << "] T.x: " << playerMatrix.m[0][3];
    matrix_dump << "\n    R1: [" << playerMatrix.m[1][0] << ", " << playerMatrix.m[1][1] << ", " << playerMatrix.m[1][2] << "] T.y: " << playerMatrix.m[1][3];
    matrix_dump << "\n    R2: [" << playerMatrix.m[2][0] << ", " << playerMatrix.m[2][1] << ", " << playerMatrix.m[2][2] << "] T.z: " << playerMatrix.m[2][3];

    logger.log(LOG_TRACE, "GetPlayerWorldTransform SUCCESS:" + matrix_dump.str());
    logger.log(LOG_TRACE, "  Converted Pos: " + Vector3ToString(outPosition) + " | Converted Rot: " + QuatToString(outOrientation));
    return true;
}
