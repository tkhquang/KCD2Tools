/**
 * @file engine/entity_access.cpp
 * @brief Validated, SEH-guarded entity, render-node and 3D-engine registration calls.
 */

#include "engine/entity_access.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "rtti_types.hpp"
#include "engine/engine_env.hpp"
#include "engine/seh.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        std::atomic<bool> s_available{false};

        using GetEntityFn = std::uintptr_t(__fastcall *)(std::uintptr_t system, EntityId id);
        using GetProxyFn = std::uintptr_t(__fastcall *)(std::uintptr_t entity, int type);
        using GetWorldBoundsFn = void(__fastcall *)(std::uintptr_t entity, game_structures::Aabb *out);
        using NodeCallFn = void(__fastcall *)(std::uintptr_t engine, std::uintptr_t node);
        using GetIteratorFn = std::uintptr_t(__fastcall *)(std::uintptr_t system);
        using IteratorVoidFn = void(__fastcall *)(std::uintptr_t iterator);
        using IteratorBoolFn = bool(__fastcall *)(std::uintptr_t iterator);
        using IteratorNextFn = std::uintptr_t(__fastcall *)(std::uintptr_t iterator);

        // Entity names carry a prefab-instance tag after the object's own path ("Procedural/Fireplace/cauldron_home/
        // fireplace_home_cauldron_empty2_prc[Fireplace/fireplace_home12_<guid>]" is 134 characters); a cut name loses
        // the tag's closing bracket, and with it the tag.
        constexpr std::size_t MAX_NAME_LENGTH = 255;
        constexpr std::size_t MAX_PATH_LENGTH = 255;

        // A salt-buffer slot read whole as words: the salt in the low 16 bits of word 0, the next slot in the low 32
        // bits of word 1, the entity in word 2.
        using SaltSlotWords = std::array<std::uint64_t, 3>;
        static_assert(
            sizeof(SaltSlotWords) == constants::SALT_SLOT_STRIDE && constants::SALT_SLOT_SALT_OFFSET == 0 &&
                constants::SALT_SLOT_NEXT_OFFSET == 8 && constants::SALT_SLOT_ENTITY_OFFSET == 16,
            "The salt slot fields must be the words of SaltSlotWords."
        );

        /**
         * @brief The direct walk's layout check, once per session: 0 = not checked yet, 1 = it matches the iterator, -1
         * = it does not (every walk then uses the iterator). Main thread only.
         */
        int s_direct_layout = 0;

        /**
         * @brief What a direct walk learned about one salt-buffer slot: the entity it held, that entity's id and class.
         * @details The id carries the slot's salt, which changes when the slot is reused, so an entry whose entity and
         *          id both still match is the same entity, and an entity's class never changes.
         */
        struct SlotClass
        {
            std::uintptr_t entity{0};
            std::uintptr_t klass{0};
            EntityId id{0};
        };

        // Indexed by slot, grown to the highest slot seen (at most ~6 MB); main thread only.
        std::vector<SlotClass> s_slot_classes;
        // The out indices of the entities a step found no class for.
        std::vector<std::size_t> s_class_misses;

        [[nodiscard]] std::uintptr_t read_entity_class(std::uintptr_t entity) noexcept
        {
            const auto klass = DMK::memory::read<std::uintptr_t>(DMK::Address{entity + constants::ENTITY_CLASS_OFFSET});
            return klass && DMK::memory::is_plausible_ptr(DMK::Address{*klass}) ? *klass : 0;
        }

        /** @brief The cached class of the entity in @p slot, or 0 when the slot holds another entity now. */
        [[nodiscard]] std::uintptr_t cached_class(std::uint32_t slot, std::uintptr_t entity, EntityId id) noexcept
        {
            if (slot >= s_slot_classes.size())
            {
                return 0;
            }
            const SlotClass &cached = s_slot_classes[slot];
            return cached.entity == entity && cached.id == id ? cached.klass : 0;
        }

        /**
         * @brief Reads the class (and checks the id) of every entity from @p first on that the slot cache did not
         *        know, prefetching ahead, and caches what it read.
         * @details The entity's own id is kept when it differs from the one the slot gave: the slot then changed
         *          between the two reads, and nothing is cached for it.
         */
        void resolve_class_misses(std::vector<WalkedEntity> &out, std::size_t first)
        {
            s_class_misses.clear();
            for (std::size_t i = first; i < out.size(); ++i)
            {
                if (out[i].klass == 0)
                {
                    s_class_misses.push_back(i);
                }
            }
            for (std::size_t k = 0; k < s_class_misses.size(); ++k)
            {
                if (k + PREFETCH_DISTANCE < s_class_misses.size())
                {
                    prefetch_entity(out[s_class_misses[k + PREFETCH_DISTANCE]].entity, false);
                }
                WalkedEntity &walked = out[s_class_misses[k]];
                const auto own_id =
                    DMK::memory::read<std::uint32_t>(DMK::Address{walked.entity + constants::ENTITY_ID_OFFSET});
                walked.klass = read_entity_class(walked.entity);
                if (!own_id || *own_id != walked.id)
                {
                    walked.id = own_id.value_or(0);
                    continue;
                }
                const std::uint32_t slot = walked.id & constants::SALT_ID_SLOT_MASK;
                if (walked.klass == 0 || slot > constants::SALT_LAST_SLOT)
                {
                    continue;
                }
                if (slot >= s_slot_classes.size())
                {
                    s_slot_classes.resize(slot + 1);
                }
                s_slot_classes[slot] = SlotClass{walked.entity, walked.klass, walked.id};
            }
        }

        [[nodiscard]] constexpr bool usable_slot(std::uint64_t slot) noexcept
        {
            return slot >= 1 && slot <= constants::SALT_LAST_SLOT;
        }

        [[nodiscard]] std::optional<SaltSlotWords> read_salt_slot(std::uintptr_t system, std::uint32_t slot) noexcept
        {
            const auto words = DMK::memory::read<SaltSlotWords>(
                DMK::Address{system + constants::ENTITY_SYSTEM_SALT_BUFFER_OFFSET + slot * constants::SALT_SLOT_STRIDE}
            );
            return words ? std::optional<SaltSlotWords>{*words} : std::nullopt;
        }

        [[nodiscard]] std::uint32_t salt_head(std::uintptr_t system) noexcept
        {
            const auto head =
                DMK::memory::read<std::uint32_t>(DMK::Address{system + constants::ENTITY_SYSTEM_SALT_HEAD_OFFSET});
            return head && usable_slot(*head) ? *head : 0;
        }

        /**
         * @brief The first entity of the salt-buffer list, the one CEntityItMap::MoveFirst stops at.
         * @return The entity, or 0 when the list is empty or unreadable.
         */
        [[nodiscard]] std::uintptr_t first_salt_entity(std::uintptr_t system) noexcept
        {
            std::uint32_t slot = salt_head(system);
            for (std::uint32_t visited = 0; slot != 0 && visited <= constants::SALT_LAST_SLOT; ++visited)
            {
                const std::optional<SaltSlotWords> words = read_salt_slot(system, slot);
                if (!words)
                {
                    return 0;
                }
                if ((*words)[2] != 0)
                {
                    return static_cast<std::uintptr_t>((*words)[2]);
                }
                slot = usable_slot((*words)[1] & 0xFFFFFFFFu) ? static_cast<std::uint32_t>((*words)[1]) : 0;
            }
            return 0;
        }

        /**
         * @brief The validated entity system and its GetEntity slot.
         */
        struct EntityCalls
        {
            std::uintptr_t system{0};
            std::uintptr_t get_entity{0};
        };

        /**
         * @brief The calls of the current main-thread tick: validated by the tick's first lookup and kept until the
         *        tick ends, so its other lookups skip the validation (the system pointer lives as long as the engine).
         *        Main thread only.
         */
        struct TickCalls
        {
            /// A tick is running (begin_entity_tick() without end_entity_tick()).
            bool open{false};
            /// This tick validated them already.
            bool validated{false};
            EntityCalls calls{};
        };
        TickCalls s_tick{};

        /** @brief Resolves the entity system from gEnv and validates its class and its GetEntity slot. */
        [[nodiscard]] EntityCalls validated_entity_calls() noexcept
        {
            const std::uintptr_t system = genv_interface(constants::GENV_ENTITY_SYSTEM_OFFSET);
            if (system == 0 || !object_is(GameClass::EntitySystem, system))
            {
                return {};
            }
            const std::uintptr_t fn =
                validated_vtable_slot(system, constants::ENTITY_SYSTEM_VTABLE_GET_ENTITY_OFFSET, AnchorId::GetEntity);
            return fn != 0 ? EntityCalls{system, fn} : EntityCalls{};
        }

        /** @brief The tick's validated calls (validated on its first lookup), or a validation now outside a tick. */
        [[nodiscard]] EntityCalls entity_calls() noexcept
        {
            if (!s_tick.open)
            {
                return validated_entity_calls();
            }
            if (!s_tick.validated)
            {
                s_tick.calls = validated_entity_calls();
                s_tick.validated = true;
            }
            return s_tick.calls;
        }

        [[nodiscard]] std::uintptr_t call_get_entity(std::uintptr_t fn, std::uintptr_t system, EntityId id) noexcept
        {
            __try
            {
                return reinterpret_cast<GetEntityFn>(fn)(system, id);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        [[nodiscard]] std::uintptr_t call_get_proxy(std::uintptr_t fn, std::uintptr_t entity, int type) noexcept
        {
            __try
            {
                return reinterpret_cast<GetProxyFn>(fn)(entity, type);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        [[nodiscard]] bool
        call_get_world_bounds(std::uintptr_t fn, std::uintptr_t entity, game_structures::Aabb *out) noexcept
        {
            __try
            {
                reinterpret_cast<GetWorldBoundsFn>(fn)(entity, out);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] bool call_node_fn(std::uintptr_t fn, std::uintptr_t engine, std::uintptr_t node) noexcept
        {
            __try
            {
                reinterpret_cast<NodeCallFn>(fn)(engine, node);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        [[nodiscard]] std::uintptr_t call_get_iterator(std::uintptr_t fn, std::uintptr_t system) noexcept
        {
            __try
            {
                return reinterpret_cast<GetIteratorFn>(fn)(system);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return 0;
            }
        }

        [[nodiscard]] bool call_iterator_void(std::uintptr_t fn, std::uintptr_t iterator) noexcept
        {
            __try
            {
                reinterpret_cast<IteratorVoidFn>(fn)(iterator);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Calls IEntityIt::IsEnd under SEH.
         * @return 1 at the end, 0 when more entities follow, -1 on a fault.
         */
        [[nodiscard]] int call_iterator_is_end(std::uintptr_t fn, std::uintptr_t iterator) noexcept
        {
            __try
            {
                return reinterpret_cast<IteratorBoolFn>(fn)(iterator) ? 1 : 0;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return -1;
            }
        }

        [[nodiscard]] bool call_iterator_next(std::uintptr_t fn, std::uintptr_t iterator, std::uintptr_t *out) noexcept
        {
            __try
            {
                *out = reinterpret_cast<IteratorNextFn>(fn)(iterator);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Exchanges an engine stamp under SEH (the engine writes it with the same interlocked exchange).
         * @return True when the exchange landed.
         */
        [[nodiscard]] bool guarded_exchange_stamp(std::uintptr_t target, long value) noexcept
        {
            __try
            {
                _InterlockedExchange(reinterpret_cast<volatile long *>(target), value);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Applies the engine's inline persistent-render-object invalidation.
         * @param node The render node.
         * @return True when the node had temp data and the stamps were written.
         */
        [[nodiscard]] bool guarded_invalidate(std::uintptr_t node) noexcept
        {
            const auto temp =
                DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::RENDERNODE_TEMP_DATA_OFFSET});
            if (!temp || !DMK::memory::is_plausible_ptr(DMK::Address{*temp}))
            {
                return false;
            }
            const auto valid =
                DMK::memory::read<std::int32_t>(DMK::Address{*temp + constants::TEMP_DATA_STAMP_SOURCE_OFFSET});
            if (!valid || !guarded_exchange_stamp(*temp + constants::TEMP_DATA_STAMP_TARGET_OFFSET, *valid))
            {
                return false;
            }
            return DMK::memory::write_in_place<std::int32_t>(
                       DMK::Address{node + constants::RENDERNODE_INVALIDATE_STAMP_OFFSET},
                       -1
            )
                .has_value();
        }

        /**
         * @brief Atomically sets or clears bits of a render node's flag qword under SEH.
         * @param node The render node.
         * @param bits The bits.
         * @param set True to OR the bits in, false to clear them.
         * @return True when the write landed.
         */
        [[nodiscard]] bool guarded_update_flags(std::uintptr_t node, std::uint64_t bits, bool set) noexcept
        {
            __try
            {
                auto *flags = reinterpret_cast<volatile long long *>(node + constants::RENDERNODE_RNDFLAGS_OFFSET);
                if (set)
                {
                    _InterlockedOr64(flags, static_cast<long long>(bits));
                }
                else
                {
                    _InterlockedAnd64(flags, static_cast<long long>(~bits));
                }
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief The two validated 3D-engine registration calls.
         */
        struct RegistrationCalls
        {
            std::uintptr_t engine{0};
            std::uintptr_t unregister_fn{0};
            std::uintptr_t register_fn{0};
        };

        /**
         * @brief Resolves p3DEngine and validates its RegisterEntity / UnRegisterEntityDirect slots.
         * @return The calls, with engine == 0 when unavailable.
         */
        [[nodiscard]] RegistrationCalls registration_calls() noexcept
        {
            RegistrationCalls calls{};
            const std::uintptr_t engine = genv_interface(constants::GENV_3DENGINE_OFFSET);
            if (engine == 0 || !object_is(GameClass::ThreeDEngine, engine))
            {
                return calls;
            }
            calls.unregister_fn = validated_vtable_slot(
                engine,
                constants::ENGINE_3D_VTABLE_UNREGISTER_ENTITY_DIRECT_OFFSET,
                AnchorId::UnRegisterEntity
            );
            calls.register_fn = validated_vtable_slot(
                engine,
                constants::ENGINE_3D_VTABLE_REGISTER_ENTITY_OFFSET,
                AnchorId::RegisterEntity
            );
            if (calls.unregister_fn != 0 && calls.register_fn != 0)
            {
                calls.engine = engine;
            }
            return calls;
        }

        /**
         * @brief Reads a render node's flag qword.
         * @param node The render node.
         * @return The flags, or std::nullopt.
         */
        [[nodiscard]] std::optional<std::uint64_t> read_render_flags(std::uintptr_t node) noexcept
        {
            const auto flags =
                DMK::memory::read<std::uint64_t>(DMK::Address{node + constants::RENDERNODE_RNDFLAGS_OFFSET});
            if (!flags)
            {
                return std::nullopt;
            }
            return *flags;
        }

        /**
         * @brief Writes a render node's view-distance ratio: exactly one byte, the bytes around it are the engine's.
         * @return True when the write landed.
         */
        [[nodiscard]] bool write_view_dist_ratio(std::uintptr_t node, std::uint8_t ratio) noexcept
        {
            return DMK::memory::write_in_place(DMK::Address{node + constants::RENDERNODE_VIEW_DIST_RATIO_OFFSET}, ratio)
                .has_value();
        }

        /**
         * @brief Reads a render node's octree node, which is null while the node is not filed in the octree.
         * @return The octree node, or std::nullopt on a failed read.
         */
        [[nodiscard]] std::optional<std::uintptr_t> read_octree_node(std::uintptr_t node) noexcept
        {
            const auto octree_node =
                DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::RENDERNODE_OCTREE_NODE_OFFSET});
            if (!octree_node)
            {
                return std::nullopt;
            }
            return *octree_node;
        }

        /**
         * @brief Moves a render node out of the octree into the 3D engine's always-visible list.
         * @details The call unregisters the node, sets ERF_RENDER_ALWAYS, and registers the node again. The node no
         *          longer owns an octree node, so the registration takes the full path, which files a flagged node in
         *          the always-visible list. That list skips the occlusion test and keeps the frustum and view-distance
         *          tests.
         * @param node The render node.
         * @param view_dist_ratio A ratio to write while the node is unregistered, so the registration computes the
         *        node's reach from it. std::nullopt keeps the node's ratio.
         * @return The outcome of the move. The ratio write is best-effort, and the callers read the ratio back.
         */
        [[nodiscard]] RenderAlwaysResult
        apply_render_always(std::uintptr_t node, std::optional<std::uint8_t> view_dist_ratio) noexcept
        {
            const std::optional<std::uint64_t> flags = read_render_flags(node);
            if (!flags)
            {
                return RenderAlwaysResult::Failed;
            }
            if ((*flags & constants::ERF_NO_3DENGINE_REGISTRATION) != 0)
            {
                return RenderAlwaysResult::NotRegistered;
            }
            if ((*flags & constants::ERF_RENDER_ALWAYS) != 0)
            {
                return RenderAlwaysResult::AlreadyAlways;
            }
            // A node without an octree node is not registered (streamed out or hidden). Registering it makes the mod
            // the owner of its registration, so the node stays untouched.
            const std::optional<std::uintptr_t> octree_node = read_octree_node(node);
            if (!octree_node || *octree_node == 0)
            {
                return RenderAlwaysResult::NotRegistered;
            }
            const RegistrationCalls calls = registration_calls();
            if (calls.engine == 0)
            {
                return RenderAlwaysResult::Failed;
            }
            if (!call_node_fn(calls.unregister_fn, calls.engine, node))
            {
                return RenderAlwaysResult::Failed;
            }
            const bool flagged = guarded_update_flags(node, constants::ERF_RENDER_ALWAYS, true);
            if (view_dist_ratio.has_value())
            {
                (void)write_view_dist_ratio(node, *view_dist_ratio);
            }
            // Re-register even when the flag write failed, so the node is never left unregistered.
            const bool registered = call_node_fn(calls.register_fn, calls.engine, node);
            return flagged && registered ? RenderAlwaysResult::Applied : RenderAlwaysResult::Failed;
        }

        /**
         * @brief Moves a render node back from the always-visible list into the octree.
         * @details UnRegisterEntityDirect erases the node from the always-visible list only while the node carries
         *          ERF_RENDER_ALWAYS, so the call sets the bit first. It then unregisters the node, clears the bit, and
         *          registers the node into the octree.
         * @param node The render node.
         * @param view_dist_ratio A ratio to write while the node is unregistered, or std::nullopt to keep the node's
         *        ratio.
         * @return Applied when the node left the list, Failed otherwise. The ratio write is best-effort.
         */
        [[nodiscard]] RenderAlwaysResult
        remove_render_always(std::uintptr_t node, std::optional<std::uint8_t> view_dist_ratio) noexcept
        {
            const RegistrationCalls calls = registration_calls();
            if (calls.engine == 0)
            {
                return RenderAlwaysResult::Failed;
            }
            if (!guarded_update_flags(node, constants::ERF_RENDER_ALWAYS, true))
            {
                return RenderAlwaysResult::Failed;
            }
            if (!call_node_fn(calls.unregister_fn, calls.engine, node))
            {
                return RenderAlwaysResult::Failed;
            }
            const bool cleared = guarded_update_flags(node, constants::ERF_RENDER_ALWAYS, false);
            if (view_dist_ratio.has_value())
            {
                (void)write_view_dist_ratio(node, *view_dist_ratio);
            }
            const bool registered = call_node_fn(calls.register_fn, calls.engine, node);
            return cleared && registered ? RenderAlwaysResult::Applied : RenderAlwaysResult::Failed;
        }

        /**
         * @brief Writes a render node's view-distance ratio so that the 3D engine applies it at once.
         * @details RegisterEntity recomputes the node's max view distance from the ratio. The octree insert then raises
         *          the max view distance of each octree cell on the node's path, and a node with a long reach stays in
         *          a large cell. The engine culls a whole cell past the cell's own max view distance, so a byte write
         *          alone does not show a raised node. The call unregisters the node, writes the ratio, and registers
         *          the node again, which files it where it was.
         *
         *          Two nodes get the byte without a registration: a node that the engine does not hold now (streamed
         *          out or hidden), and a node that another owner keeps in the always-visible list. Registering either
         *          node makes the mod the owner of its registration. The next registration of such a node computes
         *          its reach from the byte. A node that the engine never registers stays untouched.
         * @param node The render node.
         * @param ratio The ratio to write.
         * @param always_visible_by_mod The mod moved @p node into the always-visible list. The node is registered then,
         *        although it owns no octree node.
         * @note Best-effort. The callers read the ratio back to learn what is in place.
         */
        void set_view_dist_ratio(std::uintptr_t node, std::uint8_t ratio, bool always_visible_by_mod) noexcept
        {
            const std::optional<std::uint64_t> flags = read_render_flags(node);
            if (!flags || (*flags & constants::ERF_NO_3DENGINE_REGISTRATION) != 0)
            {
                return;
            }
            const bool always_visible = (*flags & constants::ERF_RENDER_ALWAYS) != 0;
            if (always_visible_by_mod && !always_visible)
            {
                // UnRegisterEntityDirect leaves a node without the bit in the always-visible list. The callers move
                // such a node back into the octree first.
                return;
            }
            if (!always_visible_by_mod)
            {
                const std::optional<std::uintptr_t> octree_node = read_octree_node(node);
                if (!octree_node)
                {
                    return;
                }
                if (always_visible || *octree_node == 0)
                {
                    (void)write_view_dist_ratio(node, ratio);
                    return;
                }
            }
            const RegistrationCalls calls = registration_calls();
            if (calls.engine == 0 || !call_node_fn(calls.unregister_fn, calls.engine, node))
            {
                return;
            }
            (void)write_view_dist_ratio(node, ratio);
            // Re-register even when the write failed, so the node is never left unregistered.
            (void)call_node_fn(calls.register_fn, calls.engine, node);
        }

        /**
         * @brief A referenced engine iterator and its validated calls.
         */
        struct IteratorCalls
        {
            std::uintptr_t iterator{0};
            std::uintptr_t release{0};
            std::uintptr_t is_end{0};
            std::uintptr_t next{0};
        };

        /**
         * @brief Creates an engine iterator and takes one reference on it.
         * @return The iterator, or std::nullopt when it could not be created or referenced.
         */
        [[nodiscard]] std::optional<IteratorCalls>
        acquire_iterator(std::uintptr_t system, std::uintptr_t get_iterator) noexcept
        {
            const std::uintptr_t iterator = call_get_iterator(get_iterator, system);
            if (iterator == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{iterator}))
            {
                return std::nullopt;
            }
            const std::uintptr_t add_ref = read_vtable_slot(iterator, constants::ENTITY_IT_VTABLE_ADD_REF_OFFSET);
            const IteratorCalls calls{
                .iterator = iterator,
                .release = read_vtable_slot(iterator, constants::ENTITY_IT_VTABLE_RELEASE_OFFSET),
                .is_end = read_vtable_slot(iterator, constants::ENTITY_IT_VTABLE_IS_END_OFFSET),
                .next = read_vtable_slot(iterator, constants::ENTITY_IT_VTABLE_NEXT_OFFSET),
            };
            if (add_ref == 0 || calls.release == 0 || calls.is_end == 0 || calls.next == 0)
            {
                return std::nullopt;
            }
            // The iterator starts at reference count zero; holding one reference keeps it alive for the walk and the
            // matching Release deletes it.
            if (!call_iterator_void(add_ref, iterator))
            {
                return std::nullopt;
            }
            return calls;
        }

        /**
         * @brief Checks the direct salt-buffer walk against the engine iterator: both must start at the same entity.
         * @return 1 when they agree, -1 when they differ twice in a row, 0 when the check could not run.
         */
        [[nodiscard]] int check_direct_layout(std::uintptr_t system, std::uintptr_t get_iterator) noexcept
        {
            constexpr int attempts = 2;
            std::uintptr_t from_iterator = 0;
            std::uintptr_t direct = 0;
            for (int attempt = 0; attempt < attempts; ++attempt)
            {
                const std::optional<IteratorCalls> calls = acquire_iterator(system, get_iterator);
                if (!calls)
                {
                    return 0;
                }
                from_iterator = 0;
                const bool read = call_iterator_is_end(calls->is_end, calls->iterator) == 0 &&
                                  call_iterator_next(calls->next, calls->iterator, &from_iterator);
                (void)call_iterator_void(calls->release, calls->iterator);
                if (!read || from_iterator == 0)
                {
                    return 0;
                }
                // A spawn on another thread between the two reads can move the head, so a mismatch is tried again.
                direct = first_salt_entity(system);
                if (direct == from_iterator)
                {
                    (void)DMK::log().try_log(
                        DMK::LogLevel::Info,
                        "EntityAccess: entity walks read the salt buffer directly (first entity 0x{:016X} matches "
                        "the engine iterator)",
                        direct
                    );
                    return 1;
                }
            }
            (void)DMK::log().try_log(
                DMK::LogLevel::Warning,
                "EntityAccess: the salt buffer's first entity 0x{:016X} is not the engine iterator's 0x{:016X}; entity "
                "walks use the engine iterator",
                direct,
                from_iterator
            );
            return -1;
        }
    } // namespace

    void prefetch_entity(std::uintptr_t entity, bool with_matrix) noexcept
    {
        // The id and the class can sit on two lines when the entity is not line-aligned; a second prefetch of a line
        // already in flight costs nothing.
        prefetch_line(entity + constants::ENTITY_ID_OFFSET);
        prefetch_line(entity + constants::ENTITY_CLASS_OFFSET);
        if (with_matrix)
        {
            prefetch_line(entity + constants::ENTITY_WORLD_MATRIX_OFFSET);
            prefetch_line(entity + constants::ENTITY_WORLD_MATRIX_OFFSET + sizeof(game_structures::Matrix34f) - 1);
        }
    }

    DMK::Result<void> initialize_entity_access()
    {
        if (!feature_ready(Feature::EntityLookup))
        {
            DMK::log().warning("EntityAccess: the entity lookup gate failed; highlights are off");
            return std::unexpected(DMK::Error{DMK::ErrorCode::NoMatch, "entity_access/gate"});
        }
        constexpr Feature optional[] = {Feature::EntityBounds, Feature::EntityIteration, Feature::RenderRegistration};
        for (const Feature feature : optional)
        {
            if (!feature_ready(feature))
            {
                DMK::log().warning("EntityAccess: the {} gate failed; that part is disabled", feature_name(feature));
            }
        }
        s_available.store(true, std::memory_order_release);
        DMK::log().info(
            "EntityAccess: ready (GetEntity {}, GetProxy {}, GetWorldBounds {}, RegisterEntity {}, "
            "UnRegisterEntity {})",
            DMK::format::format_address(anchor_address(AnchorId::GetEntity)),
            DMK::format::format_address(anchor_address(AnchorId::GetProxy)),
            DMK::format::format_address(anchor_address(AnchorId::GetWorldBounds)),
            DMK::format::format_address(anchor_address(AnchorId::RegisterEntity)),
            DMK::format::format_address(anchor_address(AnchorId::UnRegisterEntity))
        );
        return {};
    }

    void shutdown_entity_access() noexcept
    {
        s_available.store(false, std::memory_order_release);
    }

    bool entity_access_available() noexcept
    {
        return s_available.load(std::memory_order_acquire);
    }

    void begin_entity_tick() noexcept
    {
        s_tick = TickCalls{.open = true};
    }

    void end_entity_tick() noexcept
    {
        s_tick = TickCalls{};
    }

    std::uintptr_t entity_from_id(EntityId id) noexcept
    {
        if (id == 0 || !entity_access_available())
        {
            return 0;
        }
        const EntityCalls calls = entity_calls();
        if (calls.get_entity == 0)
        {
            return 0;
        }
        const std::uintptr_t entity = call_get_entity(calls.get_entity, calls.system, id);
        if (entity == 0 || !object_is(GameClass::Entity, entity))
        {
            return 0;
        }
        return entity;
    }

    EntityLookup::EntityLookup() noexcept
    {
        if (!entity_access_available())
        {
            return;
        }
        const EntityCalls calls = entity_calls();
        m_get_entity = calls.get_entity;
        m_system = calls.get_entity != 0 ? calls.system : 0;
    }

    std::uintptr_t EntityLookup::find(EntityId id) const noexcept
    {
        if (id == 0 || m_system == 0)
        {
            return 0;
        }
        const std::uintptr_t entity = call_get_entity(m_get_entity, m_system, id);
        return entity != 0 && object_is(GameClass::Entity, entity) ? entity : 0;
    }

    EntityId entity_id_of(std::uintptr_t entity) noexcept
    {
        const auto id = DMK::memory::read<std::uint32_t>(DMK::Address{entity + constants::ENTITY_ID_OFFSET});
        return id ? *id : 0;
    }

    std::uintptr_t render_node_of(std::uintptr_t entity) noexcept
    {
        if (entity == 0)
        {
            return 0;
        }
        const std::uintptr_t fn =
            validated_vtable_slot(entity, constants::ENTITY_VTABLE_GET_PROXY_OFFSET, AnchorId::GetProxy);
        if (fn == 0)
        {
            return 0;
        }
        const std::uintptr_t proxy = call_get_proxy(fn, entity, static_cast<int>(constants::ENTITY_PROXY_RENDER));
        if (proxy <= static_cast<std::uintptr_t>(constants::RENDER_PROXY_SECONDARY_OFFSET))
        {
            return 0;
        }
        const std::uintptr_t node = proxy - constants::RENDER_PROXY_SECONDARY_OFFSET;
        return object_is(GameClass::RenderProxy, node) ? node : 0;
    }

    std::uintptr_t entity_proxy(std::uintptr_t entity, std::uint32_t type) noexcept
    {
        if (entity == 0)
        {
            return 0;
        }
        const std::uintptr_t fn =
            validated_vtable_slot(entity, constants::ENTITY_VTABLE_GET_PROXY_OFFSET, AnchorId::GetProxy);
        if (fn == 0)
        {
            return 0;
        }
        const std::uintptr_t proxy = call_get_proxy(fn, entity, static_cast<int>(type));
        return DMK::memory::is_plausible_ptr(DMK::Address{proxy}) ? proxy : 0;
    }

    bool entity_is_hidden(std::uintptr_t entity) noexcept
    {
        const auto flags = DMK::memory::read<std::uint32_t>(DMK::Address{entity + constants::ENTITY_FLAGS_OFFSET});
        return !flags || (*flags & constants::ENTITY_FLAG_HIDDEN) != 0;
    }

    std::optional<std::uint32_t> entity_flags(std::uintptr_t entity) noexcept
    {
        if (entity == 0)
        {
            return std::nullopt;
        }
        const auto flags = DMK::memory::read<std::uint32_t>(DMK::Address{entity + constants::ENTITY_FLAGS_OFFSET});
        return flags ? std::optional<std::uint32_t>{*flags} : std::nullopt;
    }

    bool entity_flags_game_hidden(std::uint32_t flags) noexcept
    {
        return (flags & constants::ENTITY_INVISIBLE_FLAG) != 0 && (flags & constants::ENTITY_FLAG_HIDDEN) == 0;
    }

    bool entity_flags_active(std::uint32_t flags) noexcept
    {
        return (flags & constants::ENTITY_ACTIVE_FLAG) != 0;
    }

    std::string entity_class_name(std::uintptr_t entity)
    {
        const auto klass = DMK::memory::read<std::uintptr_t>(DMK::Address{entity + constants::ENTITY_CLASS_OFFSET});
        if (!klass || !DMK::memory::is_plausible_ptr(DMK::Address{*klass}))
        {
            return {};
        }
        const auto name = DMK::memory::read<std::uintptr_t>(DMK::Address{*klass + constants::ENTITY_CLASS_NAME_OFFSET});
        if (!name || !DMK::memory::is_plausible_ptr(DMK::Address{*name}))
        {
            return {};
        }
        return read_c_string(*name, MAX_NAME_LENGTH);
    }

    std::string entity_name(std::uintptr_t entity)
    {
        const auto name = DMK::memory::read<std::uintptr_t>(DMK::Address{entity + constants::ENTITY_NAME_OFFSET});
        if (!name || !DMK::memory::is_plausible_ptr(DMK::Address{*name}))
        {
            return {};
        }
        return read_c_string(*name, MAX_NAME_LENGTH);
    }

    std::string entity_model_path(std::uintptr_t entity)
    {
        constexpr std::size_t max_slots = 8;
        const std::uintptr_t node = render_node_of(entity);
        if (node == 0)
        {
            return {};
        }
        const auto begin =
            DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::RENDER_PROXY_SLOTS_BEGIN_OFFSET});
        const auto end =
            DMK::memory::read<std::uintptr_t>(DMK::Address{node + constants::RENDER_PROXY_SLOTS_END_OFFSET});
        if (!begin || !end || *end < *begin || (*end - *begin) / sizeof(std::uintptr_t) > 64)
        {
            return {};
        }
        auto copy_path = [](std::uintptr_t text_slot) -> std::string
        {
            const auto text = DMK::memory::read<std::uintptr_t>(DMK::Address{text_slot});
            if (!text || !DMK::memory::is_plausible_ptr(DMK::Address{*text}))
            {
                return {};
            }
            return read_c_string(*text, MAX_PATH_LENGTH);
        };
        std::size_t visited = 0;
        for (std::uintptr_t cursor = *begin; cursor < *end && visited < max_slots;
             cursor += sizeof(std::uintptr_t), ++visited)
        {
            const auto slot = DMK::memory::read<std::uintptr_t>(DMK::Address{cursor});
            if (!slot || !DMK::memory::is_plausible_ptr(DMK::Address{*slot}))
            {
                continue;
            }
            const auto stat_obj =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*slot + constants::ENTITY_SLOT_STAT_OBJ_OFFSET});
            if (stat_obj && DMK::memory::is_plausible_ptr(DMK::Address{*stat_obj}))
            {
                if (std::string path = copy_path(*stat_obj + constants::STAT_OBJ_FILE_PATH_OFFSET); !path.empty())
                {
                    return path;
                }
            }
            const auto character =
                DMK::memory::read<std::uintptr_t>(DMK::Address{*slot + constants::ENTITY_SLOT_CHARACTER_OFFSET});
            if (character && object_is(GameClass::CharInstance, *character))
            {
                if (std::string path = copy_path(*character + constants::CHAR_INSTANCE_FILE_PATH_OFFSET); !path.empty())
                {
                    return path;
                }
            }
        }
        return {};
    }

    std::optional<game_structures::Vec3f> entity_world_position(std::uintptr_t entity) noexcept
    {
        const auto matrix =
            DMK::memory::read<game_structures::Matrix34f>(DMK::Address{entity + constants::ENTITY_WORLD_MATRIX_OFFSET});
        if (!matrix)
        {
            return std::nullopt;
        }
        return game_structures::Vec3f{matrix->m[0][3], matrix->m[1][3], matrix->m[2][3]};
    }

    std::optional<game_structures::Aabb> entity_world_bounds(std::uintptr_t entity) noexcept
    {
        const std::uintptr_t fn =
            validated_vtable_slot(entity, constants::ENTITY_VTABLE_GET_WORLD_BOUNDS_OFFSET, AnchorId::GetWorldBounds);
        if (fn == 0)
        {
            return std::nullopt;
        }
        game_structures::Aabb bounds{};
        if (!call_get_world_bounds(fn, entity, &bounds))
        {
            return std::nullopt;
        }
        // An entity without geometry reports the reset box (min = +1e15, max = -1e15).
        if (!(bounds.min.x <= bounds.max.x && bounds.min.y <= bounds.max.y && bounds.min.z <= bounds.max.z))
        {
            return std::nullopt;
        }
        return bounds;
    }

    std::optional<std::uint32_t> read_hud_word(std::uintptr_t node) noexcept
    {
        const auto word =
            DMK::memory::read<std::uint32_t>(DMK::Address{node + constants::RENDERNODE_HUD_SILHOUETTE_OFFSET});
        if (!word)
        {
            return std::nullopt;
        }
        return *word;
    }

    bool write_hud_word(std::uintptr_t node, std::uint32_t word) noexcept
    {
        // Exactly 4 bytes: the qword at +0x40 is an engine atomic.
        return DMK::memory::write_in_place(DMK::Address{node + constants::RENDERNODE_HUD_SILHOUETTE_OFFSET}, word)
            .has_value();
    }

    bool invalidate_render_object(std::uintptr_t node) noexcept
    {
        if (node == 0)
        {
            return false;
        }
        return guarded_invalidate(node);
    }

    std::optional<bool> read_render_always(std::uintptr_t node) noexcept
    {
        const std::optional<std::uint64_t> flags = read_render_flags(node);
        if (!flags)
        {
            return std::nullopt;
        }
        return (*flags & constants::ERF_RENDER_ALWAYS) != 0;
    }

    std::optional<float> read_max_view_dist(std::uintptr_t node) noexcept
    {
        const auto distance = DMK::memory::read<float>(DMK::Address{node + constants::RENDERNODE_MAX_VIEW_DIST_OFFSET});
        if (!distance)
        {
            return std::nullopt;
        }
        return *distance;
    }

    std::optional<std::uint8_t> read_view_dist_ratio(std::uintptr_t node) noexcept
    {
        const auto ratio =
            DMK::memory::read<std::uint8_t>(DMK::Address{node + constants::RENDERNODE_VIEW_DIST_RATIO_OFFSET});
        if (!ratio)
        {
            return std::nullopt;
        }
        return *ratio;
    }

    std::optional<RenderAlwaysResult> update_render_node_overrides(
        std::uintptr_t node,
        RenderNodeOverrides &overrides,
        const RenderNodeWants &wants
    ) noexcept
    {
        constexpr std::uint8_t far_ratio = constants::VIEW_DIST_RATIO_FAR;
        const std::optional<std::uint8_t> ratio = read_view_dist_ratio(node);
        // A ratio that the game rewrote during a raise belongs to the game. It becomes the node's own ratio, which a
        // restore writes back, and the raise below writes the far ratio again.
        if (overrides.own_view_dist_ratio.has_value() && ratio.has_value() && *ratio != far_ratio)
        {
            overrides.own_view_dist_ratio.reset();
        }
        // A node that the game itself holds at the far ratio needs no raise and nothing restored.
        const bool raise = wants.raise_view_distance && !overrides.own_view_dist_ratio.has_value() &&
                           ratio.has_value() && *ratio != far_ratio;
        const bool lower = !wants.raise_view_distance && overrides.own_view_dist_ratio.has_value();
        std::optional<std::uint8_t> target{};
        if (raise)
        {
            target = far_ratio;
        }
        else if (lower)
        {
            target = overrides.own_view_dist_ratio;
        }

        // A move into or out of the always-visible list writes the ratio inside its own re-registration.
        std::optional<RenderAlwaysResult> moved{};
        if (wants.always_visible && !overrides.always_visible)
        {
            moved = apply_render_always(node, target);
            overrides.always_visible = *moved == RenderAlwaysResult::Applied;
        }
        else if (!wants.always_visible && overrides.always_visible)
        {
            moved = remove_render_always(node, target);
            overrides.always_visible = false;
        }
        if (!target.has_value())
        {
            return moved;
        }
        // Without a move, or after a move that stopped before its write, the ratio takes a re-registration of its own.
        std::optional<std::uint8_t> placed = read_view_dist_ratio(node);
        if (placed.has_value() && *placed != *target)
        {
            set_view_dist_ratio(node, *target, overrides.always_visible);
            placed = read_view_dist_ratio(node);
        }
        // The ratio read back decides the bookkeeping, so a raise that did not land leaves nothing to restore.
        if (raise && placed == far_ratio)
        {
            overrides.own_view_dist_ratio = ratio;
        }
        else if (lower && placed.has_value() && *placed != far_ratio)
        {
            overrides.own_view_dist_ratio.reset();
        }
        return moved;
    }

    std::optional<RenderAlwaysResult>
    restore_render_node_overrides(std::uintptr_t node, RenderNodeOverrides &overrides) noexcept
    {
        std::optional<std::uint8_t> own_ratio = overrides.own_view_dist_ratio;
        // A ratio that the game rewrote during the raise belongs to the game and stays.
        if (own_ratio.has_value() && read_view_dist_ratio(node) != constants::VIEW_DIST_RATIO_FAR)
        {
            own_ratio.reset();
        }
        std::optional<RenderAlwaysResult> moved{};
        if (overrides.always_visible)
        {
            moved = remove_render_always(node, own_ratio);
        }
        else if (own_ratio.has_value())
        {
            set_view_dist_ratio(node, *own_ratio, false);
        }
        overrides = RenderNodeOverrides{};
        return moved;
    }

    bool EntityWalk::begin() noexcept
    {
        cancel();
        if (!entity_access_available())
        {
            return false;
        }
        const std::uintptr_t system = genv_interface(constants::GENV_ENTITY_SYSTEM_OFFSET);
        if (system == 0 || !object_is(GameClass::EntitySystem, system))
        {
            return false;
        }
        const std::uintptr_t get_iterator = validated_vtable_slot(
            system,
            constants::ENTITY_SYSTEM_VTABLE_GET_ITERATOR_OFFSET,
            AnchorId::GetEntityIterator
        );
        if (get_iterator == 0)
        {
            return false;
        }
        if (s_direct_layout == 0)
        {
            s_direct_layout = check_direct_layout(system, get_iterator);
        }
        if (s_direct_layout > 0)
        {
            m_direct_system = system;
            m_slot = 1;
            return true;
        }
        const std::optional<IteratorCalls> calls = acquire_iterator(system, get_iterator);
        if (!calls)
        {
            return false;
        }
        m_iterator = calls->iterator;
        m_is_end = calls->is_end;
        m_next = calls->next;
        m_release = calls->release;
        return true;
    }

    EntityWalk::Step EntityWalk::step_direct(std::vector<WalkedEntity> &out, std::size_t max_entities) noexcept
    {
        // The slots are read in index order, in chunks, rather than along the used list: the list visits the 6 MB
        // array in allocation order, one dependent miss per entity, while the array in order streams. DeleteEntity
        // clears a slot's entity before it frees the slot, so a slot holding an entity is a live one. The class comes
        // from the slot cache, so only a new entity is read (a walk of ~98k entities: ~27 ms along the list, ~12 ms in
        // slot order, well under that once the cache knows the level).
        constexpr std::uint32_t chunSLOTS = 64;
        std::array<SaltSlotWords, chunSLOTS> chunk{};
        const std::size_t first_new = out.size();
        const std::size_t wanted = out.size() + max_entities;
        try
        {
            while (m_slot != 0 && out.size() < wanted)
            {
                const std::uint32_t count = std::min<std::uint32_t>(chunSLOTS, constants::SALT_LAST_SLOT + 1 - m_slot);
                const std::uintptr_t first = m_direct_system + constants::ENTITY_SYSTEM_SALT_BUFFER_OFFSET +
                                             m_slot * constants::SALT_SLOT_STRIDE;
                if (!DMK::memory::read_into(
                        DMK::Address{first},
                        std::as_writable_bytes(std::span{chunk.data(), count})
                    ))
                {
                    cancel();
                    return Step::Failed;
                }
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    if (const auto entity = static_cast<std::uintptr_t>(chunk[i][2]); entity != 0)
                    {
                        const std::uint32_t slot = m_slot + i;
                        const auto salt = static_cast<std::uint32_t>(chunk[i][0] & 0xFFFFu);
                        const EntityId id = slot | (salt << constants::SALT_ID_SALT_SHIFT);
                        out.push_back(WalkedEntity{entity, cached_class(slot, entity, id), id});
                    }
                }
                m_slot = m_slot + count <= constants::SALT_LAST_SLOT ? m_slot + count : 0;
            }
            resolve_class_misses(out, first_new);
        }
        catch (...)
        {
            cancel();
            return Step::Failed;
        }
        if (m_slot == 0)
        {
            cancel();
            return Step::Done;
        }
        return Step::More;
    }

    EntityWalk::Step EntityWalk::step(std::vector<WalkedEntity> &out, std::size_t max_entities) noexcept
    {
        if (m_direct_system != 0)
        {
            return step_direct(out, max_entities);
        }
        if (m_iterator == 0)
        {
            return Step::Failed;
        }
        for (std::size_t taken = 0; taken < max_entities; ++taken)
        {
            const int end = call_iterator_is_end(m_is_end, m_iterator);
            if (end != 0)
            {
                const Step result = end == 1 ? Step::Done : Step::Failed;
                cancel();
                return result;
            }
            std::uintptr_t entity = 0;
            if (!call_iterator_next(m_next, m_iterator, &entity))
            {
                cancel();
                return Step::Failed;
            }
            if (entity != 0)
            {
                try
                {
                    out.push_back(WalkedEntity{entity, read_entity_class(entity), entity_id_of(entity)});
                }
                catch (...)
                {
                    cancel();
                    return Step::Failed;
                }
            }
        }
        return Step::More;
    }

    void EntityWalk::cancel() noexcept
    {
        if (m_iterator != 0)
        {
            (void)call_iterator_void(m_release, m_iterator);
        }
        m_iterator = 0;
        m_is_end = 0;
        m_next = 0;
        m_release = 0;
        m_direct_system = 0;
        m_slot = 0;
    }

    std::optional<EntityCounters> read_entity_counters() noexcept
    {
        if (!entity_access_available())
        {
            return std::nullopt;
        }
        const std::uintptr_t system = entity_calls().system;
        if (system == 0)
        {
            return std::nullopt;
        }
        // The three insert counters and the delete counter, in one guarded read. No engine lock is taken: a counter
        // read while a writer runs is at most one behind, which the caller's one-tick lag absorbs.
        const auto words = DMK::memory::read<std::array<std::uint64_t, constants::SALT_INSERT_COUNTERS + 1>>(
            DMK::Address{system + constants::ENTITY_SYSTEM_SALT_COUNTERS_OFFSET}
        );
        if (!words)
        {
            return std::nullopt;
        }
        EntityCounters counters{};
        for (std::size_t i = 0; i < constants::SALT_INSERT_COUNTERS; ++i)
        {
            counters.inserts += (*words)[i];
        }
        counters.deletes = (*words)[constants::SALT_INSERT_COUNTERS];
        return counters;
    }

    RecentWalk walk_recent_entities(std::size_t count, std::vector<WalkedEntity> &out) noexcept
    {
        DMK_PROFILE_FUNCTION();
        // The used list is read only where the direct walk's layout check agreed with the engine iterator.
        if (s_direct_layout <= 0 || !entity_access_available())
        {
            return RecentWalk::Failed;
        }
        const std::uintptr_t system = entity_calls().system;
        if (system == 0)
        {
            return RecentWalk::Failed;
        }
        const std::size_t first = out.size();
        bool settled = true;
        try
        {
            // No engine lock: a node unlinked under the walk leads into the free list, whose slots hold no entity
            // (unsettled, retried), and the count bounds the walk whatever the links say.
            std::uint32_t slot = salt_head(system);
            for (std::size_t visited = 0; slot != 0 && visited < count; ++visited)
            {
                const std::optional<SaltSlotWords> words = read_salt_slot(system, slot);
                if (!words)
                {
                    out.resize(first);
                    return RecentWalk::Failed;
                }
                if (const auto entity = static_cast<std::uintptr_t>((*words)[2]); entity != 0)
                {
                    const auto salt = static_cast<std::uint32_t>((*words)[0] & 0xFFFFu);
                    const EntityId id = slot | (salt << constants::SALT_ID_SALT_SHIFT);
                    out.push_back(WalkedEntity{entity, cached_class(slot, entity, id), id});
                }
                else
                {
                    // A slot being allocated (its entity not stored yet) or freed right now.
                    settled = false;
                }
                const std::uint64_t next = (*words)[1] & 0xFFFFFFFFu;
                slot = usable_slot(next) ? static_cast<std::uint32_t>(next) : 0;
            }
            resolve_class_misses(out, first);
        }
        catch (...)
        {
            out.resize(first);
            return RecentWalk::Failed;
        }
        return settled ? RecentWalk::Complete : RecentWalk::Unsettled;
    }

} // namespace HenrySenses
