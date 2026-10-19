/**
 * @file preset_store.cpp
 * @brief PresetStore: JSON persistence, built-in protection, CRUD, and publishing.
 */

#include "preset_store.hpp"
#include "camera_preset_fields.hpp"
#include "config.hpp"
#include "core/blend/blend.hpp"
#include "default_presets.hpp"
#include "global_state.hpp"
#include "preset_runtime.hpp"

#include <DetourModKit.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace TPVCamera::Presets
{
    namespace
    {

        using json = nlohmann::json;

        // The presets file's schema. Version 1 files may carry orbit_sensitivity and gamepad_orbit_speed as single
        // keys from before each was split into X and Y axes (a file without "version" is read as version 1); version
        // 2 files carry the axes only. Files before version 3 have no per-preset blend_in_seconds: every preset blended
        // at the INI's single PresetBlendSpeed, at k_legacy_blend_rate_scale times it.
        constexpr int k_schema_version = 3;
        constexpr int k_schema_split_axes = 2;
        constexpr int k_schema_blend_seconds = 3;
        constexpr float k_legacy_blend_rate_scale = 1.6f;

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

        /**
         * @brief Returns the editable PresetField with this key, or nullptr when none matches.
         * @details Guards the shared-field machinery and the JSON loader against a key that names no real
         *          field, such as a stale entry left in a hand-edited file.
         */
        [[nodiscard]] const PresetField *find_field(std::string_view key) noexcept
        {
            for (const PresetField &field : fields())
            {
                if (key == field.key)
                    return &field;
            }
            return nullptr;
        }

        /**
         * @brief Sorts @p picks by mask.
         * @details A group has at most one pick, and the order carries no meaning, so sorted lists compare equal
         *          exactly when they hold the same picks.
         */
        void sort_by_mask(std::vector<CyclePick> &picks)
        {
            std::ranges::sort(picks, {}, &CyclePick::mask);
        }

        /// Copies the value of @p field from @p source into @p target.
        void copy_field_value(const PresetField &field, const CameraPreset &source, CameraPreset &target) noexcept
        {
            if (field.type == FieldType::Float)
                target.*(field.f) = source.*(field.f);
            else
                target.*(field.b) = source.*(field.b);
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

        /** @brief The schema version of a presets document root: its "version", or 1 when it has none. */
        [[nodiscard]] int schema_version_of(const json &root)
        {
            return (root.contains("version") && root["version"].is_number_integer()) ? root["version"].get<int>() : 1;
        }

        /**
         * @brief Reads one preset entry of a presets document of schema @p version.
         * @details Starts at the CameraPreset member defaults and keeps them for any missing key. A key that names
         *          no field, or a field value of the wrong JSON type, is left out and, when @p issues is given,
         *          described there for the caller's one warning per load.
         */
        CameraPreset preset_from_json(const json &in, int version, std::vector<std::string> *issues)
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
                // The single keys a version 1 file may carry from before the per-axis split.
                constexpr std::string_view k_legacy_axis_keys[] = {"orbit_sensitivity", "gamepad_orbit_speed"};
                const bool legacy = version < k_schema_split_axes;
                for (const auto &entry : values.items())
                {
                    const PresetField *field = find_field(entry.key());
                    if (field == nullptr)
                    {
                        const bool legacy_key = legacy && std::ranges::find(k_legacy_axis_keys, entry.key()) !=
                                                              std::end(k_legacy_axis_keys);
                        if (!legacy_key && issues != nullptr)
                            issues->push_back(std::format("{}: unknown key '{}'", preset.name, entry.key()));
                        continue;
                    }
                    const json &v = entry.value();
                    if (field->type == FieldType::Float && v.is_number())
                        preset.*(field->f) = v.get<float>();
                    else if (field->type == FieldType::Bool && v.is_boolean())
                        preset.*(field->b) = v.get<bool>();
                    else if (issues != nullptr)
                        issues->push_back(std::format("{}: '{}' is not a {}", preset.name, entry.key(),
                                                      field->type == FieldType::Float ? "number" : "boolean"));
                }

                // Version 1: orbit_sensitivity / gamepad_orbit_speed were each split into per-axis X/Y fields. A
                // preset written before the split carries only the old single key; seed BOTH new axes from it (when
                // neither new key is present) so an existing tune is not silently lost.
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
                if (legacy)
                {
                    seed_axes_from_legacy("orbit_sensitivity", "orbit_sensitivity_x", "orbit_sensitivity_y",
                                          &CameraPreset::orbit_sensitivity_x, &CameraPreset::orbit_sensitivity_y);
                    seed_axes_from_legacy("gamepad_orbit_speed", "gamepad_orbit_speed_x", "gamepad_orbit_speed_y",
                                          &CameraPreset::gamepad_orbit_speed_x, &CameraPreset::gamepad_orbit_speed_y);
                }
            }
            return preset;
        }

        /**
         * @brief Reads the "cycle_picks" object of a presets document root.
         * @details Keys are group token lists (see cycle_group_tokens), values preset names. Keys that name no
         *          group and non-string values are skipped. Liveness is not checked here: publish() drops a pick
         *          whose preset is missing or bound elsewhere.
         */
        [[nodiscard]] std::vector<CyclePick> cycle_picks_from_json(const json &root)
        {
            std::vector<CyclePick> picks;
            if (!root.contains("cycle_picks") || !root["cycle_picks"].is_object())
                return picks;
            for (const auto &entry : root["cycle_picks"].items())
            {
                const std::optional<std::uint32_t> mask = parse_bind_mask(entry.key());
                if (!mask || !entry.value().is_string())
                    continue;
                // Two keys can name one group, such as "crouch" and "stealth". The first one read wins.
                const bool known = std::any_of(picks.begin(), picks.end(),
                                               [&mask](const CyclePick &pick) { return pick.mask == *mask; });
                if (!known)
                {
                    picks.push_back(CyclePick{
                        .mask = *mask,
                        .preset = entry.value().get<std::string>(),
                    });
                }
            }
            return picks;
        }

        /**
         * @struct FactoryData
         * @brief The embedded factory document: the built-ins and their starting points, and the picks that keep
         *        each built-in in use in its group.
         */
        struct FactoryData
        {
            /// Built-ins in canonical order, then the factory starting points.
            std::vector<CameraPreset> presets;
            /// The member each factory cycle group uses on a fresh install.
            std::vector<CyclePick> cycle_picks;
        };

        /// Parses the embedded factory JSON once.
        const FactoryData &factory_data()
        {
            static const FactoryData data = []
            {
                FactoryData out;
                try
                {
                    const json root = json::parse(k_default_presets_json);
                    const int version = schema_version_of(root);
                    if (root.contains("presets") && root["presets"].is_array())
                    {
                        for (const json &entry : root["presets"])
                            out.presets.push_back(preset_from_json(entry, version, nullptr));
                    }
                    out.cycle_picks = cycle_picks_from_json(root);
                }
                catch (const json::exception &)
                {
                    // The literal is authored in-repo; a throw here is a build-time authoring error. Leaving the
                    // data empty makes the built-ins fall back to struct defaults rather than crashing.
                }
                return out;
            }();
            return data;
        }

    } // namespace

    std::optional<CameraPreset> factory_preset(const std::string &name)
    {
        for (const CameraPreset &p : factory_data().presets)
        {
            if (p.name == name)
                return p;
        }
        return std::nullopt;
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
        DMK::Logger &logger = DMK::log();

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
                // absent (the struct default then stands in). The reorder loop below relies on find_by_name()
                // resolving it.
                CameraPreset preset = factory_preset(spec.name).value_or(CameraPreset{});
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
        DMK::Logger &logger = DMK::log();
        m_file_path = file_path;
        m_presets.clear();
        m_ui_scale = 1.0f;
        m_value_compact = true;
        m_shared_fields.clear();
        m_saved = FileContents{};
        m_auto_save_blocked = false;

        std::string editing_name = k_builtin_default;
        std::vector<CyclePick> loaded_picks;
        bool shoulder_swapped = false;

        // Presets are user-owned customization, not a shipped asset (see default_presets.hpp). The file is
        // OPTIONAL: a missing one is created from the embedded factory defaults on first run, and a corrupt
        // one falls back to those defaults for the session without being overwritten. The mod never fails
        // to start over presets.
        bool file_present = false;
        bool file_usable = false;
        bool migrated_blend = false; // a file of an older schema took the blend time of the INI's speed

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
                if (root.contains("shoulder_swapped") && root["shoulder_swapped"].is_boolean())
                    shoulder_swapped = root["shoulder_swapped"].get<bool>();
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
                loaded_picks = cycle_picks_from_json(root);
                const int version = schema_version_of(root);
                migrated_blend = version < k_schema_blend_seconds;
                std::vector<std::string> issues;
                if (root.contains("presets") && root["presets"].is_array())
                {
                    for (const json &entry : root["presets"])
                    {
                        CameraPreset preset = preset_from_json(entry, version, &issues);
                        if (!preset.name.empty())
                            m_presets.push_back(std::move(preset));
                    }
                }
                if (!issues.empty())
                {
                    // One line per load, so a hand-edited typo is seen without repeating on every frame or save.
                    std::string listed;
                    for (const std::string &issue : issues)
                        listed += (listed.empty() ? "" : "; ") + issue;
                    logger.warning("Preset file '{}': {} value(s) ignored: {}", file_path, issues.size(), listed);
                }
                if (migrated_blend)
                {
                    // The blend time that gives every preset the feel the single PresetBlendSpeed gave it (0 cuts).
                    const float speed = settings().legacy_preset_blend_speed.load(std::memory_order_relaxed);
                    const float seconds = speed > 0.0f ? k_spring_t99 / (k_legacy_blend_rate_scale * speed) : 0.0f;
                    for (CameraPreset &preset : m_presets)
                    {
                        preset.blend_in_seconds = seconds;
                    }
                    logger.info("Presets: the file predates per-preset blend times; every preset blends in over {:.2f} "
                                "s, the feel of PresetBlendSpeed {:.2f}, and the file is saved as schema {}",
                                seconds, speed, k_schema_version);
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
                loaded_picks.clear();
                shoulder_swapped = false;
            }
        }

        // The file is read; close it before a repair save below renames the new copy over it, which Windows refuses
        // while a stream still holds the file open.
        in.close();

        // Missing or corrupt file, or a valid file that lost all its presets: seed the canonical built-ins, their
        // starting points and the picks that keep the built-ins in use from the embedded factory defaults.
        const bool seeded = m_presets.empty();
        if (seeded)
        {
            m_presets = factory_data().presets;
            loaded_picks = factory_data().cycle_picks;
        }

        // Ensure every built-in exists and order them first; a valid file missing a newer built-in (e.g.
        // AIMING introduced by an update) has it re-added here without disturbing the user's own presets.
        const bool builtins_added = arrange_builtins();

        // Resolve the editing selection BEFORE any save, so a repair-save persists the user's actual choice
        // rather than the default-0 index (a write takes the "editing" key from m_editing_index).
        const int editing_idx = find_by_name(editing_name);
        m_editing_index = (editing_idx >= 0) ? editing_idx : 0;
        m_editing_pinned = false;

        // Seed the cycle picks from the file, replacing any a previous load published. publish() drops the ones
        // whose preset is gone or bound elsewhere. The input engine starts after this load, so no hotkey changes the
        // picks or the shoulder side before the saved state below is taken.
        publish([&loaded_picks](std::vector<CyclePick> &picks) { picks = loaded_picks; });
        camera_controls().shoulder_swapped.store(shoulder_swapped, std::memory_order_relaxed);

        // Create-if-missing, and persist a repaired set (a newly-added built-in, a valid file recovered from having
        // lost its presets, or one of an older schema given its blend times) so the next launch is clean. A corrupt
        // file (file_usable == false) is deliberately left untouched on disk. Without a successful write, the values
        // as loaded become the saved state, so a later edit reverted to them clears the Save indicator.
        const bool repair = !file_present || (file_usable && (builtins_added || seeded || migrated_blend));
        if (!repair || !write(live_contents(true)))
            m_saved = live_contents(true);

        logger.info("Presets ready ({} entries) at {}", m_presets.size(), file_path);
    }

    void PresetStore::save()
    {
        write(live_contents(true));
    }

    void PresetStore::flush()
    {
        FileContents contents = live_contents(false);
        if (contents != m_saved)
            write(std::move(contents));
    }

    void PresetStore::auto_save()
    {
        if (m_auto_save_blocked)
            return;
        const LiveSettings &live = settings();
        const bool save_picks = live.auto_save_cycle_pick.load(std::memory_order_relaxed);
        const bool save_shoulder = live.auto_save_shoulder.load(std::memory_order_relaxed);
        if (!save_picks && !save_shoulder)
            return;

        std::vector<CyclePick> picks = save_picks ? live_cycle_picks() : std::vector<CyclePick>{};
        const bool swapped = camera_controls().shoulder_swapped.load(std::memory_order_relaxed);
        const bool picks_changed = save_picks && picks != m_saved.cycle_picks;
        const bool shoulder_changed = save_shoulder && swapped != m_saved.shoulder_swapped;
        if (!picks_changed && !shoulder_changed)
            return;

        // The rest stays as saved, so a pending preset edit is not written with the pick.
        FileContents contents = m_saved;
        if (picks_changed)
            contents.cycle_picks = std::move(picks);
        if (shoulder_changed)
            contents.shoulder_swapped = swapped;
        if (!write(std::move(contents)))
            m_auto_save_blocked = true;
    }

    bool PresetStore::dirty() const
    {
        const LiveSettings &live = settings();
        // An auto-saved value is pending only while auto-save is blocked: otherwise the next auto_save() writes it.
        const bool picks_auto = !m_auto_save_blocked && live.auto_save_cycle_pick.load(std::memory_order_relaxed);
        const bool shoulder_auto = !m_auto_save_blocked && live.auto_save_shoulder.load(std::memory_order_relaxed);
        const FileContents contents = live_contents(true);
        return contents.presets != m_saved.presets || contents.shared_fields != m_saved.shared_fields ||
               contents.ui_scale != m_saved.ui_scale || contents.value_compact != m_saved.value_compact ||
               (!picks_auto && contents.cycle_picks != m_saved.cycle_picks) ||
               (!shoulder_auto && contents.shoulder_swapped != m_saved.shoulder_swapped);
    }

    void PresetStore::commit(const PickEdit &edit_picks)
    {
        write(live_contents(false, edit_picks));
    }

    PresetStore::FileContents PresetStore::live_contents(bool keep_session, const PickEdit &edit_saved_picks) const
    {
        const LiveSettings &live = settings();
        FileContents contents{
            .presets = m_presets,
            .shared_fields = m_shared_fields,
            .cycle_picks = (keep_session || live.auto_save_cycle_pick.load(std::memory_order_relaxed))
                               ? live_cycle_picks()
                               : saved_cycle_picks(edit_saved_picks),
            .editing = (m_editing_index >= 0 && m_editing_index < static_cast<int>(m_presets.size()))
                           ? m_presets[static_cast<std::size_t>(m_editing_index)].name
                           : std::string{k_builtin_default},
            .ui_scale = m_ui_scale,
            .value_compact = m_value_compact,
            .shoulder_swapped = (keep_session || live.auto_save_shoulder.load(std::memory_order_relaxed))
                                    ? camera_controls().shoulder_swapped.load(std::memory_order_relaxed)
                                    : m_saved.shoulder_swapped,
        };
        std::ranges::sort(contents.shared_fields);
        return contents;
    }

    bool PresetStore::write(FileContents contents)
    {
        DMK::Logger &logger = DMK::log();
        if (m_file_path.empty())
        {
            logger.warning("Preset save skipped: no file path set");
            return false;
        }

        json root;
        root["version"] = k_schema_version;
        root["ui_scale"] = contents.ui_scale;
        root["value_compact"] = contents.value_compact;
        root["shoulder_swapped"] = contents.shoulder_swapped;
        root["shared_fields"] = contents.shared_fields;
        // Keyed by the group's token list so a hand-edited file reads like the presets' own bind_state values.
        json picks = json::object();
        for (const CyclePick &pick : contents.cycle_picks)
            picks[cycle_group_tokens(pick.mask)] = pick.preset;
        root["cycle_picks"] = std::move(picks);
        root["editing"] = contents.editing;
        json arr = json::array();
        for (const CameraPreset &p : contents.presets)
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
                return false;
            }
            // dump(2) can throw (e.g. nlohmann type_error.316 on invalid UTF-8 in a preset string); the catch
            // below treats it like a write failure so the live preset file is never disturbed.
            out << root.dump(2);
            out.flush();
            if (!out)
            {
                logger.warning("Preset save failed: write error on {}", temp_path);
                return false;
            }
        } // close the stream so the rename sees a fully flushed file
        catch (const std::exception &e)
        {
            logger.warning("Preset save failed: serialization error ({})", e.what());
            std::error_code remove_ec;
            std::filesystem::remove(temp_path, remove_ec); // do not leave a partial temp behind
            return false;
        }

        std::error_code rename_ec;
        std::filesystem::rename(temp_path, m_file_path, rename_ec);
        if (rename_ec)
        {
            logger.warning("Preset save failed: cannot replace {} ({})", m_file_path, rename_ec.message());
            std::error_code remove_ec;
            std::filesystem::remove(temp_path, remove_ec); // best-effort: do not leave the temp behind
            return false;
        }

        // The saved state is what was written, not a fresh read, so a hotkey that cycles during the write still
        // differs from it and is written later.
        m_saved = std::move(contents);
        m_auto_save_blocked = false;
        logger.info("Presets saved to {}", m_file_path);
        return true;
    }

    void PresetStore::set_editing_index(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_presets.size()))
            return;
        if (index == m_editing_index)
            return; // re-selecting the current preset is a no-op: nothing to persist or republish
        // The file keeps the selection ("editing" key), but a switch to another preset is no change to report, so
        // dirty() leaves it out and flush() writes it on shutdown.
        m_editing_index = index;
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
    }

    void PresetStore::set_value_compact(bool compact)
    {
        m_value_compact = compact;
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
            broadcast_field(key); // republishes
        }
        else
        {
            const std::string key_str(key);
            m_shared_fields.erase(std::remove(m_shared_fields.begin(), m_shared_fields.end(), key_str),
                                  m_shared_fields.end());
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
            copy_field_value(field, source, preset);
    }

    void PresetStore::copy_shared_fields(const CameraPreset &source, CameraPreset &target) const noexcept
    {
        for (const std::string &key : m_shared_fields)
        {
            if (const PresetField *field = find_field(key))
                copy_field_value(*field, source, target);
        }
    }

    void PresetStore::broadcast_field(std::string_view key)
    {
        const PresetField *field = find_field(key);
        if (field == nullptr)
            return;
        copy_field_from_editing(*field);
        publish();
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
        publish();
        commit();
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
        publish();
        commit();
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
        const std::string previous = std::exchange(m_presets[static_cast<std::size_t>(index)].name, name);
        // A pick names its preset, so it follows the rename and the group keeps the member it uses.
        const PickEdit rekey = [&previous, &name](std::vector<CyclePick> &picks)
        {
            for (CyclePick &pick : picks)
            {
                if (pick.preset == previous)
                    pick.preset = name;
            }
        };
        publish(rekey);
        commit(rekey);
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
        // A binding change alters auto-apply routing: republish so the live resolver picks it up this frame (and
        // the preset's pick in its old group is dropped), then persist it like a rename.
        publish();
        commit();
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
        publish();
        commit();
        return true;
    }

    void PresetStore::reset_to_factory(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_presets.size()))
            return;
        CameraPreset &preset = m_presets[static_cast<std::size_t>(index)];
        std::optional<CameraPreset> factory = factory_preset(preset.name);
        if (!factory)
            return;

        // Restore the embedded factory values but keep identity and state binding. A shared field keeps the value all
        // presets share: the reset concerns one preset, and a factory value in a shared field must reach every other
        // preset to keep the field shared.
        const CameraPreset before = preset;
        preset = std::move(*factory);
        preset.name = before.name;
        preset.builtin = before.builtin;
        preset.bind_state = before.bind_state;
        copy_shared_fields(before, preset);

        // A built-in's reset also adds back each factory starting point of its states that the list lacks, deleted
        // or never there because the file predates it. A group without a pick uses its last member, and a starting
        // point joins at the end, so such a group is pointed at the member it used before the reset. Nothing reads
        // `preset` past this point: the push_back can reallocate the list.
        const std::optional<std::uint32_t> mask = parse_bind_mask(before.bind_state);
        std::string in_use;
        bool restored = false;
        if (before.builtin && mask)
        {
            for (const CameraPreset &member : m_presets)
            {
                if (parse_bind_mask(member.bind_state) == mask)
                    in_use = member.name;
            }
            for (const CameraPreset &start : factory_data().presets)
            {
                if (start.builtin || find_by_name(start.name) >= 0 || parse_bind_mask(start.bind_state) != mask)
                    continue;
                CameraPreset added = start;
                copy_shared_fields(before, added);
                m_presets.push_back(std::move(added));
                restored = true;
            }
        }

        const PickEdit keep_in_use = [restored, &mask, &in_use](std::vector<CyclePick> &picks)
        {
            // `restored` implies the built-in's mask was parsed.
            if (!restored)
                return;
            const std::uint32_t group = *mask;
            const bool picked =
                std::any_of(picks.begin(), picks.end(), [group](const CyclePick &pick) { return pick.mask == group; });
            if (!picked)
            {
                picks.push_back(CyclePick{
                    .mask = group,
                    .preset = in_use,
                });
            }
        };
        publish(keep_in_use);
        commit(keep_in_use);
    }

    void PresetStore::reset_all_to_factory()
    {
        // A shared field holds one value in every preset, so any preset carries it. The reset keeps it, as the
        // single-preset reset does.
        const CameraPreset shared_source = m_presets.empty() ? CameraPreset{} : m_presets.front();
        int restored = 0;
        int added = 0;
        for (const CameraPreset &factory : factory_data().presets)
        {
            CameraPreset preset = factory;
            copy_shared_fields(shared_source, preset);
            const int index = find_by_name(factory.name);
            if (index >= 0)
            {
                m_presets[static_cast<std::size_t>(index)] = std::move(preset);
                ++restored;
            }
            else
            {
                m_presets.push_back(std::move(preset));
                ++added;
            }
        }

        // The factory picks replace the picks of the groups they name, so those groups start on their built-in
        // again. A group the player made keeps its pick.
        const PickEdit factory_picks = [](std::vector<CyclePick> &picks)
        {
            for (const CyclePick &factory_pick : factory_data().cycle_picks)
            {
                std::erase_if(picks, [&factory_pick](const CyclePick &pick) { return pick.mask == factory_pick.mask; });
                picks.push_back(factory_pick);
            }
        };
        publish(factory_picks);
        commit(factory_picks);
        DMK::log().info("Presets: reset {} factory presets to factory values and added back {}", restored, added);
    }

    void PresetStore::apply_edit()
    {
        publish();
    }

    bool PresetStore::is_live_pick(const CyclePick &pick) const
    {
        const int index = find_by_name(pick.preset);
        if (index < 0)
            return false;
        const std::optional<std::uint32_t> mask =
            parse_bind_mask(m_presets[static_cast<std::size_t>(index)].bind_state);
        return mask.has_value() && *mask == pick.mask;
    }

    std::vector<CyclePick> PresetStore::live_cycle_picks() const
    {
        // The overlay sets a pick from the snapshot it read at the start of its frame, so a rename or rebind earlier
        // in that frame can leave the pick naming a preset outside its group until the next publish drops it.
        std::vector<CyclePick> live;
        if (const std::shared_ptr<const StateBindingTable> table = published_table())
        {
            for (const CyclePick &pick : table->cycle_picks)
            {
                if (is_live_pick(pick))
                    live.push_back(pick);
            }
        }
        sort_by_mask(live);
        return live;
    }

    std::vector<CyclePick> PresetStore::saved_cycle_picks(const PickEdit &edit_picks) const
    {
        std::vector<CyclePick> picks = m_saved.cycle_picks;
        if (edit_picks)
            edit_picks(picks);
        std::erase_if(picks, [this](const CyclePick &pick) { return !is_live_pick(pick); });
        sort_by_mask(picks);
        return picks;
    }

    void PresetStore::publish(const PickEdit &edit_picks)
    {
        StateBindingTable table;
        table.presets.reserve(m_presets.size());
        table.masks.reserve(m_presets.size());

        // Publish every state-bound preset (built-ins plus any user presets the player bound to a state),
        // keeping the store order: it is the cycle order of each group of identical masks, and a group without a
        // pick uses its last member, so a user preset still overrides a built-in bound to the same states.
        // Unbound presets ("none") are skipped: they are reachable only via the editing pin.
        for (const CameraPreset &preset : m_presets)
        {
            const std::optional<std::uint32_t> mask = parse_bind_mask(preset.bind_state);
            if (!mask)
                continue;
            table.presets.push_back(preset);
            table.masks.push_back(*mask);
        }

        if (m_editing_pinned && m_editing_index >= 0 && m_editing_index < static_cast<int>(m_presets.size()))
        {
            table.has_pin = true;
            table.pinned = m_presets[static_cast<std::size_t>(m_editing_index)];
        }

        // Every publish drops a pick that no longer names a member of its group (the preset was removed, renamed away
        // or rebound), and that group returns to its default member. The edit runs first, so a rename can re-key a
        // pick before the liveness check sees the new name.
        publish_table(std::move(table),
                      [this, &edit_picks](std::vector<CyclePick> &picks)
                      {
                          if (edit_picks)
                              edit_picks(picks);
                          std::erase_if(picks, [this](const CyclePick &pick) { return !is_live_pick(pick); });
                      });
    }

} // namespace TPVCamera::Presets
