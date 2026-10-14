/**
 * @file hooks/input_hook.hpp
 * @brief The input-dispatcher hook: free-look takes the mouse and pad look while the orbit is on.
 */
#ifndef TPVCAMERA_HOOKS_INPUT_HOOK_HPP
#define TPVCAMERA_HOOKS_INPUT_HOOK_HPP

#include "hooks/hook_set.hpp"

namespace TPVCamera
{
    /**
     * @brief Hooks the generic input-event dispatcher. Best-effort: a miss only disables free-look orbit, so it is a
     *        warning. The detour is inert until free-look is on, so it is harmless when free-look is unused.
     */
    void install_input_dispatch_hook(HookSet &hooks);
} // namespace TPVCamera

#endif // TPVCAMERA_HOOKS_INPUT_HOOK_HPP
