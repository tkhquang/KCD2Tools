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
 *          The built-ins (DEFAULT/COMBAT/AIMING/MOUNT/STEALTH/LYING/SITTING/KNEEL/CART) are defined by
 *          embedded factory data (default_presets.hpp), NOT shipped in a file: load() creates the presets
 *          file from them on first run and re-adds any built-in a user file lacks (so an update
 *          that introduces a built-in adds it without overwriting the player's customizations).
 *          They are kept at the front in that order and cannot be removed or renamed, but CAN be
 *          edited and reset to factory defaults. User presets follow.
 */
#ifndef TPVCAMERA_PRESETS_PRESET_STORE_HPP
#define TPVCAMERA_PRESETS_PRESET_STORE_HPP

#include "camera_preset.hpp"

#include <string>
#include <vector>

namespace TPVCamera::Presets
{

    /**
     * @brief The embedded factory copy of a built-in preset by @p name, or a struct-default CameraPreset for
     *        any non-built-in name. Backs reset-to-factory (whole preset) and per-field reset in the overlay.
     */
    [[nodiscard]] CameraPreset factory_preset(const std::string &name);

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

        /** @brief Writes only if there are unsaved changes (call on shutdown). */
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

        /** @brief Whether there are changes not yet written to disk. */
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

        /** @brief Resets a built-in to its hard-coded factory values (no-op for user presets). */
        void reset_to_factory(int index);

        /**
         * @brief Marks the store dirty and republishes the binding table (live preview).
         * @details Call after the editor mutates a field value via a slider/checkbox. Does NOT
         *          write to disk; the explicit Save action / shutdown flush persists.
         */
        void mark_dirty();

        /** @brief Rebuilds and publishes the StateBindingTable snapshot for the render thread. */
        void publish();

    private:
        PresetStore() = default;

        /** @brief Re-adds any missing built-in from factory and orders all built-ins first; true if any was added. */
        [[nodiscard]] bool arrange_builtins();
        [[nodiscard]] int find_by_name(const std::string &name) const;
        [[nodiscard]] bool name_in_use(const std::string &name, int ignore_index) const;
        [[nodiscard]] std::string make_unique_name(const std::string &base) const;

        std::vector<CameraPreset> m_presets;
        int m_editing_index = 0;
        bool m_editing_pinned = false;
        bool m_dirty = false;
        float m_ui_scale = 1.0f;
        bool m_value_compact = true;
        std::string m_file_path;
    };

} // namespace TPVCamera::Presets

#endif // TPVCAMERA_PRESETS_PRESET_STORE_HPP
