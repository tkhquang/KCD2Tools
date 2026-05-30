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
using DMK::Format::format_address;

namespace TPVToggle
{

namespace
{
/**
 * @brief Locates the scroll accumulator in memory using pattern scanning and direct RVA
 * @param module_base Base address of the target game module
 * @param module_size Size of the target game module in bytes
 * @return true if successfully located the accumulator, false otherwise
 */
[[nodiscard]] bool findScrollAccumulator(uintptr_t module_base, size_t module_size)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    logger.info("Attempting to find Scroll Accumulator address via AOB scan...");

    if (module_base == 0 || module_size == 0)
    {
        logger.error("findScrollAccumulator: Invalid module base/size provided.");
        return false;
    }

    auto scroll_pat = DMK::Scanner::parse_aob(Constants::SCROLL_STATE_BASE_AOB_PATTERN);
    if (!scroll_pat.has_value())
    {
        logger.error("Failed to parse scroll state AOB pattern from constants.");
        return false; // Cannot proceed without pattern
    }

    const std::byte *scroll_aob_result = DMK::Scanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *scroll_pat);

    if (!scroll_aob_result)
    {
        logger.error("Scroll state AOB pattern not found in module. Cannot locate accumulator.");
        return false; // Pattern not found, cannot proceed
    }

    logger.info("Found scroll state AOB pattern at: {}", format_address(reinterpret_cast<uintptr_t>(scroll_aob_result)));

    // Resolve RIP-relative target: mov rdx, [rip + offset] (48 8B 15 xx xx xx xx, 7 bytes, disp32 at offset 3)
    auto scroll_ptr_addr = DMK::Scanner::resolve_rip_relative(scroll_aob_result, 3, 7);
    if (!scroll_ptr_addr.has_value())
    {
        logger.error("Failed to resolve scroll state RIP-relative address");
        return false;
    }
    TPVToggle::scroll_hook_state().ptrStorageAddress = reinterpret_cast<volatile uintptr_t *>(scroll_ptr_addr.value());
    logger.info("Calculated scroll state pointer storage address: {}", format_address(scroll_ptr_addr.value()));

    return true;
}

[[nodiscard]] volatile float *getResolvedScrollAccumulatorAddress()
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    // Read the base pointer from the game's storage slot under one fault guard.
    // The slot lives in WHGame.dll data and can briefly hold null before the
    // scroll-state object is constructed.
    const auto scroll_state_base = DMK::Memory::seh_read<uintptr_t>(reinterpret_cast<uintptr_t>(TPVToggle::scroll_hook_state().ptrStorageAddress));
    if (!scroll_state_base)
    {
        logger.error("Cannot read scroll state base pointer from storage address!");
        return nullptr;
    }
    uintptr_t scroll_state_base_ptr = *scroll_state_base;

    if (scroll_state_base_ptr == 0)
    {
        logger.error("Scroll state base pointer read from storage is NULL.");
        return nullptr; // Cannot proceed with NULL base pointer
    }
    logger.debug("Scroll state base structure located at: {}", format_address(scroll_state_base_ptr));

    uintptr_t final_accum_addr_val = scroll_state_base_ptr + Constants::OFFSET_ScrollAccumulatorFloat; // +0x1C

    // Screen the computed address with a syscall-free arithmetic check, then
    // confirm readability with a single SEH-guarded read instead of the
    // is_readable + is_writable predicate pair (each takes a shard lock and may
    // issue a VirtualQuery). The accumulator is engine-written by definition, so
    // a successful read implies it is the live, writable slot.
    if (!DMK::Memory::plausible_userspace_ptr(final_accum_addr_val))
    {
        logger.error("Final accumulator address is implausible!");
        return nullptr;
    }
    const auto currentValue = DMK::Memory::seh_read<float>(final_accum_addr_val);
    if (!currentValue)
    {
        logger.error("Final accumulator address is not readable!");
        return nullptr;
    }

    // The accumulator is a 4-byte float (the game writes it via movss [rdx+1C],xmm0),
    // so type the pointer as float to read and zero exactly those 4 bytes.
    volatile float *final_accum_addr = reinterpret_cast<volatile float *>(final_accum_addr_val);
    TPVToggle::scroll_hook_state().accumulatorAddress = final_accum_addr;

    logger.info("Successfully located scroll accumulator via AOB at {}, current value: {}",
                format_address(final_accum_addr_val), *currentValue);

    return final_accum_addr;
}

} // namespace

