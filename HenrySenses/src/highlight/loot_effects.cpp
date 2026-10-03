/**
 * @file highlight/loot_effects.cpp
 * @brief Particle effects shown on highlighted containers: emitters loaded into entity slots, freed when proven owned.
 */

#include "highlight/loot_effects.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "rtti_types.hpp"
#include "engine/engine_env.hpp"
#include "engine/entity_access.hpp"
#include "engine/seh.hpp"
#include "highlight/groups.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        using FindEffectFn = std::uintptr_t(__fastcall *)(
            std::uintptr_t manager,
            const char *name,
            const char *source,
            bool load_resources
        );
        // CRenderProxy::LoadParticleEmitter(this, slot, effect, const SpawnParams *, bPrime, bSerialize) -> slot.
        using LoadParticleEmitterFn = int(__fastcall *)(
            std::uintptr_t proxy,
            int slot,
            std::uintptr_t effect,
            const void *spawn_params,
            bool prime,
            bool serialize
        );
        using SetSlotLocalTmFn =
            bool(__fastcall *)(std::uintptr_t proxy, int slot, const game_structures::Matrix34f *local_tm);
        using FreeSlotFn = void(__fastcall *)(std::uintptr_t proxy, int slot);
        // IParticleManager::LoadLibrary(this, name, XmlNodeRef &library, bLoadResources).
        using LoadLibraryFn =
            bool(__fastcall *)(std::uintptr_t manager, const char *name, std::uintptr_t *library, bool load_resources);
        // CXmlUtils::LoadXmlFromBuffer(this, XmlNodeRef *out, buffer, size, bReuseStrings, bSuppressWarnings).
        using LoadXmlFromBufferFn = std::uintptr_t *(__fastcall *)(std::uintptr_t xml_utils,
                                                                   std::uintptr_t *out,
                                                                   const char *buffer,
                                                                   std::size_t size,
                                                                   bool reuse_strings,
                                                                   bool suppress_warnings);
        using XmlReleaseFn = void(__fastcall *)(std::uintptr_t node);

        /**
         * @struct QuatTs
         * @brief Engine QuatTS / ParticleLoc: rotation (x, y, z, w), position, uniform scale.
         */
        struct QuatTs
        {
            float qx{0.0f};
            float qy{0.0f};
            float qz{0.0f};
            float qw{1.0f};
            game_structures::Vec3f position{};
            float scale{1.0f};
        };
        static_assert(sizeof(QuatTs) == 32, "QuatTs must match the engine's QuatTS.");

        // CParticleManager::CreateEmitter(this, const ParticleLoc &, const IParticleEffect *, const SpawnParams *).
        using CreateEmitterFn = std::uintptr_t(__fastcall *)(
            std::uintptr_t manager,
            const QuatTs *location,
            std::uintptr_t effect,
            const void *spawn_params
        );
        using KillFn = void(__fastcall *)(std::uintptr_t emitter);

        /// The source FindEffect names in the game log when an effect is missing.
        constexpr const char *EFFECT_SOURCE = "HenrySenses";
        /// A load not seen in its slot after this long is treated as run (the queue runs every frame).
        constexpr std::int64_t LOAD_GRACE_MS = 2000;
        /// How often the slot emitters are proven still in place between batches.
        constexpr std::int64_t VALIDATE_INTERVAL_MS = 500;
        /// How often the free-standing emitters are looked up in the manager's list (a walk of ~11k emitters).
        constexpr std::int64_t FREE_VALIDATE_INTERVAL_MS = 2000;
        /// The most emitters a walk of the manager's list reads (a level holds ~11k).
        constexpr std::size_t MAX_LISTED_EMITTERS = std::size_t{1} << 17;
        /// A free emitter's rotation: +90 degrees about X, so its Y axis (the pfx1 emit direction) points up.
        constexpr float HALF_SQRT2 = 0.70710678f;
        /// New emitters per batch; the rest follow in the next batches (a scan every ScanIntervalMs).
        constexpr std::size_t MAX_SPAWNS_PER_SYNC = 16;
        /// Below this share of its group's fade the effect is released while the outline finishes fading.
        constexpr float MIN_INTENSITY = 0.3f;
        /// A render proxy with more slots than this is not trusted.
        constexpr std::size_t MAX_SLOTS = 64;
        /// The largest particle library file read.
        constexpr std::uintmax_t MAX_LIBRARY_BYTES = 4u << 20;

        /**
         * @struct EffectLibrary
         * @brief The optional particle library beside the ASI (KCD2_HenrySenses.particles.xml).
         * @details Parsed into a CXmlNode tree the mod holds one reference to, and registered with the particle
         *          manager before every batch that loads one of its effects: a level unload drops the manager's
         *          libraries and effects, and FindEffect for a name of an unregistered library would look for it in
         *          the paks and keep a disabled placeholder under that name for the rest of the session.
         */
        struct EffectLibrary
        {
            /// The parsed root node (one reference held), 0 without a usable file.
            std::uintptr_t node{0};
            /// Write time of the file last read (parsed or refused), so an unchanged file is not read again.
            std::filesystem::file_time_type stamp{};
            bool read{false};
            /// The last file state logged: 0 unknown, 1 missing, 2 loaded, 3 unreadable or not parsed.
            int logged_state{0};
        };

        EffectLibrary s_library{};

        /**
         * @struct Attached
         * @brief One emitter the mod created: in an entity slot, or free-standing.
         */
        struct Attached
        {
            EffectChannel channel{EffectChannel::Loot};
            /// Identity within the channel (the entity id for the loot channel).
            std::uint64_t key{0};
            /// Free-standing (the particle manager owns it) rather than in an entity slot.
            bool free{false};
            EntityId entity_id{0};
            /// The CEntity and its render proxy when loaded; compared, never cached for calls.
            std::uintptr_t entity{0};
            std::uintptr_t node{0};
            int slot{-1};
            /**
             * @brief The CParticleEmitter (for a slot, once seen: 0 while its load is still queued); compared, and
             * called only after proving it is still the mod's.
             */
            std::uintptr_t emitter{0};
            /// A free emitter's location, its identity in the manager's list.
            QuatTs location{};
            std::uint16_t effect{0};
            float scale{1.0f};
            std::int64_t loaded_ms{0};
        };

        /**
         * @struct Wanted
         * @brief One object of the latest batch that should show an effect.
         */
        struct Wanted
        {
            std::uint64_t key{0};
            /// The host entity (loot channel), or 0 for a free emitter on @ref bounds.
            EntityId entity_id{0};
            game_structures::Aabb bounds{};
            std::uint16_t effect{0};
            float scale{1.0f};
        };

        /**
         * @struct LiveEmitters
         * @brief The particle manager's live emitters, read once per public call and only when a free emitter needs
         *        proving.
         */
        struct LiveEmitters
        {
            std::vector<std::uintptr_t> sorted{};
            std::size_t count{0};
            /// The list was read this call.
            bool read{false};
            /// The whole list was read (a fault or a runaway list leaves every free emitter unproven).
            bool complete{false};
        };

        /**
         * @struct EffectStats
         * @brief Counters since load, for the state report.
         */
        struct EffectStats
        {
            std::size_t loaded{0};
            std::size_t freed{0};
            /// Emitters that went with their entity or proxy (nothing to free).
            std::size_t lost{0};
            std::size_t failed{0};
        };

        std::vector<Attached> s_attached;
        /// Slot emitters released before their emitter showed up: freed once it does or the grace period ends.
        std::vector<Attached> s_retiring;
        std::vector<Wanted> s_wanted;
        /// Effects FindEffect did not return, warned about once each.
        std::vector<std::uint16_t> s_missing;
        EffectStats s_stats{};
        std::int64_t s_next_validate_ms{0};
        std::int64_t s_next_free_validate_ms{0};
        bool s_gate_logged{false};
        bool s_world_gate_logged{false};
        LiveEmitters s_live{};

        /** @brief Steady-clock milliseconds. */
        [[nodiscard]] std::int64_t steady_ms() noexcept
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
            )
                .count();
        }

        // Guarded engine calls. Each takes only trivially destructible arguments (__try forbids unwinding).

        [[nodiscard]] std::uintptr_t
        call_find_effect(std::uintptr_t fn, std::uintptr_t manager, const char *name) noexcept
        {
            __try
            {
                return reinterpret_cast<FindEffectFn>(fn)(manager, name, EFFECT_SOURCE, true);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        [[nodiscard]] int call_load_emitter(std::uintptr_t fn, std::uintptr_t node, std::uintptr_t effect) noexcept
        {
            __try
            {
                // A new slot (-1), default spawn params, primed so the effect shows fully at once, not serialized.
                return reinterpret_cast<LoadParticleEmitterFn>(fn)(node, -1, effect, nullptr, true, false);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return -1;
            }
        }

        [[nodiscard]] bool call_set_slot_tm(
            std::uintptr_t fn,
            std::uintptr_t node,
            int slot,
            const game_structures::Matrix34f *local_tm
        ) noexcept
        {
            __try
            {
                (void)reinterpret_cast<SetSlotLocalTmFn>(fn)(node, slot, local_tm);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] bool call_free_slot(std::uintptr_t fn, std::uintptr_t node, int slot) noexcept
        {
            __try
            {
                reinterpret_cast<FreeSlotFn>(fn)(node, slot);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] bool
        call_load_library(std::uintptr_t fn, std::uintptr_t manager, const char *name, std::uintptr_t *library) noexcept
        {
            __try
            {
                return reinterpret_cast<LoadLibraryFn>(fn)(manager, name, library, true);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /** @brief Returns the parsed root node with one caller-owned reference, or 0. */
        [[nodiscard]] std::uintptr_t
        call_parse_xml(std::uintptr_t fn, std::uintptr_t xml_utils, const char *buffer, std::size_t size) noexcept
        {
            std::uintptr_t root = 0;
            __try
            {
                (void)reinterpret_cast<LoadXmlFromBufferFn>(fn)(xml_utils, &root, buffer, size, false, false);
                return root;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        void call_release_xml(std::uintptr_t fn, std::uintptr_t node) noexcept
        {
            __try
            {
                reinterpret_cast<XmlReleaseFn>(fn)(node);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
            }
        }

        [[nodiscard]] std::uintptr_t call_create_emitter(
            std::uintptr_t fn,
            std::uintptr_t manager,
            const QuatTs *location,
            std::uintptr_t effect
        ) noexcept
        {
            __try
            {
                // Default spawn params: the manager's list holds the emitter's only reference.
                return reinterpret_cast<CreateEmitterFn>(fn)(manager, location, effect, nullptr);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        [[nodiscard]] bool call_kill(std::uintptr_t fn, std::uintptr_t emitter) noexcept
        {
            __try
            {
                reinterpret_cast<KillFn>(fn)(emitter);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Copies the manager's live emitter list into @p out.
         * @details The list is only changed by the particle manager's update on the main thread, the thread this runs
         *          on, so one guarded pass reads it consistently.
         * @return True when the whole list was read (it ended within @p capacity entries and the listed count).
         */
        [[nodiscard]] bool walk_emitter_list(
            std::uintptr_t manager,
            std::uintptr_t *out,
            std::size_t capacity,
            std::size_t &count
        ) noexcept
        {
            count = 0;
            __try
            {
                const std::uintptr_t list = manager + constants::PARTICLE_MANAGER_EMITTERS_OFFSET;
                const std::size_t listed =
                    *reinterpret_cast<const std::uint32_t *>(list + constants::PARTICLE_EMITTER_LIST_COUNT_OFFSET);
                std::uintptr_t node = *reinterpret_cast<const std::uintptr_t *>(list);
                while (node != 0 && count < capacity && count <= listed)
                {
                    out[count++] = node;
                    node = *reinterpret_cast<const std::uintptr_t *>(node + constants::PARTICLE_EMITTER_NEXT_OFFSET);
                }
                return node == 0;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /** @brief CParticleManager's primary base, RTTI-checked, or 0. */
        [[nodiscard]] std::uintptr_t particle_manager_primary() noexcept
        {
            const std::uintptr_t manager = genv_interface(constants::GENV_PARTICLE_MANAGER_OFFSET);
            if (manager <= static_cast<std::uintptr_t>(constants::PARTICLE_MANAGER_PRIMARY_DELTA))
            {
                return 0;
            }
            const std::uintptr_t primary = manager - constants::PARTICLE_MANAGER_PRIMARY_DELTA;
            return object_is(GameClass::ParticleManager, primary) ? primary : 0;
        }

        /** @brief Reads the live emitter list once per public call. */
        [[nodiscard]] const LiveEmitters &live_emitters()
        {
            if (s_live.read)
            {
                return s_live;
            }
            s_live.read = true;
            s_live.complete = false;
            s_live.count = 0;
            const std::uintptr_t manager = particle_manager_primary();
            if (manager == 0)
            {
                return s_live;
            }
            if (s_live.sorted.size() < MAX_LISTED_EMITTERS)
            {
                s_live.sorted.resize(MAX_LISTED_EMITTERS);
            }
            s_live.complete = walk_emitter_list(manager, s_live.sorted.data(), s_live.sorted.size(), s_live.count);
            std::sort(s_live.sorted.begin(), s_live.sorted.begin() + static_cast<std::ptrdiff_t>(s_live.count));
            return s_live;
        }

        /** @brief Marks the live list stale (at the start of every public call). */
        void begin_call() noexcept
        {
            s_live.read = false;
        }

        /**
         * @brief True when a free emitter is still the mod's: listed by the manager, a CParticleEmitter, at the
         *        location it was created at.
         * @return std::nullopt when the list could not be read in full (nothing is decided then).
         */
        [[nodiscard]] std::optional<bool> free_emitter_alive(const Attached &entry)
        {
            const LiveEmitters &live = live_emitters();
            if (!live.complete)
            {
                return std::nullopt;
            }
            const auto end = live.sorted.begin() + static_cast<std::ptrdiff_t>(live.count);
            if (!std::binary_search(live.sorted.begin(), end, entry.emitter) ||
                !object_is(GameClass::ParticleEmitter, entry.emitter))
            {
                return false;
            }
            // Position x, y, z and scale follow the quaternion's four floats.
            const std::uintptr_t location = entry.emitter + constants::PARTICLE_EMITTER_LOCATION_OFFSET + 16;
            const float expected[4] = {
                entry.location.position.x,
                entry.location.position.y,
                entry.location.position.z,
                entry.location.scale,
            };
            for (std::size_t index = 0; index < 4; ++index)
            {
                const auto value = DMK::memory::read<float>(DMK::Address{location + index * sizeof(float)});
                if (!value || std::fabs(*value - expected[index]) > 1e-3f)
                {
                    return false;
                }
            }
            return true;
        }

        /** @brief True for an effect of the mod's own library ("HenrySenses.<name>"). */
        [[nodiscard]] bool is_library_effect(std::string_view name) noexcept
        {
            const std::string_view prefix = constants::PARTICLE_LIBRARY_NAME;
            if (name.size() <= prefix.size() || name[prefix.size()] != '.')
            {
                return false;
            }
            return std::equal(
                prefix.begin(),
                prefix.end(),
                name.begin(),
                [](char a, char b)
                { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }
            );
        }

        [[nodiscard]] std::filesystem::path library_path()
        {
            return std::filesystem::path(DMK::filesystem::get_runtime_directory()) /
                   (std::string(constants::MOD_NAME) + constants::PARTICLE_LIBRARY_FILE_SUFFIX);
        }

        /** @brief Drops the mod's reference to the parsed library (the particle manager keeps its own). */
        void release_library_node() noexcept
        {
            if (s_library.node == 0)
            {
                return;
            }
            const std::uintptr_t node = s_library.node;
            s_library.node = 0;
            if (object_is(GameClass::XmlNode, node))
            {
                if (const std::uintptr_t release = read_vtable_slot(node, constants::XML_NODE_VTABLE_RELEASE_OFFSET))
                {
                    call_release_xml(release, node);
                }
            }
        }

        /** @brief Logs a change of the library file's state once. */
        void log_library_state(int state, const std::filesystem::path &path, std::string_view detail)
        {
            if (state == s_library.logged_state)
            {
                return;
            }
            s_library.logged_state = state;
            const DMK::LogLevel level = state == 3 ? DMK::LogLevel::Warning : DMK::LogLevel::Info;
            (void)DMK::log().try_log(level, "Effects: {} {}", path.string(), detail);
        }

        /**
         * @brief The top-level effect names of a library text, for the log (a scan of the <Particles> tags, nested
         *        child effects and comments skipped).
         */
        [[nodiscard]] std::string library_effect_names(std::string_view text)
        {
            std::string names;
            int depth = 0;
            for (std::size_t at = text.find("Particles"); at != std::string_view::npos;
                 at = text.find("Particles", at + 1))
            {
                if (at == 0)
                {
                    continue;
                }
                // Inside a comment: the last "<!--" before this point is not closed by a "-->" before it.
                if (const std::size_t open = text.rfind("<!--", at); open != std::string_view::npos)
                {
                    const std::size_t close = text.find("-->", open);
                    if (close == std::string_view::npos || close > at)
                    {
                        continue;
                    }
                }
                const bool closing = at >= 2 && text[at - 1] == '/' && text[at - 2] == '<';
                if (!closing && text[at - 1] != '<')
                {
                    continue;
                }
                const std::size_t end = text.find('>', at);
                if (end == std::string_view::npos)
                {
                    break;
                }
                if (closing)
                {
                    depth = std::max(depth - 1, 0);
                    continue;
                }
                const std::string_view tag = text.substr(at, end - at);
                if (depth == 0)
                {
                    if (const std::size_t name = tag.find("Name=\""); name != std::string_view::npos)
                    {
                        const std::size_t first = name + 6;
                        const std::size_t last = tag.find('"', first);
                        if (last != std::string_view::npos)
                        {
                            names += std::format(
                                "{}{}.{}",
                                names.empty() ? "" : ", ",
                                constants::PARTICLE_LIBRARY_NAME,
                                tag.substr(first, last - first)
                            );
                        }
                    }
                }
                if (tag.empty() || tag.back() != '/')
                {
                    ++depth;
                }
            }
            return names;
        }

        /**
         * @brief Reads the library file again when it appeared, changed or went away.
         * @return True when a parsed library is held.
         */
        [[nodiscard]] bool refresh_library()
        {
            const std::filesystem::path path = library_path();
            std::error_code error;
            const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(path, error);
            if (error)
            {
                release_library_node();
                s_library.read = false;
                log_library_state(1, path, "not found: the HenrySenses.* effects are off");
                return false;
            }
            if (s_library.read && stamp == s_library.stamp)
            {
                return s_library.node != 0;
            }
            s_library.read = true;
            s_library.stamp = stamp;
            // A new version of the file: its outcome is logged whatever the last one was.
            s_library.logged_state = 0;
            release_library_node();

            const std::uintmax_t size = std::filesystem::file_size(path, error);
            if (error || size == 0 || size > MAX_LIBRARY_BYTES)
            {
                log_library_state(3, path, "is empty, too large or unreadable: the HenrySenses.* effects are off");
                return false;
            }
            std::ifstream file(path, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (!file.good() && !file.eof())
            {
                log_library_state(3, path, "could not be read: the HenrySenses.* effects are off");
                return false;
            }

            std::uintptr_t utils = 0;
            if (const std::uintptr_t system = genv_interface(constants::GENV_SYSTEM_OFFSET); system != 0)
            {
                if (const auto value =
                        DMK::memory::read<std::uintptr_t>(DMK::Address{system + constants::SYSTEM_XML_UTILS_OFFSET}))
                {
                    utils = *value;
                }
            }
            const std::uintptr_t parse = object_is(GameClass::XmlUtils, utils)
                                             ? validated_vtable_slot(
                                                   utils,
                                                   constants::XML_UTILS_VTABLE_LOAD_FROM_BUFFER_OFFSET,
                                                   AnchorId::XmlLoadFromBuffer
                                               )
                                             : 0;
            if (parse == 0)
            {
                log_library_state(
                    3,
                    path,
                    "found, but the game's XML parser is unavailable: the HenrySenses.* effects are off"
                );
                return false;
            }
            const std::uintptr_t root = call_parse_xml(parse, utils, text.data(), text.size());
            if (!object_is(GameClass::XmlNode, root))
            {
                log_library_state(
                    3,
                    path,
                    "is not valid XML (the game log names the error): the HenrySenses.* effects are off"
                );
                return false;
            }
            s_library.node = root;
            log_library_state(2, path, std::format("loaded ({} bytes): {}", text.size(), library_effect_names(text)));
            return true;
        }

        /**
         * @brief Makes the mod's library current in the particle manager: re-reads a changed file and registers it
         *        again (a level unload drops it; a registered effect is kept as it was).
         * @return True when HenrySenses.* effects can be looked up.
         */
        [[nodiscard]] bool ensure_library()
        {
            if (!feature_ready(Feature::EffectLibrary))
            {
                log_library_state(3, library_path(), "cannot be used: the particle library anchors did not resolve");
                return false;
            }
            if (!refresh_library())
            {
                return false;
            }
            const std::uintptr_t manager = genv_interface(constants::GENV_PARTICLE_MANAGER_OFFSET);
            const std::uintptr_t load_library = validated_vtable_slot(
                manager,
                constants::PARTICLE_MANAGER_VTABLE_LOAD_LIBRARY_OFFSET,
                AnchorId::ParticleLoadLibrary
            );
            std::uintptr_t library = s_library.node;
            return load_library != 0 &&
                   call_load_library(load_library, manager, constants::PARTICLE_LIBRARY_NAME, &library);
        }

        /**
         * @brief Reads the slot object at @p slot of a render proxy.
         * @return The CEntitySlot (0 for an empty slot), or std::nullopt when the index is out of the slot vector or
         *         the vector does not read.
         */
        [[nodiscard]] std::optional<std::uintptr_t> slot_object(std::uintptr_t node, int slot) noexcept
        {
            if (slot < 0)
            {
                return std::nullopt;
            }
            const auto begin =
                DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::RENDER_PROXY_SLOTS_BEGIN_OFFSET});
            const auto end =
                DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::RENDER_PROXY_SLOTS_END_OFFSET});
            if (!begin || !end || *end < *begin)
            {
                return std::nullopt;
            }
            const std::size_t count = (*end - *begin) / sizeof(std::uintptr_t);
            if (count > MAX_SLOTS || static_cast<std::size_t>(slot) >= count)
            {
                return std::nullopt;
            }
            const auto object =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*begin + static_cast<std::size_t>(slot) * 8});
            if (!object)
            {
                return std::nullopt;
            }
            return *object;
        }

        /**
         * @brief Reads a slot's child render node.
         * @return The node, 0 for none, or std::nullopt when it does not read.
         */
        [[nodiscard]] std::optional<std::uintptr_t> slot_child(std::uintptr_t object) noexcept
        {
            const auto child = DMK::memory::read<std::uintptr_t>(
                DMK::Address{object + constants::ENTITY_SLOT_CHILD_RENDER_NODE_OFFSET}
            );
            if (!child)
            {
                return std::nullopt;
            }
            return *child;
        }

        /**
         * @brief Records the emitter of an entry's slot once its load has run.
         * @note Called only while the entry is still proven to be its own (same entity, same proxy).
         */
        void note_emitter(Attached &entry) noexcept
        {
            if (entry.emitter != 0)
            {
                return;
            }
            const std::optional<std::uintptr_t> object = slot_object(entry.node, entry.slot);
            if (!object.has_value() || *object == 0)
            {
                return;
            }
            const std::optional<std::uintptr_t> child = slot_child(*object);
            if (child.has_value() && object_is(GameClass::ParticleEmitter, *child))
            {
                entry.emitter = *child;
            }
        }

        /**
         * @enum Ownership
         * @brief Whether an entry's slot is still the mod's to free.
         */
        enum class Ownership : std::uint8_t
        {
            /// The same entity and proxy, and the slot still holds the mod's emitter (or its pending load).
            Owned,
            /// The entity, its proxy or the slot's content changed: the emitter went with it, nothing to free.
            Gone,
        };

        /** @brief Proves an entry's slot is still the mod's. Refreshes its emitter on the way. */
        [[nodiscard]] Ownership prove(Attached &entry) noexcept
        {
            const std::uintptr_t entity = entity_from_id(entry.entity_id);
            if (entity == 0 || entity != entry.entity || render_node_of(entity) != entry.node)
            {
                return Ownership::Gone;
            }
            const std::optional<std::uintptr_t> object = slot_object(entry.node, entry.slot);
            if (!object.has_value() || *object == 0)
            {
                return Ownership::Gone;
            }
            const std::optional<std::uintptr_t> child = slot_child(*object);
            if (!child.has_value())
            {
                return Ownership::Gone;
            }
            if (entry.emitter != 0)
            {
                return *child == entry.emitter ? Ownership::Owned : Ownership::Gone;
            }
            // Not seen yet: the slot index is the one the load reserved, so an empty slot (the load still queued) or
            // an emitter that appeared since is the mod's.
            if (*child == 0)
            {
                return Ownership::Owned;
            }
            if (object_is(GameClass::ParticleEmitter, *child))
            {
                entry.emitter = *child;
                return Ownership::Owned;
            }
            return Ownership::Gone;
        }

        /** @brief Kills a free emitter when it is still the mod's. */
        void kill_free_entry(const Attached &entry, const char *reason) noexcept
        {
            const std::optional<bool> alive = free_emitter_alive(entry);
            if (!alive.value_or(false))
            {
                // Gone with a level unload (or unprovable): never touched.
                ++s_stats.lost;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Effects: key={:#x} free emitter {} ({})",
                    entry.key,
                    alive.has_value() ? "already gone" : "left alone (the emitter list did not read)",
                    reason
                );
                return;
            }
            const std::uintptr_t kill = validated_vtable_slot(
                entry.emitter,
                constants::PARTICLE_EMITTER_VTABLE_KILL_OFFSET,
                AnchorId::EmitterKill
            );
            if (kill == 0 || !call_kill(kill, entry.emitter))
            {
                ++s_stats.failed;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Effects: key={:#x} free emitter could not be killed ({})",
                    entry.key,
                    reason
                );
                return;
            }
            ++s_stats.freed;
            (void)DMK::log()
                .try_log(DMK::LogLevel::Debug, "Effects: key={:#x} free emitter killed ({})", entry.key, reason);
        }

        /** @brief Frees an entry's slot (or kills its free emitter) when it is still the mod's. */
        void free_entry(Attached &entry, const char *reason) noexcept
        {
            if (entry.free)
            {
                kill_free_entry(entry, reason);
                return;
            }
            if (prove(entry) != Ownership::Owned)
            {
                ++s_stats.lost;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Effects: id={:#x} slot {} already gone with its entity or proxy ({})",
                    entry.entity_id,
                    entry.slot,
                    reason
                );
                return;
            }
            const std::uintptr_t free_slot = gated_anchor_address(Feature::LootEffects, AnchorId::ProxyFreeSlot);
            if (free_slot == 0 || !call_free_slot(free_slot, entry.node, entry.slot))
            {
                ++s_stats.failed;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Effects: id={:#x} slot {} could not be freed ({})",
                    entry.entity_id,
                    entry.slot,
                    reason
                );
                return;
            }
            ++s_stats.freed;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Effects: id={:#x} slot {} freed ({})",
                entry.entity_id,
                entry.slot,
                reason
            );
        }

        /**
         * @brief Releases an entry: frees it now, or parks it until its pending load has run.
         * @param immediate Free now even when the load may still be pending (shutdown).
         */
        void release_entry(Attached &entry, const char *reason, bool immediate, std::int64_t now) noexcept
        {
            // prove() records the emitter if its load has run since; only a slot load still pending parks the entry
            // (a free emitter exists from its creation on).
            const bool pending = !immediate && !entry.free && entry.emitter == 0 &&
                                 now - entry.loaded_ms < LOAD_GRACE_MS && prove(entry) == Ownership::Owned &&
                                 entry.emitter == 0;
            if (pending)
            {
                s_retiring.push_back(entry);
                return;
            }
            free_entry(entry, reason);
        }

        /**
         * @brief The slot transform that puts an emitter at @p position (world) with its Y axis (the pfx1 emit
         *        direction) up and a uniform @p scale, given the entity's world matrix.
         * @return The local transform, or std::nullopt when the entity's matrix is singular.
         */
        [[nodiscard]] std::optional<game_structures::Matrix34f>
        emitter_local_tm(const game_structures::Matrix34f &world, const game_structures::Vec3f &position, float scale)
        {
            const auto &a = world.m;
            const float c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
            const float c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
            const float c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
            const float det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
            if (!std::isfinite(det) || std::fabs(det) < 1e-8f)
            {
                return std::nullopt;
            }
            const float inv_det = 1.0f / det;
            // inverse(A) = adjugate / det; the adjugate is the transposed cofactor matrix.
            const float inv[3][3] = {
                {c00 * inv_det,
                 (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * inv_det,
                 (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * inv_det},
                {c01 * inv_det,
                 (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * inv_det,
                 (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * inv_det},
                {c02 * inv_det,
                 (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * inv_det,
                 (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * inv_det},
            };
            // The wanted world rotation, scaled: X = world X, Y = world up, Z = -world Y (right-handed).
            const float wanted[3][3] = {
                {scale, 0.0f, 0.0f},
                {0.0f, 0.0f, -scale},
                {0.0f, scale, 0.0f},
            };
            const float offset[3] = {position.x - a[0][3], position.y - a[1][3], position.z - a[2][3]};
            game_structures::Matrix34f local{};
            for (int row = 0; row < 3; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    local.m[row][column] = inv[row][0] * wanted[0][column] + inv[row][1] * wanted[1][column] +
                                           inv[row][2] * wanted[2][column];
                }
                local.m[row][3] = inv[row][0] * offset[0] + inv[row][1] * offset[1] + inv[row][2] * offset[2];
            }
            return local;
        }

        /** @brief Warns once that an effect name did not load. */
        void warn_missing(std::uint16_t effect)
        {
            if (std::find(s_missing.begin(), s_missing.end(), effect) != s_missing.end())
            {
                return;
            }
            s_missing.push_back(effect);
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Effects: '{}' did not load (FindEffect returned nothing): check the name against the game's "
                "Libs/Particles libraries, written <library>.<effect>",
                effect_name(effect)
            );
        }

        /** @brief The top centre of a box, where an effect sits. */
        [[nodiscard]] game_structures::Vec3f top_centre(const game_structures::Aabb &bounds) noexcept
        {
            return game_structures::Vec3f{
                (bounds.min.x + bounds.max.x) * 0.5f,
                (bounds.min.y + bounds.max.y) * 0.5f,
                bounds.max.z,
            };
        }

        /**
         * @brief Looks an effect up, warning once when it does not load.
         * @return The IParticleEffect, or 0.
         */
        [[nodiscard]] std::uintptr_t find_effect(const Wanted &wanted, const std::string &name)
        {
            const std::uintptr_t manager = genv_interface(constants::GENV_PARTICLE_MANAGER_OFFSET);
            const std::uintptr_t find = validated_vtable_slot(
                manager,
                constants::PARTICLE_MANAGER_VTABLE_FIND_EFFECT_OFFSET,
                AnchorId::FindEffect
            );
            if (find == 0)
            {
                ++s_stats.failed;
                return 0;
            }
            const std::uintptr_t effect = call_find_effect(find, manager, name.c_str());
            if (effect == 0)
            {
                warn_missing(wanted.effect);
            }
            return effect;
        }

        /**
         * @brief Creates a free-standing emitter on top of @p bounds.
         * @return True when the emitter was created.
         */
        bool attach_free(
            EffectChannel channel,
            const Wanted &wanted,
            const std::string &name,
            const game_structures::Aabb &bounds,
            std::int64_t now
        )
        {
            const std::uintptr_t manager = particle_manager_primary();
            const std::uintptr_t create = gated_anchor_address(Feature::WorldEffects, AnchorId::ManagerCreateEmitter);
            if (manager == 0 || create == 0)
            {
                ++s_stats.failed;
                return false;
            }
            const std::uintptr_t effect = find_effect(wanted, name);
            if (effect == 0)
            {
                return false;
            }
            const QuatTs location{
                .qx = HALF_SQRT2,
                .qw = HALF_SQRT2,
                .position = top_centre(bounds),
                .scale = wanted.scale,
            };
            const std::uintptr_t emitter = call_create_emitter(create, manager, &location, effect);
            if (emitter == 0)
            {
                ++s_stats.failed;
                return false;
            }
            ++s_stats.loaded;
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Effects: key={:#x} <- '{}' x{:.2f} free at ({:.2f}, {:.2f}, {:.2f}) emitter {}",
                wanted.key,
                name,
                wanted.scale,
                location.position.x,
                location.position.y,
                location.position.z,
                DMK::format::format_address(emitter)
            );
            s_attached.push_back(
                Attached{
                    .channel = channel,
                    .key = wanted.key,
                    .free = true,
                    .entity_id = wanted.entity_id,
                    .emitter = emitter,
                    .location = location,
                    .effect = wanted.effect,
                    .scale = wanted.scale,
                    .loaded_ms = now,
                }
            );
            return true;
        }

        /**
         * @brief Loads one effect into a new slot of an entity's render proxy, on top of its bounds; an entity without
         *        a render proxy gets a free-standing emitter on its bounds instead.
         * @param name The effect's full name (effect_name(wanted.effect)).
         * @return True when the emitter was created.
         */
        bool attach_to_entity(EffectChannel channel, const Wanted &wanted, const std::string &name, std::int64_t now)
        {
            const std::uintptr_t entity = entity_from_id(wanted.entity_id);
            if (entity == 0)
            {
                return false;
            }
            const std::optional<game_structures::Aabb> bounds = entity_world_bounds(entity);
            if (!bounds.has_value())
            {
                return false;
            }
            const std::uintptr_t node = render_node_of(entity);
            if (node == 0)
            {
                return feature_ready(Feature::WorldEffects) && attach_free(channel, wanted, name, *bounds, now);
            }
            const auto world = DMK::memory::read<game_structures::Matrix34f>(
                DMK::Address{entity + constants::ENTITY_WORLD_MATRIX_OFFSET}
            );
            if (!world)
            {
                return false;
            }
            const game_structures::Vec3f top = top_centre(*bounds);
            const std::optional<game_structures::Matrix34f> local = emitter_local_tm(*world, top, wanted.scale);
            if (!local.has_value())
            {
                return false;
            }
            const std::uintptr_t load = gated_anchor_address(Feature::LootEffects, AnchorId::ProxyLoadParticleEmitter);
            const std::uintptr_t set_tm = gated_anchor_address(Feature::LootEffects, AnchorId::ProxySetSlotLocalTM);
            if (load == 0 || set_tm == 0)
            {
                ++s_stats.failed;
                return false;
            }
            const std::uintptr_t effect = find_effect(wanted, name);
            if (effect == 0)
            {
                return false;
            }
            const int slot = call_load_emitter(load, node, effect);
            if (slot < 0)
            {
                ++s_stats.failed;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Warning,
                    "Effects: id={:#x} '{}' LoadParticleEmitter refused it",
                    wanted.entity_id,
                    name
                );
                return false;
            }
            Attached entry{
                .channel = channel,
                .key = wanted.key,
                .entity_id = wanted.entity_id,
                .entity = entity,
                .node = node,
                .slot = slot,
                .effect = wanted.effect,
                .scale = wanted.scale,
                .loaded_ms = now,
            };
            if (!call_set_slot_tm(set_tm, node, slot, &*local))
            {
                ++s_stats.failed;
                release_entry(entry, "placing failed", false, now);
                return false;
            }
            note_emitter(entry);
            ++s_stats.loaded;
            if (DMK::log().is_enabled(DMK::LogLevel::Debug))
            {
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Effects: id={:#x} {} '{}' <- '{}' x{:.2f} slot {} at ({:.2f}, {:.2f}, {:.2f}) emitter {}",
                    wanted.entity_id,
                    entity_class_name(entity),
                    entity_name(entity),
                    name,
                    wanted.scale,
                    slot,
                    top.x,
                    top.y,
                    top.z,
                    entry.emitter != 0 ? DMK::format::format_address(entry.emitter) : std::string{"queued"}
                );
            }
            s_attached.push_back(entry);
            return true;
        }

        [[nodiscard]] const Wanted *find_wanted(std::uint64_t key) noexcept
        {
            const auto it = std::find_if(
                s_wanted.begin(),
                s_wanted.end(),
                [key](const Wanted &wanted) { return wanted.key == key; }
            );
            return it != s_wanted.end() ? &*it : nullptr;
        }

        [[nodiscard]] bool attached_to(EffectChannel channel, std::uint64_t key) noexcept
        {
            return std::any_of(
                s_attached.begin(),
                s_attached.end(),
                [channel, key](const Attached &entry) { return entry.channel == channel && entry.key == key; }
            );
        }

        /**
         * @brief Replaces one channel's effects with s_wanted: releases what is no longer wanted, drops what went
         *        with its entity or a level unload, and creates what is missing.
         * @param feature_ok The channel's feature gates passed (otherwise nothing new is created).
         */
        void sync_channel(EffectChannel channel, bool feature_ok, std::int64_t now)
        {
            begin_call();
            if (!feature_ok)
            {
                s_wanted.clear();
            }

            // Release what is no longer wanted, or wanted with another effect or scale.
            for (auto it = s_attached.begin(); it != s_attached.end();)
            {
                if (it->channel != channel)
                {
                    ++it;
                    continue;
                }
                const Wanted *wanted = find_wanted(it->key);
                if (wanted != nullptr && wanted->effect == it->effect && wanted->scale == it->scale)
                {
                    ++it;
                    continue;
                }
                release_entry(*it, wanted == nullptr ? "no longer highlighted" : "effect changed", false, now);
                it = s_attached.erase(it);
            }

            // A proxy recreated by streaming took its slots along, and a level unload the free emitters; their
            // objects get new emitters below.
            const bool checSLOTS = now >= s_next_validate_ms;
            const bool check_free =
                now >= s_next_free_validate_ms &&
                std::any_of(s_attached.begin(), s_attached.end(), [](const Attached &entry) { return entry.free; });
            if (checSLOTS)
            {
                s_next_validate_ms = now + VALIDATE_INTERVAL_MS;
            }
            if (check_free)
            {
                s_next_free_validate_ms = now + FREE_VALIDATE_INTERVAL_MS;
            }
            for (auto it = s_attached.begin(); it != s_attached.end();)
            {
                // An unreadable emitter list decides nothing.
                const bool gone = it->free ? check_free && !free_emitter_alive(*it).value_or(true)
                                           : checSLOTS && prove(*it) != Ownership::Owned;
                if (!gone)
                {
                    ++it;
                    continue;
                }
                ++s_stats.lost;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug,
                    "Effects: key={:#x} {} is gone; creating it again",
                    it->key,
                    it->free ? "free emitter" : "slot"
                );
                it = s_attached.erase(it);
            }

            std::size_t spawned = 0;
            // The mod's library is made current once per batch, and only when a batch loads one of its effects.
            std::optional<bool> library_ready;
            for (const Wanted &wanted : s_wanted)
            {
                if (spawned >= MAX_SPAWNS_PER_SYNC)
                {
                    break;
                }
                if (attached_to(channel, wanted.key))
                {
                    continue;
                }
                const std::string name = effect_name(wanted.effect);
                if (is_library_effect(name))
                {
                    if (!library_ready.has_value())
                    {
                        library_ready = ensure_library();
                    }
                    // Never looked up without the library: FindEffect would keep a disabled placeholder under the
                    // name for the rest of the session.
                    if (!*library_ready)
                    {
                        continue;
                    }
                }
                const bool created = wanted.entity_id != 0 ? attach_to_entity(channel, wanted, name, now)
                                                           : attach_free(channel, wanted, name, wanted.bounds, now);
                spawned += created ? 1 : 0;
            }
        }

        /** @brief Logs once that a channel's feature gates failed. */
        void log_gate(bool &logged, std::string_view what)
        {
            if (logged)
            {
                return;
            }
            logged = true;
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "Effects: a group has an Effect but the particle anchors for {} did not resolve on this build; they "
                "show none",
                what
            );
        }

    } // namespace

    float effect_min_intensity() noexcept
    {
        return MIN_INTENSITY;
    }

    void sync_loot_effects(std::span<const HighlightRequest> batch) noexcept
    {
        try
        {
            s_wanted.clear();
            for (const HighlightRequest &request : batch)
            {
                if (request.effect != 0 && request.marker_intensity >= MIN_INTENSITY)
                {
                    s_wanted.push_back(
                        Wanted{
                            .key = request.entity_id,
                            .entity_id = request.entity_id,
                            .effect = request.effect,
                            .scale = request.effect_scale,
                        }
                    );
                }
            }
            const bool ready = feature_ready(Feature::LootEffects);
            if (!s_wanted.empty() && !ready)
            {
                log_gate(s_gate_logged, "entities");
            }
            sync_channel(EffectChannel::Loot, ready, steady_ms());
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Effects: loot sync failed");
        }
    }

    void sync_world_effects(std::span<const EffectTarget> targets) noexcept
    {
        try
        {
            s_wanted.clear();
            for (const EffectTarget &target : targets)
            {
                if (target.effect != 0)
                {
                    s_wanted.push_back(
                        Wanted{
                            .key = target.key,
                            .bounds = target.bounds,
                            .effect = target.effect,
                            .scale = target.scale,
                        }
                    );
                }
            }
            const bool ready = feature_ready(Feature::WorldEffects);
            if (!s_wanted.empty() && !ready)
            {
                log_gate(s_world_gate_logged, "interactables, herbs and static objects");
            }
            sync_channel(EffectChannel::World, ready, steady_ms());
        }
        catch (...)
        {
            (void)DMK::log().log_noexcept(DMK::LogLevel::Error, "Effects: world sync failed");
        }
    }

    void maintain_loot_effects() noexcept
    {
        if (s_retiring.empty())
        {
            return;
        }
        begin_call();
        const std::int64_t now = steady_ms();
        for (auto it = s_retiring.begin(); it != s_retiring.end();)
        {
            if (prove(*it) == Ownership::Owned && it->emitter == 0 && now - it->loaded_ms < LOAD_GRACE_MS)
            {
                ++it;
                continue;
            }
            free_entry(*it, "released");
            it = s_retiring.erase(it);
        }
    }

    void clear_loot_effects(EffectChannel channel, bool immediate) noexcept
    {
        begin_call();
        const std::int64_t now = steady_ms();
        std::size_t count = 0;
        for (auto it = s_attached.begin(); it != s_attached.end();)
        {
            if (it->channel != channel)
            {
                ++it;
                continue;
            }
            release_entry(*it, "cleared", immediate, now);
            it = s_attached.erase(it);
            ++count;
        }
        if (immediate)
        {
            for (auto it = s_retiring.begin(); it != s_retiring.end();)
            {
                if (it->channel != channel)
                {
                    ++it;
                    continue;
                }
                free_entry(*it, "cleared");
                it = s_retiring.erase(it);
            }
        }
        if (count != 0)
        {
            (void)DMK::log().try_log(
                DMK::LogLevel::Debug,
                "Effects: cleared {} {} effect(s){}",
                count,
                channel == EffectChannel::Loot ? "loot" : "world",
                s_retiring.empty() ? "" : std::format(", {} slot(s) freed once their load has run", s_retiring.size())
            );
        }
    }

    void clear_all_loot_effects(bool immediate) noexcept
    {
        clear_loot_effects(EffectChannel::Loot, immediate);
        clear_loot_effects(EffectChannel::World, immediate);
    }

    void shutdown_loot_effects() noexcept
    {
        clear_all_loot_effects(true);
        release_library_node();
        s_library = EffectLibrary{};
    }

    std::size_t loot_effect_count() noexcept
    {
        return s_attached.size() + s_retiring.size();
    }

    void log_loot_effects_state()
    {
        DMK::log().info(
            "Effects: gates entity={} world={} library={} attached={} retiring={} loaded={} freed={} lost={} "
            "failed={}",
            feature_ready(Feature::LootEffects) ? "ready" : "off",
            feature_ready(Feature::WorldEffects) ? "ready" : "off",
            s_library.node != 0 ? "loaded" : (feature_ready(Feature::EffectLibrary) ? "none" : "off"),
            s_attached.size(),
            s_retiring.size(),
            s_stats.loaded,
            s_stats.freed,
            s_stats.lost,
            s_stats.failed
        );
        const std::int64_t now = steady_ms();
        for (const Attached &entry : s_attached)
        {
            DMK::log().info(
                "Effects:   {} key={:#x} '{}' x{:.2f} {} emitter {} age {} ms",
                entry.channel == EffectChannel::Loot ? "loot" : "world",
                entry.key,
                effect_name(entry.effect),
                entry.scale,
                entry.free ? std::string{"free"}
                           : std::format("slot {} node {}", entry.slot, DMK::format::format_address(entry.node)),
                entry.emitter != 0 ? DMK::format::format_address(entry.emitter) : std::string{"queued"},
                now - entry.loaded_ms
            );
        }
    }

} // namespace HenrySenses
