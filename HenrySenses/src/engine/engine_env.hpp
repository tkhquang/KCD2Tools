/**
 * @file engine/engine_env.hpp
 * @brief SSystemGlobalEnvironment (gEnv) access, engine interface pointers and the local player.
 *
 * gEnv is an inline struct inside WHGame.dll, resolved by AnchorId::Genv. Every interface pointer it holds is read
 * fresh on each call through guarded memory, because several are null until the engine creates them and the game
 * can recreate them on a level load.
 */
#ifndef HENRYSENSES_ENGINE_ENV_HPP
#define HENRYSENSES_ENGINE_ENV_HPP

#include "aob_resolver.hpp"

#include <DetourModKit/error.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace HenrySenses
{
    /**
     * @brief Publishes the resolved gEnv base.
     * @return An empty value on success, or ErrorCode::NoMatch when the Genv anchor did not resolve.
     * @note Call after resolve_all_anchors(). Everything engine-facing in the mod depends on it, so a failure is fatal
     *       to init.
     */
    [[nodiscard]] DMK::Result<void> initialize_engine_env();

    /** @brief Clears the published gEnv base and the cached framework pointer. */
    void shutdown_engine_env() noexcept;

    /**
     * @brief Returns the gEnv base address.
     * @return The address, or 0 before initialize_engine_env() or after shutdown.
     */
    [[nodiscard]] std::uintptr_t genv() noexcept;

    /**
     * @brief Reads the interface pointer stored at gEnv + @p member_offset.
     * @param member_offset One of the constants::GENV_*_OFFSET values.
     * @return The pointer, or 0 when gEnv is unresolved, the read faults, or the value is not a plausible pointer.
     * @note Callback-safe: one guarded read.
     */
    [[nodiscard]] std::uintptr_t genv_interface(std::ptrdiff_t member_offset) noexcept;

    /**
     * @brief Returns CCryAction, the IGameFramework implementation (resolved once through IGame::GetIGameFramework).
     * @return The CCryAction address, or 0 before the game framework exists.
     */
    [[nodiscard]] std::uintptr_t cry_action() noexcept;

    /**
     * @brief Resolves the live local player (C_Player), validated by RTTI.
     * @details Walks gEnv -> pGame -> IGame::GetIGameFramework (CCryAction, cached) -> CActionGame -> C_Player and
     *          publishes each live base to the offset self-heal. The GetIGameFramework virtual call runs under SEH
     *          after its target is screened against the WHGame.dll image.
     * @return The C_Player address, or 0 when the player does not exist (main menu, loading) or any link fails.
     */
    [[nodiscard]] std::uintptr_t resolve_c_player() noexcept;

    /**
     * @brief Returns the local player's CEntity, validated by RTTI.
     * @param c_player A C_Player from resolve_c_player().
     * @return The CEntity address, or 0.
     */
    [[nodiscard]] std::uintptr_t player_entity(std::uintptr_t c_player) noexcept;

    /**
     * @brief Reports whether an address lies inside the WHGame.dll image.
     * @param address The address to screen.
     * @return True when inside the image.
     * @note Callback-safe: two loads and a compare.
     */
    [[nodiscard]] bool in_game_image(std::uintptr_t address) noexcept;

    /**
     * @brief Follows up to two leading E9 rel32 jumps (incremental-link or ICF thunks) to the real function.
     * @param function A code address inside the game image.
     * @return The final target, or @p function when it does not start with a jump or a read fails.
     */
    [[nodiscard]] std::uintptr_t follow_jump_thunk(std::uintptr_t function) noexcept;

    /**
     * @brief Reads a virtual function pointer from a live object's vtable.
     * @param object The object (its first qword is the vtable).
     * @param slot_offset Byte offset of the slot inside the vtable.
     * @return The function address when the vtable and the function both lie inside the game image, else 0.
     * @note Callback-safe: guarded reads only.
     */
    [[nodiscard]] std::uintptr_t read_vtable_slot(std::uintptr_t object, std::ptrdiff_t slot_offset) noexcept;

    /**
     * @brief Reads a vtable slot and proves it is the AOB-resolved function of @p expected.
     * @details The slot's target, after following jump thunks, must equal the resolved anchor address. This is how
     *          every identity-critical virtual call is validated before the mod calls it: a vtable layout that shifted
     *          in a game patch fails here instead of calling the wrong method.
     * @param object The object.
     * @param slot_offset Byte offset of the slot inside the vtable.
     * @param expected The validator anchor.
     * @return The function address to call (the slot value), or 0 on any mismatch or failed read.
     */
    [[nodiscard]] std::uintptr_t
    validated_vtable_slot(std::uintptr_t object, std::ptrdiff_t slot_offset, AnchorId expected) noexcept;

    /**
     * @brief Reads a NUL-terminated engine string through guarded reads.
     * @details The string is read in aligned chunks that never cross a page, so a short string near the end of a
     *          mapped page still reads.
     * @param text The string's address.
     * @param max_length The longest text kept; a longer string is cut there.
     * @return The text, or an empty string when the address is not plausible or the string runs into memory that
     *         does not read before its terminator.
     */
    [[nodiscard]] std::string read_c_string(std::uintptr_t text, std::size_t max_length);

} // namespace HenrySenses

#endif // HENRYSENSES_ENGINE_ENV_HPP
