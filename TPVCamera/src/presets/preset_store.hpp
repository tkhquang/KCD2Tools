/**
 * @file preset_store.hpp
 * @brief Owns the preset list and its JSON persistence; the only writer of presets.
 *
 * @details Single-threaded: every method is intended to run on ONE thread (the overlay
 *          UI thread at runtime, or the Bootstrap thread during init). The store never
 *          touches the live camera directly; instead it publishes an immutable
 *          StateBindingTable snapshot (see preset_runtime.hpp) that the render thread
 *          consumes via a single atomic shared_ptr load (no allocation). CRUD operations that change
 *          applied values call publish().
 *
 *          The cycle picks (which member of each cycle group is in use) live in that snapshot, because
 *          the cycle hotkeys change them on the input thread without touching the store. The store
 *          seeds them on load, re-keys them on rename, drops the ones a change invalidates, and
 *          persists them in the presets JSON.
 *
 *          The built-ins (DEFAULT/COMBAT/AIMING/MOUNT/STEALTH/LYING/SITTING/KNEEL/CART) are defined by
 *          embedded factory data (default_presets.hpp), NOT shipped in a file: load() creates the presets
 *          file from them on first run and re-adds any built-in a user file lacks (so an update
 *          that introduces a built-in adds it without overwriting the player's customizations).
 *          They are kept at the front in that order and cannot be removed or renamed, but CAN be
 *          edited and reset to factory defaults. User presets follow. A fresh file also gets the
 *          factory starting points, which are ordinary user presets from then on.
 */
#ifndef TPVCAMERA_PRESETS_PRESET_STORE_HPP
#define TPVCAMERA_PRESETS_PRESET_STORE_HPP

