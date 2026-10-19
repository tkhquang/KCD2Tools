/**
 * @file preset_store.hpp
 * @brief Owns the preset list and its JSON persistence; the only writer of presets.
 *
 * @details Single-threaded: every method is intended to run on ONE thread (the overlay
 *          UI thread at runtime, or the Bootstrap thread during init). Apart from the shoulder
 *          side load() restores, the store never touches the live camera. It publishes an immutable
 *          StateBindingTable snapshot (see preset_runtime.hpp) that the main thread
 *          consumes via a single atomic shared_ptr load (no allocation). CRUD operations that change
 *          applied values call publish().
 *
 *          The cycle picks (which member of each cycle group is in use) live in that snapshot, because
 *          the cycle hotkeys change them on the input thread without touching the store. The store
 *          seeds them on load, re-keys them on rename, drops the ones a change invalidates, and
 *          persists them in the presets JSON. The file also keeps the side SwapShoulderKey puts the
 *          camera on (CameraControls::shoulder_swapped). A cycle pick or shoulder change is written at
 *          once while its [Presets] AutoSaveCyclePick or [Camera] AutoSaveShoulder key is on. While the
 *          key is off the change lasts for the session, and only the Save button keeps it.
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

        /**
         * @brief Writes every live value to the cached path now: the Save button.
         * @details Also keeps a cycle pick or shoulder change that its auto-save key leaves unsaved.
         */
        void save();

        /**
         * @brief Writes the live values when they differ from the file (call on shutdown).
         * @details A cycle pick or shoulder change keeps the saved value while its auto-save key is off.
         */
        void flush();

        /**
         * @brief Writes the live cycle picks and shoulder side when they changed and their auto-save key is on.
         * @details Call on every pass of the overlay thread. The write keeps the saved presets, so a pending
         *          preset edit stays pending. After a failed write, auto-save stops until the next successful
         *          write, and the change shows on the Save indicator instead, so a locked file is not retried on
         *          every pass.
         */
        void auto_save();

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
        /** @brief Sets the overlay UI scale, clamped to [0.5, 3.0]. */
        void set_ui_scale(float scale);

        /** @brief Edit/show preset float values at 2 decimals (true) or 3 (false). Persisted in JSON root. */
        [[nodiscard]] bool value_compact() const noexcept { return m_value_compact; }
        /** @brief Sets the 2-vs-3 decimal editing precision. */
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

        /**
         * @brief Whether a value the presets file holds differs from the file: drives the Save indicator.
         * @details Counts the presets, the shared fields, the overlay settings, and a cycle pick or shoulder
         *          change that auto_save() does not write. The editing selection does not count: flush() writes
         *          it on shutdown.
         */
        [[nodiscard]] bool dirty() const;

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
         * @brief Republishes the binding table after the editor changed a field value (live preview).
         * @details Does not write the file: the Save button and the shutdown flush do.
         */
        void apply_edit();

    private:
        /// Rewrite of a list of cycle picks (see publish_table).
        using PickEdit = std::function<void(std::vector<CyclePick> &)>;

        /**
         * @struct FileContents
         * @brief Every value the presets file holds, as written or read.
         * @details The two unordered lists are kept sorted, so equality compares their content.
         */
        struct FileContents
        {
            std::vector<CameraPreset> presets;
            /// Preset field keys kept identical across every preset, sorted.
            std::vector<std::string> shared_fields;
            /// Cycle picks, sorted by mask.
            std::vector<CyclePick> cycle_picks;
            /// Name of the preset the editor shows.
            std::string editing;
            float ui_scale = 1.0f;
            bool value_compact = true;
            bool shoulder_swapped = false;

            /// Content equality: the shutdown flush writes only a difference.
            bool operator==(const FileContents &) const = default;
        };

        PresetStore() = default;

        /**
         * @brief Rebuilds and publishes the StateBindingTable snapshot for the main thread.
         * @details The live cycle picks carry over, pass through @p edit_picks, and lose every pick that no longer
         *          names a member of its group. A write reads the picks from the published snapshot, so a change
         *          publishes before it writes.
         * @param edit_picks Optional rewrite of the carried-over picks.
         */
        void publish(const PickEdit &edit_picks = {});

        /**
         * @brief Writes the live values after a list change (new, duplicate, rename, rebind, remove, reset).
         * @details A cycle pick or shoulder change that its auto-save key leaves unsaved keeps the saved value. The
         *          saved picks then pass through @p edit_picks, the same rewrite publish() gave the live ones, so a
         *          rename or a reset reaches the file.
         * @param edit_picks Optional rewrite of the saved picks.
         */
        void commit(const PickEdit &edit_picks = {});

        /**
         * @brief The live values in the form the file holds them.
         * @param keep_session Whether a cycle pick or shoulder change that its auto-save key leaves unsaved goes in.
         *                     When false the saved value goes in instead.
         * @param edit_saved_picks Optional rewrite of the saved picks when they go in.
         */
        [[nodiscard]] FileContents live_contents(bool keep_session, const PickEdit &edit_saved_picks = {}) const;

        /**
         * @brief Serializes @p contents to a sibling temp file and renames it over the presets file.
         * @details On success @p contents becomes the saved state and auto-save resumes. On failure the file and
         *          the saved state are unchanged.
         * @return True when the file now holds @p contents.
         */
        bool write(FileContents contents);

        /** @brief Re-adds any missing built-in from factory and orders all built-ins first; true if any was added. */
        [[nodiscard]] bool arrange_builtins();
        [[nodiscard]] int find_by_name(const std::string &name) const;
        [[nodiscard]] bool name_in_use(const std::string &name, int ignore_index) const;
        [[nodiscard]] std::string make_unique_name(const std::string &base) const;
        /** @brief Whether @p pick names a preset that is bound to exactly the pick's mask. */
        [[nodiscard]] bool is_live_pick(const CyclePick &pick) const;
        /** @brief The published cycle picks that name a member of their group. */
        [[nodiscard]] std::vector<CyclePick> live_cycle_picks() const;
        /** @brief The saved cycle picks after @p edit_picks, less those that no longer name a member of their group. */
        [[nodiscard]] std::vector<CyclePick> saved_cycle_picks(const PickEdit &edit_picks) const;
        /** @brief Copies field @p field's value from the editing preset into every preset (no save/publish). */
        void copy_field_from_editing(const PresetField &field) noexcept;
        /** @brief Copies the value of every shared field from @p source into @p target. */
        void copy_shared_fields(const CameraPreset &source, CameraPreset &target) const noexcept;

        std::vector<CameraPreset> m_presets;
        int m_editing_index = 0;
        bool m_editing_pinned = false;
        float m_ui_scale = 1.0f;
        bool m_value_compact = true;
        std::vector<std::string> m_shared_fields; // preset field keys kept identical across every preset
        // What the file holds since the last successful load or write. dirty() compares the live values against it,
        // so an edit reverted to the saved value clears the Save indicator.
        FileContents m_saved;
        // Set by a failed auto_save() write and cleared by the next successful write.
        bool m_auto_save_blocked = false;
        std::string m_file_path;
    };

} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_PRESET_STORE_HPP
