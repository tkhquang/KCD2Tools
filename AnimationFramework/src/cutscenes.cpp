/**
 * @file cutscenes.cpp
 * @brief Mods' in-game cutscenes (see cutscenes.hpp).
 *
 * A file parses through the CXmlUtils the engine loads moviedata.xml with: a game path through its LoadXmlFromFile
 * (CryPak, so mods' paks), a file on disk through its LoadXmlFromBuffer. Each `<Sequence>` element then goes to
 * CMovieSystem::LoadSequence, which keeps its own copy of what it reads, so the parsed tree is released right after.
 *
 * The cutscene command makes the C_CutsceneConfiguration and enqueues it as the quest CutsceneHandler node does (Steam
 * 0x182B12644). The node's module argument only names the owner in the player's logs and soul links. The create call
 * indexes the module table with its id unchecked, so the command passes slot 0, then puts the configuration in the
 * state C_CutsceneConfiguration's constructor leaves it in before a module is set (Steam 0x182B35084). The
 * configuration is a shared_ptr the node keeps while the cutscene runs, so the command keeps its reference too, until
 * the player drops its own: then the cutscene ended or was skipped, and its holder leaves animfw_cutscene_playing.
 */

#include "cutscenes.hpp"

#include "console_vars.hpp"
#include "constants.hpp"
#include "cutscene_lua.hpp"
#include "game_memory.hpp"
#include "game_xml.hpp"
#include "hooks/anim_patch.hpp"
#include "hooks/hook_install.hpp"