#include "camera_preset.hpp"
#include "preset_runtime.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TPVCamera::Presets
{

    struct PresetField; // defined in camera_preset_fields.hpp; used here only by reference

    /**
     * @brief The embedded factory copy of the preset named @p name: a built-in or a factory starting point.
     * @details Backs reset-to-factory (whole preset) and per-field reset in the overlay.
     * @return The factory preset, or std::nullopt when no factory preset has that name.
     */
    [[nodiscard]] std::optional<CameraPreset> factory_preset(const std::string &name);

    /**
     * @class PresetStore
     * @brief Process-wide preset collection with JSON load/save and built-in protection.
     */
    class PresetStore
    {
    public:
        /** @brief Returns the process-wide store singleton. */
        [[nodiscard]] static PresetStore &instance();

        /**
         * @brief Loads presets from @p file_path (creating it from factory defaults if absent), then publishes.
         * @details Presets are optional and user-owned. A missing file is created from the embedded factory
         *          defaults; a corrupt file falls back to those defaults for the session and is left on disk
         *          untouched. The built-ins (DEFAULT/COMBAT/AIMING/MOUNT/STEALTH/LYING/SITTING/KNEEL/CART) are
         *          always present and ordered at the front afterward, and the path is cached for save()/flush().
         * @param file_path Absolute path to the presets JSON file (created next to the INI if it does not exist).
         */
        void load(const std::string &file_path);

        /** @brief Writes the presets to the cached path immediately and clears the dirty flag. */
        void save();

        /** @brief Writes only if there are unsaved changes, cycle picks included (call on shutdown). */
        void flush();

        /** @brief The preset list (built-ins first). UI thread only. */
        [[nodiscard]] std::vector<CameraPreset> &presets() noexcept { return m_presets; }

        /** @brief Index of the preset the editor is bound to. */
        [[nodiscard]] int editing_index() const noexcept { return m_editing_index; }
        /** @brief Selects the editing preset; clamps to range and republishes (preview). */
        void set_editing_index(int index);

        /** @brief Whether the editing preset overrides state selection (live preview). */
        [[nodiscard]] bool editing_pinned() const noexcept { return m_editing_pinned; }
        /** @brief Pins/unpins the editing preset for live preview; republishes. */
        void set_editing_pinned(bool pinned);

        /** @brief Overlay UI scale (ImGui font global scale); persisted in the presets JSON root. */
        [[nodiscard]] float ui_scale() const noexcept { return m_ui_scale; }
        /** @brief Sets the overlay UI scale (clamped to [0.5, 3.0]) and marks the store dirty. */
        void set_ui_scale(float scale);

        /** @brief Edit/show preset float values at 2 decimals (true) or 3 (false). Persisted in JSON root. */
        [[nodiscard]] bool value_compact() const noexcept { return m_value_compact; }
        /** @brief Sets the 2-vs-3 decimal editing precision and marks the store dirty. */
        void set_value_compact(bool compact);

        /** @brief Whether preset field @p key is shared (kept identical) across every preset. Persisted in JSON root.
         */
        [[nodiscard]] bool is_field_shared(std::string_view key) const noexcept;
        /**
         * @brief Links/unlinks preset field @p key so its value is shared across every preset.
         * @details Enabling immediately copies the editing preset's value of @p key into every preset (broadcast)
         *          so they all match, then republishes; disabling just drops the link (values stay where they are).
         *          Unknown keys are ignored. The shared-key set is persisted in the presets JSON root.
         */
        void set_field_shared(std::string_view key, bool shared);
        /**
         * @brief Copies the editing preset's value of shared field @p key into every preset, then republishes.
         * @details The live-sync half of a shared field: call after the editor mutates a shared field so the new
         *          value lands in every preset. No-op for an unknown key or an out-of-range editing index.
         */
        void broadcast_field(std::string_view key);

        /** @brief Whether there are unsaved preset VALUE changes (drives the Save indicator). UI-only bookkeeping
         *         such as the editing selection or overlay scale does not count here; it persists on shutdown. */
        [[nodiscard]] bool dirty() const noexcept { return m_dirty; }

        /**
         * @brief Adds a new user preset (auto-named, non-built-in), selects it, republishes.
         * @return Index of the new preset.
         */
        int add_new();

        /**
         * @brief Duplicates preset @p index as a new user preset, selects it, republishes.
         * @return Index of the new preset, or the original index if @p index is invalid.
         */
        int duplicate(int index);

        /**
         * @brief Renames preset @p index. Refused for built-ins and empty/duplicate names.
         * @return True on success.
         */
        bool rename(int index, const std::string &name);

        /**
         * @brief Sets the game states a USER preset auto-applies on (a bind_state token list). No-op for
         *        built-ins, whose bindings are fixed. Saves and republishes so the change takes effect live.
         * @param index The preset row to rebind.
         * @param bind_state Comma-separated tokens (e.g. "aiming,crouch"), or "none" to make it pin-only.
         */
        void set_bind_state(int index, const std::string &bind_state);

        /**
         * @brief Removes user preset @p index. Refused for built-ins. Clamps the editing index.
         * @return True if a preset was removed.
         */
        bool remove(int index);

        /**
         * @brief Resets preset @p index to its factory values, or does nothing when it has none (see factory_preset).
         * @details Name, binding and every shared field keep their values. A built-in's reset also adds back each
         *          factory starting point of its states that the list lacks, and keeps its cycle group on the
         *          member it used before. Saves and republishes.
         */
        void reset_to_factory(int index);

        /**
         * @brief Resets every factory preset (the built-ins and their starting points) to factory values and binding.
         * @details Adds back each missing starting point, and the cycle groups the factory ships start on their
         *          built-in again. The player's own presets, the picks of the groups they made and every shared field
         *          keep their values. Saves and republishes.
         */
        void reset_all_to_factory();

        /**
         * @brief Marks the store dirty and republishes the binding table (live preview).
         * @details Call after the editor mutates a field value via a slider/checkbox. Does NOT
         *          write to disk; the explicit Save action / shutdown flush persists.
         */
        void mark_dirty();

    private:
        PresetStore() = default;

        /**
         * @brief Rebuilds and publishes the StateBindingTable snapshot for the render thread.
         * @details The live cycle picks carry over, pass through @p edit_picks, and lose every pick that no longer
         *          names a member of its group. save() reads the picks from the published snapshot, so a change
         *          publishes before it saves.
         * @param edit_picks Optional rewrite of the carried-over picks (see publish_table).
         */
        void publish(const std::function<void(std::vector<CyclePick> &)> &edit_picks = {});

        /** @brief Re-adds any missing built-in from factory and orders all built-ins first; true if any was added. */
        [[nodiscard]] bool arrange_builtins();
        [[nodiscard]] int find_by_name(const std::string &name) const;
        [[nodiscard]] bool name_in_use(const std::string &name, int ignore_index) const;
        [[nodiscard]] std::string make_unique_name(const std::string &base) const;
        /** @brief Whether @p pick names a preset that is bound to exactly the pick's mask. */
        [[nodiscard]] bool is_live_pick(const CyclePick &pick) const;
        /** @brief The published cycle picks that name a member of their group: what save() writes. */
        [[nodiscard]] std::vector<CyclePick> persistable_cycle_picks() const;
        /** @brief Copies field @p field's value from the editing preset into every preset (no save/publish). */
        void copy_field_from_editing(const PresetField &field) noexcept;
        /** @brief Copies the value of every shared field from @p source into @p target. */
        void copy_shared_fields(const CameraPreset &source, CameraPreset &target) const noexcept;
        /** @brief Recomputes m_dirty as the difference between the live presets/shared set and the saved baseline. */
        void recompute_dirty() noexcept;
        /**
         * @brief Snapshots the live presets/shared set and @p cycle_picks as the saved baseline (after a successful
         *        load/save).
         * @param cycle_picks The cycle picks the file now holds.
         */
        void capture_saved_baseline(std::vector<CyclePick> cycle_picks);

        std::vector<CameraPreset> m_presets;
        int m_editing_index = 0;
        bool m_editing_pinned = false;
        bool m_dirty = false;       // unsaved preset VALUE changes; drives the Save * highlight and forces a save
        bool m_prefs_dirty = false; // unsaved editor/UI bookkeeping (selection, scale, precision): flushed on
                                    // shutdown but NOT surfaced as a pending value change
        float m_ui_scale = 1.0f;
        bool m_value_compact = true;
        std::vector<std::string> m_shared_fields; // preset field keys kept identical across every preset
        // Snapshot of the on-disk state (presets + shared set) taken at the last successful load/save. The Save
        // indicator (m_dirty) is the difference between the live data and this baseline, so reverting an edit back
        // to the saved value clears it.
        std::vector<CameraPreset> m_saved_presets;
        std::vector<std::string> m_saved_shared_fields;
        // The cycle picks the file holds. The cycle hotkeys change the live picks without the store, so flush()
        // compares against this instead of a dirty flag. The picks never light the Save indicator.
        std::vector<CyclePick> m_saved_cycle_picks;
        std::string m_file_path;
    };

} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_PRESET_STORE_HPP
