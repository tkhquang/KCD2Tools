/**
 * @file preset_store.cpp
 * @brief PresetStore: JSON persistence, built-in protection, CRUD, and publishing.
 */

#include "preset_store.hpp"
#include "camera_preset_fields.hpp"
#include "default_presets.hpp"
#include "preset_runtime.hpp"

#include <DetourModKit.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace TPVCamera::Presets
{
    namespace
    {

        using json = nlohmann::json;

        constexpr int k_schema_version = 1;

        /// Canonical built-in order, with the state each binds to.
        struct BuiltinSpec
        {
            const char *name;
            const char *bind_state;
        };
        constexpr BuiltinSpec k_builtins[] = {
            {k_builtin_default, "default"}, {k_builtin_combat, "combat"},  {k_builtin_aiming, "aiming"},
            {k_builtin_mount, "mount"},     {k_builtin_stealth, "crouch"}, {k_builtin_lying, "lying"},
            {k_builtin_sitting, "sitting"}, {k_builtin_kneel, "kneel"},    {k_builtin_cart, "cart"},
        };

        [[nodiscard]] bool is_builtin_name(const std::string &name) noexcept
        {
            for (const BuiltinSpec &spec : k_builtins)
            {
                if (name == spec.name)
                    return true;
            }
            return false;
        }

        /// The editable PresetField with this key, or nullptr if none. Guards the shared-field machinery and the
        /// JSON loader against a key that is not a real field (e.g. a stale entry in a hand-edited file).
        [[nodiscard]] const PresetField *find_field(std::string_view key) noexcept
        {
            for (const PresetField &field : fields())
            {
                if (key == field.key)
                    return &field;
            }
            return nullptr;
        }

        json preset_to_json(const CameraPreset &preset)
        {
            json out;
            out["name"] = preset.name;
            out["builtin"] = preset.builtin;
            out["bind_state"] = preset.bind_state;

            // Always write EVERY field (even at its default) so the JSON is a complete, self-documenting,
            // hand-editable record of the preset. preset_from_json still tolerates missing keys.
            json values = json::object();
            for (const PresetField &field : fields())
            {
                if (field.type == FieldType::Float)
                    values[field.key] = preset.*(field.f);
                else
                    values[field.key] = preset.*(field.b);
            }
            out["fields"] = values;
            return out;
        }

        CameraPreset preset_from_json(const json &in)
        {
            CameraPreset preset; // starts at factory defaults

            if (in.contains("name") && in["name"].is_string())
                preset.name = in["name"].get<std::string>();
            if (in.contains("builtin") && in["builtin"].is_boolean())
                preset.builtin = in["builtin"].get<bool>();
            if (in.contains("bind_state") && in["bind_state"].is_string())
                preset.bind_state = in["bind_state"].get<std::string>();

            if (in.contains("fields") && in["fields"].is_object())
            {
                const json &values = in["fields"];
                for (const PresetField &field : fields())
                {
                    if (!values.contains(field.key))
                        continue;
                    const json &v = values[field.key];
                    if (field.type == FieldType::Float && v.is_number())
                        preset.*(field.f) = v.get<float>();
                    else if (field.type == FieldType::Bool && v.is_boolean())
                        preset.*(field.b) = v.get<bool>();
                }

                // Backward compatibility: orbit_sensitivity / gamepad_orbit_speed were each split into per-axis
                // X/Y fields. A preset written before the split carries only the old single key; seed BOTH new
                // axes from it (when neither new key is present) so an existing tune is not silently lost.
                const auto seed_axes_from_legacy = [&values, &preset](const char *legacy_key, const char *x_key,
                                                                      const char *y_key, float CameraPreset::*x,
                                                                      float CameraPreset::*y)
                {
                    if (values.contains(legacy_key) && values[legacy_key].is_number() && !values.contains(x_key) &&
                        !values.contains(y_key))
                    {
                        const float v = values[legacy_key].get<float>();
                        preset.*x = v;
                        preset.*y = v;
                    }
                };
                seed_axes_from_legacy("orbit_sensitivity", "orbit_sensitivity_x", "orbit_sensitivity_y",
                                      &CameraPreset::orbit_sensitivity_x, &CameraPreset::orbit_sensitivity_y);
                seed_axes_from_legacy("gamepad_orbit_speed", "gamepad_orbit_speed_x", "gamepad_orbit_speed_y",
                                      &CameraPreset::gamepad_orbit_speed_x, &CameraPreset::gamepad_orbit_speed_y);
            }
            return preset;
        }

        /// Parses the embedded factory JSON once into the canonical built-in preset list.
        const std::vector<CameraPreset> &factory_presets()
        {
            static const std::vector<CameraPreset> presets = []
            {
                std::vector<CameraPreset> out;
                try
                {
                    const json root = json::parse(k_default_presets_json);
                    if (root.contains("presets") && root["presets"].is_array())
                    {
                        for (const json &entry : root["presets"])
                            out.push_back(preset_from_json(entry));
                    }
                }
                catch (const json::exception &)
                {
                    // The literal is authored in-repo; a throw here is a build-time authoring error. Leaving the
                    // list empty makes factory_preset() fall back to struct defaults rather than crashing.
                }
                return out;
            }();
            return presets;
        }

    } // namespace

    CameraPreset factory_preset(const std::string &name)
    {
        for (const CameraPreset &p : factory_presets())
        {
            if (p.name == name)
                return p;
        }
        return CameraPreset{};
    }

    PresetStore &PresetStore::instance()
    {
        static PresetStore store;
        return store;
    }

    int PresetStore::find_by_name(const std::string &name) const
    {
        for (std::size_t i = 0; i < m_presets.size(); ++i)
        {
            if (m_presets[i].name == name)
                return static_cast<int>(i);
        }
        return -1;
    }

    bool PresetStore::name_in_use(const std::string &name, int ignore_index) const
    {
        for (std::size_t i = 0; i < m_presets.size(); ++i)
        {
            if (static_cast<int>(i) == ignore_index)
                continue;
            if (m_presets[i].name == name)
                return true;
        }
        return false;
    }

    std::string PresetStore::make_unique_name(const std::string &base) const
    {
        if (find_by_name(base) < 0)
            return base;
        for (int n = 2; n < 10000; ++n)
        {
            std::string candidate = base + " " + std::to_string(n);
            if (find_by_name(candidate) < 0)
                return candidate;
        }
        return base + " (copy)";
    }

    bool PresetStore::arrange_builtins()
    {
        DMK::Logger &logger = DMK::Logger::get_instance();

        // Built-ins are NOT shipped in a file; they are embedded factory data. Any canonical built-in the
        // loaded set lacks (a fresh install, a user who deleted one, or a file that predates a new built-in
        // such as AIMING added by an update) is re-added from the embedded factory copy. The return value
        // tells load() whether the on-disk set changed and should be re-saved.
        bool added = false;
        for (const BuiltinSpec &spec : k_builtins)
        {
            if (find_by_name(spec.name) < 0)
            {
                // Set identity explicitly so the entry is locatable even if the embedded factory entry is
                // absent (factory_preset() then returns a nameless struct default); the reorder loop below
                // relies on find_by_name() resolving it.
                CameraPreset preset = factory_preset(spec.name);
                preset.name = spec.name;
                preset.builtin = true;
                preset.bind_state = spec.bind_state;
                m_presets.push_back(std::move(preset));
                added = true;
                logger.info("Added missing built-in preset '{}' from factory defaults", spec.name);
            }
        }

        // Reorder the canonical built-ins to the front (normalizing builtin / bind_state), users after.
        std::vector<CameraPreset> users;
        for (CameraPreset &p : m_presets)
        {
            if (!is_builtin_name(p.name))
            {
                p.builtin = false;
                users.push_back(p);
            }
        }

        std::vector<CameraPreset> result;
        result.reserve(std::size(k_builtins) + users.size());
        for (const BuiltinSpec &spec : k_builtins)
        {
            CameraPreset preset = m_presets[static_cast<std::size_t>(find_by_name(spec.name))];
            preset.name = spec.name;
            preset.builtin = true;
            preset.bind_state = spec.bind_state;
            result.push_back(std::move(preset));
        }
        for (CameraPreset &u : users)
            result.push_back(std::move(u));

        m_presets = std::move(result);
        return added;
    }

    void PresetStore::load(const std::string &file_path)
    {
        DMK::Logger &logger = DMK::Logger::get_instance();
        m_file_path = file_path;
        m_presets.clear();
        m_dirty = false;
        m_prefs_dirty = false;
        m_ui_scale = 1.0f;
        m_value_compact = true;
        m_shared_fields.clear();
        m_saved_presets.clear();
        m_saved_shared_fields.clear();

        std::string editing_name = k_builtin_default;

        // Presets are user-owned customization, not a shipped asset (see default_presets.hpp). The file is
        // OPTIONAL: a missing one is created from the embedded factory defaults on first run, and a corrupt
        // one falls back to those defaults for the session without being overwritten. The mod never fails
        // to start over presets.
        bool file_present = false;
        bool file_usable = false;

        std::ifstream in(file_path);
        if (in)
        {
            file_present = true;
            try
            {
                const json root = json::parse(in);
                if (root.contains("editing") && root["editing"].is_string())
                    editing_name = root["editing"].get<std::string>();
                if (root.contains("ui_scale") && root["ui_scale"].is_number())
                    m_ui_scale = std::clamp(root["ui_scale"].get<float>(), 0.5f, 3.0f);
                if (root.contains("value_compact") && root["value_compact"].is_boolean())
                    m_value_compact = root["value_compact"].get<bool>();
                if (root.contains("shared_fields") && root["shared_fields"].is_array())
                {
                    for (const json &shared_key : root["shared_fields"])
                    {
                        if (!shared_key.is_string())
                            continue;
                        // Keep only keys that are real editable fields and not already listed (defensive against a
                        // hand-edited file).
                        const std::string key = shared_key.get<std::string>();
                        if (find_field(key) != nullptr && !is_field_shared(key))
                            m_shared_fields.push_back(key);
                    }
                }
                if (root.contains("presets") && root["presets"].is_array())
                {
                    for (const json &entry : root["presets"])
                    {
                        CameraPreset preset = preset_from_json(entry);
                        if (!preset.name.empty())
                            m_presets.push_back(std::move(preset));
                    }
                }
                file_usable = true;
            }
            catch (const json::exception &e)
            {
                // A corrupt user file is recoverable: use the embedded defaults this session and leave the
                // broken file on disk so the player can inspect or fix it (deleting it regenerates a clean one).
                logger.warning("Preset file '{}' is corrupt ({}); using built-in defaults this session. "
                               "Delete the file to regenerate a clean copy.",
                               file_path, e.what());
                m_presets.clear();
                m_shared_fields.clear();
            }
        }

        // Missing or corrupt file, or a valid file that lost all its presets: seed the canonical built-ins
        // from the embedded factory defaults.
        const bool seeded = m_presets.empty();
        if (seeded)
            m_presets = factory_presets();

        // Ensure every built-in exists and order them first; a valid file missing a newer built-in (e.g.
        // AIMING introduced by an update) has it re-added here without disturbing the user's own presets.
        const bool builtins_added = arrange_builtins();

        // Resolve the editing selection BEFORE any save, so a repair-save persists the user's actual choice
        // rather than the default-0 index (save() writes the "editing" key from m_editing_index).
        const int editing_idx = find_by_name(editing_name);
        m_editing_index = (editing_idx >= 0) ? editing_idx : 0;
        m_editing_pinned = false;

        // Create-if-missing, and persist a repaired set (a newly-added built-in, or a valid file recovered
        // from having lost its presets) so the next launch is clean. A corrupt file (file_usable == false)
        // is deliberately left untouched on disk.
        if (!file_present || (file_usable && (builtins_added || seeded)))
            save(); // clears m_dirty and snapshots the saved baseline
        else
            m_dirty = false;

        // Baseline for the content-aware Save indicator = the data as just loaded/repaired. The save() path above
        // already captured it; this covers the no-save path so a later edit-then-revert can clear the flag.
        capture_saved_baseline();

        logger.info("Presets ready ({} entries) at {}", m_presets.size(), file_path);

        publish();
    }

    void PresetStore::save()
    {
        DMK::Logger &logger = DMK::Logger::get_instance();
        if (m_file_path.empty())
        {
            logger.warning("Preset save skipped: no file path set");
            return;
        }

        json root;
        root["version"] = k_schema_version;
        root["ui_scale"] = m_ui_scale;
        root["value_compact"] = m_value_compact;
        root["shared_fields"] = m_shared_fields;
        root["editing"] = (m_editing_index >= 0 && m_editing_index < static_cast<int>(m_presets.size()))
                              ? m_presets[static_cast<std::size_t>(m_editing_index)].name
                              : std::string{k_builtin_default};
        json arr = json::array();
        for (const CameraPreset &p : m_presets)
            arr.push_back(preset_to_json(p));
        root["presets"] = std::move(arr);

        // Atomic write: serialize to a sibling temp file, flush and close it, then rename it over the
        // target. A crash mid-write can only ever leave the temp truncated, never the live file, so the
        // user's preset set survives (load() falls back to factory defaults only on a genuinely corrupt
        // file, and an in-place truncation would have looked exactly like one). std::filesystem::rename
        // replaces the destination on Windows (MoveFileEx semantics), so the swap is atomic on one volume.
        const std::string temp_path = m_file_path + ".tmp";
        try
        {
            std::ofstream out(temp_path, std::ios::trunc);
            if (!out)
            {
                logger.warning("Preset save failed: cannot open {}", temp_path);
                return;
            }
            // dump(2) can throw (e.g. nlohmann type_error.316 on invalid UTF-8 in a preset string); the catch
            // below treats it like a write failure so the live preset file is never disturbed.
            out << root.dump(2);
            out.flush();
            if (!out)
            {
                logger.warning("Preset save failed: write error on {}", temp_path);
                return;
            }
        } // close the stream so the rename sees a fully flushed file
        catch (const std::exception &e)
        {
            logger.warning("Preset save failed: serialization error ({})", e.what());
            std::error_code remove_ec;
            std::filesystem::remove(temp_path, remove_ec); // do not leave a partial temp behind
            return;
        }

        std::error_code rename_ec;
        std::filesystem::rename(temp_path, m_file_path, rename_ec);
        if (rename_ec)
        {
            logger.warning("Preset save failed: cannot replace {} ({})", m_file_path, rename_ec.message());
            std::error_code remove_ec;
            std::filesystem::remove(temp_path, remove_ec); // best-effort: do not leave the temp behind
            return;
        }

        m_dirty = false;
        m_prefs_dirty = false;
        capture_saved_baseline(); // the file now matches the live data: this is the new revert baseline
        logger.info("Presets saved to {}", m_file_path);
    }

    void PresetStore::flush()
    {
        if (m_dirty || m_prefs_dirty)
            save();
    }

    void PresetStore::set_editing_index(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_presets.size()))
            return;
        if (index == m_editing_index)
            return; // re-selecting the current preset is a no-op: nothing to persist or republish
        m_editing_index = index;
        // The editing selection is persisted ("editing" key), but it is UI bookkeeping, not a preset-value edit:
        // flag it prefs-dirty so it flushes on shutdown WITHOUT lighting the Save indicator (changing which
        // preset you are looking at is not an unsaved change to report).
        m_prefs_dirty = true;
        publish();
    }

    void PresetStore::set_editing_pinned(bool pinned)
    {
        m_editing_pinned = pinned;
        publish();
    }

    void PresetStore::set_ui_scale(float scale)
    {
        const float clamped = std::clamp(scale, 0.5f, 3.0f);
        if (clamped == m_ui_scale)
            return;
        m_ui_scale = clamped;
        m_prefs_dirty = true; // editor preference: persisted on shutdown / next save, not a Save-worthy change
    }

    void PresetStore::set_value_compact(bool compact)
    {
        if (compact == m_value_compact)
            return;
        m_value_compact = compact;
        m_prefs_dirty = true; // editor-only preference; persisted on shutdown / next save, not a Save-worthy change
    }

    bool PresetStore::is_field_shared(std::string_view key) const noexcept
    {
        for (const std::string &shared_key : m_shared_fields)
        {
            if (shared_key == key)
                return true;
        }
        return false;
    }

    void PresetStore::set_field_shared(std::string_view key, bool shared)
    {
        if (find_field(key) == nullptr) // ignore anything that is not an editable preset field
            return;
        if (is_field_shared(key) == shared)
            return;

        if (shared)
        {
            m_shared_fields.emplace_back(key);
            // Sync the value across every preset right away so enabling the link matches them immediately.
            broadcast_field(key); // marks the store dirty and republishes
        }
        else
        {
            const std::string key_str(key);
            m_shared_fields.erase(std::remove(m_shared_fields.begin(), m_shared_fields.end(), key_str),
                                  m_shared_fields.end());
            mark_dirty(); // persist the dropped link
        }
    }

    void PresetStore::copy_field_from_editing(const PresetField &field) noexcept
    {
        if (m_editing_index < 0 || m_editing_index >= static_cast<int>(m_presets.size()))
            return;

        // Copy ONLY this field's value from the editing preset into every preset, leaving the rest of each
        // preset untouched, so a shared value stays identical everywhere.
        const CameraPreset &source = m_presets[static_cast<std::size_t>(m_editing_index)];
        for (CameraPreset &preset : m_presets)
        {
            if (field.type == FieldType::Float)
                preset.*(field.f) = source.*(field.f);
            else
                preset.*(field.b) = source.*(field.b);
        }
    }

    void PresetStore::broadcast_field(std::string_view key)
    {
        const PresetField *field = find_field(key);
        if (field == nullptr)
            return;
        copy_field_from_editing(*field);
        mark_dirty();
    }

    int PresetStore::add_new()
    {
        CameraPreset preset;
        if (!m_presets.empty())
            preset = m_presets.front(); // start from DEFAULT as a usable base
        preset.name = make_unique_name("Preset");
        preset.builtin = false;
        preset.bind_state = "none";
        m_presets.push_back(std::move(preset));
        m_editing_index = static_cast<int>(m_presets.size()) - 1;
        save();
        publish();
        return m_editing_index;
    }

    int PresetStore::duplicate(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_presets.size()))
            return m_editing_index;
        CameraPreset preset = m_presets[static_cast<std::size_t>(index)];
        preset.name = make_unique_name(preset.name + " copy");
        preset.builtin = false;
        preset.bind_state = "none";
        m_presets.push_back(std::move(preset));
        m_editing_index = static_cast<int>(m_presets.size()) - 1;
        save();
        publish();
        return m_editing_index;
    }

    bool PresetStore::rename(int index, const std::string &name)
    {
        if (index < 0 || index >= static_cast<int>(m_presets.size()))
            return false;
        if (m_presets[static_cast<std::size_t>(index)].builtin)
            return false;
        if (name.empty() || name_in_use(name, index))
            return false;
        m_presets[static_cast<std::size_t>(index)].name = name;
        save();
        publish();
        return true;
    }

    void PresetStore::set_bind_state(int index, const std::string &bind_state)
    {
        if (index < 0 || index >= static_cast<int>(m_presets.size()))
            return;
        // Built-in bindings are fixed (arrange_builtins re-asserts them); only user presets are rebindable.
        if (m_presets[static_cast<std::size_t>(index)].builtin)
            return;
        m_presets[static_cast<std::size_t>(index)].bind_state = bind_state;
        // A binding change alters auto-apply routing: persist it (like rename) and republish so the live
        // resolver picks it up this frame.
        save();
        publish();
    }

    bool PresetStore::remove(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_presets.size()))
            return false;
        if (m_presets[static_cast<std::size_t>(index)].builtin)
            return false;
        m_presets.erase(m_presets.begin() + index);
        if (m_editing_index >= static_cast<int>(m_presets.size()))
            m_editing_index = static_cast<int>(m_presets.size()) - 1;
        if (m_editing_index < 0)
            m_editing_index = 0;
        save();
        publish();
        return true;
    }

    void PresetStore::reset_to_factory(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_presets.size()))
            return;
        CameraPreset &p = m_presets[static_cast<std::size_t>(index)];
        if (!p.builtin)
            return;
        // Restore the embedded factory values for this built-in, keeping its identity and state binding.
        const std::string name = p.name;
        const std::string bind = p.bind_state;
        p = factory_preset(name);
        p.name = name;
        p.builtin = true;
        p.bind_state = bind;

        // Restoring factory values would diverge any SHARED field on this built-in from the rest. Re-broadcast
        // every shared field from the editing preset so the "identical across all presets" invariant holds and
        // the divergence is never written to disk by the save() below.
        for (const std::string &key : m_shared_fields)
        {
            if (const PresetField *field = find_field(key))
                copy_field_from_editing(*field);
        }

        save();
        publish();
    }

    void PresetStore::mark_dirty()
    {
        // Content-aware: an edit that lands back on the saved value (a manual revert, or a per-field reset to a
        // value that was already saved) leaves nothing to write, so diff against the baseline instead of latching
        // the flag true.
        recompute_dirty();
        publish();
    }

    void PresetStore::recompute_dirty() noexcept
    {
        // The shared-field collection is an unordered SET: vector order is not meaningful (disabling then
        // re-enabling a key relocates it to the end), so compare it set-wise rather than with the order-sensitive
        // vector operator!=, or a no-op reorder would falsely light the Save indicator. Neither side holds
        // duplicates, so equal size plus one-way containment proves the sets match.
        bool shared_changed = m_shared_fields.size() != m_saved_shared_fields.size();
        for (const std::string &key : m_shared_fields)
        {
            if (shared_changed)
            {
                break;
            }
            shared_changed = std::find(m_saved_shared_fields.begin(), m_saved_shared_fields.end(), key) ==
                             m_saved_shared_fields.end();
        }
        m_dirty = (m_presets != m_saved_presets) || shared_changed;
    }

    void PresetStore::capture_saved_baseline()
    {
        m_saved_presets = m_presets;
        m_saved_shared_fields = m_shared_fields;
    }

    void PresetStore::publish()
    {
        auto table = std::make_shared<StateBindingTable>();
        table->presets.reserve(m_presets.size());
        table->masks.reserve(m_presets.size());

        // Publish every state-bound preset (built-ins plus any user presets the player bound to a state),
        // keeping the store order so built-ins precede user presets for the resolver's identical-mask
        // tiebreak. Unbound presets ("none") are skipped: they are reachable only via the editing pin.
        for (const CameraPreset &preset : m_presets)
        {
            const std::optional<std::uint32_t> mask = parse_bind_mask(preset.bind_state);
            if (!mask)
                continue;
            table->presets.push_back(preset);
            table->masks.push_back(*mask);
        }

        if (m_editing_pinned && m_editing_index >= 0 && m_editing_index < static_cast<int>(m_presets.size()))
        {
            table->has_pin = true;
            table->pinned = m_presets[static_cast<std::size_t>(m_editing_index)];
        }

        publish_table(std::move(table));
    }

} // namespace TPVCamera::Presets
