/**
 * @file global_state.cpp
 * @brief Storage backing the cross-module shared-state accessors.
 */

#include "global_state.hpp"

extern "C"
{
    std::atomic<std::byte *> g_global_context_ptr_address{nullptr};
}

namespace HenrySenses
{
    ModuleInfo &module_info() noexcept
    {
        static ModuleInfo state;
        return state;
    }

    std::atomic<bool> &game_world_ready() noexcept
    {
        static std::atomic<bool> ready{false};
        return ready;
    }

} // namespace HenrySenses