#include <DetourModKit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace AnimationFramework
{
    namespace
    {
        constexpr char COMMAND_FAILED[] = "Cutscenes: the sequence load failed";
        constexpr const char *LOAD_COMMAND = "animfw_load_sequence";
        constexpr const char *LOAD_HELP =
            "Animation Framework: loads TrackView sequences (cutscenes) into the movie system. animfw_load_sequence "
            "<file> [file...]: a path in the game's paks (mods' paks too), a file in the DEV folder, or an absolute "
            "path, holding a <Sequence> or a root of <Sequence> elements. Entity nodes bind to the entities of their "
            "Name as the file loads. A trailing `@ x y z yaw` places a sequence authored around the origin "
            "(forward +Y) there: node positions turn by yaw degrees and move, and node yaw adds yaw. A load replaces "
            "the sequence of the same name.";
        constexpr const char *PLAY_COMMAND = "animfw_play_cutscene";
        constexpr const char *PLAY_HELP =
            "Animation Framework: starts a cutscene through the game's cutscene player, as a quest does. "
            "animfw_play_cutscene <holder>: the name of a CutsceneHolder entity whose cutscene is not already queued "
            "or playing.";
        constexpr char PLAY_FAILED[] = "Cutscenes: the cutscene start failed";
        constexpr const char *LINKABLE_COMMAND = "animfw_make_linkable";
        constexpr const char *LINKABLE_HELP =
            "Animation Framework: gives spawned entities a WUID, so XGenAIModule.AddLink can link them. "
            "animfw_make_linkable <entity> [entity...]: entity names.";
        constexpr char LINKABLE_FAILED[] = "Cutscenes: making an entity linkable failed";
        constexpr const char *SCENE_LAYER_COMMAND = "animfw_scene_layer";
        constexpr const char *SCENE_LAYER_HELP =
            "Animation Framework: adds a streamed entity layer of the current level, as AnimationFramework.Cutscene "
            "uses one, puts the named entities in it and disables it. animfw_scene_layer <layer> <id> [entity...]: "
            "the layer file's name (Levels/<level>/layers/<layer>.xml) and a layer id 1..65534 no level uses. The "
            "layer's file numbers its entities 1..N, and each load moves them to free entity ids.";
        constexpr char SCENE_LAYER_FAILED[] = "Cutscenes: the scene layer failed";
        constexpr const char *SCENE_PROFILE_COMMAND = "animfw_scene_profile";
        constexpr const char *SCENE_PROFILE_HELP =
            "Animation Framework: adds a game profile over layers, as the level loader adds one, for a cutscene's "
            "IngameCutsceneData to switch on. animfw_scene_profile <profile> <layer> [layer...].";
        constexpr char SCENE_PROFILE_FAILED[] = "Cutscenes: the scene profile failed";
        // CEntitySystem's layer slots: AddLayer, AddEntityToLayer, FindLayer, EnableLayer.
        constexpr std::size_t ENTITY_SYSTEM_ADD_LAYER_SLOT = 64;
        constexpr std::size_t ENTITY_SYSTEM_ADD_TO_LAYER_SLOT = 67;
        constexpr std::size_t ENTITY_SYSTEM_FIND_LAYER_SLOT = 71;
        constexpr std::size_t ENTITY_SYSTEM_ENABLE_LAYER_SLOT = 74;
        constexpr int MAX_LAYER_ID = 0xFFFE;
        // C_GameProfileManager: slot 4 says whether a profile name is known; the record vector at +0x10.
        constexpr std::size_t PROFILE_MANAGER_EXISTS_SLOT = 4;
        constexpr std::ptrdiff_t PROFILE_RECORDS_OFFSET = 0x10;
        // A profile record (constructor Steam 0x1812FDD78): name, the flag bytes, id, name CRC, layer names.
        constexpr std::ptrdiff_t PROFILE_FLAGS_OFFSET = 0x08;
        constexpr std::size_t PROFILE_FLAG_COUNT = 4;
        constexpr std::ptrdiff_t PROFILE_ID_OFFSET = 0x10;
        constexpr std::ptrdiff_t PROFILE_NAME_CRC_OFFSET = 0x14;
        constexpr std::ptrdiff_t PROFILE_LAYERS_OFFSET = 0x18;
        constexpr std::string_view PLAYING_CVAR = "animfw_cutscene_playing";
        constexpr std::string_view PLAYING_CVAR_HELP =
            "Animation Framework (read-only): the CutsceneHolder names of the cutscenes started with "
            "animfw_play_cutscene that the game's player still runs, space separated. A holder leaves it when its "
            "cutscene ends or is skipped, so a scene polls it for its own holder before it removes its entities.";
        constexpr std::string_view SEQUENCE_TAG = "Sequence";
        constexpr std::string_view NODES_TAG = "Nodes";
        constexpr std::string_view NODE_TAG = "Node";
        constexpr const char *ENTITY_GUID_ATTR = "EntityGUID";
        constexpr std::string_view TRACK_TAG = "Track";
        constexpr std::string_view SUB_TRACK_TAG = "NewSubTrack";
        constexpr std::string_view KEY_TAG = "Key";
        constexpr std::string_view PLACEMENT_MARK = "@";

        /** @brief Where a sequence authored around the origin plays: a position and a yaw in degrees. */
        struct Placement
        {
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            double yaw = 0.0;
        };
        // Where C_CutsceneConfiguration keeps the cutscene its holder names, from the cutscene table. No row, no
        // cutscene.
        constexpr std::ptrdiff_t CONFIGURATION_CUTSCENE_OFFSET = 0x20;
        // The configuration's module reference: the id, then its table generation.
        constexpr std::ptrdiff_t CONFIGURATION_MODULE_ID_OFFSET = 0x08;
        constexpr std::ptrdiff_t CONFIGURATION_MODULE_GENERATION_OFFSET = 0x0C;
        constexpr std::int32_t NO_MODULE_ID = -1;
        // LoadXmlFromBuffer takes a 32-bit size; a sequence file is far smaller.
        constexpr std::uintmax_t MAX_FILE_BYTES = 64ull * 1024 * 1024;

        /** @brief A std::shared_ptr<C_CutsceneConfiguration> as the game lays it out. */
        struct SharedConfiguration
        {
            void *object = nullptr;
            void *control = nullptr;
        };

        // The game's std::shared_ptr control block (MSVC _Ref_count_base): its vtable's _Destroy and _Delete_this, then
        // the strong and weak counts.
        constexpr std::size_t CONTROL_DESTROY_SLOT = 0;
        constexpr std::size_t CONTROL_DELETE_THIS_SLOT = 1;
        constexpr std::ptrdiff_t CONTROL_USES_OFFSET = 0x08;
        constexpr std::ptrdiff_t CONTROL_WEAKS_OFFSET = 0x0C;

        /** @brief What the create call reads the module id through, as the node's port value lays it out. */
        struct ModuleId
        {
            std::uint64_t reserved = 0;
            std::int32_t id = 0;
        };

        using CreateConfigurationFunc = SharedConfiguration *(__fastcall *)(SharedConfiguration * out,
                                                                            void *holder,
                                                                            const bool *auto_play,
                                                                            const bool *auto_finish,
                                                                            const ModuleId *const *module);
        using EnqueueConfigurationFunc = void *(__fastcall *)(void *player, SharedConfiguration *configuration);
        using GameGetterFunc = std::uintptr_t(__fastcall *)();
        using LinkableFilterFunc = bool(__fastcall *)(void *manager, void *entity);
        using LinkableMakeFunc = void *(__fastcall *)(void *manager, void *entity, bool ownership);
        using StringFromTextFunc = void **(__fastcall *)(void **out, const char *text);
        using StringReleaseFunc = void(__fastcall *)(void **string);
        using RecordCreateFunc = void **(__fastcall *)(void **out, void *unused);
        using RecordsInsertFunc = void *(__fastcall *)(void *vector, void *where, void **record);
        using LayerPushFunc = void(__fastcall *)(void *vector, void **string);

        // What the loader reads through, decoded and checked at init. Written once at setup, before s_bound
        // publishes it.
        Anchors s_anchors{};
        std::atomic<bool> s_bound{false};
        // Main thread only.
        bool s_added = false;
        bool s_play_added = false;
        bool s_linkable_added = false;
        bool s_scene_added = false;
        bool s_lua_registered = false;
        /** @brief One cutscene the play command enqueued: the framework's configuration reference and its holder. */
        struct StartedCutscene
        {
            SharedConfiguration configuration;
            std::string holder;
        };
        // Main thread only. One reference per started cutscene while the player holds its own: the player keeps
        // comparing the running configuration with its holder's, so ours must not be the one that frees it under the
        // player. Once ours is the last (the cutscene ended, was skipped or was refused), the frame work drops it.
        std::vector<StartedCutscene> s_started;
        /// True while a cutscene the framework started is queued or running: only then are track events kept.
        std::atomic<bool> s_scenes_running{false};

        /// One Event-node key that reached its sequence, waiting for the frame work to hand it to Lua.
        struct SceneEvent
        {
            std::string sequence;
            std::string event;
            std::string param;
        };
        constexpr std::size_t MAX_PENDING_EVENTS = 256;
        std::mutex s_event_mutex;
        std::vector<SceneEvent> s_pending_events;

        using TriggerTrackEventFunc =
            void(__fastcall *)(void *sequence, const char *event, const char *param, void *user);
        std::atomic<TriggerTrackEventFunc> s_trigger_original{nullptr};

        /** @brief Keeps the event while the framework's cutscenes run, then lets the sequence notify its listeners. */
        void __fastcall
        detour_trigger_track_event(void *sequence, const char *event, const char *param, void *user) noexcept
        {
            if (s_scenes_running.load(std::memory_order_acquire) && sequence != nullptr && event != nullptr)
            {
                try
                {
                    const char *name = vcall<const char *>(sequence, constants::ANIM_SEQUENCE_GET_NAME_SLOT);
                    const std::lock_guard lock{s_event_mutex};
                    if (s_pending_events.size() < MAX_PENDING_EVENTS)
                    {
                        s_pending_events.push_back(
                            SceneEvent{
                                .sequence = name != nullptr ? name : "",
                                .event = event,
                                .param = param != nullptr ? param : "",
                            }
                        );
                    }
                }
                catch (...)
                {
                    (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Cutscenes: a track event was dropped");
                }
            }
            const TriggerTrackEventFunc original = s_trigger_original.load(std::memory_order_acquire);
            if (original != nullptr)
            {
                original(sequence, event, param, user);
            }
        }

        /** @brief @p text as a double-quoted Lua string literal. */
        [[nodiscard]] std::string lua_quoted(std::string_view text)
        {
            constexpr char QUOTE = 0x22;
            constexpr char BACKSLASH = 0x5C;
            std::string quoted(1, QUOTE);
            for (const char c : text)
            {
                const auto code = static_cast<unsigned char>(c);
                if (c == QUOTE || c == BACKSLASH)
                {
                    quoted += BACKSLASH;
                    quoted += c;
                }
                else if (code >= 0x20)
                {
                    quoted += c;
                }
            }
            quoted += QUOTE;
            return quoted;
        }

        // Main thread only. The playing-holders variable; the console keeps it for the process.
        void *s_playing_cvar = nullptr;
        bool s_playing_cvar_failed = false;

        /** @brief One line to the console and kcd.log, and to the framework's log. */
        void print(std::string_view line)
        {
            game_log_line(line);
            DMK::log().info("Cutscenes: {}", line);
        }

        /** @brief The live movie system, or nullptr before it exists or when gEnv holds something else there. */
        [[nodiscard]] void *movie_system() noexcept
        {
            const std::uintptr_t movie = read_ptr(s_anchors.genv + constants::GENV_MOVIE_SYSTEM_OFFSET);
            return movie != 0 && read_ptr(movie) == s_anchors.movie_system_vtable ? reinterpret_cast<void *>(movie)
                                                                                  : nullptr;
        }

        /** @brief The CXmlUtils the movie system's ISystem loads with, or nullptr. */
        [[nodiscard]] void *xml_utils(void *movie) noexcept
        {
            const std::uintptr_t system =
                read_ptr(reinterpret_cast<std::uintptr_t>(movie) + s_anchors.movie_system_system_offset);
            if (system == 0 || read_ptr(system) != s_anchors.system_vtable)
            {
                return nullptr;
            }
            return reinterpret_cast<void *>(read_ptr(system + s_anchors.system_xml_utils_offset));
        }

        /** @brief The live entity system when its vtable is CEntitySystem's, else nullptr. */
        [[nodiscard]] void *entity_system() noexcept
        {
            const std::uintptr_t system = read_ptr(s_anchors.genv + constants::GENV_ENTITY_SYSTEM_OFFSET);
            return system != 0 && read_ptr(system) == s_anchors.entity_system_vtable ? reinterpret_cast<void *>(system)
                                                                                     : nullptr;
        }

        /** @brief The live entity named @p name, or nullptr. */
        [[nodiscard]] void *find_entity(const std::string &name) noexcept
        {
            void *system = entity_system();
            return system != nullptr ? vcall<void *>(system, constants::ENTITY_SYSTEM_FIND_BY_NAME_SLOT, name.c_str())
                                     : nullptr;
        }

        /** @brief @p entity's GUID in the text form a node's EntityGUID holds (`%8x-%4hx-%4hx`). */
        [[nodiscard]] std::optional<std::string> entity_guid_text(void *entity)
        {
            const std::optional<std::uint64_t> guid =
                read_value<std::uint64_t>(reinterpret_cast<std::uintptr_t>(entity) + constants::ENTITY_GUID_OFFSET);
            if (!guid.has_value())
            {
                return std::nullopt;
            }
            return std::format(
                "{:08x}-{:04x}-{:04x}",
                static_cast<std::uint32_t>(*guid),
                static_cast<std::uint16_t>(*guid >> 32),
                static_cast<std::uint16_t>(*guid >> 48)
            );
        }

        /**
         * @brief Points each node whose Name names a live entity at that entity.
         * @return How many nodes it set.
         */
        int bind_nodes_to_live_entities(void *sequence)
        {
            int bound = 0;
            const int sections = child_count(sequence);
            for (int i = 0; i < sections; ++i)
            {
                const XmlRef nodes = child_at(sequence, i);
                if (!nodes || tag_of(nodes.get()) != NODES_TAG)
                {
                    continue;
                }
                const int count = child_count(nodes.get());
                for (int j = 0; j < count; ++j)
                {
                    const XmlRef node = child_at(nodes.get(), j);
                    if (!node || tag_of(node.get()) != NODE_TAG)
                    {
                        continue;
                    }
                    void *entity = find_entity(std::string{attr_of(node.get(), "Name")});
                    if (entity == nullptr)
                    {
                        continue;
                    }
                    if (const std::optional<std::string> guid = entity_guid_text(entity))
                    {
                        vcall<void>(node.get(), XML_NODE_SET_ATTR_SLOT, ENTITY_GUID_ATTR, guid->c_str());
                        ++bound;
                    }
                }
            }
            return bound;
        }

        /** @brief The whole file at @p path, or nothing when it cannot be read or is too large. */
        [[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path &path)
        {
            std::error_code error;
            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error || size > MAX_FILE_BYTES)
            {
                return std::nullopt;
            }
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return std::nullopt;
            }
            std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
            if (stream.bad())
            {
                return std::nullopt;
            }
            return text;
        }

        /** @brief A file on disk: @p argument when it is absolute, else the DEV folder's file of that name. */
        [[nodiscard]] std::optional<std::filesystem::path> disk_file(std::string_view argument)
        {
            const std::filesystem::path given{std::string{argument}};
            std::error_code error;
            if (given.is_absolute())
            {
                return std::filesystem::is_regular_file(given, error) ? std::optional{given} : std::nullopt;
            }
            const std::filesystem::path dev = animation_dev_folder() / given;
            if (std::filesystem::is_regular_file(dev, error))
            {
                return dev;
            }
            return std::nullopt;
        }

        /**
         * @brief @p argument parsed as a mutable tree: a file on disk (disk_file), else a game path read through
         *        CryPak.
         * @param where Set to what was read, for the report.
         */
        [[nodiscard]] XmlRef parse_file(void *utils, std::string_view argument, std::string &where)
        {
            XmlRef root;
            if (const std::optional<std::filesystem::path> path = disk_file(argument))
            {
                where = path->string();
                const std::optional<std::string> text = read_file(*path);
                if (text.has_value() && !text->empty())
                {
                    vcall<void **>(
                        utils,
                        XML_UTILS_LOAD_BUFFER_SLOT,
                        root.out(),
                        text->data(),
                        static_cast<unsigned>(text->size()),
                        false,
                        false
                    );
                }
                return root;
            }
            where = std::string{argument};
            vcall<void **>(utils, XML_UTILS_LOAD_FILE_SLOT, root.out(), where.c_str(), false, true, false, false);
            return root;
        }

        /** @brief A spline key's "time,value", or nothing when @p text is not two numbers. */
        [[nodiscard]] std::optional<std::pair<double, double>> key_pair(std::string_view text)
        {
            const std::size_t comma = text.find(',');
            if (comma == std::string_view::npos)
            {
                return std::nullopt;
            }
            double time = 0.0;
            double value = 0.0;
            const std::string_view first = text.substr(0, comma);
            const std::string_view second = text.substr(comma + 1);
            if (std::from_chars(first.data(), first.data() + first.size(), time).ec != std::errc{} ||
                std::from_chars(second.data(), second.data() + second.size(), value).ec != std::errc{})
            {
                return std::nullopt;
            }
            return std::pair{time, value};
        }

        /** @brief One sub-track's default and keys: the key elements and their (time, value). */
        struct SubTrack
        {
            XmlRef element;
            std::optional<std::pair<double, double>> fallback;
            std::vector<XmlRef> keys;
            std::vector<std::pair<double, double>> values;
        };

        /// Reads the default and the parseable keys of the sub-track @p element.
        [[nodiscard]] SubTrack read_sub_track(XmlRef element)
        {
            SubTrack sub{.element = std::move(element)};
            sub.fallback = key_pair(attr_of(sub.element.get(), "defaultValue"));
            const int count = child_count(sub.element.get());
            for (int i = 0; i < count; ++i)
            {
                XmlRef key = child_at(sub.element.get(), i);
                if (!key || tag_of(key.get()) != KEY_TAG)
                {
                    continue;
                }
                if (const std::optional<std::pair<double, double>> pair = key_pair(attr_of(key.get(), "value")))
                {
                    sub.values.push_back(*pair);
                    sub.keys.push_back(std::move(key));
                }
            }
            return sub;
        }

        /** @brief @p sub's value at @p time: the key at that time, else linear between its keys, else its default. */
        [[nodiscard]] double value_at(const SubTrack &sub, double time)
        {
            if (sub.values.empty())
            {
                return sub.fallback.has_value() ? sub.fallback->second : 0.0;
            }
            if (time <= sub.values.front().first)
            {
                return sub.values.front().second;
            }
            for (std::size_t i = 1; i < sub.values.size(); ++i)
            {
                const auto &[t1, v1] = sub.values[i];
                if (time <= t1)
                {
                    const auto &[t0, v0] = sub.values[i - 1];
                    const double span = t1 - t0;
                    return span > 1e-9 ? v0 + (v1 - v0) * (time - t0) / span : v1;
                }
            }
            return sub.values.back().second;
        }

        /// Writes "time,value" into @p attribute of @p element, the form key_pair reads.
        void set_pair(void *element, const char *attribute, double time, double value)
        {
            const std::string text = std::format("{:.4f},{:.4f}", time, value);
            vcall<void>(element, XML_NODE_SET_ATTR_SLOT, attribute, text.c_str());
        }

        /** @brief The X, Y and Z sub-tracks of the first `<Track paramType=@p type>` of @p node; empty when absent. */
        [[nodiscard]] std::vector<SubTrack> sub_tracks(void *node, std::string_view type)
        {
            std::vector<SubTrack> subs;
            const int count = child_count(node);
            for (int i = 0; i < count; ++i)
            {
                const XmlRef track = child_at(node, i);
                if (!track || tag_of(track.get()) != TRACK_TAG || attr_of(track.get(), "paramType") != type)
                {
                    continue;
                }
                const int children = child_count(track.get());
                for (int j = 0; j < children; ++j)
                {
                    XmlRef sub = child_at(track.get(), j);
                    if (sub && tag_of(sub.get()) == SUB_TRACK_TAG)
                    {
                        subs.push_back(read_sub_track(std::move(sub)));
                    }
                }
                break;
            }
            return subs;
        }

        /**
         * @brief Moves one node to @p at: its positions turn by the yaw about the origin and move, its yaw adds the
         *        yaw.
         * @details A key on one horizontal axis takes the other axis's value at its time (linear between keys), so
         *          positions keyed at the same times on X and Y move exactly.
         */
        void place_node(void *node, const Placement &at)
        {
            const double radians = at.yaw * std::numbers::pi / 180.0;
            const double c = std::cos(radians);
            const double s = std::sin(radians);
            std::vector<SubTrack> position = sub_tracks(node, "Position");
            if (position.size() == 3)
            {
                SubTrack &px = position[0];
                SubTrack &py = position[1];
                SubTrack &pz = position[2];
                std::vector<std::pair<double, double>> new_x;
                std::vector<std::pair<double, double>> new_y;
                for (const auto &[time, x] : px.values)
                {
                    const double y = value_at(py, time);
                    new_x.emplace_back(time, at.x + x * c - y * s);
                }
                for (const auto &[time, y] : py.values)
                {
                    const double x = value_at(px, time);
                    new_y.emplace_back(time, at.y + x * s + y * c);
                }
                for (std::size_t i = 0; i < px.keys.size(); ++i)
                {
                    set_pair(px.keys[i].get(), "value", new_x[i].first, new_x[i].second);
                }
                for (std::size_t i = 0; i < py.keys.size(); ++i)
                {
                    set_pair(py.keys[i].get(), "value", new_y[i].first, new_y[i].second);
                }
                for (std::size_t i = 0; i < pz.keys.size(); ++i)
                {
                    set_pair(pz.keys[i].get(), "value", pz.values[i].first, pz.values[i].second + at.z);
                }
                const double dx = px.fallback.has_value() ? px.fallback->second : 0.0;
                const double dy = py.fallback.has_value() ? py.fallback->second : 0.0;
                if (px.fallback.has_value())
                {
                    set_pair(px.element.get(), "defaultValue", px.fallback->first, at.x + dx * c - dy * s);
                }
                if (py.fallback.has_value())
                {
                    set_pair(py.element.get(), "defaultValue", py.fallback->first, at.y + dx * s + dy * c);
                }
                if (pz.fallback.has_value())
                {
                    set_pair(pz.element.get(), "defaultValue", pz.fallback->first, pz.fallback->second + at.z);
                }
            }
            std::vector<SubTrack> rotation = sub_tracks(node, "Rotation");
            if (rotation.size() == 3)
            {
                SubTrack &rz = rotation[2];
                for (std::size_t i = 0; i < rz.keys.size(); ++i)
                {
                    set_pair(rz.keys[i].get(), "value", rz.values[i].first, rz.values[i].second + at.yaw);
                }
                if (rz.fallback.has_value())
                {
                    set_pair(rz.element.get(), "defaultValue", rz.fallback->first, rz.fallback->second + at.yaw);
                }
            }
        }

        /** @brief Places every node of @p sequence (see place_node). @return How many nodes it placed. */
        int place_sequence(void *sequence, const Placement &at)
        {
            int placed = 0;
            const int sections = child_count(sequence);
            for (int i = 0; i < sections; ++i)
            {
                const XmlRef nodes = child_at(sequence, i);
                if (!nodes || tag_of(nodes.get()) != NODES_TAG)
                {
                    continue;
                }
                const int count = child_count(nodes.get());
                for (int j = 0; j < count; ++j)
                {
                    const XmlRef node = child_at(nodes.get(), j);
                    if (node && tag_of(node.get()) == NODE_TAG)
                    {
                        place_node(node.get(), at);
                        ++placed;
                    }
                }
            }
            return placed;
        }

        /** @brief Loads one `<Sequence>` element. True when the movie system took it. */
        bool load_sequence_element(
            void *movie,
            void *element,
            std::string_view where,
            const std::optional<Placement> &placement
        )
        {
            const std::string_view name = attr_of(element, "Name");
            if (name.empty())
            {
                print(std::format("{}: a <Sequence> has no Name; skipped", where));
                return false;
            }
            if (placement.has_value())
            {
                const int placed = place_sequence(element, *placement);
                print(
                    std::format(
                        "{}: {} node(s) placed at {:.2f} {:.2f} {:.2f}, yaw {:.1f}",
                        name,
                        placed,
                        placement->x,
                        placement->y,
                        placement->z,
                        placement->yaw
                    )
                );
            }
            const int bound = bind_nodes_to_live_entities(element);
            // LoadSequence reads through an XmlNodeRef & and takes no reference, so the caller's one stays valid.
            void *reference = element;
            void *sequence = vcall<void *>(movie, constants::MOVIE_SYSTEM_LOAD_SEQUENCE_SLOT, &reference, true);
            if (sequence == nullptr)
            {
                print(std::format("{}: the movie system refused sequence '{}'", where, name));
                return false;
            }
            const char *loaded = vcall<const char *>(sequence, constants::ANIM_SEQUENCE_GET_NAME_SLOT);
            print(
                std::format(
                    "loaded sequence '{}' from {}, {} node(s) bound to live entities",
                    loaded != nullptr ? loaded : std::string(name),
                    where,
                    bound
                )
            );
            return true;
        }

        /** @brief Loads every `<Sequence>` of @p argument's file. */
        void load_file(void *movie, void *utils, std::string_view argument, const std::optional<Placement> &placement)
        {
            std::string where;
            const XmlRef root = parse_file(utils, argument, where);
            if (!root)
            {
                print(std::format("{}: not found, or not XML", where));
                return;
            }
            if (tag_of(root.get()) == SEQUENCE_TAG)
            {
                (void)load_sequence_element(movie, root.get(), where, placement);
                return;
            }
            int loaded = 0;
            const int count = child_count(root.get());
            for (int i = 0; i < count; ++i)
            {
                const XmlRef child = child_at(root.get(), i);
                if (child && tag_of(child.get()) == SEQUENCE_TAG &&
                    load_sequence_element(movie, child.get(), where, placement))
                {
                    ++loaded;
                }
            }
            if (loaded == 0)
            {
                print(std::format("{}: no <Sequence> loaded", where));
            }
        }

        /// The load command: loads each named file's sequences.
        void load(void *args)
        {
            const int count = command_argument_count(args);
            if (count < 2)
            {
                print(std::format("usage: {} <file> [file...]", LOAD_COMMAND));
                return;
            }
            void *movie = movie_system();
            void *utils = movie != nullptr ? xml_utils(movie) : nullptr;
            if (movie == nullptr || utils == nullptr)
            {
                print("the movie system is not up yet; load a level first");
                return;
            }
            // A trailing `@ x y z yaw` places every file's sequences.
            int files_end = count;
            std::optional<Placement> placement;
            for (int i = 1; i < count; ++i)
            {
                if (command_argument(args, i) != PLACEMENT_MARK)
                {
                    continue;
                }
                std::array<double, 4> values{};
                bool good = count - i - 1 == static_cast<int>(values.size());
                for (std::size_t k = 0; good && k < values.size(); ++k)
                {
                    const std::string_view text = command_argument(args, i + 1 + static_cast<int>(k));
                    good = std::from_chars(text.data(), text.data() + text.size(), values[k]).ec == std::errc{};
                }
                if (!good)
                {
                    print(std::format("usage: {} <file> [file...] [@ x y z yaw]", LOAD_COMMAND));
                    return;
                }
                placement = Placement{.x = values[0], .y = values[1], .z = values[2], .yaw = values[3]};
                files_end = i;
                break;
            }
            for (int i = 1; i < files_end; ++i)
            {
                const std::string_view argument = command_argument(args, i);
                if (!argument.empty())
                {
                    load_file(movie, utils, argument, placement);
                }
            }
        }

        /** @brief The C_CutsceneHolder extension of @p entity, or nullptr when it is not a CutsceneHolder. */
        [[nodiscard]] void *cutscene_holder_of(void *entity) noexcept
        {
            void *game_object =
                vcall<void *>(entity, constants::ENTITY_GET_PROXY_SLOT, constants::ENTITY_PROXY_GAME_OBJECT);
            if (game_object == nullptr)
            {
                return nullptr;
            }
            const auto base = reinterpret_cast<std::uintptr_t>(game_object) + constants::GAME_OBJECT_EXTENSIONS_OFFSET;
            const std::uintptr_t end = read_ptr(base + sizeof(std::uintptr_t));
            for (std::uintptr_t at = read_ptr(base); at != 0 && at < end; at += constants::GAME_OBJECT_EXTENSION_SIZE)
            {
                const std::uintptr_t extension = read_ptr(at + constants::GAME_OBJECT_EXTENSION_POINTER_OFFSET);
                if (extension != 0 && read_ptr(extension) == s_anchors.cutscene_holder_vtable)
                {
                    return reinterpret_cast<void *>(extension);
                }
            }
            return nullptr;
        }

        /** @brief The strong count of a configuration's control block, or 0 when it cannot be read. */
        [[nodiscard]] std::int32_t use_count(const SharedConfiguration &configuration) noexcept
        {
            const auto control = reinterpret_cast<std::uintptr_t>(configuration.control);
            return control != 0 ? read_value<std::int32_t>(control + CONTROL_USES_OFFSET).value_or(0) : 0;
        }

        /** @brief Drops our reference as std::shared_ptr's destructor does (_Decref, then _Decwref). */
        void release(const SharedConfiguration &configuration) noexcept
        {
            void *control = configuration.control;
            if (control == nullptr)
            {
                return;
            }
            auto *bytes = static_cast<std::byte *>(control);
            std::atomic_ref<std::int32_t> uses{*reinterpret_cast<std::int32_t *>(bytes + CONTROL_USES_OFFSET)};
            std::atomic_ref<std::int32_t> weaks{*reinterpret_cast<std::int32_t *>(bytes + CONTROL_WEAKS_OFFSET)};
            if (uses.fetch_sub(1) == 1)
            {
                vcall<void>(control, CONTROL_DESTROY_SLOT);
                if (weaks.fetch_sub(1) == 1)
                {
                    vcall<void>(control, CONTROL_DELETE_THIS_SLOT);
                }
            }
        }

        /// The play command: makes the holder's configuration and enqueues it on the game's cutscene player.
        void play(void *args)
        {
            if (command_argument_count(args) != 2)
            {
                print(std::format("usage: {} <holder>", PLAY_COMMAND));
                return;
            }
            const std::string name{command_argument(args, 1)};
            if (std::ranges::find(s_started, name, &StartedCutscene::holder) != s_started.end())
            {
                print(std::format("the cutscene of '{}' is already queued or playing", name));
                return;
            }
            void *entity = find_entity(name);
            void *holder = entity != nullptr ? cutscene_holder_of(entity) : nullptr;
            if (holder == nullptr)
            {
                print(std::format("'{}' is not a live CutsceneHolder entity", name));
                return;
            }
            const std::uintptr_t game = reinterpret_cast<GameGetterFunc>(s_anchors.cutscene_game_getter)();
            const std::uintptr_t outer = game != 0 ? read_ptr(game + s_anchors.cutscene_player_outer_offset) : 0;
            const std::uintptr_t player = outer != 0 ? read_ptr(outer + s_anchors.cutscene_player_inner_offset) : 0;
            if (player == 0)
            {
                print("the cutscene player is not up yet");
                return;
            }
            const bool auto_play = true;
            // Nothing calls the node's FinishCutscene input here: without this the player waits for it on a black
            // screen.
            const bool auto_finish = true;
            const ModuleId module_id{};
            const ModuleId *module = &module_id;
            // Reserved before the create call, so no allocation can fail while the new reference is ours alone.
            s_started.reserve(s_started.size() + 1);
            StartedCutscene started{
                .holder = name,
            };
            reinterpret_cast<CreateConfigurationFunc>(s_anchors.cutscene_create)(
                &started.configuration,
                holder,
                &auto_play,
                &auto_finish,
                &module
            );
            if (started.configuration.object == nullptr ||
                read_ptr(
                    reinterpret_cast<std::uintptr_t>(started.configuration.object) + CONFIGURATION_CUTSCENE_OFFSET
                ) == 0)
            {
                release(started.configuration);
                print(std::format("'{}' names no row of the cutscene table", name));
                return;
            }
            const auto object = static_cast<std::byte *>(started.configuration.object);
            *reinterpret_cast<std::int32_t *>(object + CONFIGURATION_MODULE_ID_OFFSET) = NO_MODULE_ID;
            *reinterpret_cast<std::uint8_t *>(object + CONFIGURATION_MODULE_GENERATION_OFFSET) = 0;
            s_started.push_back(std::move(started));
            s_scenes_running.store(true, std::memory_order_release);
            // The enqueue copies the shared_ptr it is handed, so the player holds its own reference from here on.
            reinterpret_cast<EnqueueConfigurationFunc>(s_anchors.cutscene_enqueue)(
                reinterpret_cast<void *>(player),
                &s_started.back().configuration
            );
            print(std::format("enqueued the cutscene of '{}' on the game's player", name));
        }

        /// The linkable command: makes each named entity linkable, as the game's spawn listener does.
        void make_linkable(void *args)
        {
            const int count = command_argument_count(args);
            if (count < 2)
            {
                print(std::format("usage: {} <entity> [entity...]", LINKABLE_COMMAND));
                return;
            }
            void *manager = reinterpret_cast<void *>(read_ptr(s_anchors.linkable_manager_static));
            if (manager == nullptr)
            {
                print("the linkable manager is not up yet");
                return;
            }
            for (int i = 1; i < count; ++i)
            {
                const std::string name{command_argument(args, i)};
                void *entity = find_entity(name);
                if (entity == nullptr)
                {
                    print(std::format("'{}' is not a live entity", name));
                }
                else if (!reinterpret_cast<LinkableFilterFunc>(s_anchors.linkable_filter)(manager, entity))
                {
                    print(std::format("the game refuses to link '{}'", name));
                }
                else
                {
                    reinterpret_cast<LinkableMakeFunc>(s_anchors.linkable_make)(manager, entity, false);
                    print(std::format("'{}' is linkable", name));
                }
            }
        }

        /**
         * @brief Defines AnimationFramework.Cutscene in the game's Lua once its script system is up.
         * @details Cutscene.Play runs every command of this file, so it is defined only when all of them were added: a
         *          scene without its layer, profile or link would only end in a refused cutscene.
         */
        void register_cutscene_lua() noexcept
        {
            if (s_lua_registered || s_anchors.script_system_vtable == 0 || !s_linkable_added || !s_scene_added)
            {
                return;
            }
            const std::uintptr_t scripts = read_ptr(s_anchors.genv + constants::GENV_SCRIPT_SYSTEM_OFFSET);
            if (scripts == 0 || read_ptr(scripts) != s_anchors.script_system_vtable)
            {
                return;
            }
            s_lua_registered = true;
            const bool ran = vcall<bool>(
                reinterpret_cast<void *>(scripts),
                constants::SCRIPT_EXECUTE_BUFFER_SLOT,
                CUTSCENE_LUA,
                sizeof(CUTSCENE_LUA) - 1,
                CUTSCENE_LUA_CHUNK,
                static_cast<void *>(nullptr)
            );
            DMK::log().info(
                "Cutscenes: AnimationFramework.Cutscene {}",
                ran ? "defined" : "failed to load (see kcd.log)"
            );
        }

        /** @brief Hands the kept track events to AnimationFramework.Cutscene._TrackEvent, in the order they fired. */
        void dispatch_scene_events() noexcept
        {
            std::vector<SceneEvent> events;
            {
                const std::lock_guard lock{s_event_mutex};
                events.swap(s_pending_events);
            }
            if (events.empty() || !s_lua_registered)
            {
                return;
            }
            const std::uintptr_t scripts = read_ptr(s_anchors.genv + constants::GENV_SCRIPT_SYSTEM_OFFSET);
            if (scripts == 0 || read_ptr(scripts) != s_anchors.script_system_vtable)
            {
                return;
            }
            try
            {
                for (const SceneEvent &event : events)
                {
                    const std::string code = std::format(
                        "local C = AnimationFramework and AnimationFramework.Cutscene "
                        "if C and C._TrackEvent then C._TrackEvent({}, {}, {}) end",
                        lua_quoted(event.sequence),
                        lua_quoted(event.event),
                        lua_quoted(event.param)
                    );
                    (void)vcall<bool>(
                        reinterpret_cast<void *>(scripts),
                        constants::SCRIPT_EXECUTE_BUFFER_SLOT,
                        code.c_str(),
                        code.size(),
                        CUTSCENE_LUA_CHUNK,
                        static_cast<void *>(nullptr)
                    );
                }
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Cutscenes: track events did not reach Lua");
            }
        }

        /** @brief Drops the references of finished cutscenes and publishes the holders that still run. */
        void track_started() noexcept
        {
            for (std::size_t i = s_started.size(); i-- > 0;)
            {
                if (use_count(s_started[i].configuration) <= 1)
                {
                    release(s_started[i].configuration);
                    s_started.erase(s_started.begin() + static_cast<std::ptrdiff_t>(i));
                }
            }
            s_scenes_running.store(!s_started.empty(), std::memory_order_release);
            if (s_playing_cvar == nullptr && !s_playing_cvar_failed)
            {
                bool registered = false;
                s_playing_cvar = find_or_register_string_cvar(PLAYING_CVAR, PLAYING_CVAR_HELP, registered);
                s_playing_cvar_failed = s_playing_cvar == nullptr;
                if (s_playing_cvar_failed)
                {
                    DMK::log().warning("Cutscenes: {} could not be registered", PLAYING_CVAR);
                }
            }
            if (s_playing_cvar == nullptr)
            {
                return;
            }
            try
            {
                std::string playing;
                for (const StartedCutscene &started : s_started)
                {
                    playing += playing.empty() ? started.holder : " " + started.holder;
                }
                if (cvar_text(s_playing_cvar) != playing)
                {
                    set_cvar_text(s_playing_cvar, playing);
                }
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "Cutscenes: the playing holders were not published"
                );
            }
        }

        /** @brief The live profile manager when its vtable is C_GameProfileManager's, else nullptr. */
        [[nodiscard]] void *profile_manager() noexcept
        {
            const std::uintptr_t game = reinterpret_cast<GameGetterFunc>(s_anchors.profile_game_getter)();
            const std::uintptr_t outer = game != 0 ? read_ptr(game + s_anchors.profile_manager_outer_offset) : 0;
            const std::uintptr_t manager = outer != 0 ? read_ptr(outer + s_anchors.profile_manager_inner_offset) : 0;
            return manager != 0 && read_ptr(manager) == s_anchors.profile_manager_vtable
                       ? reinterpret_cast<void *>(manager)
                       : nullptr;
        }

        /** @brief CRC32 of @p text lowercased, as the level loader keys a profile record. */
        [[nodiscard]] std::uint32_t profile_name_crc(std::string_view text) noexcept
        {
            std::uint32_t crc = 0xFFFFFFFFu;
            for (char c : text)
            {
                auto byte = static_cast<std::uint8_t>(c);
                if (byte >= 'A' && byte <= 'Z')
                {
                    byte = static_cast<std::uint8_t>(byte + 32);
                }
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit)
                {
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
                }
            }
            return ~crc;
        }

        // CEntitySystem's id allocator (see anchors.hpp): an id is salt << 18 | slot index; a slot is 24 bytes, its
        // salt at +0, next link at +8 and entity at +16; index 0 ends a list. The lock is a spin word the game takes
        // with cmpxchg 0 -> 0x10000 and drops by adding -0x10000.
        constexpr std::uint32_t ENTITY_ID_INDEX_BITS = 18;
        constexpr std::uint32_t ENTITY_ID_MAX_INDEX = 0x3FFFD;
        constexpr std::uintptr_t ENTITY_ID_SLOT_SIZE = 24;
        constexpr std::uintptr_t ENTITY_ID_SLOT_NEXT = 8;
        constexpr std::uintptr_t ENTITY_ID_SLOT_ENTITY = 16;
        constexpr std::uint32_t ENTITY_ID_LOCK_WRITER = 0x10000;
        // The free ids the game hands out next stay with the game: the lock drops between the walk and the reservation.
        constexpr std::size_t ENTITY_ID_FREE_SKIP = 256;
        constexpr int MAX_LAYER_DEPTH = 64;

        /** @brief A streamed layer animfw_scene_layer added. Its file numbers its entities 1..N. */
        struct SceneLayer
        {
            std::string name;
            /// `Levels/<level>/layers/<name>.xml`, fixed when the command runs, on the main thread.
            std::string file;
        };
        // The command adds on the main thread, the id hook reads on the thread that loads the layer.
        std::mutex s_scene_layers_mutex;
        std::vector<SceneLayer> s_scene_layers;

        using ReserveLayerIdsFunc = bool(__fastcall *)(void *load_manager, void **entities);
        std::atomic<ReserveLayerIdsFunc> s_reserve_original{nullptr};

        /** @brief The file of scene layer @p layer, or empty when animfw_scene_layer did not add it. */
        [[nodiscard]] std::string scene_layer_file(std::string_view layer)
        {
            const std::lock_guard lock{s_scene_layers_mutex};
            const auto found = std::ranges::find(s_scene_layers, layer, &SceneLayer::name);
            return found != s_scene_layers.end() ? found->file : std::string{};
        }

        /** @brief Adds or updates scene layer @p layer of level @p level. */
        void add_scene_layer(const std::string &layer, std::string_view level)
        {
            std::string file = std::format("Levels/{}/layers/{}.xml", level, layer);
            const std::lock_guard lock{s_scene_layers_mutex};
            const auto found = std::ranges::find(s_scene_layers, layer, &SceneLayer::name);
            if (found != s_scene_layers.end())
            {
                found->file = std::move(file);
                return;
            }
            s_scene_layers.push_back(
                SceneLayer{
                    .name = layer,
                    .file = std::move(file),
                }
            );
        }

        /**
         * @brief @p count ids of free slots in the entity system's allocator, past the ones the game takes next, each
         *        with the salt its slot holds. Empty when the free list is shorter.
         */
        [[nodiscard]] std::vector<std::uint32_t> take_free_entity_ids(std::uintptr_t system, std::size_t count)
        {
            std::vector<std::uint32_t> ids;
            ids.reserve(count);
            const std::uintptr_t lock_word = system + s_anchors.entity_id_lock_offset;
            if (!read_value<std::uint32_t>(lock_word).has_value())
            {
                return ids;
            }
            std::atomic_ref<std::uint32_t> lock{*reinterpret_cast<std::uint32_t *>(lock_word)};
            for (std::uint32_t expected = 0; !lock.compare_exchange_weak(expected, ENTITY_ID_LOCK_WRITER); expected = 0)
            {
                std::this_thread::yield();
            }
            const std::uintptr_t slots = system + s_anchors.entity_id_slots_offset;
            std::uint32_t index = read_value<std::uint32_t>(slots + s_anchors.entity_id_free_head_offset).value_or(0);
            bool faulted = false;
            for (std::size_t seen = 0;
                 index != 0 && index <= ENTITY_ID_MAX_INDEX && ids.size() < count && seen <= ENTITY_ID_MAX_INDEX;
                 ++seen)
            {
                const std::uintptr_t slot = slots + index * ENTITY_ID_SLOT_SIZE;
                const std::optional<std::uint16_t> salt = read_value<std::uint16_t>(slot);
                const std::optional<std::uintptr_t> entity = read_value<std::uintptr_t>(slot + ENTITY_ID_SLOT_ENTITY);
                const std::optional<std::uint32_t> next = read_value<std::uint32_t>(slot + ENTITY_ID_SLOT_NEXT);
                if (!salt.has_value() || !entity.has_value() || !next.has_value())
                {
                    faulted = true;
                    break;
                }
                if (seen >= ENTITY_ID_FREE_SKIP && *entity == 0)
                {
                    ids.push_back((std::uint32_t{*salt} << ENTITY_ID_INDEX_BITS) | index);
                }
                index = *next;
            }
            lock.fetch_sub(ENTITY_ID_LOCK_WRITER);
            if (faulted || ids.size() < count)
            {
                ids.clear();
            }
            return ids;
        }

        /// Attribute @p key of @p node as a whole unsigned number, or nothing.
        [[nodiscard]] std::optional<std::uint32_t> id_attr(void *node, const char *key) noexcept
        {
            const std::string_view text = attr_of(node, key);
            std::uint32_t value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return !text.empty() && error == std::errc{} && end == text.data() + text.size()
                       ? std::optional<std::uint32_t>{value}
                       : std::nullopt;
        }

        /// Swaps @p node's @p key from a file id to the id the scene got for it.
        void remap_id_attr(void *node, const char *key, const std::unordered_map<std::uint32_t, std::uint32_t> &ids)
        {
            const std::optional<std::uint32_t> local = id_attr(node, key);
            const auto found = local.has_value() ? ids.find(*local) : ids.end();
            if (found != ids.end())
            {
                vcall<void>(node, XML_NODE_SET_ATTR_SLOT, key, std::to_string(found->second).c_str());
            }
        }

        /**
         * @brief Every entity reference in @p node's subtree: an entity's own id and parent, its links' targets, its
         *        area's entities and its sequence's cleanup areas.
         */
        void remap_entity_ids(
            void *node,
            std::string_view parent_tag,
            const std::unordered_map<std::uint32_t, std::uint32_t> &ids,
            int depth
        )
        {
            const std::string tag{tag_of(node)};
            if (tag == "Entity")
            {
                remap_id_attr(node, "EntityId", ids);
                remap_id_attr(node, "ParentId", ids);
                if (parent_tag == "Entities")
                {
                    remap_id_attr(node, "Id", ids);
                }
            }
            else if (tag == "Link")
            {
                remap_id_attr(node, "TargetId", ids);
            }
            else if (tag == "CleanupArea")
            {
                remap_id_attr(node, "ID", ids);
            }
            if (depth >= MAX_LAYER_DEPTH)
            {
                return;
            }
            const int count = child_count(node);
            for (int i = 0; i < count; ++i)
            {
                XmlRef child = child_at(node, i);
                if (child)
                {
                    remap_entity_ids(child.get(), tag, ids, depth + 1);
                }
            }
        }

        /**
         * @brief The layer file @p file again as a writable tree: the streaming loader parses layers read-only
         *        (CXMLReadOnlyNode ignores setAttr). Null when the file is missing.
         */
        [[nodiscard]] XmlRef load_writable_layer(const std::string &file)
        {
            XmlRef root;
            void *movie = movie_system();
            void *utils = movie != nullptr ? xml_utils(movie) : nullptr;
            if (utils != nullptr)
            {
                // LoadXmlFromFile(out, name, patch, clean, read-only, reuse).
                vcall<void **>(utils, XML_UTILS_LOAD_FILE_SLOT, root.out(), file.c_str(), false, true, false, false);
            }
            return root;
        }

        /**
         * @brief A scene layer's file numbers its entities 1..N, ids the level never reserved. Before the layer's first
         *        slice reserves and spawns them, the layer's entities node (which the layer keeps for every slice) is
         *        swapped for a writable copy of its file whose ids, and every reference to them, are free ids of the
         *        running game.
         */
        void remap_scene_layer(void **entities_ref)
        {
            void *entities = *entities_ref;
            std::string layer;
            std::string file;
            std::vector<std::uint32_t> locals;
            const int count = child_count(entities);
            for (int i = 0; i < count; ++i)
            {
                XmlRef child = child_at(entities, i);
                if (!child || tag_of(child.get()) != "Entity")
                {
                    continue;
                }
                if (layer.empty())
                {
                    layer = attr_of(child.get(), "Layer");
                    file = layer.empty() ? std::string{} : scene_layer_file(layer);
                    if (file.empty())
                    {
                        return;
                    }
                }
                if (const std::optional<std::uint32_t> id = id_attr(child.get(), "EntityId"); id.has_value())
                {
                    locals.push_back(*id);
                }
            }
            void *system = entity_system();
            if (locals.empty() || system == nullptr)
            {
                return;
            }
            XmlRef writable = load_writable_layer(file);
            if (!writable || tag_of(writable.get()) != tag_of(entities) || child_count(writable.get()) != count)
            {
                DMK::log().warning(
                    "Cutscenes: scene layer '{}' did not load again as a writable file; its ids stay",
                    layer
                );
                return;
            }
            const std::vector<std::uint32_t> taken =
                take_free_entity_ids(reinterpret_cast<std::uintptr_t>(system), locals.size());
            if (taken.empty())
            {
                DMK::log().warning("Cutscenes: no {} free entity ids for scene layer '{}'", locals.size(), layer);
                return;
            }
            std::unordered_map<std::uint32_t, std::uint32_t> ids;
            for (std::size_t i = 0; i < locals.size(); ++i)
            {
                ids.emplace(locals[i], taken[i]);
            }
            for (int i = 0; i < count; ++i)
            {
                XmlRef child = child_at(writable.get(), i);
                if (child)
                {
                    remap_entity_ids(child.get(), "", ids, 0);
                }
            }
            // The layer holds one reference to its entities node: hand it the writable tree's and drop the old one.
            *entities_ref = writable.release();
            vcall<void>(entities, XML_NODE_RELEASE_SLOT);
            DMK::log().info(
                "Cutscenes: scene layer '{}' takes {} free entity ids ({:#x}..)",
                layer,
                ids.size(),
                taken.front()
            );
        }

        bool __fastcall detour_reserve_layer_ids(void *load_manager, void **entities) noexcept
        {
            try
            {
                if (entities != nullptr && *entities != nullptr && s_bound.load(std::memory_order_acquire))
                {
                    remap_scene_layer(entities);
                }
            }
            catch (...)
            {
                (void)DMK::log().log_noexcept(
                    DMK::LogLevel::Error,
                    "Cutscenes: a scene layer's ids were not moved; it loads with its file's ids"
                );
            }
            const ReserveLayerIdsFunc original = s_reserve_original.load(std::memory_order_acquire);
            return original != nullptr && original(load_manager, entities);
        }

        /**
         * @brief The scene layer command: AddLayer (streamed), AddEntityToLayer for each named entity, then EnableLayer
         *        off, so the cutscene's game profile switches the layer on and its end switches it off again.
         * @details The layer is always streamed: a layer added at run time without streaming makes every later save
         *          load hang. Its file is fixed here, on the main thread, for the id hook to reload it.
         */
        void scene_layer(void *args)
        {
            const int count = command_argument_count(args);
            if (count < 3)
            {
                print(std::format("usage: {} <layer> <id> [entity...]", SCENE_LAYER_COMMAND));
                return;
            }
            void *system = entity_system();
            if (system == nullptr)
            {
                print("the entity system is not up");
                return;
            }
            const std::string layer{command_argument(args, 1)};
            const std::string_view id_text = command_argument(args, 2);
            int id = 0;
            const auto [id_end, id_error] = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
            if (id_error != std::errc{} || id_end != id_text.data() + id_text.size() || id <= 0 || id > MAX_LAYER_ID)
            {
                print(std::format("the layer id must be 1..{}", MAX_LAYER_ID));
                return;
            }
            void *map = find_cvar("sv_map");
            const std::string_view level = map != nullptr ? cvar_text(map) : std::string_view{};
            if (level.empty())
            {
                print("no level is loaded");
                return;
            }
            if (vcall<void *>(system, ENTITY_SYSTEM_FIND_LAYER_SLOT, layer.c_str()) == nullptr)
            {
                // name, parent, id, physics, specs, default loaded, streamed, last flag (LoadLayers passes 0).
                void *added = vcall<void *>(
                    system,
                    ENTITY_SYSTEM_ADD_LAYER_SLOT,
                    layer.c_str(),
                    "",
                    static_cast<std::uint16_t>(id),
                    true,
                    -1,
                    false,
                    true,
                    false
                );
                if (added == nullptr)
                {
                    print(std::format("the entity system refused layer '{}' (id {})", layer, id));
                    return;
                }
                print(std::format("added streamed layer '{}' (id {})", layer, id));
            }
            add_scene_layer(layer, level);
            for (int i = 3; i < count; ++i)
            {
                const std::string name{command_argument(args, i)};
                void *entity = find_entity(name);
                if (entity == nullptr)
                {
                    print(std::format("'{}' is not a live entity", name));
                    continue;
                }
                // Retail AddEntityToLayer takes the entity itself (the reference engine takes its id).
                vcall<void>(system, ENTITY_SYSTEM_ADD_TO_LAYER_SLOT, layer.c_str(), entity);
                print(std::format("'{}' is in layer '{}'", name, layer));
            }
            vcall<void>(system, ENTITY_SYSTEM_ENABLE_LAYER_SLOT, layer.c_str(), false, false);
        }

        /// The scene profile command: a profile record over the given layers, made as the level loader makes one.
        void scene_profile(void *args)
        {
            const int count = command_argument_count(args);
            if (count < 3)
            {
                print(std::format("usage: {} <profile> <layer> [layer...]", SCENE_PROFILE_COMMAND));
                return;
            }
            void *manager = profile_manager();
            if (manager == nullptr)
            {
                print("the game profile manager is not up");
                return;
            }
            const std::string name{command_argument(args, 1)};
            const char *key = name.c_str();
            if (vcall<bool>(manager, PROFILE_MANAGER_EXISTS_SLOT, &key))
            {
                print(std::format("profile '{}' already exists", name));
                return;
            }
            // A std::vector of record pointers: begin, then end.
            const auto records = reinterpret_cast<std::uintptr_t>(manager) + PROFILE_RECORDS_OFFSET;
            const std::uintptr_t records_end = records + sizeof(std::uintptr_t);
            std::uint16_t next_id = 0;
            for (std::uintptr_t at = read_ptr(records); at != 0 && at < read_ptr(records_end);
                 at += sizeof(std::uintptr_t))
            {
                const std::uint16_t id = read_value<std::uint16_t>(read_ptr(at) + PROFILE_ID_OFFSET).value_or(0);
                if (id != 0xFFFF && id >= next_id)
                {
                    next_id = static_cast<std::uint16_t>(id + 1);
                }
            }
            void *record = nullptr;
            reinterpret_cast<RecordCreateFunc>(s_anchors.profile_record_create)(&record, nullptr);
            if (record == nullptr)
            {
                print("the profile record was not made");
                return;
            }
            const auto make_string = reinterpret_cast<StringFromTextFunc>(s_anchors.string_from_text);
            const auto release_string = reinterpret_cast<StringReleaseFunc>(s_anchors.string_release);
            auto *bytes = static_cast<std::byte *>(record);
            void *text = nullptr;
            make_string(&text, key);
            std::swap(*reinterpret_cast<void **>(bytes), text);
            release_string(&text);
            // HasNavigation, DoesPrepareSOAnimData, AllowRuntimeAnimValidationFallback, NeedsSVORebuild: all off.
            std::memset(bytes + PROFILE_FLAGS_OFFSET, 0, PROFILE_FLAG_COUNT);
            *reinterpret_cast<std::uint16_t *>(bytes + PROFILE_ID_OFFSET) = next_id;
            *reinterpret_cast<std::uint32_t *>(bytes + PROFILE_NAME_CRC_OFFSET) = profile_name_crc(name);
            for (int i = 2; i < count; ++i)
            {
                const std::string layer{command_argument(args, i)};
                void *layer_name = nullptr;
                make_string(&layer_name, layer.c_str());
                reinterpret_cast<LayerPushFunc>(s_anchors.profile_layer_push)(
                    bytes + PROFILE_LAYERS_OFFSET,
                    &layer_name
                );
                release_string(&layer_name);
            }
            void *holder = record;
            reinterpret_cast<RecordsInsertFunc>(s_anchors.profile_records_insert)(
                reinterpret_cast<void *>(records),
                reinterpret_cast<void *>(read_ptr(records_end)),
                &holder
            );
            // The manager's own lookup is what IngameCutsceneData asks when the cutscene starts.
            if (!vcall<bool>(manager, PROFILE_MANAGER_EXISTS_SLOT, &key))
            {
                print(std::format("profile '{}' was added but the profile manager does not find it", name));
                return;
            }
            print(std::format("profile '{}' (id {}) added over {} layer(s)", name, next_id, count - 2));
        }
    } // namespace

    void bind_cutscenes(const Anchors &anchors) noexcept
    {
        if (anchors.genv == 0 || anchors.movie_system_vtable == 0 || anchors.system_vtable == 0)
        {
            DMK::log().warning("Cutscenes: the movie system was not found; the cutscene commands are off");
            return;
        }
        s_anchors = anchors;
        s_bound.store(true, std::memory_order_release);
    }

    void run_cutscene_frame_work() noexcept
    {
        if (!s_bound.load(std::memory_order_acquire) || !console_ready())
        {
            return;
        }
        if (s_play_added)
        {
            track_started();
            register_cutscene_lua();
            dispatch_scene_events();
        }
        if (s_added)
        {
            return;
        }
        add_console_command(LOAD_COMMAND, &guarded_command<&load, COMMAND_FAILED>, LOAD_HELP);
        s_added = true;
        if (s_anchors.cutscene_create != 0)
        {
            add_console_command(PLAY_COMMAND, &guarded_command<&play, PLAY_FAILED>, PLAY_HELP);
            s_play_added = true;
        }
        if (s_anchors.linkable_make != 0)
        {
            add_console_command(LINKABLE_COMMAND, &guarded_command<&make_linkable, LINKABLE_FAILED>, LINKABLE_HELP);
            s_linkable_added = true;
        }
        if (s_anchors.profile_record_create != 0)
        {
            add_console_command(
                SCENE_LAYER_COMMAND,
                &guarded_command<&scene_layer, SCENE_LAYER_FAILED>,
                SCENE_LAYER_HELP
            );
            add_console_command(
                SCENE_PROFILE_COMMAND,
                &guarded_command<&scene_profile, SCENE_PROFILE_FAILED>,
                SCENE_PROFILE_HELP
            );
            s_scene_added = true;
        }
    }

    void install_scene_event_hook(const Anchors &anchors, DMK::hook::HookStack &hooks) noexcept
    {
        if (anchors.trigger_track_event == 0)
        {
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Warning,
                "Cutscenes: TriggerTrackEvent was not found; cutscene events do not reach Lua"
            );
            return;
        }
        try
        {
            auto installed = install_inline_hook<&detour_trigger_track_event>(
                hooks,
                "TriggerTrackEvent",
                anchors.trigger_track_event,
                s_trigger_original
            );
            if (!installed.has_value())
            {
                DMK::log().warning("Cutscenes: TriggerTrackEvent hook failed ({})", installed.error().message());
                return;
            }
            DMK::log().info("Cutscenes: cutscene events reach AnimationFramework.Cutscene");
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Error,
                "Cutscenes: installing the TriggerTrackEvent hook failed"
            );
        }
    }

    void install_scene_layer_id_hook(const Anchors &anchors, DMK::hook::HookStack &hooks) noexcept
    {
        if (anchors.reserve_layer_ids == 0)
        {
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Warning,
                "Cutscenes: ReserveEntityIds was not found; scene layers keep their file's entity ids"
            );
            return;
        }
        try
        {
            auto installed = install_inline_hook<&detour_reserve_layer_ids>(
                hooks,
                "ReserveEntityIds",
                anchors.reserve_layer_ids,
                s_reserve_original
            );
            if (!installed.has_value())
            {
                DMK::log().warning("Cutscenes: ReserveEntityIds hook failed ({})", installed.error().message());
                return;
            }
            DMK::log().info("Cutscenes: scene layers take free entity ids as they stream");
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(
                DMK::LogLevel::Error,
                "Cutscenes: installing the ReserveEntityIds hook failed"
            );
        }
    }

    void remove_cutscene_console() noexcept
    {
        if (s_added)
        {
            remove_console_command(LOAD_COMMAND);
        }
        if (s_play_added)
        {
            remove_console_command(PLAY_COMMAND);
        }
        if (s_linkable_added)
        {
            remove_console_command(LINKABLE_COMMAND);
        }
        if (s_scene_added)
        {
            remove_console_command(SCENE_LAYER_COMMAND);
            remove_console_command(SCENE_PROFILE_COMMAND);
        }
        s_added = false;
        s_play_added = false;
        s_linkable_added = false;
        s_scene_added = false;
        // The player holds its own reference to every running cutscene, so dropping ours here frees nothing under it,
        // and a reload of the framework does not leak one configuration per scene.
        for (const StartedCutscene &started : s_started)
        {
            release(started.configuration);
        }
        s_started.clear();
        s_scenes_running.store(false, std::memory_order_release);
    }
} // namespace AnimationFramework
