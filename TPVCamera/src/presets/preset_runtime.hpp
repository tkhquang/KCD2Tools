/**
 * @file preset_runtime.hpp
 * @brief Render-thread resolver that selects the active preset by game state (or an
 *        editing pin), eases toward it, and applies it to the live settings.
 *
 * @details The UI/init thread publishes an immutable StateBindingTable snapshot via an
 *          atomic shared_ptr swap; the render thread reads it with one atomic shared_ptr load each
 *          frame inside the frustum-builder detour (a spinlock-guarded refcount bump on MSVC, no heap
 *          allocation). The render thread NEVER touches the PresetStore.
 *          Each preset binds to a SET of game-state bits (its mask). The resolver picks the bound
 *          preset whose mask is the most specific subset of the active state (most bits wins); equal-
 *          specificity ties break by a fixed bit priority (Aiming > Crouch > Combat > Mount > Lying > Sitting > Kneel > Cart), and an
 *          empty mask (DEFAULT) is the always-matching floor. This lets a user preset bound to e.g.
 *          {Aiming, Crouch} win over the single-state built-ins when both are active, without anyone
 *          having to author every combination.
 */
#ifndef TPVCAMERA_PRESETS_PRESET_RUNTIME_HPP
#define TPVCAMERA_PRESETS_PRESET_RUNTIME_HPP

#include "camera_preset.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace TPVCamera::Presets
{

/**
 * @struct StateBindingTable
 * @brief Parallel preset/mask vectors for every bound preset plus an optional editing-pin override,
 *        published as an immutable snapshot for the render thread.
 */
struct StateBindingTable
{
    /// Bindable presets (built-ins plus any state-bound user presets), in publish order (built-ins first).
    std::vector<CameraPreset> presets;
    /// Parallel to @ref presets: masks[i] is the GameState bit mask preset[i] auto-applies on (0 = floor).
    std::vector<std::uint32_t> masks;

    bool has_pin = false; ///< When set, @ref pinned overrides state selection (UI preview).
    CameraPreset pinned;  ///< The editing preset to preview live while the panel pins it.
};

/**
 * @brief Parses a preset's bind_state token list into the GameState mask it auto-applies on.
 * @details Tokens are comma-separated, lowercase: the stance/state tokens "combat", "aiming", "mount",
 *          "crouch" (alias "stealth"), "lying", "sitting", "kneel", "cart"; "minigame" and the per-minigame
 *          tokens from k_minigames (each binds {Minigame | that child}); and "default" (the empty-mask
 *          floor). "none" or an empty list means the preset is unbound and reachable only via the editing
 *          pin. Unknown tokens are ignored.
 * @param bind_state The preset's bind_state string (e.g. "aiming,crouch").
 * @return The mask (0 for the default floor), or std::nullopt when the preset is unbound.
 */
[[nodiscard]] std::optional<std::uint32_t> parse_bind_mask(std::string_view bind_state);

/**
 * @brief Builds the canonical bind_state token list for a user preset bound to @p mask.
 * @details Inverse of parse_bind_mask, emitting in priority order the stance/state tokens (aiming, crouch,
 *          combat, mount, lying, sitting, kneel, cart) plus a minigame token (the per-minigame token when a
 *          child bit is set, else "minigame"). An empty mask yields "none" (unbound / pin-only).
 */
[[nodiscard]] std::string bind_mask_to_tokens(std::uint32_t mask);

/**
 * @brief Picks the most-specific binding whose mask is a subset of @p active_state.
 * @details Specificity (set-bit count) dominates; equal-specificity ties break by a fixed bit priority
 *          (Aiming > Crouch > Combat > Mount > Lying > Sitting > Kneel > Cart); on identical masks the LATER entry wins (the '>=' tiebreak),
 *          so a user preset published after the built-ins overrides a built-in bound to the same states. A
 *          zero mask always matches (the floor). Shared by the render resolver and the overlay's
 *          active-preset highlight so the two cannot drift.
 * @param active_state The debounced GameState mask for this frame.
 * @param masks The candidate bind masks, parallel to the caller's preset list.
 * @return Index into @p masks of the winner, or -1 if no mask is a subset of @p active_state (given the
 *         always-matching empty-mask DEFAULT floor, that happens only when @p masks is empty).
 */
[[nodiscard]] int resolve_active_binding(std::uint32_t active_state,
                                         std::span<const std::uint32_t> masks) noexcept;

/**
 * @brief Publishes a new binding-table snapshot for the render thread (atomic swap).
 * @details Called by the PresetStore on the UI/init thread whenever presets, the
 *          editing selection, or the pin state change. Passing nullptr disables the
 *          resolver (the camera then keeps the INI-loaded live settings).
 */
void publish_table(std::shared_ptr<const StateBindingTable> table) noexcept;

/**
 * @brief Resolves the target preset for @p state, eases toward it, and applies it live.
 * @details Render-thread only. No-op until a binding table has been published. Reads the preset
 *          blend speed from LiveSettings.
 * @param state Debounced GameState mask for this frame.
 * @param delta_seconds Seconds since the previous active frame.
 */
void resolve_and_apply(uint32_t state, float delta_seconds) noexcept;

/**
 * @brief Marks the next active frame to SNAP (skip easing) instead of blending.
 * @details Render-thread only. Called when the offset goes inactive so resuming the
 *          third-person view does not ease across a suppression gap from a stale pose.
 */
void reset_transition() noexcept;

} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_PRESET_RUNTIME_HPP
