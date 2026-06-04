/**
 * @file hooks/ui_overlay_hooks.hpp
 * @brief Header for UI overlay hooks functionality.
 *
 * Provides a function to initialize hooks that directly intercept the game's UI
 * overlay show and hide functions. The camera reads overlay_state().active to
 * suppress the third-person offset while an overlay (inventory, map, dialog,
 * codex) is up, so the view renders from the untouched engine frame under any UI.
 */
#ifndef TPVCAMERA_UI_OVERLAY_HOOKS_HPP
#define TPVCAMERA_UI_OVERLAY_HOOKS_HPP

#include <cstdint>
#include <cstddef>

namespace TPVCamera
{

/**
 * @brief Initialize UI overlay hooks.
 * @param module_base Base address of the target game module.
 * @param module_size Size of the target game module in bytes.
 * @return true if initialization successful, false otherwise.
 */
[[nodiscard]] bool initialize_ui_overlay_hooks(uintptr_t module_base, size_t module_size);

} // namespace TPVCamera

#endif // TPVCAMERA_UI_OVERLAY_HOOKS_HPP
