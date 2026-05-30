/**
 * @file game_interface.cpp
 * @brief Implementation of game interface functions.
 *
 * Provides safe access to game memory structures and state management.
 */

#include "game_interface.hpp"
#include "constants.hpp"
#include "game_structures.hpp"
#include "utils.hpp"
#include "global_state.hpp"

#include <DetourModKit.hpp>
#include <stdexcept>

using DetourModKit::LogLevel;
using DMKFormat::format_address;

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
    DMKLogger &logger = DMKLogger::get_instance();

    logger.log(LogLevel::Info, "Attempting to find Scroll Accumulator address via AOB scan...");

    if (module_base == 0 || module_size == 0)
    {
        logger.log(LogLevel::Error, "findScrollAccumulator: Invalid module base/size provided.");
        return false;
    }

    // Use the AOB pattern provided in constants.h
    auto scroll_pat = DMKScanner::parse_aob(Constants::SCROLL_STATE_BASE_AOB_PATTERN);
    if (!scroll_pat.has_value())
    {
        logger.log(LogLevel::Error, "Failed to parse scroll state AOB pattern from constants.");
        return false; // Cannot proceed without pattern
    }

    const std::byte *scroll_aob_result = DMKScanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *scroll_pat);

    if (!scroll_aob_result)
    {
        logger.log(LogLevel::Error, "Scroll state AOB pattern not found in module. Cannot locate accumulator.");
        return false; // Pattern not found, cannot proceed
    }

    logger.log(LogLevel::Info, "Found scroll state AOB pattern at: " + format_address(reinterpret_cast<uintptr_t>(scroll_aob_result)));

    // Resolve RIP-relative target: mov rdx, [rip + offset] (48 8B 15 xx xx xx xx, 7 bytes, disp32 at offset 3)
    auto scroll_ptr_addr = DMKScanner::resolve_rip_relative(scroll_aob_result, 3, 7);
    if (!scroll_ptr_addr.has_value())
    {
        logger.log(LogLevel::Error, "Failed to resolve scroll state RIP-relative address");
        return false;
    }
    g_scrollPtrStorageAddress = reinterpret_cast<volatile uintptr_t *>(scroll_ptr_addr.value());
    logger.log(LogLevel::Info, "Calculated scroll state pointer storage address: " + format_address(scroll_ptr_addr.value()));

    return true;
}

