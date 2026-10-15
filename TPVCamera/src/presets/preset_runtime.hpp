/**
 * @file preset_runtime.hpp
 * @brief Main-thread resolver that selects the active preset by game state (or an
 *        editing pin), eases toward it, and applies it to the live settings.
 *
 * @details The UI/init thread publishes an immutable StateBindingTable snapshot via an
 *          atomic shared_ptr swap; the main thread reads it with one atomic shared_ptr load each
 *          frame inside the frustum-builder detour (a spinlock-guarded refcount bump on MSVC, no heap
 *          allocation). The main thread NEVER touches the PresetStore.
 *          Each preset binds to a SET of game-state bits (its mask). The resolver picks the bound
 *          preset whose mask is the most specific subset of the active state (most bits wins); equal-
 *          specificity ties break by a fixed bit priority
 *          (Aiming > Crouch > Combat > Mount > Lying > Sitting > Kneel > Cart), and an
 *          empty mask (DEFAULT) is the always-matching floor. This lets a user preset bound to e.g.
 *          {Aiming, Crouch} win over the single-state built-ins when both are active, without anyone
 *          having to author every combination.
 *
 *          Presets bound to the identical mask form a cycle group, and the snapshot's cycle picks name the
 *          member each group uses. Three writers publish: the store (presets, masks, pin) and the overlay's
 *          group panel (picks) on the UI thread, and the cycle hotkeys (picks) on the input thread. Every
 *          publish is a compare-exchange on the one snapshot that carries the other writers' part over from
 *          the live table, so no update is lost and the main thread always reads presets and picks that
 *          belong together.
 */
#ifndef TPVCAMERA_PRESETS_PRESET_RUNTIME_HPP
#define TPVCAMERA_PRESETS_PRESET_RUNTIME_HPP

