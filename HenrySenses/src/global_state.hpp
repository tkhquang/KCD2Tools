/**
 * @file global_state.hpp
 * @brief Cross-module shared state grouped behind accessor functions.
 *
 * @details Related state lives in small grouped structs reached through reference-returning accessors (C++ Core
 *          Guideline I.2: avoid non-const globals). Each accessor owns a single function-local static, so storage is
 *          constructed on first use with no static-initialization-order dependency between translation units.
 */
#ifndef HENRYSENSES_GLOBAL_STATE_HPP
#define HENRYSENSES_GLOBAL_STATE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>

// Resolved global-context storage slot. Kept as a stable unmangled symbol so external tooling (Cheat Engine, x64dbg
// with the PDB) can locate it. Atomic because the gate readers load it while shutdown nulls it; relaxed is enough
// for a standalone pointer whose every use re-validates the chain it starts.
extern "C"
{
    extern std::atomic<std::byte *> g_global_context_ptr_address;
}

namespace HenrySenses
{
    /** @brief Base address and image size of the resolved game module. */
    struct ModuleInfo
    {
        std::uintptr_t base{0};
        std::size_t size{0};
    };

    /** @brief Returns the process-wide resolved game module info. */
    [[nodiscard]] ModuleInfo &module_info() noexcept;

    /**
     * @brief One-shot flag, set once the local player (C_Player) first resolves in-world.
     * @details Gates the context-member self-heal so it never scans a half-built context at the main menu.
     */
    [[nodiscard]] std::atomic<bool> &game_world_ready() noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_GLOBAL_STATE_HPP