/**
 * @brief Safely resets the scroll accumulator to zero
 * @param logReset Whether to log successful resets (to avoid log spam)
 * @return true if successfully reset, false if pointer invalid or already zero
 */
bool resetScrollAccumulator(bool logReset)
{
    if (TPVToggle::scroll_hook_state().accumulatorAddress == nullptr)
    {
        TPVToggle::scroll_hook_state().accumulatorAddress = getResolvedScrollAccumulatorAddress();
    }

    // resetScrollAccumulator runs from the per-event menu/overlay hooks, so it
    // reads and writes the engine-owned accumulator directly rather than gating
    // each access with is_writable (a shard lock plus possible syscall per call).
    // The slot was confirmed live when the address was resolved, and the engine
    // writes it every frame, so it stays writable.
    volatile float *accum = TPVToggle::scroll_hook_state().accumulatorAddress;
    if (accum != nullptr)
    {
        float currentValue = *accum;
        if (currentValue != 0.0f)
        {
            *accum = 0.0f;
            if (logReset)
            {
                DMK::Logger::get_instance().debug("resetScrollAccumulator: Reset value from {} to 0.0", currentValue);
            }
            return true;
        }
    }
    return false;
}

bool initializeGameInterface(uintptr_t module_base, size_t module_size)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    try
    {
        logger.info("GameInterface: Initializing with dynamic AOB scanning...");

        auto ctx_pat = DMK::Scanner::parse_aob(Constants::CONTEXT_PTR_LOAD_AOB_PATTERN);
        if (!ctx_pat.has_value())
        {
            throw std::runtime_error("Failed to parse context pointer AOB pattern");
        }

        const std::byte *ctx_aob = DMK::Scanner::find_pattern(reinterpret_cast<const std::byte *>(module_base), module_size, *ctx_pat);
        if (!ctx_aob)
        {
            throw std::runtime_error("Context pointer AOB pattern not found");
        }

        logger.debug("GameInterface: Found context AOB at {}", format_address(reinterpret_cast<uintptr_t>(ctx_aob)));

        // Resolve RIP-relative target: mov rax, [rip + offset] (48 8B 05 xx xx xx xx)
        // AOB match is 2 bytes before the MOV instruction
        auto ctx_target_addr = DMK::Scanner::resolve_rip_relative(ctx_aob + 2, 3, 7);
        if (!ctx_target_addr.has_value())
        {
            throw std::runtime_error("Failed to resolve context pointer RIP-relative address");
        }

        g_global_context_ptr_address = reinterpret_cast<std::byte *>(ctx_target_addr.value());

        logger.info("GameInterface: Global context pointer storage at {}", format_address(ctx_target_addr.value()));

        if (findScrollAccumulator(module_base, module_size))
        {
            logger.info("Scroll accumulator locator initialized successfully");
        }
        else
        {
            logger.warning("Could not locate scroll accumulator - hold-to-scroll feature may not work correctly");
        }

        return true;
    }
    catch (const std::exception &e)
    {
        logger.error("GameInterface: Initialization failed: {}", e.what());
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

    const auto flag_addr = DMK::Memory::seh_resolve_chain(
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
    const auto val = DMK::Memory::seh_read<uint8_t>(reinterpret_cast<uintptr_t>(flag_addr));
    if (!val)
        return -1;

    return (*val == 0 || *val == 1) ? static_cast<int>(*val) : -1;
}

bool setViewState(BYTE new_state, int *key_pressed_vk)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (new_state != 0 && new_state != 1)
        return false;

    std::string trigger = key_pressed_vk ? (" (K:" + DMK::Format::format_vkcode(*key_pressed_vk) + ")") : "(I)";
    std::string desc = (new_state == 0) ? "FPV" : "TPV";

    volatile std::byte *flag_addr = getResolvedTpvFlagAddress();
    if (!flag_addr)
    {
        logger.error("Set{}{}: Failed to resolve address", desc, trigger);
        return false;
    }

    // Read the current state once from the already-resolved address rather than
    // re-walking the whole pointer chain through getViewState().
    const auto current = DMK::Memory::seh_read<uint8_t>(reinterpret_cast<uintptr_t>(flag_addr));
    if (current && *current == new_state)
    {
        return true; // Already in desired state
    }

    logger.debug("Set{}{}: Writing {} at {}", desc, trigger, static_cast<int>(new_state), format_address(reinterpret_cast<uintptr_t>(flag_addr)));

    // The chain was walkable when it was resolved, but the camera-manager object
    // it ends in can be torn down between frames, so write through the validating
    // write_bytes (which also fixes up page protection) instead of a raw store
    // that would fault on a stale address.
    const std::byte state_byte = static_cast<std::byte>(new_state);
    if (!DMK::Memory::write_bytes(const_cast<std::byte *>(flag_addr), &state_byte, sizeof(state_byte)).has_value())
    {
        logger.error("Set{}{}: Write failed", desc, trigger);
        return false;
    }

    Sleep(1); // Yield briefly so the engine observes the store before the read-back verifies it.

    const auto after = DMK::Memory::seh_read<uint8_t>(reinterpret_cast<uintptr_t>(flag_addr));
    if (after && *after == new_state)
    {
        logger.info("Set{}{}: Success", desc, trigger);
        return true;
    }

    logger.error("Set{}{}: Failed (State={})", desc, trigger, after ? static_cast<int>(*after) : -1);
    return false;
}