#include "camera_preset.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TPVCamera::Presets
{

    /**
     * @struct CyclePick
     * @brief The member a cycle group uses: the group's bind mask and the name of the chosen preset.
     */
    struct CyclePick
    {
        /// Bind mask shared by every member of the group (0 = the empty-mask floor).
        std::uint32_t mask = 0;
        /// Name of the chosen member.
        std::string preset;

        /// Content equality, so the store can tell whether the picks changed since the last save.
        bool operator==(const CyclePick &) const = default;
    };

    /**
     * @struct StateBindingTable
     * @brief Parallel preset/mask vectors for every bound preset, the cycle picks, and an optional editing-pin
     *        override, published as an immutable snapshot for the main thread.
     */
    struct StateBindingTable
    {
        /// Bindable presets (built-ins plus any state-bound user presets), in publish order (built-ins first).
        std::vector<CameraPreset> presets;
        /// Parallel to @ref presets: masks[i] is the GameState bit mask preset[i] auto-applies on (0 = floor).
        std::vector<std::uint32_t> masks;
        /**
         * @brief The chosen member of each cycle group, at most one per mask.
         * @details A group without a pick, or whose pick names a preset that is not one of its members, uses its
         *          last member (see cycle_group_entry).
         */
        std::vector<CyclePick> cycle_picks;

        /// When set, @ref pinned overrides state selection (UI preview).
        bool has_pin = false;
        /// The editing preset to preview live while the panel pins it.
        CameraPreset pinned;
    };

    /**
     * @struct CycleStep
     * @brief Outcome of one cycle-hotkey press: the group that was cycled and the member it now uses.
     */
    struct CycleStep
    {
        /// Bind mask of the cycled group.
        std::uint32_t mask = 0;
        /// Name of the member the group now uses.
        std::string preset;
        /// 1-based position of that member in the group, in publish order.
        int position = 0;
        /// Number of presets in the group, where 1 means there was nothing to cycle to.
        int count = 0;
    };

    /**
     * @brief Parses a preset's bind_state token list into the GameState mask it auto-applies on.
     * @details Tokens are comma-separated, lowercase: the stance/state tokens "combat", "aiming", "mount",
     *          "crouch" (alias "stealth"), "lying", "sitting", "kneel", "cart"; "minigame" and the per-minigame
     *          tokens from k_minigames (each binds {Minigame | that child}); "indoors"; and "default" (the empty-mask
     *          floor). "none" or an empty list means the preset is unbound and reachable only via the editing
     *          pin. Unknown tokens are ignored.
     * @param bind_state The preset's bind_state string (e.g. "aiming,crouch").
     * @return The mask (0 for the default floor), or std::nullopt when the preset is unbound.
     */
    [[nodiscard]] std::optional<std::uint32_t> parse_bind_mask(std::string_view bind_state);

    /**
     * @brief Builds the canonical bind_state token list for a user preset bound to @p mask.
     * @details Inverse of parse_bind_mask, emitting in priority order the stance/state tokens (aiming, crouch,
     *          combat, mount, lying, sitting, kneel, cart), a minigame token (the per-minigame token when a child bit
     *          is set, else "minigame") and "indoors". An empty mask yields "none" (unbound / pin-only).
     */
    [[nodiscard]] std::string bind_mask_to_tokens(std::uint32_t mask);

    /**
     * @brief The token list that names the cycle group bound to exactly @p mask.
     * @details "default" for the empty-mask floor, otherwise bind_mask_to_tokens(mask). parse_bind_mask reads it
     *          back to @p mask, so it keys a group's pick in the presets file and names the group in the log.
     */
    [[nodiscard]] std::string cycle_group_tokens(std::uint32_t mask);

    /**
     * @brief Index of the entry that drives the camera in @p active_state, ignoring the editing pin.
     * @details The most-specific binding whose mask is a subset of @p active_state wins: specificity (set-bit
     *          count) dominates, and equal-specificity ties break by a fixed bit priority (Aiming > Crouch >
     *          Combat > Mount > Lying > Sitting > Kneel > Cart > Minigame > a per-minigame child > Indoors). cycle_group_entry then picks the member of the
     *          winning mask's group. Shared by the main-thread resolver, the cycle hotkeys and the overlay's
     *          active-preset highlight, so the three cannot drift.
     * @param table The published snapshot.
     * @param active_state The debounced GameState mask for this frame.
     * @return Index into table.presets, or -1 when the table has no entries (the always-matching empty-mask
     *         DEFAULT floor otherwise guarantees a winner).
     */
    [[nodiscard]] int resolve_active_entry(const StateBindingTable &table, std::uint32_t active_state) noexcept;

    /**
     * @brief Index of the entry cycle group @p mask uses: its picked member, or else its last member.
     * @details The last-member default keeps the rule that a preset published later (a user preset after the
     *          built-ins) overrides an earlier one bound to the identical states until the player cycles.
     * @param table The published snapshot.
     * @param mask The group's bind mask.
     * @return Index into table.presets, or -1 when no entry is bound to exactly @p mask.
     */
    [[nodiscard]] int cycle_group_entry(const StateBindingTable &table, std::uint32_t mask) noexcept;

    /**
     * @brief Indices of the entries bound to exactly @p mask, in publish order, which is the cycle order.
     * @param table The published snapshot.
     * @param mask The group's bind mask.
     */
    [[nodiscard]] std::vector<std::size_t> cycle_group_members(const StateBindingTable &table, std::uint32_t mask);

    /**
     * @brief Publishes the store's presets, masks and pin as the new snapshot for the main thread.
     * @details Called by the PresetStore on the UI/init thread whenever presets, the editing selection, or the
     *          pin state change. The store does not own the cycle picks: the call carries them over from the live
     *          snapshot and passes them through @p edit_picks. If a cycle hotkey publishes in between, the
     *          compare-exchange fails and the call carries the newer picks over, so the hotkey's pick survives.
     * @param table The store's presets, masks and pin. Its cycle_picks are replaced.
     * @param edit_picks Rewrite of the carried-over picks. It can run more than once and must derive its result from
     *                   its argument alone.
     */
    void publish_table(StateBindingTable table, const std::function<void(std::vector<CyclePick> &)> &edit_picks);

    /** @brief The snapshot the main thread resolves from, or nullptr before the first publish. */
    [[nodiscard]] std::shared_ptr<const StateBindingTable> published_table() noexcept;

    /**
     * @brief Every GameState bit some published binding requires: the union of the table's masks, 0 before the first
     *        publish.
     * @details Lets the main thread skip asking the engine for a state no preset can match on (Indoors).
     */
    [[nodiscard]] std::uint32_t bound_state_bits() noexcept;

    /**
     * @brief Makes cycle group @p mask use the member named @p preset and publishes the pick.
     * @details No-op before the first publish. A name that is not a member of the group is kept as the pick but
     *          leaves the group on its default member.
     */
    void set_cycle_pick(std::uint32_t mask, std::string_view preset);

    /**
     * @brief Moves the cycle group of @p state @p step members along, wrapping around, and publishes the pick.
     * @details The group is the one resolve_active_entry resolves for @p state, and the step starts from the member
     *          the camera uses now. A group with one member is left unchanged. Safe on any thread, concurrently with
     *          the store's publishes.
     * @param step +1 for the next member, -1 for the previous one.
     * @param state The debounced GameState mask the main thread last published.
     * @return The cycled group and the member it now uses, or std::nullopt before the first publish.
     */
    [[nodiscard]] std::optional<CycleStep> step_cycle(int step, std::uint32_t state);

    /**
     * @brief Resolves the target preset for @p state, eases toward it, and applies it live.
     * @details Main-thread only. No-op until a binding table has been published. Reads the preset
     *          blend speed from LiveSettings.
     * @param state Debounced GameState mask for this frame.
     * @param delta_seconds Seconds since the previous active frame.
     */
    void resolve_and_apply(uint32_t state, float delta_seconds) noexcept;

    /**
     * @brief Adds @p offset to the follow distance the preset ease currently holds.
     * @details Main-thread only. A zoom reset hands its offset over here: the camera keeps its distance on the
     *          reset frame and then eases to the preset's own distance in its blend time instead of jumping by
     *          the zoom. Before the first applied frame, or when the next frame snaps, the offset is dropped.
     */
    void ease_out_zoom_offset(float offset) noexcept;

    /**
     * @brief Marks the next active frame to SNAP (skip easing) instead of blending.
     * @details Main-thread only. Called when the offset goes inactive so resuming the
     *          third-person view does not ease across a suppression gap from a stale pose.
     */
    void reset_transition() noexcept;

} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_PRESET_RUNTIME_HPP
