/**
 * @file game/entity_tracker.cpp
 * @brief Name-to-entity resolution and per-frame position reads through the entity system's vtables.
 *
 * A tracked entity is found once by name, then re-read every frame by id: GetEntity returns nullptr once the id's
 * entity is gone (the id carries a salt), and the name is searched again at most twice a second while it is missing.
 */

#include "game/entity_tracker.hpp"
#include "constants.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>

namespace SoundFramework::game::entities
{
    namespace
    {
        using GetEntityFn = void *(__fastcall *)(void *system, std::uint32_t id);
        using FindByNameFn = void *(__fastcall *)(void *system, const char *name);
        using GetIdFn = std::uint32_t(__fastcall *)(const void *entity);
        using GetWorldPosFn = Position *(__fastcall *)(const void *entity, Position *out);

        constexpr std::uint64_t SEARCH_INTERVAL_MS = 500;

        struct Tracked
        {
            std::string name;
            std::uint32_t id = 0;
            std::uint64_t next_search_ms = 0;
            bool found = false;
            Position position{};
        };

        Anchors s_anchors{};
        std::atomic<bool> s_enabled{false};

        std::atomic<std::shared_ptr<const std::vector<std::string>>> s_watch;

        /// Main-thread working set; positions are copied out under s_positions_mutex for the audio thread.
        std::vector<Tracked> s_tracked;
        std::shared_ptr<const std::vector<std::string>> s_tracked_list;

        std::mutex s_positions_mutex;
        std::vector<Tracked> s_published;

        [[nodiscard]] void *const *vtable_of(const void *object) noexcept
        {
            return *static_cast<void *const *const *>(object);
        }

        /** @brief gEnv->pEntitySystem when it is the CEntitySystem the anchors describe, or nullptr. */
        [[nodiscard]] void *entity_system() noexcept
        {
            const auto *genv = reinterpret_cast<const std::uint8_t *>(s_anchors.genv);
            void *const system = *reinterpret_cast<void *const *>(genv + constants::GENV_ENTITY_SYSTEM);
            if (system == nullptr ||
                reinterpret_cast<std::uintptr_t>(vtable_of(system)) != s_anchors.entity_system_vtable)
            {
                return nullptr;
            }
            return system;
        }

        [[nodiscard]] bool is_entity(const void *entity) noexcept
        {
            return entity != nullptr && reinterpret_cast<std::uintptr_t>(vtable_of(entity)) == s_anchors.entity_vtable;
        }

        /** @brief Refreshes one tracked entity; returns false when it is not in the world right now. */
        [[nodiscard]] bool refresh(void *system, Tracked &tracked, std::uint64_t now) noexcept
        {
            void *const *slots = vtable_of(system);
            void *entity = nullptr;
            if (tracked.id != 0)
            {
                entity =
                    reinterpret_cast<GetEntityFn>(slots[constants::ENTITY_SYSTEM_GET_ENTITY_SLOT])(system, tracked.id);
            }
            if (!is_entity(entity))
            {
                tracked.id = 0;
                if (now < tracked.next_search_ms)
                {
                    return false;
                }
                tracked.next_search_ms = now + SEARCH_INTERVAL_MS;
                entity = reinterpret_cast<FindByNameFn>(slots[constants::ENTITY_SYSTEM_FIND_BY_NAME_SLOT])(
                    system,
                    tracked.name.c_str()
                );
                if (!is_entity(entity))
                {
                    return false;
                }
                void *const *entity_slots = vtable_of(entity);
                tracked.id = reinterpret_cast<GetIdFn>(entity_slots[constants::ENTITY_GET_ID_SLOT])(entity);
                (void)DMK::log()
                    .try_log(DMK::LogLevel::Info, "Entities: tracking '{}' (id {:#x})", tracked.name, tracked.id);
            }
            void *const *entity_slots = vtable_of(entity);
            (void)reinterpret_cast<GetWorldPosFn>(entity_slots[constants::ENTITY_GET_WORLD_POS_SLOT])(
                entity,
                &tracked.position
            );
            return true;
        }
    } // namespace

    void configure(const Anchors &anchors) noexcept
    {
        s_anchors = anchors;
        s_enabled.store(anchors.entities, std::memory_order_release);
    }

    void set_watch_list(std::vector<std::string> names)
    {
        s_watch.store(std::make_shared<const std::vector<std::string>>(std::move(names)));
    }

    void on_frame() noexcept
    {
        if (!s_enabled.load(std::memory_order_acquire))
        {
            return;
        }
        try
        {
            const std::shared_ptr<const std::vector<std::string>> watch = s_watch.load();
            if (watch != s_tracked_list)
            {
                // The list only changes on a pack reload; rebuilding the working set here keeps every write on this
                // thread. The new set is built first, so a failed rebuild keeps the old one and retries next frame.
                std::vector<Tracked> rebuilt;
                if (watch != nullptr)
                {
                    rebuilt.reserve(watch->size());
                    for (const std::string &name : *watch)
                    {
                        rebuilt.push_back(
                            Tracked{
                                .name = name,
                            }
                        );
                    }
                }
                s_tracked = std::move(rebuilt);
                s_tracked_list = watch;
            }
            if (s_tracked.empty())
            {
                return;
            }
            void *const system = entity_system();
            const std::uint64_t now = GetTickCount64();
            for (Tracked &tracked : s_tracked)
            {
                tracked.found = system != nullptr && refresh(system, tracked, now);
            }
            const std::lock_guard lock(s_positions_mutex);
            s_published = s_tracked;
        }
        catch (const std::exception &error)
        {
            (void)DMK::log().try_log(DMK::LogLevel::Error, "Entities: frame update failed ({})", error.what());
        }
    }

    std::uint32_t find_id(const std::string &name) noexcept
    {
        if (!s_enabled.load(std::memory_order_acquire))
        {
            return 0;
        }
        void *const system = entity_system();
        if (system == nullptr)
        {
            return 0;
        }
        void *const entity = reinterpret_cast<FindByNameFn>(vtable_of(
            system
        )[constants::ENTITY_SYSTEM_FIND_BY_NAME_SLOT])(system, name.c_str());
        if (!is_entity(entity))
        {
            return 0;
        }
        return reinterpret_cast<GetIdFn>(vtable_of(entity)[constants::ENTITY_GET_ID_SLOT])(entity);
    }

    bool any_near(const std::vector<std::string> &names, const Position &at, float radius) noexcept
    {
        const float limit = radius * radius;
        const std::lock_guard lock(s_positions_mutex);
        for (const Tracked &tracked : s_published)
        {
            if (!tracked.found)
            {
                continue;
            }
            const float dx = tracked.position.x - at.x;
            const float dy = tracked.position.y - at.y;
            const float dz = tracked.position.z - at.z;
            if (dx * dx + dy * dy + dz * dz > limit)
            {
                continue;
            }
            for (const std::string &name : names)
            {
                if (name == tracked.name)
                {
                    return true;
                }
            }
        }
        return false;
    }

    void clear() noexcept
    {
        s_enabled.store(false, std::memory_order_release);
        s_watch.store(nullptr);
        const std::lock_guard lock(s_positions_mutex);
        s_published.clear();
    }
} // namespace SoundFramework::game::entities
