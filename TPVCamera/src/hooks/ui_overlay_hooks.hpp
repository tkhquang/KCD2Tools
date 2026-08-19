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

#include <DetourModKit/error.hpp>
#include <DetourModKit/hook.hpp>

namespace TPVCamera
{

    /**
     * @brief Installs the UI overlay show/hide hooks from the pre-resolved anchors.
     * @param hooks The mod's hook stack; each installed handle is pushed in install order.
     * @return An empty value when both hooks installed, or the library Error that refused the install
     *         (ErrorCode::NoMatch when an anchor cascade did not resolve).
     * @note Call after resolve_all_anchors(); the hook targets are read via anchor_address().
     */
    [[nodiscard]] DMK::Result<void> initialize_ui_overlay_hooks(DMK::hook::HookStack &hooks);

} // namespace TPVCamera

#endif // TPVCAMERA_UI_OVERLAY_HOOKS_HPP