volatile float *getResolvedScrollAccumulatorAddress()
{
    DMKLogger &logger = DMKLogger::get_instance();
    // Read the base pointer from the game's storage slot under one fault guard.
    // The slot lives in WHGame.dll data and can briefly hold null before the
    // scroll-state object is constructed.
    const auto scroll_state_base = DMKMemory::seh_read<uintptr_t>(reinterpret_cast<uintptr_t>(g_scrollPtrStorageAddress));
    if (!scroll_state_base)
    {
        logger.log(LogLevel::Error, "Cannot read scroll state base pointer from storage address!");
        return nullptr;
    }
    uintptr_t scroll_state_base_ptr = *scroll_state_base;

    if (scroll_state_base_ptr == 0)
    {
        logger.log(LogLevel::Error, "Scroll state base pointer read from storage is NULL.");
        return nullptr; // Cannot proceed with NULL base pointer
    }
    logger.log(LogLevel::Debug, "Scroll state base structure located at: " + format_address(scroll_state_base_ptr));

    uintptr_t final_accum_addr_val = scroll_state_base_ptr + Constants::OFFSET_ScrollAccumulatorFloat; // +0x1C
    // The accumulator is a 4-byte float (the game writes it via movss [rdx+1C],xmm0),
    // so type the pointer as float to read and zero exactly those 4 bytes.
    volatile float *final_accum_addr = reinterpret_cast<volatile float *>(final_accum_addr_val);
    logger.log(LogLevel::Debug, "Calculated final accumulator address: " + format_address(final_accum_addr_val));

    if (!DMKMemory::is_readable(const_cast<float *>(final_accum_addr), sizeof(float)))
    {
        logger.log(LogLevel::Error, "Final accumulator address is not readable!");
        return nullptr;
    }
    if (!DMKMemory::is_writable(const_cast<float *>(final_accum_addr), sizeof(float)))
    {
        logger.log(LogLevel::Error, "Final accumulator address is not writable!");
        return nullptr;
    }

    g_scrollAccumulatorAddress = final_accum_addr;

    float currentValue = *g_scrollAccumulatorAddress;
    logger.log(LogLevel::Info, "Successfully located scroll accumulator via AOB at " +
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
    if (g_scrollAccumulatorAddress == nullptr)
    {
        g_scrollAccumulatorAddress = getResolvedScrollAccumulatorAddress();
    }

    if (g_scrollAccumulatorAddress != nullptr && DMKMemory::is_writable(const_cast<float *>(g_scrollAccumulatorAddress), sizeof(float)))
    {
        float currentValue = *g_scrollAccumulatorAddress;
        if (currentValue != 0.0f)
        {
            *g_scrollAccumulatorAddress = 0.0f;
            if (logReset)
            {
                DMKLogger::get_instance().log(LogLevel::Debug, "resetScrollAccumulator: Reset value from " +
                                                         std::to_string(currentValue) + " to 0.0");
            }
            return true;
        }
    }
    return false;
}

bool initializeGameInterface(uintptr_t module_base, size_t module_size)
{
    DMKLogger &logger = DMKLogger::get_instance();

    try
    {
        logger.log(LogLevel::Info, "GameInterface: Initializing with dynamic AOB scanning...");

        auto ctx_pat = DMKScanner::parse_aob(Constants::CONTEXT_PTR_LOAD_AOB_PATTERN);
        if (!ctx_pat.has_value())
        {
            throw std::runtime_error("Failed to parse context pointer AOB pattern");
        }

        const std::byte *ctx_aob = DMKScanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *ctx_pat);
        if (!ctx_aob)
        {
            throw std::runtime_error("Context pointer AOB pattern not found");
        }

        logger.log(LogLevel::Debug, "GameInterface: Found context AOB at " + format_address(reinterpret_cast<uintptr_t>(ctx_aob)));

        // Resolve RIP-relative target: mov rax, [rip + offset] (48 8B 05 xx xx xx xx)
        // AOB match is 2 bytes before the MOV instruction
        auto ctx_target_addr = DMKScanner::resolve_rip_relative(ctx_aob + 2, 3, 7);
        if (!ctx_target_addr.has_value())
        {
            throw std::runtime_error("Failed to resolve context pointer RIP-relative address");
        }

        g_global_context_ptr_address = reinterpret_cast<std::byte *>(ctx_target_addr.value());

        logger.log(LogLevel::Info, "GameInterface: Global context pointer storage at " + format_address(ctx_target_addr.value()));

        if (findScrollAccumulator(module_base, module_size))
        {
            logger.log(LogLevel::Info, "Scroll accumulator locator initialized successfully");
        }
        else
        {
            logger.log(LogLevel::Warning, "Could not locate scroll accumulator - hold-to-scroll feature may not work correctly");
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.log(LogLevel::Error, "GameInterface: Initialization failed: " + std::string(e.what()));
        return false;
    }
}

void cleanupGameInterface()
{
    g_global_context_ptr_address = nullptr;
}

/**
 * @brief Resolves the TPV flag address via the global-context -> camera-manager -> flag chain.
 * @details Walks (*(*(storage) + OFFSET_ManagerPtrStorage)) + OFFSET_TpvFlag under a single
 *          fault guard. Each intermediate link is screened by plausible_userspace_ptr, so a
 *          stale or torn pointer aborts the walk and yields nullptr instead of faulting.
 */
volatile std::byte *getResolvedTpvFlagAddress()
{
    if (!g_global_context_ptr_address)
        return nullptr;

    const auto flag_addr = DMKMemory::seh_resolve_chain(
        reinterpret_cast<uintptr_t>(g_global_context_ptr_address),
        {0, Constants::OFFSET_ManagerPtrStorage, Constants::OFFSET_TpvFlag});
    if (!flag_addr)
        return nullptr;

    return reinterpret_cast<volatile std::byte *>(*flag_addr);
}

int getViewState()
{
    volatile std::byte *flag_addr = getResolvedTpvFlagAddress();
    if (!flag_addr)
        return -1;

    // Read the flag under one SEH frame; the camera-manager object it lives in
    // is engine-owned and can be torn down between frames.
    const auto val = DMKMemory::seh_read<uint8_t>(reinterpret_cast<uintptr_t>(flag_addr));
    if (!val)
        return -1;

    return (*val == 0 || *val == 1) ? static_cast<int>(*val) : -1;
}

bool setViewState(BYTE new_state, int *key_pressed_vk)
{
    DMKLogger &logger = DMKLogger::get_instance();

    if (new_state != 0 && new_state != 1)
        return false;

    std::string trigger = key_pressed_vk ? (" (K:" + DMKFormat::format_vkcode(*key_pressed_vk) + ")") : "(I)";
    std::string desc = (new_state == 0) ? "FPV" : "TPV";

    volatile std::byte *flag_addr = getResolvedTpvFlagAddress();
    if (!flag_addr)
    {
        logger.log(LogLevel::Error, "Set" + desc + trigger + ": Failed to resolve address");
        return false;
    }

    int current = getViewState();
    if (current == static_cast<int>(new_state))
    {
        return true; // Already in desired state
    }

    // flag_addr was just resolved through a fault-guarded chain whose final link
    // (the camera-manager object) is live engine memory, so the flag byte is
    // writable; the post-write read-back below confirms the store landed.
    logger.log(LogLevel::Debug, "Set" + desc + trigger + ": Writing " + std::to_string(new_state) + " at " + format_address(reinterpret_cast<uintptr_t>(flag_addr)));
    *flag_addr = static_cast<std::byte>(new_state);

    Sleep(1); // Yield briefly so the engine observes the store before the read-back verifies it.

    int after = getViewState();
    if (after == static_cast<int>(new_state))
    {
        logger.log(LogLevel::Info, "Set" + desc + trigger + ": Success");
        return true;
    }
    else
    {
        logger.log(LogLevel::Error, "Set" + desc + trigger + ": Failed (State=" + std::to_string(after) + ")");
        return false;
    }
}

bool safeToggleViewState(int *key_pressed_vk)
{
    DMKLogger &logger = DMKLogger::get_instance();
    std::string trigger = key_pressed_vk ? "(K:" + DMKFormat::format_vkcode(*key_pressed_vk) + ")" : "(I)";

    int current = getViewState();
    if (current == 0)
    {
        logger.log(LogLevel::Info, "Toggle" + trigger + ": FPV->TPV");
        return setViewState(1, key_pressed_vk);
    }
    else if (current == 1)
    {
        logger.log(LogLevel::Info, "Toggle" + trigger + ": TPV->FPV");
        return setViewState(0, key_pressed_vk);
    }
    else
    {
        logger.log(LogLevel::Error, "Toggle" + trigger + ": Invalid state " + std::to_string(current));
        return false;
    }
}

extern "C" uintptr_t __cdecl getCameraManagerInstance()
{
    if (!isValidated())
        return 0;

    // Read global context -> camera manager (one dereference) under a single
    // fault guard instead of gating each link with is_readable.
    return DMKMemory::seh_read_chain<uintptr_t>(
               reinterpret_cast<uintptr_t>(g_global_context_ptr_address),
               {0, Constants::OFFSET_ManagerPtrStorage})
        .value_or(0);
}

bool GetPlayerWorldTransform(::Vector3 &outPosition, ::Quaternion &outOrientation)
{
    DMKLogger &logger = DMKLogger::get_instance();

    if (!g_thePlayerEntity)
    {
        logger.log(LogLevel::Debug, "GetPlayerWorldTransform: Called but g_thePlayerEntity is currently NULL.");
        return false;
    }

    uintptr_t matrix_address = reinterpret_cast<uintptr_t>(g_thePlayerEntity) + Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER;

    // g_thePlayerEntity is engine-owned and may have been freed since it was
    // captured, so read the whole 3x4 matrix under one SEH frame rather than
    // gating a raw dereference with is_readable (which cannot prevent a fault
    // between the check and the read).
    const auto playerMatrixOpt = DMKMemory::seh_read<GameStructures::Matrix34f>(matrix_address);
    if (!playerMatrixOpt)
    {
        logger.log(LogLevel::Warning, "GetPlayerWorldTransform: Cannot read CEntity's m_worldTransform at " +
                                    format_address(matrix_address) + " for entity " +
                                    format_address(reinterpret_cast<uintptr_t>(g_thePlayerEntity)));
        return false;
    }

    const GameStructures::Matrix34f &playerMatrix = *playerMatrixOpt;

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
    matrix_dump << "\n  Matrix Read from Entity " << format_address(reinterpret_cast<uintptr_t>(g_thePlayerEntity)) << " @ offset " << DMKFormat::format_hex(Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER) << " (Addr: " << format_address(matrix_address) << "):";
    matrix_dump << "\n    R0: [" << playerMatrix.m[0][0] << ", " << playerMatrix.m[0][1] << ", " << playerMatrix.m[0][2] << "] T.x: " << playerMatrix.m[0][3];
    matrix_dump << "\n    R1: [" << playerMatrix.m[1][0] << ", " << playerMatrix.m[1][1] << ", " << playerMatrix.m[1][2] << "] T.y: " << playerMatrix.m[1][3];
    matrix_dump << "\n    R2: [" << playerMatrix.m[2][0] << ", " << playerMatrix.m[2][1] << ", " << playerMatrix.m[2][2] << "] T.z: " << playerMatrix.m[2][3];

    logger.log(LogLevel::Trace, "GetPlayerWorldTransform SUCCESS:" + matrix_dump.str());
    logger.log(LogLevel::Trace, "  Converted Pos: " + Vector3ToString(outPosition) + " | Converted Rot: " + QuatToString(outOrientation));
    return true;
}
