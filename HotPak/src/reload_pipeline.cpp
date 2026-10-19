/**
 * @file reload_pipeline.cpp
 * @brief Mounting staged generations and dispatching per-file reloads on the main thread.
 */

#include "reload_pipeline.hpp"
#include "config.hpp"
#include "constants.hpp"
#include "engine.hpp"
#include "pak_names.hpp"
#include "pak_watch.hpp"
#include "proxy_reload.hpp"
#include "text_util.hpp"
#include "zip_index.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace HotPak
{
    namespace
    {
        /** @brief What the game reads for one watched pak, owned by the main thread. */
        struct SourceRecord
        {
            std::string file_name;
            std::vector<ZipEntry> baseline;
            std::string bind_root = constants::DEFAULT_BIND_ROOT;
            bool baseline_known = false;
        };

        /** @brief A delayed report that shows whether the replaced skins and materials were loaded again. */
        struct FollowUp
        {
            std::uint64_t due = 0;
            std::vector<std::string> skins;
            std::vector<std::string> materials;
        };

        std::vector<SourceRecord> s_sources;
        std::vector<FollowUp> s_follow_ups;
        // What the open reset window looks for. A generation applied while the window is open adds to it, so a quick
        // rebuild does not drop proxies the previous one still had to reset.
        std::vector<std::string> s_shipped_files;
        std::vector<std::string> s_restream_files;
        std::vector<std::uintptr_t> s_replaced_skins;
        // Materials taken out of CMatMan, to tell an old object from a reloaded one in the follow-up report.
        std::vector<std::uintptr_t> s_retired_materials;
        std::uint64_t s_next_stats = 0;

        constexpr std::uint64_t FOLLOW_UP_DELAY_MS = 5000;
        constexpr std::uint64_t STATS_INTERVAL_MS = 1000;

        /// One table database, by its Libs/Tables subfolder and name.
        struct TableId
        {
            std::string_view subfolder;
            std::string_view name;
        };

        /**
         * @brief Table databases that are never reloaded live.
         * @details A reload clears a database's rows and builds new ones. Dependent databases are linked again (see
         *          reload_table_database), but live game objects that point at a row are not.
         *          Character/CharacterComponent: every dressed character's clothing proxies point at its C_SkinElement
         *          rows, and only characters wearing the pak's own skins are restreamed. The others keep pointers to
         *          freed rows, and the next lookup through them (an uberlod build when the game dresses a character
         *          on a save load, the proxy pass) reads freed memory and crashes the game. A change to one of these
         *          tables waits for a game restart.
         */
        constexpr std::array<TableId, 1> RESTART_ONLY_TABLES{{
            {
                .subfolder = "Character",
                .name = "CharacterComponent",
            },
        }};

        [[nodiscard]] bool restart_only(const DatabaseInfo &database) noexcept
        {
            return std::any_of(
                RESTART_ONLY_TABLES.begin(),
                RESTART_ONLY_TABLES.end(),
                [&database](const TableId &id)
                { return iequals(id.subfolder, database.subfolder) && iequals(id.name, database.name); }
            );
        }

        void add_unique(std::vector<std::uintptr_t> &list, std::uintptr_t value)
        {
            if (value != 0 && std::find(list.begin(), list.end(), value) == list.end())
            {
                list.push_back(value);
            }
        }

        void push_unique(std::vector<std::string> &list, std::string value)
        {
            if (!value.empty() && std::find(list.begin(), list.end(), value) == list.end())
            {
                list.push_back(std::move(value));
            }
        }

        /** @brief "<root>" for "<root>\Bin\<platform>\WHGame.dll"; CryPak's relative pak paths resolve against it. */
        [[nodiscard]] std::wstring game_root()
        {
            wchar_t path[MAX_PATH]{};
            const HMODULE game = GetModuleHandleW(to_wide(constants::GAME_MODULE).c_str());
            if (game == nullptr || GetModuleFileNameW(game, path, MAX_PATH) == 0)
            {
                return {};
            }
            std::wstring root(path);
            for (int level = 0; level < 3; ++level)
            {
                const std::size_t slash = root.find_last_of(L"\\/");
                if (slash == std::wstring::npos)
                {
                    return {};
                }
                root.resize(slash);
            }
            return root;
        }

        [[nodiscard]] bool is_absolute(std::string_view path) noexcept
        {
            return path.size() > 2 && (path[1] == ':' || (path[0] == '\\' && path[1] == '\\'));
        }

        /**
         * @brief Finds what the game reads for the pak now: the newest overlay this tool mounted, else the installed
         *        copy.
         * @details Overlays are found in CryPak's own pak list, so a reloaded logic generation adopts the mounts of
         *          the one before it instead of diffing against the installed copy again.
         */
        void discover_baseline(SourceRecord &record)
        {
            record.baseline_known = true;
            const std::vector<MountedPak> paks = mounted_paks();
            const std::string shadow_dir = normalize_path(to_utf8(shadow_directory())) + "/";
            // Every watched file name ends in ".pak" (parse_pak_list).
            const std::string_view stem =
                std::string_view(record.file_name).substr(0, record.file_name.size() - std::string_view(".pak").size());

            for (auto it = paks.rbegin(); it != paks.rend(); ++it)
            {
                const std::string path = normalize_path(it->path);
                const std::string_view name = file_name_of(path);
                const bool in_shadow_dir = path.starts_with(shadow_dir);
                const bool overlay = in_shadow_dir && shadow_generation_of(to_wide(name)) != 0 &&
                                     name.substr(0, name.rfind(".g")) == stem;
                const bool installed = !in_shadow_dir && name == record.file_name;
                if (!overlay && !installed)
                {
                    continue;
                }
                const std::wstring file =
                    is_absolute(it->path) ? to_wide(it->path) : game_root() + L"\\" + to_wide(it->path);
                auto index = read_zip_index(file);
                if (!index.has_value())
                {
                    DMK::log().warning(
                        "Pipeline: baseline {} is unreadable ({}); every entry counts as changed",
                        it->path,
                        index.error().message()
                    );
                    return;
                }
                record.baseline = std::move(*index);
                record.bind_root = it->bind_root.empty() ? constants::DEFAULT_BIND_ROOT : it->bind_root;
                DMK::log().info(
                    "Pipeline: baseline for {} is the {} copy {} ({} entries, root '{}')",
                    record.file_name,
                    overlay ? "overlay" : "installed",
                    it->path,
                    record.baseline.size(),
                    record.bind_root
                );
                return;
            }
            DMK::log().info("Pipeline: {} is not mounted yet; every entry counts as new", record.file_name);
        }

        [[nodiscard]] SourceRecord &record_for(const std::string &file_name)
        {
            for (SourceRecord &record : s_sources)
            {
                if (record.file_name == file_name)
                {
                    return record;
                }
            }
            s_sources.push_back(
                SourceRecord{
                    .file_name = file_name,
                }
            );
            return s_sources.back();
        }

        /**
         * @brief Lua that calls `<id>:OnHotReload()` when the global table `<id>` defines it.
         * @details rawget reads the global without its metatable, so a mod that has no such table reads nil even under
         *          a strict-globals guard. The call is logged to the game log only when it happens.
         */
        [[nodiscard]] std::string hot_reload_hook_source(std::string_view id)
        {
            const std::string name(id);
            const std::string method = constants::SCRIPT_HOT_RELOAD_METHOD;
            std::string source = "local m = rawget(_G, \"" + name + "\")\n";
            source += "if type(m) == \"table\" and type(m." + method + ") == \"function\" then\n";
            source += "    System.LogAlways(\"[HotPak] " + name + ":" + method + "()\")\n";
            source += "    m:" + method + "()\n";
            source += "end\n";
            return source;
        }

        /**
         * @brief Runs every mod init script of the generation again when any of its scripts changed.
         * @details The game runs "scripts/mods/<modid>.lua" once at startup and never again, and Script.ReloadScript
         *          does not force a rerun outside the Editor, so a changed script stays dead until a restart. Each init
         *          script the pak ships is run again with a forced ExecuteFile, read through CryPak from the overlay
         *          just mounted. The forced call reruns every script the init script loads with Script.ReloadScript or
         *          Script.LoadScript while it runs, so a changed helper script is picked up through its init script.
         *          Rerunning stacks whatever the script patched on its first run (a wrapped Player.OnAction wraps the
         *          previous wrapper). Before each rerun the mod's own `<modid>:OnHotReload()` is called when it exists,
         *          so the old code can undo its patches, listeners and timers.
         * @param changed The changed .lua entries.
         * @param index Every entry the generation ships.
         */
        void rerun_scripts(const std::vector<std::string> &changed, const std::vector<ZipEntry> &index)
        {
            DMK::Logger &logger = DMK::log();
            if (changed.empty())
            {
                return;
            }
            std::vector<std::string> init_scripts;
            for (const ZipEntry &entry : index)
            {
                if (!mod_script_id(entry.name).empty())
                {
                    push_unique(init_scripts, entry.name);
                }
            }
            std::sort(init_scripts.begin(), init_scripts.end());

            for (const std::string &script : changed)
            {
                if (std::find(init_scripts.begin(), init_scripts.end(), script) != init_scripts.end())
                {
                    continue;
                }
                if (init_scripts.empty())
                {
                    logger.warning(
                        "Script: {} changed, but the pak ships no {}<id>.lua to run again; the game reads it again "
                        "only when it next loads it",
                        script,
                        constants::MOD_SCRIPTS_PREFIX
                    );
                }
                else
                {
                    logger.info(
                        "Script: {} changed; it runs again if an init script loads it with Script.ReloadScript or "
                        "Script.LoadScript",
                        script
                    );
                }
            }

            for (const std::string &script : init_scripts)
            {
                const std::string_view id = mod_script_id(script);
                if (is_plain_script_id(id))
                {
                    const std::string chunk =
                        "=[HotPak] " + std::string(id) + ":" + constants::SCRIPT_HOT_RELOAD_METHOD;
                    if (!run_script_buffer(hot_reload_hook_source(id), chunk))
                    {
                        logger.error(
                            "Script: {}:{}() raised a Lua error (the game log has it); rerunning {} anyway",
                            id,
                            constants::SCRIPT_HOT_RELOAD_METHOD,
                            script
                        );
                    }
                }
                else
                {
                    logger.warning(
                        "Script: {} has an id that cannot name a Lua global; {}() is not called",
                        script,
                        constants::SCRIPT_HOT_RELOAD_METHOD
                    );
                }
                if (rerun_script_file(script))
                {
                    logger.info("Script: {} ran again (forced reload, read from the overlay)", script);
                }
                else
                {
                    logger.error("Script: {} FAILED to run again; the game log has the Lua error", script);
                }
            }
        }

        /** @brief The file name a C_SkinElement row stores for a pak path ("a/b/x.skin" gives "x.skin"). */
        [[nodiscard]] std::string row_name(std::string_view path)
        {
            return std::string(file_name_of(path));
        }

        /** @brief Every skin, and unless @p skins_only every material, the generation ships, as row names. */
        [[nodiscard]] std::vector<std::string> row_names_in(const std::vector<ZipEntry> &index, bool skins_only)
        {
            std::vector<std::string> names;
            for (const ZipEntry &entry : index)
            {
                if (std::string skin = skin_base(entry.name); !skin.empty())
                {
                    push_unique(names, row_name(skin));
                }
                else if (!skins_only && !material_key(entry.name).empty())
                {
                    push_unique(names, row_name(entry.name));
                }
            }
            return names;
        }

        /**
         * @brief Marks the changed skins unshared and adds every marked CSkin to the window.
         * @details A proxy that still shows a marked skin is reset. One that shows none loads the file from the overlay
         *          on its own, and one whose last load failed is recovered from Error.
         */
        void retire_skins(const std::vector<std::string> &skins)
        {
            DMK::Logger &logger = DMK::log();
            const std::vector<SkinCacheEntry> entries = mark_skins_unshared(skins);
            for (const SkinCacheEntry &entry : entries)
            {
                logger.info(
                    "Skin: {} cached at {:#x} (refs {}, flags {:#x}) marked unshared",
                    entry.key,
                    entry.skin,
                    entry.refs,
                    entry.flags
                );
                if ((entry.flags & constants::SKIN_FLAG_UNSHARED) != 0)
                {
                    add_unique(s_replaced_skins, entry.skin);
                }
            }
            for (const std::string &skin : skins)
            {
                const bool cached = std::any_of(
                    entries.begin(),
                    entries.end(),
                    [&skin](const SkinCacheEntry &entry) { return entry.key == skin; }
                );
                if (!cached)
                {
                    logger.info("Skin: {} is not cached (not loaded, or its last load failed)", skin);
                }
            }
        }

        /**
         * @brief Takes the changed materials out of CMatMan's map.
         * @details A proxy that still renders with a retired material is reset, found by the marker in its name.
         */
        void retire_materials(const std::vector<std::string> &materials)
        {
            DMK::Logger &logger = DMK::log();
            for (const std::string &material : materials)
            {
                const std::uintptr_t retired = retire_material(material);
                if (retired == 0)
                {
                    logger.info("Material: {} is not loaded", material);
                }
                else
                {
                    logger.info("Material: {} at {:#x} retired from CMatMan", material, retired);
                    add_unique(s_retired_materials, retired);
                }
            }
        }

        /**
         * @brief Opens (or extends) the reset window over every skin and material @p index ships.
         * @details Recovery resets a proxy in Error that asks for any of them: a broken file left it there, the manager
         *          holds back the character's other proxies behind it, and the build now mounted is the one to try
         *          again.
         */
        void open_window(const std::vector<ZipEntry> &index)
        {
            for (std::string &name : row_names_in(index, false))
            {
                push_unique(s_shipped_files, std::move(name));
            }
            const std::chrono::milliseconds window = reset_window();
            begin_proxy_resets(
                ResetTargets{
                    .shipped_files = s_shipped_files,
                    .restream_files = s_restream_files,
                    .replaced_skins = s_replaced_skins,
                },
                window
            );
            DMK::log().info(
                "Reset window open for {} s over {} shipped file(s): {} replaced skin(s), {} retired material(s), {} "
                "file(s) restreamed for new table rows",
                std::chrono::duration_cast<std::chrono::seconds>(window).count(),
                s_shipped_files.size(),
                s_replaced_skins.size(),
                s_retired_materials.size(),
                s_restream_files.size()
            );
        }

        /**
         * @brief Reloads each table database whose table or patch changed.
         * @details The database is matched by name and subfolder (see table_of), then reloaded the way the manager
         *          reloads it, so the overlay's patch is part of the new rows. A database in RESTART_ONLY_TABLES is
         *          skipped with a warning.
         * @return True when at least one database was reloaded.
         */
        [[nodiscard]] bool reload_tables(const std::vector<std::string> &tables)
        {
            DMK::Logger &logger = DMK::log();
            if (tables.empty())
            {
                return false;
            }
            const std::vector<DatabaseInfo> databases = table_databases();
            std::vector<std::uintptr_t> reloaded;
            bool any = false;
            for (const std::string &table : tables)
            {
                const std::optional<TableRef> ref = table_of(table);
                if (!ref.has_value())
                {
                    continue;
                }
                const auto match = std::find_if(
                    databases.begin(),
                    databases.end(),
                    [&ref](const DatabaseInfo &database)
                    { return iequals(database.name, ref->name) && iequals(database.subfolder, ref->subfolder); }
                );
                if (match == databases.end())
                {
                    logger.warning("Table: {} matches no registered database; not reloaded", table);
                    continue;
                }
                if (restart_only(*match))
                {
                    logger.warning(
                        "Table: {} changed; database {}/{} is linked from other databases and is not reloaded live. "
                        "Restart the game to use the change",
                        table,
                        match->subfolder,
                        match->name
                    );
                    continue;
                }
                if (std::find(reloaded.begin(), reloaded.end(), match->object) != reloaded.end())
                {
                    continue;
                }
                reloaded.push_back(match->object);
                const std::optional<TableReloadResult> result = reload_table_database(match->object);
                if (!result.has_value())
                {
                    logger.warning("Table: database {}/{} not reloaded for {}", match->subfolder, match->name, table);
                    continue;
                }
                if (result->relink_failed != 0)
                {
                    logger.error(
                        "Table: database {}/{} reloaded for {} ({}), but {} of its {} dependent database(s) did not "
                        "link again; restart the game before loading a save",
                        match->subfolder,
                        match->name,
                        table,
                        result->ok ? "ok" : "FAILED",
                        result->relink_failed,
                        result->relinked + result->relink_failed
                    );
                }
                else
                {
                    logger.info(
                        "Table: database {}/{} reloaded for {} ({}); {} dependent database(s) linked again",
                        match->subfolder,
                        match->name,
                        table,
                        result->ok ? "ok" : "FAILED",
                        result->relinked
                    );
                }
                any = any || result->ok;
            }
            return any;
        }

        void apply_generation(PendingGeneration &generation)
        {
            DMK::Logger &logger = DMK::log();
            SourceRecord &record = record_for(generation.file_name);
            if (!record.baseline_known)
            {
                discover_baseline(record);
            }

            if (!proxy_resets_active())
            {
                s_shipped_files.clear();
                s_restream_files.clear();
                s_replaced_skins.clear();
                s_retired_materials.clear();
            }

            const std::vector<ZipEntry> changed = changed_entries(record.baseline, generation.index);
            if (changed.empty())
            {
                // Nothing to mount, but a build that fixed a broken file may already be mounted while a proxy it
                // broke still sits in Error: an unchanged rebuild (or a HotPak reload) is the way to retry it.
                logger.info(
                    "Pipeline: generation {} of {} matches what the game reads; not mounted, recovery pass only",
                    generation.generation,
                    generation.file_name
                );
                DeleteFileW(generation.shadow.c_str());
                open_window(generation.index);
                s_follow_ups.push_back(
                    FollowUp{
                        .due = GetTickCount64() + FOLLOW_UP_DELAY_MS,
                    }
                );
                return;
            }

            const std::string shadow = to_utf8(generation.shadow);
            if (!open_overlay_pak(record.bind_root, shadow))
            {
                logger.error("Pipeline: CryPak refused to mount {}", shadow);
                DeleteFileW(generation.shadow.c_str());
                return;
            }
            const std::vector<MountedPak> paks = mounted_paks();
            const bool last = !paks.empty() && iequals(paks.back().path, shadow);
            logger.info(
                "Pipeline: mounted {} under '{}' (pak {} of the search list, {}); {} changed entr{}",
                shadow,
                record.bind_root,
                paks.size(),
                last ? "searched first" : "NOT LAST, an older pak may still win",
                changed.size(),
                changed.size() == 1 ? "y" : "ies"
            );

            std::vector<std::string> textures;
            std::vector<std::string> skins;
            std::vector<std::string> statics;
            std::vector<std::string> materials;
            std::vector<std::string> tables;
            std::vector<std::string> scripts;
            std::vector<std::string> others;
            for (const ZipEntry &entry : changed)
            {
                logger.debug("Pipeline:   {} ({} bytes, crc {:08x})", entry.name, entry.size, entry.crc32);
                if (std::string base = texture_base(entry.name); !base.empty())
                {
                    push_unique(textures, std::move(base));
                }
                else if (std::string skin = skin_base(entry.name); !skin.empty())
                {
                    push_unique(skins, std::move(skin));
                }
                else if (entry.name.ends_with(".cgf"))
                {
                    push_unique(statics, entry.name);
                }
                else if (std::string key = material_key(entry.name); !key.empty())
                {
                    push_unique(materials, std::move(key));
                }
                else if (table_of(entry.name).has_value())
                {
                    push_unique(tables, entry.name);
                }
                else if (entry.name.ends_with(".lua"))
                {
                    push_unique(scripts, entry.name);
                }
                else
                {
                    push_unique(others, entry.name);
                }
            }

            for (const std::string &texture : textures)
            {
                const bool queued = request_texture_reload(texture);
                logger.info("Texture: {} reload {}", texture, queued ? "queued" : "REFUSED");
            }
            for (const std::string &file : statics)
            {
                const bool refreshed = reload_file_now(file);
                logger.info(
                    "Static: {} {}",
                    file,
                    refreshed ? "refreshed" : "not loaded (its next load reads the overlay)"
                );
            }
            if (!skins.empty())
            {
                retire_skins(skins);
            }
            if (!materials.empty())
            {
                retire_materials(materials);
            }
            // A reloaded table feeds the material instances the clothing builds at load, so every character wearing
            // one of this pak's skins is restreamed to rebuild them from the new rows.
            if (reload_tables(tables))
            {
                for (std::string &name : row_names_in(generation.index, true))
                {
                    logger.info("Table: proxies of {} restream for the new rows", name);
                    push_unique(s_restream_files, std::move(name));
                }
            }
            // Scripts run after the tables, so a rerun init script reads the reloaded rows.
            rerun_scripts(scripts, generation.index);
            for (const std::string &other : others)
            {
                logger.warning("Other: {} changed; the game reads it again only when it next loads it", other);
            }

            open_window(generation.index);
            record.baseline = std::move(generation.index);
            s_follow_ups.push_back(
                FollowUp{
                    .due = GetTickCount64() + FOLLOW_UP_DELAY_MS,
                    .skins = std::move(skins),
                    .materials = std::move(materials),
                }
            );
        }

        /**
         * @brief Reports what every proxy reset since the last report did, and names a failed load loudly.
         * @details A proxy still in Error a few seconds after its reset could not load what its row asks for from the
         *          files now mounted: a broken build. The next build, changed or not, retries it.
         */
        void report_outcomes()
        {
            DMK::Logger &logger = DMK::log();
            for (const ProxyOutcome &outcome : read_outcomes(take_reset_records()))
            {
                const ResetRecord &reset = outcome.record;
                if (outcome.state == constants::PROXY_STATE_ERROR)
                {
                    logger.error(
                        "Reload FAILED: proxy {:#x} ({} / {}) is in Error after its reset; a file it needs is broken "
                        "in this build. The character stays hidden until a build that loads; rebuild (even unchanged) "
                        "to retry",
                        reset.proxy,
                        reset.model,
                        reset.material
                    );
                    continue;
                }
                if (outcome.state == constants::PROXY_STATE_WAITING_FOR_SWAP)
                {
                    std::string blockers;
                    for (const std::string &name : blocking_attachments(reset.proxy))
                    {
                        blockers += blockers.empty() ? name : ", " + name;
                    }
                    if (!blockers.empty())
                    {
                        logger.error(
                            "Reload BLOCKED: proxy {:#x} ({} / {}) waits for its swap because SKIN attachment(s) [{}] "
                            "on that character have no skin; the character swaps nothing in until they are removed "
                            "(Lua: entity:DestroyAttachment(slot, name)). The next build retries",
                            reset.proxy,
                            reset.model,
                            reset.material,
                            blockers
                        );
                    }
                    else
                    {
                        logger.warning(
                            "Reload WAITING: proxy {:#x} ({} / {}) still waits for its swap; the character's other "
                            "waiting proxies are reset 4 s after its last reset",
                            reset.proxy,
                            reset.model,
                            reset.material
                        );
                    }
                    continue;
                }
                logger.info(
                    "Reset: proxy {:#x} ({} / {}) for {}: state {} -> {}{}",
                    reset.proxy,
                    reset.model,
                    reset.material,
                    reset_reason_name(reset.reason),
                    reset.state_before,
                    outcome.state,
                    outcome.state == constants::PROXY_STATE_STREAMING ? " (loaded)" : ""
                );
            }
        }

        void run_follow_ups()
        {
            const std::uint64_t now = GetTickCount64();
            for (auto it = s_follow_ups.begin(); it != s_follow_ups.end();)
            {
                if (now < it->due)
                {
                    ++it;
                    continue;
                }
                for (const SkinCacheEntry &entry : find_cached_skins(it->skins))
                {
                    DMK::log().info(
                        "Skin: after reload {} -> {:#x} (refs {}, flags {:#x}){}",
                        entry.key,
                        entry.skin,
                        entry.refs,
                        entry.flags,
                        (entry.flags & constants::SKIN_FLAG_UNSHARED) != 0 ? " old" : " NEW"
                    );
                }
                for (const std::string &material : it->materials)
                {
                    const std::uintptr_t loaded = find_material(material);
                    const bool stale = std::find(s_retired_materials.begin(), s_retired_materials.end(), loaded) !=
                                       s_retired_materials.end();
                    DMK::log().info(
                        "Material: after reload {} -> {:#x}{}",
                        material,
                        loaded,
                        loaded == 0 ? " (not loaded again yet)" : (stale ? " old" : " NEW")
                    );
                }
                report_outcomes();
                it = s_follow_ups.erase(it);
            }
        }

        void report_resets()
        {
            const std::uint64_t now = GetTickCount64();
            const bool closed = expire_proxy_resets();
            if (!closed && (!proxy_resets_active() || now < s_next_stats))
            {
                return;
            }
            s_next_stats = now + STATS_INTERVAL_MS;
            const ProxyReloadStats stats = take_proxy_reload_stats();
            if (stats.proxies_reset != 0 || closed)
            {
                DMK::log().info(
                    "Reset: {} proxy reset(s) over {} manager pass(es){}",
                    stats.proxies_reset,
                    stats.manager_passes,
                    closed ? "; reset window closed" : ""
                );
            }
        }
    } // namespace

    void reset_reload_pipeline()
    {
        s_sources.clear();
        s_follow_ups.clear();
        s_shipped_files.clear();
        s_restream_files.clear();
        s_replaced_skins.clear();
        s_retired_materials.clear();
        s_next_stats = 0;
    }

    void reload_tick() noexcept
    {
        try
        {
            std::vector<PendingGeneration> pending = take_pending_generations();
            for (PendingGeneration &generation : pending)
            {
                apply_generation(generation);
            }
            run_follow_ups();
            report_resets();
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Pipeline: a reload step threw; skipped");
        }
    }

} // namespace HotPak
