/**
 * @file engine/game_natives.cpp
 * @brief The game's gameplay predicates reached natively (see game_natives.hpp for the object map).
 */

#include "engine/game_natives.hpp"
#include "aob_resolver.hpp"
#include "constants.hpp"
#include "global_state.hpp"
#include "rtti_types.hpp"
#include "engine/engine_env.hpp"
#include "engine/entity_access.hpp"
#include "engine/seh.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace HenrySenses
{
    namespace
    {
        // Safety bounds on the engine structures walked here.
        constexpr std::size_t MAX_TREE_DEPTH = 64;
        constexpr std::size_t MAX_SHOPS = 4096;
        constexpr std::size_t MAX_SCRIPT_CONTEXTS = 4096;
        constexpr std::size_t MAX_CONTEXT_NAME = 128;

        /**
         * @brief Calls @p fn(args...) under SEH.
         * @return True when the call returned.
         */
        template <typename R, typename... A>
        [[nodiscard]] bool guarded_call(std::uintptr_t fn, R *out, A... args) noexcept
        {
            __try
            {
                *out = reinterpret_cast<R(__fastcall *)(A...)>(fn)(args...);
                return true;
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
                return false;
            }
        }

        /**
         * @brief Calls the virtual at @p slot of @p object, whose vtable and target must lie in the game image.
         * @return The result, or std::nullopt when the slot is not plausible or the call faulted.
         */
        template <typename R, typename... A>
        [[nodiscard]] std::optional<R> vcall(std::uintptr_t object, std::ptrdiff_t slot, A... args) noexcept
        {
            const std::uintptr_t fn = read_vtable_slot(object, slot);
            if (fn == 0)
            {
                return std::nullopt;
            }
            R result{};
            if (!guarded_call<R>(fn, &result, object, args...))
            {
                return std::nullopt;
            }
            return result;
        }

        /** @brief A plausible, 8-byte aligned pointer: the structural screen before a guarded read of an engine node.
         */
        [[nodiscard]] bool node_pointer(std::uintptr_t p) noexcept
        {
            return DMK::memory::is_plausible_ptr(DMK::Address{p}) && (p & 7) == 0;
        }

        /** @brief The entity's world position is within @p reach_sq (squared metres) of @p center. */
        [[nodiscard]] bool
        entity_within(std::uintptr_t entity, const game_structures::Vec3f &center, float reach_sq) noexcept
        {
            if (!node_pointer(entity))
            {
                return false;
            }
            const std::optional<game_structures::Vec3f> position = entity_world_position(entity);
            if (!position.has_value())
            {
                return false;
            }
            const float dx = position->x - center.x;
            const float dy = position->y - center.y;
            const float dz = position->z - center.z;
            return dx * dx + dy * dy + dz * dz <= reach_sq;
        }

        // A node of the actor system's list, read whole: next, prev, then the (EntityId, IActor *) pair.
        using ActorNodeWords = std::array<std::uintptr_t, 4>;
        constexpr std::size_t ACTOR_NODE_NEXT = 0;
        constexpr std::size_t ACTOR_NODE_KEY = constants::ACTOR_NODE_KEY_OFFSET / sizeof(std::uintptr_t);
        constexpr std::size_t ACTOR_NODE_VALUE = constants::ACTOR_NODE_VALUE_OFFSET / sizeof(std::uintptr_t);
        static_assert(
            constants::ACTOR_NODE_KEY_OFFSET % sizeof(std::uintptr_t) == 0 &&
                constants::ACTOR_NODE_VALUE_OFFSET % sizeof(std::uintptr_t) == 0 &&
                ACTOR_NODE_VALUE < std::tuple_size_v<ActorNodeWords>,
            "The actor node fields must be whole words of ActorNodeWords."
        );

        // A node of the item system's map, read whole: left, parent, right, the colour and nil bytes, then the
        // (EntityId, C_PickableItem *) pair.
        using ItemNodeWords = std::array<std::uintptr_t, 6>;
        constexpr std::size_t ITEM_NODE_LEFT = 0;
        constexpr std::size_t ITEM_NODE_RIGHT = 2;
        constexpr std::size_t ITEM_NODE_KEY = constants::ITEM_NODE_KEY_OFFSET / sizeof(std::uintptr_t);
        constexpr std::size_t ITEM_NODE_VALUE = constants::ITEM_NODE_VALUE_OFFSET / sizeof(std::uintptr_t);
        static_assert(
            constants::ITEM_NODE_KEY_OFFSET % sizeof(std::uintptr_t) == 0 &&
                constants::ITEM_NODE_VALUE_OFFSET % sizeof(std::uintptr_t) == 0 &&
                ITEM_NODE_VALUE < std::tuple_size_v<ItemNodeWords> &&
                constants::ITEM_NODE_IS_NIL_OFFSET < sizeof(ItemNodeWords),
            "The item node fields must lie inside ItemNodeWords."
        );

        /**
         * @brief One object of a level-wide map (an actor or a pickable item) on its way through the near pick.
         */
        struct NodeRef
        {
            EntityId id{0};
            std::uintptr_t object{0};
            std::uintptr_t entity{0};
        };

        // Main-thread scratch of the near picks, reused so a pick does not allocate.
        std::vector<NodeRef> s_actor_refs;
        std::vector<NodeRef> s_item_refs;

        /**
         * @class NodeHistory
         * @brief The node addresses a map walk read last time, in order, so this walk can prefetch its next nodes.
         * @details A linked walk cannot look ahead (each next pointer sits in the node being read), but a level's maps
         *          change little between two picks, so the node read k steps on is most likely the one read k steps on
         *          last time. A wrong guess costs only a wasted prefetch. Main thread only.
         */
        class NodeHistory
        {
        public:
            /** @brief Starts a walk. */
            void begin() noexcept { m_current.clear(); }

            /**
             * @brief Records the node about to be read and prefetches the one last walk read PREFETCH_DISTANCE on.
             * @param node The node.
             * @param size The bytes read from it.
             */
            void visit(std::uintptr_t node, std::size_t size)
            {
                if (const std::size_t ahead = m_current.size() + PREFETCH_DISTANCE; ahead < m_last.size())
                {
                    prefetch_line(m_last[ahead]);
                    prefetch_line(m_last[ahead] + size - 1);
                }
                m_current.push_back(node);
            }

            /** @brief Ends a walk: its nodes are the guesses of the next one. */
            void finish() noexcept { m_last.swap(m_current); }

        private:
            std::vector<std::uintptr_t> m_last;
            std::vector<std::uintptr_t> m_current;
        };

        NodeHistory s_actor_history;
        NodeHistory s_item_history;

        /**
         * @brief Reads each object's entity pointer, then keeps the objects whose entity lies within reach.
         * @details The map walk itself is a chain of dependent reads, but these two passes read independent objects,
         *          so each prefetches a few entries ahead and the cache misses overlap instead of adding up (the actor
         *          pick over ~2400 actors went from ~1.5 ms to ~1 ms, the item pick from ~0.8 ms to ~0.55 ms).
         * @param refs The objects in map order; left holding the ones within reach, in the same order.
         * @param entity_offset Where the object keeps its CEntity pointer.
         */
        void keep_within(
            std::vector<NodeRef> &refs,
            std::ptrdiff_t entity_offset,
            const game_structures::Vec3f &center,
            float reach_sq
        ) noexcept
        {
            const std::size_t count = refs.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                if (i + PREFETCH_DISTANCE < count)
                {
                    prefetch_line(refs[i + PREFETCH_DISTANCE].object + entity_offset);
                }
                const auto entity = DMK::memory::read<std::uintptr_t>(DMK::Address{refs[i].object + entity_offset});
                refs[i].entity = entity ? *entity : 0;
            }
            std::size_t kept = 0;
            for (std::size_t i = 0; i < count; ++i)
            {
                if (i + PREFETCH_DISTANCE < count)
                {
                    prefetch_entity(refs[i + PREFETCH_DISTANCE].entity, true);
                }
                if (entity_within(refs[i].entity, center, reach_sq))
                {
                    refs[kept++] = refs[i];
                }
            }
            refs.resize(kept);
        }

        /**
         * @brief Reads an item map node whole.
         * @param node The node.
         * @param words Receives its words.
         * @return True for a readable node that is not the map's nil sentinel.
         */
        [[nodiscard]] bool load_item_node(std::uintptr_t node, ItemNodeWords &words) noexcept
        {
            if (!node_pointer(node))
            {
                return false;
            }
            const auto read = DMK::memory::read<ItemNodeWords>(DMK::Address{node});
            if (!read)
            {
                return false;
            }
            words = *read;
            constexpr std::size_t nil_word = constants::ITEM_NODE_IS_NIL_OFFSET / sizeof(std::uintptr_t);
            constexpr std::size_t nil_shift = constants::ITEM_NODE_IS_NIL_OFFSET % sizeof(std::uintptr_t) * 8;
            return static_cast<std::uint8_t>(words[nil_word] >> nil_shift) == 0;
        }

        [[nodiscard]] std::uintptr_t read_ptr(std::uintptr_t address) noexcept
        {
            const auto value = DMK::memory::read<std::uintptr_t>(DMK::Address{address});
            return value && DMK::memory::is_plausible_ptr(DMK::Address{*value}) ? *value : 0;
        }

        /**
         * @brief Compares a NUL-terminated game string with @p name, reading it in chunks that never cross a page.
         */
        [[nodiscard]] bool cstring_equals(std::uintptr_t text, std::string_view name) noexcept
        {
            constexpr std::uintptr_t chunk_size = 16;
            std::size_t matched = 0;
            std::uintptr_t cursor = text;
            while (matched <= name.size())
            {
                std::array<char, chunk_size> chunk{};
                const std::size_t length = static_cast<std::size_t>(chunk_size - (cursor % chunk_size));
                if (!DMK::memory::read_into(
                        DMK::Address{cursor},
                        std::as_writable_bytes(std::span{chunk.data(), length})
                    ))
                {
                    return false;
                }
                for (std::size_t i = 0; i < length; ++i, ++matched)
                {
                    if (matched == name.size())
                    {
                        return chunk[i] == 0;
                    }
                    if (chunk[i] != name[matched])
                    {
                        return false;
                    }
                }
                cursor += length;
            }
            return false;
        }

        /** @brief The global context (module registry), or 0. */
        [[nodiscard]] std::uintptr_t context() noexcept
        {
            std::byte *const slot = g_global_context_ptr_address.load(std::memory_order_relaxed);
            return slot != nullptr ? read_ptr(reinterpret_cast<std::uintptr_t>(slot)) : 0;
        }

        /** @brief A context module, class-checked. */
        [[nodiscard]] std::uintptr_t context_module(std::ptrdiff_t offset, GameClass klass) noexcept
        {
            const std::uintptr_t ctx = context();
            const std::uintptr_t module = ctx != 0 ? read_ptr(ctx + offset) : 0;
            return object_is(klass, module) ? module : 0;
        }

        [[nodiscard]] std::uintptr_t actor_system() noexcept
        {
            const std::uintptr_t cry = cry_action();
            const std::uintptr_t system = cry != 0 ? read_ptr(cry + constants::CRYACTION_ACTOR_SYSTEM_OFFSET) : 0;
            return object_is(GameClass::ActorSystem, system) ? system : 0;
        }

        /** @brief The IItemSystem interface of CItemSystem. */
        [[nodiscard]] std::uintptr_t item_system() noexcept
        {
            const std::uintptr_t cry = cry_action();
            const std::uintptr_t system = cry != 0 ? read_ptr(cry + constants::CRYACTION_ITEM_SYSTEM_OFFSET) : 0;
            return object_is(GameClass::ItemSystem, system) ? system + constants::ITEM_SYSTEM_INTERFACE_OFFSET : 0;
        }

        [[nodiscard]] bool wuid_type_is(Wuid wuid, std::uint8_t type) noexcept
        {
            return static_cast<std::uint8_t>(wuid >> 56) == type && (wuid & 0xFFFF) != 0;
        }

        /**
         * @brief Resolves a WUID in one of the game's handle tables (index in the low 16 bits, generation in the next
         *        16).
         * @return The object, or 0 when the slot is empty or holds another generation.
         */
        [[nodiscard]] std::uintptr_t wuid_table_lookup(std::uintptr_t table, Wuid wuid) noexcept
        {
            const std::uintptr_t index = static_cast<std::uintptr_t>(wuid & 0xFFFF);
            const std::uint16_t generation = static_cast<std::uint16_t>((wuid >> 16) & 0xFFFF);
            const std::uintptr_t entry = table + index * constants::WUID_TABLE_STRIDE;
            const auto stored =
                DMK::memory::read<std::uint16_t>(DMK::Address{entry + constants::WUID_TABLE_GENERATION_OFFSET});
            if (!stored || *stored != generation)
            {
                return 0;
            }
            return read_ptr(entry + constants::WUID_TABLE_OBJECT_OFFSET);
        }

        /** @brief The C_ScriptContextManager (C_Game -> C_GameModel -> its script-context manager). */
        [[nodiscard]] std::uintptr_t script_context_manager() noexcept
        {
            const std::uintptr_t game = context_module(constants::CONTEXT_GAME_OFFSET, GameClass::Game);
            if (game == 0)
            {
                return 0;
            }
            const std::optional<std::uintptr_t> model =
                vcall<std::uintptr_t>(game, constants::GAME_VTABLE_MODEL_OFFSET);
            if (!model || !object_is(GameClass::GameModel, *model))
            {
                return 0;
            }
            const std::optional<std::uintptr_t> manager =
                vcall<std::uintptr_t>(*model, constants::GAME_MODEL_VTABLE_SCRIPT_CONTEXTS_OFFSET);
            return manager && object_is(GameClass::ScriptContextManager, *manager) ? *manager : 0;
        }

        /**
         * @brief Finds a script-context entry by name (the table soul:HasScriptContext searches).
         * @return The entry address, or 0 when the name is unknown.
         */
        [[nodiscard]] std::uintptr_t script_context_entry(std::string_view name) noexcept
        {
            const std::uintptr_t map = gated_anchor_address(Feature::ScriptContexts, AnchorId::ScriptContextMap);
            if (map == 0 || name.empty() || name.size() >= MAX_CONTEXT_NAME)
            {
                return 0;
            }
            const auto begin =
                DMK::memory::read<std::uintptr_t>(DMK::Address{map + constants::SCRIPT_CONTEXT_MAP_BEGIN_OFFSET});
            const auto end =
                DMK::memory::read<std::uintptr_t>(DMK::Address{map + constants::SCRIPT_CONTEXT_MAP_END_OFFSET});
            if (!begin || !end || *end < *begin ||
                (*end - *begin) / constants::SCRIPT_CONTEXT_ENTRY_SIZE > MAX_SCRIPT_CONTEXTS)
            {
                return 0;
            }
            for (std::uintptr_t entry = *begin; entry < *end; entry += constants::SCRIPT_CONTEXT_ENTRY_SIZE)
            {
                const std::uintptr_t text = read_ptr(entry);
                if (text == 0)
                {
                    continue;
                }
                if (cstring_equals(text, name))
                {
                    return entry;
                }
            }
            return 0;
        }

        /** @brief Every C_Shop of the shop manager. */
        void collect_shops(std::vector<std::uintptr_t> &out)
        {
            out.clear();
            const std::uintptr_t module = context_module(constants::CONTEXT_SHOP_MODULE_OFFSET, GameClass::ShopModule);
            if (module == 0)
            {
                return;
            }
            constexpr std::array<std::ptrdiff_t, 3> head_chain{
                constants::SHOP_MODULE_MANAGER_OFFSET,
                constants::SHOP_MANAGER_LIST_OFFSET,
                0
            };
            const auto head_slot = DMK::memory::walk(DMK::Address{module}, head_chain);
            const std::uintptr_t head = head_slot ? head_slot->raw() : 0;
            if (head == 0)
            {
                return;
            }
            std::uintptr_t node = read_ptr(head);
            for (std::size_t n = 0; node != 0 && node != head && n < MAX_SHOPS; ++n)
            {
                const std::uintptr_t shop = read_ptr(node + constants::SHOP_NODE_SHOP_OFFSET);
                if (object_is(GameClass::Shop, shop))
                {
                    out.push_back(shop);
                }
                node = read_ptr(node);
            }
        }

        /**
         * @brief The two fields the shop goods' "holds" compares: the goods' owner and the item's.
         */
        struct ShopOwnerLayout
        {
            std::ptrdiff_t goods_owner{0};
            std::ptrdiff_t item_owner{0};
        };

        /**
         * @brief Reads the fields out of the goods interface's "holds" (vtable slot 3; 1.5.6 sub_1805663FC).
         * @details The function is `mov rax, [rcx + goods]; cmp [rdx + item], rax; sete al; ret`: an item belongs to a
         *          shop when its owner (C_Item +0x90) is the goods' owner (+0xD8). Any other shape answers
         *          std::nullopt, and the per-shop virtual call stays in charge.
         */
        [[nodiscard]] std::optional<ShopOwnerLayout> decode_shop_holds(std::uintptr_t shop) noexcept
        {
            const std::uintptr_t fn =
                read_vtable_slot(shop + constants::SHOP_GOODS_OFFSET, constants::SHOP_GOODS_VTABLE_HOLDS_OFFSET);
            if (fn == 0)
            {
                return std::nullopt;
            }
            const auto code = DMK::memory::read<std::array<std::uint8_t, 18>>(DMK::Address{fn});
            if (!code)
            {
                return std::nullopt;
            }
            const std::array<std::uint8_t, 18> &b = *code;
            const bool shape = b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x81 && b[7] == 0x48 && b[8] == 0x39 &&
                               b[9] == 0x82 && b[14] == 0x0F && b[15] == 0x94 && b[16] == 0xC0 && b[17] == 0xC3;
            if (!shape)
            {
                return std::nullopt;
            }
            std::int32_t goods = 0;
            std::int32_t item = 0;
            std::memcpy(&goods, &b[3], sizeof(goods));
            std::memcpy(&item, &b[10], sizeof(item));
            return ShopOwnerLayout{goods, item};
        }

        /**
         * @brief Every shop of the level and, when "holds" decodes, the sorted set of their goods' owners.
         * @details A shop check was a virtual call per shop, ~0.57 ms per item with Kuttenberg's 530 shops; the set
         *          answers an item with one read. Shops and their goods live as long as the level, so the set is
         *          rebuilt only every SHOP_OWNERS_REFRESH_MS. Main thread only.
         */
        struct ShopOwners
        {
            std::vector<std::uintptr_t> shops;
            std::vector<std::uintptr_t> owners;
            std::optional<ShopOwnerLayout> layout;
            std::int64_t at_ms{0};
            bool valid{false};
        };

        constexpr std::int64_t SHOP_OWNERS_REFRESH_MS = 5000;
        ShopOwners s_shop_owners;

        void refresh_shop_owners(std::int64_t now_ms) noexcept
        {
            s_shop_owners.at_ms = now_ms;
            s_shop_owners.valid = false;
            try
            {
                collect_shops(s_shop_owners.shops);
                s_shop_owners.owners.clear();
                s_shop_owners.layout.reset();
                if (context_module(constants::CONTEXT_SHOP_MODULE_OFFSET, GameClass::ShopModule) == 0)
                {
                    return;
                }
                s_shop_owners.valid = true;
                if (s_shop_owners.shops.empty() ||
                    !(s_shop_owners.layout = decode_shop_holds(s_shop_owners.shops.front())).has_value())
                {
                    return;
                }
                // A shop whose field cannot be read holds nothing, as its faulting virtual call answered.
                for (const std::uintptr_t shop : s_shop_owners.shops)
                {
                    if (const auto owner = DMK::memory::read<std::uintptr_t>(
                            DMK::Address{shop + constants::SHOP_GOODS_OFFSET + s_shop_owners.layout->goods_owner}
                        ))
                    {
                        s_shop_owners.owners.push_back(*owner);
                    }
                }
                std::sort(s_shop_owners.owners.begin(), s_shop_owners.owners.end());
            }
            catch (...)
            {
                s_shop_owners.valid = false;
            }
        }

        [[nodiscard]] ActorKind kind_of_vtable(std::uintptr_t actor) noexcept
        {
            // One guarded vtable read, then a compare per class (every actor of a pick comes through here).
            if (actor == 0 || !DMK::memory::is_plausible_ptr(DMK::Address{actor}))
            {
                return ActorKind::None;
            }
            const auto vtable = DMK::memory::read<std::uintptr_t>(DMK::Address{actor});
            if (!vtable)
            {
                return ActorKind::None;
            }
            if (vtable_is(GameClass::NpcActor, *vtable))
            {
                return ActorKind::Human;
            }
            if (vtable_is(GameClass::Player, *vtable))
            {
                return ActorKind::Player;
            }
            if (vtable_is(GameClass::Horse, *vtable))
            {
                return ActorKind::Horse;
            }
            if (vtable_is(GameClass::Dog, *vtable))
            {
                return ActorKind::Dog;
            }
            if (vtable_is(GameClass::Animal, *vtable))
            {
                return ActorKind::Animal;
            }
            return ActorKind::None;
        }

        [[nodiscard]] bool read_view(std::uintptr_t actor, EntityId id, ActorView &view) noexcept
        {
            view.kind = kind_of_vtable(actor);
            if (view.kind == ActorKind::None)
            {
                return false;
            }
            view.actor = actor;
            view.id = id;
            view.entity = read_ptr(actor + constants::ACTOR_ENTITY_OFFSET);
            if (!object_is(GameClass::Entity, view.entity))
            {
                return false;
            }
            view.soul = actor_soul(actor);
            return true;
        }

        /**
         * @struct ScriptAnyValue
         * @brief The engine's ScriptAnyValue: the requested (then read) type at +0, the payload at +8.
         */
        struct alignas(8) ScriptAnyValue
        {
            std::int32_t type;
            std::int32_t pad;
            union
            {
                // The engine's bool, read as its byte: a foreign byte other than 0 or 1 is not a valid bool.
                std::uint8_t boolean;
                float number;
                std::uint64_t handle;
            };
            std::byte rest[constants::SCRIPT_ANY_VALUE_SIZE - 16];
        };
        static_assert(sizeof(ScriptAnyValue) == constants::SCRIPT_ANY_VALUE_SIZE);

        /**
         * @brief Reads one script-table field of an entity with the requested type.
         * @return True when the field exists and converted to @p type.
         */
        [[nodiscard]] bool
        read_script_value(std::uintptr_t entity, const char *key, std::int32_t type, ScriptAnyValue &value) noexcept
        {
            const std::uintptr_t proxy = entity_proxy(entity, constants::ENTITY_PROXY_SCRIPT);
            if (proxy == 0)
            {
                return false;
            }
            const std::optional<std::uintptr_t> table =
                vcall<std::uintptr_t>(proxy, constants::SCRIPT_PROXY_VTABLE_GET_TABLE_OFFSET);
            if (!table || !DMK::memory::is_plausible_ptr(DMK::Address{*table}))
            {
                return false;
            }
            value = ScriptAnyValue{};
            value.type = type;
            const std::optional<bool> read =
                vcall<bool>(*table, constants::SCRIPT_TABLE_VTABLE_GET_VALUE_ANY_OFFSET, key, &value, false);
            return read.value_or(false) && value.type == type;
        }

        /**
         * @brief Drops the reference a table read handed over (IScriptTable::Release, which frees the wrapper and its
         *        registry reference at zero).
         */
        void release_script_table(std::uintptr_t table) noexcept
        {
            const std::uintptr_t fn = read_vtable_slot(table, constants::SCRIPT_TABLE_VTABLE_RELEASE_OFFSET);
            if (fn == 0)
            {
                return;
            }
            __try
            {
                reinterpret_cast<void(__fastcall *)(std::uintptr_t)>(fn)(table);
            }
            __except (engine_fault_filter(GetExceptionCode()))
            {
            }
        }
    } // namespace

    DMK::Result<void> initialize_game_natives()
    {
        struct Feature
        {
            AnchorId anchor;
            const char *effect;
        };
        constexpr std::array<Feature, 5> features = {{
            {AnchorId::StashFromEntity, "containers are not detected"},
            {AnchorId::StashMasterInventory, "containers linked to a master stash read their own inventory"},
            {AnchorId::InventoryOwner, "containers are never flagged as stealing"},
            {AnchorId::PublicEnemyTag, "public enemies are not told apart (corpses count as illegal to loot)"},
            {AnchorId::ScriptContextMap, "script contexts are not read (animal loot switches, legal-to-loot souls)"},
        }};
        std::size_t missing = 0;
        for (const Feature &feature : features)
        {
            if (anchor_address(feature.anchor) == 0)
            {
                ++missing;
                DMK::log().warning("GameNatives: {} did not resolve; {}", anchor_label(feature.anchor), feature.effect);
            }
        }
        DMK::log().info("GameNatives: ready ({} of {} anchors resolved)", features.size() - missing, features.size());
        return {};
    }

    void shutdown_game_natives() noexcept {}

    // Actors and souls

    bool collect_actors_near(
        std::vector<ActorView> &out,
        const game_structures::Vec3f &center,
        float reach,
        std::size_t max_actors,
        std::size_t *total
    )
    {
        DMK_PROFILE_FUNCTION();
        out.clear();
        if (total != nullptr)
        {
            *total = 0;
        }
        const std::uintptr_t system = actor_system();
        const std::uintptr_t head = system != 0 ? read_ptr(system + constants::ACTOR_SYSTEM_LIST_HEAD_OFFSET) : 0;
        if (head == 0)
        {
            return false;
        }
        std::uintptr_t node = read_ptr(head);
        if (node == 0)
        {
            return false;
        }
        s_actor_refs.clear();
        s_actor_history.begin();
        std::size_t visited = 0;
        while (node != head)
        {
            if (!node_pointer(node) || ++visited > max_actors)
            {
                break;
            }
            s_actor_history.visit(node, sizeof(ActorNodeWords));
            const auto words = DMK::memory::read<ActorNodeWords>(DMK::Address{node});
            if (!words)
            {
                break;
            }
            const auto id = static_cast<std::uint32_t>((*words)[ACTOR_NODE_KEY]);
            const std::uintptr_t actor = (*words)[ACTOR_NODE_VALUE];
            if (id != 0 && node_pointer(actor))
            {
                s_actor_refs.push_back(NodeRef{.id = id, .object = actor});
            }
            node = (*words)[ACTOR_NODE_NEXT];
        }
        s_actor_history.finish();
        keep_within(s_actor_refs, constants::ACTOR_ENTITY_OFFSET, center, reach * reach);
        for (const NodeRef &ref : s_actor_refs)
        {
            ActorView view{};
            if (read_view(ref.object, ref.id, view))
            {
                out.push_back(view);
            }
        }
        if (total != nullptr)
        {
            *total = visited;
        }
        return node == head;
    }

    std::uintptr_t actor_of(EntityId id) noexcept
    {
        const std::uintptr_t system = actor_system();
        if (system == 0 || id == 0)
        {
            return 0;
        }
        const std::optional<std::uintptr_t> actor =
            vcall<std::uintptr_t>(system, constants::ACTOR_SYSTEM_VTABLE_GET_ACTOR_OFFSET, id);
        return actor && kind_of_vtable(*actor) != ActorKind::None ? *actor : 0;
    }

    std::uintptr_t actor_soul(std::uintptr_t actor) noexcept
    {
        const std::uintptr_t soul = actor != 0 ? read_ptr(actor + constants::ACTOR_SOUL_OFFSET) : 0;
        return object_is(GameClass::Soul, soul) ? soul : 0;
    }

    bool actor_despawned(std::uintptr_t actor) noexcept
    {
        const std::uintptr_t adapter = actor != 0 ? read_ptr(actor + constants::ACTOR_MOVEMENT_CONTROLLER_OFFSET) : 0;
        if (adapter == 0)
        {
            return false;
        }
        const std::uintptr_t ai = adapter - constants::AI_NPC_MOVEMENT_ADAPTER_OFFSET;
        if (!object_is(GameClass::AiNpc, ai))
        {
            return false;
        }
        const auto bound = DMK::memory::read<std::uint32_t>(DMK::Address{ai + constants::AI_NPC_ACTOR_ID_OFFSET});
        return bound.has_value() && *bound == 0;
    }

    std::optional<bool> actor_is_dead(std::uintptr_t actor) noexcept
    {
        return actor != 0 ? vcall<bool>(actor, constants::ACTOR_VTABLE_IS_DEAD_OFFSET) : std::nullopt;
    }

    std::optional<float> actor_health(std::uintptr_t actor) noexcept
    {
        return actor != 0 ? vcall<float>(actor, constants::ACTOR_VTABLE_GET_HEALTH_OFFSET) : std::nullopt;
    }

    std::optional<bool> soul_is_unconscious(std::uintptr_t soul) noexcept
    {
        return soul != 0 ? vcall<bool>(soul, constants::SOUL_VTABLE_IS_UNCONSCIOUS_OFFSET) : std::nullopt;
    }

    std::optional<bool> soul_can_be_butchered(std::uintptr_t soul) noexcept
    {
        return soul != 0 ? vcall<bool>(soul, constants::SOUL_VTABLE_CAN_BE_BUTCHERED_OFFSET) : std::nullopt;
    }

    std::optional<bool> soul_is_public_enemy(std::uintptr_t soul) noexcept
    {
        const std::uintptr_t tag = gated_anchor_address(Feature::PublicEnemy, AnchorId::PublicEnemyTag);
        if (soul == 0 || tag == 0)
        {
            return std::nullopt;
        }
        const std::optional<std::uintptr_t> reputation =
            vcall<std::uintptr_t>(soul, constants::SOUL_VTABLE_REPUTATION_OFFSET);
        if (!reputation || !DMK::memory::is_plausible_ptr(DMK::Address{*reputation}))
        {
            return std::nullopt;
        }
        return vcall<bool>(*reputation, constants::REPUTATION_VTABLE_HAS_TAG_OFFSET, tag);
    }

    std::optional<bool> soul_is_legal_to_loot(std::uintptr_t soul) noexcept
    {
        const std::optional<bool> enemy = soul_is_public_enemy(soul);
        if (enemy.value_or(false))
        {
            return true;
        }
        const std::uintptr_t manager = script_context_manager();
        const Wuid wuid = soul_wuid(soul);
        if (manager == 0 || wuid == 0)
        {
            return std::nullopt;
        }
        const std::optional<bool> context = vcall<bool>(
            manager,
            constants::SCRIPT_CONTEXTS_VTABLE_HAS_ID_OFFSET,
            constants::SCRIPT_CONTEXT_LEGAL_TO_LOOT,
            wuid
        );
        if (!context.has_value() || !enemy.has_value())
        {
            return context.value_or(false) ? std::optional<bool>{true} : std::nullopt;
        }
        return *context;
    }

    std::optional<bool> soul_has_script_context(std::uintptr_t soul, std::string_view name) noexcept
    {
        const std::uintptr_t manager = script_context_manager();
        const Wuid wuid = soul_wuid(soul);
        if (manager == 0 || wuid == 0 || !feature_ready(Feature::ScriptContexts))
        {
            return std::nullopt;
        }
        const std::uintptr_t entry = script_context_entry(name);
        if (entry == 0)
        {
            return false;
        }
        return vcall<bool>(manager, constants::SCRIPT_CONTEXTS_VTABLE_HAS_OFFSET, wuid, entry);
    }

    Wuid soul_wuid(std::uintptr_t soul) noexcept
    {
        if (soul == 0)
        {
            return 0;
        }
        const auto wuid = DMK::memory::read<std::uint64_t>(DMK::Address{soul + constants::SOUL_WUID_OFFSET});
        return wuid ? *wuid : 0;
    }

    std::uintptr_t soul_from_wuid(Wuid wuid) noexcept
    {
        if (!wuid_type_is(wuid, constants::WUID_TYPE_SOUL))
        {
            return 0;
        }
        const std::uintptr_t module = context_module(constants::CONTEXT_RPG_MODULE_OFFSET, GameClass::RpgModule);
        const std::uintptr_t souls = module != 0 ? read_ptr(module + constants::RPG_MODULE_SOULS_OFFSET) : 0;
        if (souls == 0)
        {
            return 0;
        }
        const std::uintptr_t soul = wuid_table_lookup(souls + constants::SOUL_MANAGER_TABLE_OFFSET, wuid);
        return object_is(GameClass::Soul, soul) && soul_wuid(soul) == wuid ? soul : 0;
    }

    std::uintptr_t soul_inventory(std::uintptr_t soul) noexcept
    {
        const std::optional<std::uintptr_t> holder =
            soul != 0 ? vcall<std::uintptr_t>(soul, constants::SOUL_VTABLE_INVENTORY_HOLDER_OFFSET) : std::nullopt;
        if (!holder || !DMK::memory::is_plausible_ptr(DMK::Address{*holder}))
        {
            return 0;
        }
        const std::optional<std::uintptr_t> inventory =
            vcall<std::uintptr_t>(*holder, constants::INVENTORY_HOLDER_VTABLE_GET_OFFSET);
        return inventory && object_is(GameClass::Inventory, *inventory) ? *inventory : 0;
    }

    std::optional<bool> actor_can_loot(std::uintptr_t looter, EntityId victim) noexcept
    {
        const std::uintptr_t system = context_module(constants::CONTEXT_ACTOR_SYSTEM_OFFSET, GameClass::ActorSystem);
        if (looter == 0 || victim == 0 || system == 0)
        {
            return std::nullopt;
        }
        const std::optional<std::uintptr_t> target =
            vcall<std::uintptr_t>(system, constants::ACTOR_SYSTEM_VTABLE_GET_ACTOR_OFFSET, victim);
        if (!target.has_value())
        {
            return std::nullopt;
        }
        if (*target == 0)
        {
            return false;
        }
        const std::uintptr_t state = read_ptr(*target + constants::ACTOR_STATE_OFFSET);
        if (state == 0)
        {
            return std::nullopt;
        }
        const std::optional<bool> dead =
            vcall<bool>(state, constants::ACTOR_STATE_VTABLE_IS_IN_OFFSET, constants::ACTOR_STATE_DEAD);
        const std::optional<bool> unconscious =
            vcall<bool>(state, constants::ACTOR_STATE_VTABLE_IS_IN_OFFSET, constants::ACTOR_STATE_UNCONSCIOUS);
        const std::optional<bool> carried =
            vcall<bool>(state, constants::ACTOR_STATE_VTABLE_IS_IN_OFFSET, constants::ACTOR_STATE_CARRIED);
        if (!dead || !unconscious || !carried)
        {
            return std::nullopt;
        }
        if ((!*dead && !*unconscious) || *carried)
        {
            return false;
        }
        if (const std::uintptr_t soul = actor_soul(*target); soul != 0)
        {
            const std::optional<bool> lootable = vcall<bool>(soul, constants::SOUL_VTABLE_IS_LOOTABLE_OFFSET);
            if (!lootable.has_value())
            {
                return std::nullopt;
            }
            if (!*lootable)
            {
                return false;
            }
        }
        if (const std::uintptr_t carried_corpse = read_ptr(looter + constants::ACTOR_CARRIED_CORPSE_OFFSET);
            carried_corpse != 0)
        {
            const std::optional<std::uint32_t> id =
                vcall<std::uint32_t>(carried_corpse, constants::CARRIED_CORPSE_VTABLE_ID_OFFSET);
            if (id.has_value() && *id == victim)
            {
                return false;
            }
        }
        if (const std::uintptr_t looter_state = read_ptr(looter + constants::ACTOR_LOOTER_STATE_OFFSET);
            looter_state != 0)
        {
            const std::optional<bool> blocked =
                vcall<bool>(looter_state, constants::LOOTER_STATE_VTABLE_BLOCKED_OFFSET);
            if (!blocked.has_value())
            {
                return std::nullopt;
            }
            if (*blocked)
            {
                return false;
            }
        }
        return true;
    }

    // Inventories

    std::optional<std::size_t> inventory_item_count(std::uintptr_t inventory) noexcept
    {
        if (!object_is(GameClass::Inventory, inventory))
        {
            return std::nullopt;
        }
        const auto begin =
            DMK::memory::read<std::uintptr_t>(DMK::Address{inventory + constants::INVENTORY_ITEMS_BEGIN_OFFSET});
        const auto end =
            DMK::memory::read<std::uintptr_t>(DMK::Address{inventory + constants::INVENTORY_ITEMS_END_OFFSET});
        if (!begin || !end || *end < *begin)
        {
            return std::nullopt;
        }
        return static_cast<std::size_t>((*end - *begin) / sizeof(std::uintptr_t));
    }

    std::optional<bool> inventory_is_empty_for_player(std::uintptr_t inventory) noexcept
    {
        if (!object_is(GameClass::Inventory, inventory))
        {
            return std::nullopt;
        }
        return vcall<bool>(
            inventory + constants::INVENTORY_INTERFACE_OFFSET,
            constants::INVENTORY_VTABLE_EMPTY_FOR_PLAYER_OFFSET
        );
    }

    std::optional<bool> inventory_is_usable(std::uintptr_t inventory) noexcept
    {
        if (!object_is(GameClass::Inventory, inventory))
        {
            return std::nullopt;
        }
        const auto lockable =
            DMK::memory::read<std::uint8_t>(DMK::Address{inventory + constants::INVENTORY_LOCKABLE_OFFSET});
        if (!lockable)
        {
            return std::nullopt;
        }
        if (*lockable == 0)
        {
            return true;
        }
        const std::optional<bool> locked =
            vcall<bool>(inventory + constants::INVENTORY_INTERFACE_OFFSET, constants::INVENTORY_VTABLE_LOCKED_OFFSET);
        return locked.has_value() ? std::optional<bool>{!*locked} : std::nullopt;
    }

    Wuid inventory_wuid(std::uintptr_t inventory) noexcept
    {
        if (!object_is(GameClass::Inventory, inventory))
        {
            return 0;
        }
        const std::optional<std::uintptr_t> wuid = vcall<std::uintptr_t>(
            inventory + constants::INVENTORY_INTERFACE_OFFSET,
            constants::INVENTORY_VTABLE_WUID_OFFSET
        );
        if (!wuid || *wuid == 0)
        {
            return 0;
        }
        const auto value = DMK::memory::read<std::uint64_t>(DMK::Address{*wuid});
        return value ? *value : 0;
    }

    std::uintptr_t inventory_from_wuid(Wuid wuid) noexcept
    {
        if (!wuid_type_is(wuid, constants::WUID_TYPE_INVENTORY))
        {
            return 0;
        }
        const std::uintptr_t module = context_module(constants::CONTEXT_ENTITY_MODULE_OFFSET, GameClass::EntityModule);
        const std::uintptr_t manager =
            module != 0 ? read_ptr(module + constants::ENTITY_MODULE_INVENTORY_MANAGER_OFFSET) : 0;
        if (!object_is(GameClass::InventoryManager, manager))
        {
            return 0;
        }
        const std::uintptr_t inventory = wuid_table_lookup(manager + constants::INVENTORY_MANAGER_TABLE_OFFSET, wuid);
        return object_is(GameClass::Inventory, inventory) && inventory_wuid(inventory) == wuid ? inventory : 0;
    }

    std::optional<Wuid> inventory_owner(std::uintptr_t inventory) noexcept
    {
        const std::uintptr_t fn = gated_anchor_address(Feature::InventoryOwner, AnchorId::InventoryOwner);
        if (fn == 0 || !object_is(GameClass::Inventory, inventory))
        {
            return std::nullopt;
        }
        std::uint64_t owner = 0;
        std::uint64_t *returned = nullptr;
        if (!guarded_call<std::uint64_t *>(fn, &returned, inventory, &owner))
        {
            return std::nullopt;
        }
        return owner;
    }

    // World items

    bool collect_items_near(
        std::vector<ItemView> &out,
        const game_structures::Vec3f &center,
        float reach,
        std::size_t max_items,
        std::size_t *total
    )
    {
        DMK_PROFILE_FUNCTION();
        out.clear();
        if (total != nullptr)
        {
            *total = 0;
        }
        const std::uintptr_t system = item_system();
        const std::uintptr_t head = system != 0 ? read_ptr(system + constants::ITEM_MAP_HEAD_OFFSET) : 0;
        if (head == 0)
        {
            return false;
        }
        const auto root = DMK::memory::read<std::uintptr_t>(DMK::Address{head + sizeof(std::uintptr_t)});
        if (!root)
        {
            return false;
        }
        s_item_refs.clear();
        s_item_history.begin();
        auto load = [](std::uintptr_t node, ItemNodeWords &into)
        {
            s_item_history.visit(node, sizeof(ItemNodeWords));
            return load_item_node(node, into);
        };
        // In-order walk from the root (the head's parent) with an explicit stack of whole nodes, so each node is read
        // once.
        std::array<ItemNodeWords, MAX_TREE_DEPTH> stack{};
        std::size_t depth = 0;
        ItemNodeWords words{};
        bool have = load(*root, words);
        std::size_t visited = 0;
        bool complete = true;
        while (have || depth != 0)
        {
            while (have)
            {
                if (depth == stack.size())
                {
                    complete = false;
                    break;
                }
                stack[depth++] = words;
                have = load(words[ITEM_NODE_LEFT], words);
            }
            if (!complete || depth == 0)
            {
                break;
            }
            const ItemNodeWords current = stack[--depth];
            if (++visited > max_items)
            {
                complete = false;
                break;
            }
            const auto id = static_cast<std::uint32_t>(current[ITEM_NODE_KEY]);
            const std::uintptr_t item = current[ITEM_NODE_VALUE];
            if (id != 0 && node_pointer(item))
            {
                s_item_refs.push_back(NodeRef{.id = id, .object = item});
            }
            have = load(current[ITEM_NODE_RIGHT], words);
        }
        s_item_history.finish();
        keep_within(s_item_refs, constants::PICKABLE_ITEM_ENTITY_OFFSET, center, reach * reach);
        for (const NodeRef &ref : s_item_refs)
        {
            if (object_is(GameClass::PickableItem, ref.object) && object_is(GameClass::Entity, ref.entity) &&
                entity_id_of(ref.entity) == ref.id)
            {
                const std::uintptr_t data = read_ptr(ref.object + constants::PICKABLE_ITEM_DATA_OFFSET);
                if (object_is(GameClass::Item, data))
                {
                    out.push_back(ItemView{ref.id, ref.object, data, ref.entity});
                }
            }
        }
        if (total != nullptr)
        {
            *total = visited;
        }
        return complete;
    }

    std::optional<std::size_t> item_map_size() noexcept
    {
        const std::uintptr_t system = item_system();
        if (system == 0)
        {
            return std::nullopt;
        }
        const auto size = DMK::memory::read<std::uint64_t>(DMK::Address{system + constants::ITEM_MAP_SIZE_OFFSET});
        return size ? std::optional<std::size_t>{static_cast<std::size_t>(*size)} : std::nullopt;
    }

    bool item_still_on(const ItemView &item, std::uintptr_t entity) noexcept
    {
        if (entity == 0 || entity != item.entity)
        {
            return false;
        }
        const auto stored =
            DMK::memory::read<std::uintptr_t>(DMK::Address{item.item + constants::PICKABLE_ITEM_ENTITY_OFFSET});
        return stored && *stored == entity;
    }

    EntityPresence entity_presence(
        std::uintptr_t entity,
        EntityId id,
        std::uintptr_t klass,
        const game_structures::Vec3f &center,
        float reach
    ) noexcept
    {
        if (!node_pointer(entity))
        {
            return EntityPresence::Gone;
        }
        const auto stored_id = DMK::memory::read<std::uint32_t>(DMK::Address{entity + constants::ENTITY_ID_OFFSET});
        if (!stored_id || *stored_id != id)
        {
            return EntityPresence::Gone;
        }
        const auto stored_class =
            DMK::memory::read<std::uintptr_t>(DMK::Address{entity + constants::ENTITY_CLASS_OFFSET});
        if (!stored_class || *stored_class != klass)
        {
            return EntityPresence::Gone;
        }
        return entity_within(entity, center, reach * reach) ? EntityPresence::Near : EntityPresence::Far;
    }

    std::optional<ItemView> item_of(EntityId id) noexcept
    {
        const std::uintptr_t system = item_system();
        if (system == 0 || id == 0)
        {
            return std::nullopt;
        }
        const std::optional<std::uintptr_t> item =
            vcall<std::uintptr_t>(system, constants::ITEM_SYSTEM_VTABLE_GET_ITEM_OFFSET, id);
        if (!item || !object_is(GameClass::PickableItem, *item))
        {
            return std::nullopt;
        }
        const std::uintptr_t data = read_ptr(*item + constants::PICKABLE_ITEM_DATA_OFFSET);
        const std::uintptr_t entity = read_ptr(*item + constants::PICKABLE_ITEM_ENTITY_OFFSET);
        if (!object_is(GameClass::Item, data) || !object_is(GameClass::Entity, entity))
        {
            return std::nullopt;
        }
        return ItemView{id, *item, data, entity};
    }

    std::optional<bool> item_is_pickable(const ItemView &item) noexcept
    {
        const std::optional<bool> used = item_in_use(item);
        if (!used.has_value())
        {
            return std::nullopt;
        }
        if (*used)
        {
            return false;
        }
        const std::uintptr_t klass = read_ptr(item.data + constants::ITEM_CLASS_OFFSET);
        if (klass == 0)
        {
            return std::nullopt;
        }
        return vcall<bool>(klass, constants::ITEM_CLASS_VTABLE_IS_A_OFFSET, constants::ITEM_CLASS_PLAYER_ITEM);
    }

    std::optional<bool> item_in_use(const ItemView &item) noexcept
    {
        const auto state =
            DMK::memory::read<std::uint8_t>(DMK::Address{item.item + constants::PICKABLE_ITEM_STATE_OFFSET});
        return state ? std::optional<bool>{(*state & constants::PICKABLE_ITEM_IN_USE) != 0} : std::nullopt;
    }

    std::optional<bool> item_is_npc_only(const ItemView &item) noexcept
    {
        const auto flags = DMK::memory::read<std::uint32_t>(DMK::Address{item.data + constants::ITEM_FLAGS_OFFSET});
        return flags ? std::optional<bool>{(*flags & constants::ITEM_FLAG_NPC_ONLY) != 0} : std::nullopt;
    }

    std::uintptr_t item_owner(const ItemView &item) noexcept
    {
        const auto primary = DMK::memory::read<std::uintptr_t>(DMK::Address{item.data + constants::ITEM_OWNER_OFFSET});
        const auto fallback =
            DMK::memory::read<std::uintptr_t>(DMK::Address{item.data + constants::ITEM_OWNER_FALLBACK_OFFSET});
        if (!primary || !fallback)
        {
            return 0;
        }
        return *primary != 0 ? *primary : *fallback;
    }

    ItemHolder item_holder(const ItemView &item) noexcept
    {
        const auto primary = DMK::memory::read<std::uintptr_t>(DMK::Address{item.data + constants::ITEM_OWNER_OFFSET});
        const auto fallback =
            DMK::memory::read<std::uintptr_t>(DMK::Address{item.data + constants::ITEM_OWNER_FALLBACK_OFFSET});
        if (!primary || !fallback)
        {
            return ItemHolder::Unknown;
        }
        const std::uintptr_t owner = *primary != 0 ? *primary : *fallback;
        if (owner == 0 || object_is(GameClass::WorldInventory, owner))
        {
            return ItemHolder::None;
        }
        if (object_is(GameClass::Inventory, owner))
        {
            return ItemHolder::Inventory;
        }
        if (object_is(GameClass::ItemVectorBorrower, owner))
        {
            return ItemHolder::Borrowed;
        }
        if (object_is(GameClass::ItemWrapper, owner))
        {
            return ItemHolder::Attached;
        }
        if (object_is(GameClass::ItemSlot, owner) || object_is(GameClass::ItemSlotPile, owner))
        {
            return ItemHolder::Slot;
        }
        return ItemHolder::Unknown;
    }

    std::optional<bool> item_can_steal(const ItemView &item, EntityId user) noexcept
    {
        return vcall<bool>(item.item, constants::PICKABLE_ITEM_VTABLE_CAN_STEAL_OFFSET, user);
    }

    std::optional<bool> item_is_from_shop(const ItemView &item) noexcept
    {
        const std::int64_t now = GetTickCount64();
        if (s_shop_owners.at_ms == 0 || now - s_shop_owners.at_ms >= SHOP_OWNERS_REFRESH_MS)
        {
            refresh_shop_owners(now);
        }
        if (!s_shop_owners.valid)
        {
            return std::nullopt;
        }
        if (s_shop_owners.layout.has_value())
        {
            const auto owner =
                DMK::memory::read<std::uintptr_t>(DMK::Address{item.data + s_shop_owners.layout->item_owner});
            if (!owner)
            {
                return std::nullopt;
            }
            return std::binary_search(s_shop_owners.owners.begin(), s_shop_owners.owners.end(), *owner);
        }
        for (const std::uintptr_t shop : s_shop_owners.shops)
        {
            const std::optional<bool> holds =
                vcall<bool>(shop + constants::SHOP_GOODS_OFFSET, constants::SHOP_GOODS_VTABLE_HOLDS_OFFSET, item.data);
            if (holds.value_or(false))
            {
                return true;
            }
        }
        return false;
    }

    // Stashes and shops

    std::uintptr_t stash_of(EntityId id) noexcept
    {
        const std::uintptr_t fn = gated_anchor_address(Feature::Stashes, AnchorId::StashFromEntity);
        if (fn == 0 || id == 0)
        {
            return 0;
        }
        std::uintptr_t stash = 0;
        if (!guarded_call<std::uintptr_t>(fn, &stash, id))
        {
            return 0;
        }
        return object_is(GameClass::Stash, stash) ? stash : 0;
    }

    std::uintptr_t stash_inventory(std::uintptr_t stash) noexcept
    {
        if (!object_is(GameClass::Stash, stash))
        {
            return 0;
        }
        if (const std::uintptr_t fn = gated_anchor_address(Feature::Stashes, AnchorId::StashMasterInventory); fn != 0)
        {
            std::uint64_t master = 0;
            std::uint64_t *returned = nullptr;
            if (guarded_call<std::uint64_t *>(fn, &returned, stash, &master) && master != 0)
            {
                if (const std::uintptr_t inventory = inventory_from_wuid(master); inventory != 0)
                {
                    return inventory;
                }
            }
        }
        const std::uintptr_t own = read_ptr(stash + constants::STASH_INVENTORY_OFFSET);
        return object_is(GameClass::Inventory, own) ? own : 0;
    }

    // Script-table fields

    std::optional<bool> script_bool(std::uintptr_t entity, const char *key) noexcept
    {
        ScriptAnyValue value{};
        if (!read_script_value(entity, key, constants::SCRIPT_ANY_BOOLEAN, value))
        {
            return std::nullopt;
        }
        return value.boolean != 0;
    }

    std::optional<float> script_number(std::uintptr_t entity, const char *key) noexcept
    {
        ScriptAnyValue value{};
        if (!read_script_value(entity, key, constants::SCRIPT_ANY_NUMBER, value))
        {
            return std::nullopt;
        }
        return value.number;
    }

    std::optional<Wuid> script_handle(std::uintptr_t entity, const char *key) noexcept
    {
        ScriptAnyValue value{};
        if (!read_script_value(entity, key, constants::SCRIPT_ANY_HANDLE, value))
        {
            return std::nullopt;
        }
        return value.handle;
    }

    std::optional<bool> script_table_bool(std::uintptr_t entity, const char *table_key, const char *key) noexcept
    {
        ScriptAnyValue table{};
        const bool found = read_script_value(entity, table_key, constants::SCRIPT_ANY_TABLE, table);
        // An empty value asks the engine for a new table wrapper that holds one reference; it is dropped below
        // whatever the nested read finds.
        const auto nested = static_cast<std::uintptr_t>(table.handle);
        if (table.type != constants::SCRIPT_ANY_TABLE || nested == 0 ||
            !DMK::memory::is_plausible_ptr(DMK::Address{nested}))
        {
            return std::nullopt;
        }
        std::optional<bool> result{};
        if (found)
        {
            ScriptAnyValue value{};
            value.type = constants::SCRIPT_ANY_BOOLEAN;
            const std::optional<bool> read =
                vcall<bool>(nested, constants::SCRIPT_TABLE_VTABLE_GET_VALUE_ANY_OFFSET, key, &value, false);
            if (read.value_or(false) && value.type == constants::SCRIPT_ANY_BOOLEAN)
            {
                result = value.boolean != 0;
            }
        }
        release_script_table(nested);
        return result;
    }

} // namespace HenrySenses