bool safeToggleViewState(int *key_pressed_vk)
{
    DMK::Logger &logger = DMK::Logger::get_instance();
    std::string trigger = key_pressed_vk ? "(K:" + DMK::Format::format_vkcode(*key_pressed_vk) + ")" : "(I)";

    int current = getViewState();
    if (current == 0)
    {
        logger.info("Toggle{}: FPV->TPV", trigger);
        return setViewState(1, key_pressed_vk);
    }
    else if (current == 1)
    {
        logger.info("Toggle{}: TPV->FPV", trigger);
        return setViewState(0, key_pressed_vk);
    }
    else
    {
        logger.error("Toggle{}: Invalid state {}", trigger, current);
        return false;
    }
}

bool GetPlayerWorldTransform(::Vector3 &outPosition, ::Quaternion &outOrientation)
{
    DMK::Logger &logger = DMK::Logger::get_instance();

    if (!TPVToggle::player_transform().entity)
    {
        logger.debug("GetPlayerWorldTransform: Called but the player entity is currently NULL.");
        return false;
    }

    uintptr_t matrix_address = reinterpret_cast<uintptr_t>(TPVToggle::player_transform().entity) + Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER;

    // The player entity is engine-owned and may have been freed since it was
    // captured, so read the whole 3x4 matrix under one SEH frame rather than
    // gating a raw dereference with is_readable (which cannot prevent a fault
    // between the check and the read).
    const auto playerMatrixOpt = DMK::Memory::seh_read<GameStructures::Matrix34f>(matrix_address);
    if (!playerMatrixOpt)
    {
        logger.warning("GetPlayerWorldTransform: Cannot read CEntity's m_worldTransform at {} for entity {}",
                       format_address(matrix_address), format_address(reinterpret_cast<uintptr_t>(TPVToggle::player_transform().entity)));
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

    // Build and emit the verbose matrix dump only when trace logging is enabled;
    // the ostringstream and string concatenations are otherwise pure overhead.
    if (logger.is_enabled(LogLevel::Trace))
    {
        std::ostringstream matrix_dump;
        matrix_dump << std::fixed << std::setprecision(4);
        matrix_dump << "\n  Matrix Read from Entity " << format_address(reinterpret_cast<uintptr_t>(TPVToggle::player_transform().entity)) << " @ offset " << DMK::Format::format_hex(Constants::OFFSET_ENTITY_WORLD_MATRIX_MEMBER) << " (Addr: " << format_address(matrix_address) << "):";
        matrix_dump << "\n    R0: [" << playerMatrix.m[0][0] << ", " << playerMatrix.m[0][1] << ", " << playerMatrix.m[0][2] << "] T.x: " << playerMatrix.m[0][3];
        matrix_dump << "\n    R1: [" << playerMatrix.m[1][0] << ", " << playerMatrix.m[1][1] << ", " << playerMatrix.m[1][2] << "] T.y: " << playerMatrix.m[1][3];
        matrix_dump << "\n    R2: [" << playerMatrix.m[2][0] << ", " << playerMatrix.m[2][1] << ", " << playerMatrix.m[2][2] << "] T.z: " << playerMatrix.m[2][3];

        logger.log(LogLevel::Trace, "GetPlayerWorldTransform SUCCESS:" + matrix_dump.str());
        logger.log(LogLevel::Trace, "  Converted Pos: " + Vector3ToString(outPosition) + " | Converted Rot: " + QuatToString(outOrientation));
    }
    return true;
}

} // namespace TPVToggle
