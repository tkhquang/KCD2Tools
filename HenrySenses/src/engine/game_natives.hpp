/**
 * @file engine/game_natives.hpp
 * @brief The game's own gameplay predicates, called natively: actors and souls, world items, inventories, stashes
 *        and shops.
 *
 * Every function here is the C++ the game's script binds run (C_ScriptBindActor, C_ScriptBindSoul,
 * C_ScriptBindPickableItem, C_ScriptBindInventory, C_ScriptBindStash, C_ScriptBindEntityModule,
 * C_ScriptBindShop), reached without Lua:
 *
 * - Actors: IGameFramework::GetIActorSystem (CCryAction + 0x518) holds every actor in an EntityId hash map. An actor
 *   keeps its CEntity at +0x38 and its C_Soul at +0x668; its class (C_Player, C_NPCActor, C_Horse, C_Dog, C_Animal)
 *   is its kind. IsDead and GetHealth are actor virtuals; IsUnconscious, CanBeButchered and the inventory are soul
 *   virtuals.
 * - World items: IGameFramework::GetIItemSystem (CCryAction + 0x520, interface at +8) holds every C_PickableItem in
 *   an EntityId map; its C_Item is at +0x58, whose owner (+0x98, else +0x90) is a C_Inventory while an inventory
 *   holds it and a C_ItemSlot while it sits in a world slot.
 * - The global context (AnchorId::Context) is a module registry: C_Game +0x18, C_EntityModule +0xE0 (whose
 *   C_InventoryManager at +0xE8 resolves inventory WUIDs), C_ShopModule +0x110, C_RPGModule +0x130 (souls by WUID),
 *   CActorSystem +0x180.
 *
 * Objects are validated by class before any virtual is called, every call runs under SEH, and nothing is cached
 * across calls except per-session lookups the caller keys itself. Main thread only: the script-table reads enter the
 * game's Lua state, and the engine maps are not synchronised.
 */
#ifndef HENRYSENSES_GAME_NATIVES_HPP
#define HENRYSENSES_GAME_NATIVES_HPP

#include "game_structures.hpp"
#include "engine/entity_access.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace HenrySenses
{
    /// The game's 64-bit object handle (souls, inventories, items); type in the top byte.
    using Wuid = std::uint64_t;

    /**
     * @enum ActorKind
     * @brief The class of an actor.
     */
    enum class ActorKind : std::uint8_t
    {
        /// Not an actor, or an actor class the mod does not know.
        None,
        /// The local player (C_Player).
        Player,
        /// A human NPC (C_NPCActor).
        Human,
        /// A horse (C_Horse).
        Horse,
        /// A dog (C_Dog).
        Dog,
        /// Any other animal (C_Animal): game, livestock, birds.
        Animal,
    };

    /**
     * @struct ActorView
     * @brief One actor of the actor system.
     */
    struct ActorView
    {
        EntityId id{0};
        std::uintptr_t actor{0};
        std::uintptr_t entity{0};
        std::uintptr_t soul{0};
        ActorKind kind{ActorKind::None};
    };

    /**
     * @struct ItemView
     * @brief One world item (C_PickableItem) of the item system.
     */
    struct ItemView
    {
        EntityId id{0};
        std::uintptr_t item{0};
        /// Its C_Item.
        std::uintptr_t data{0};
        /// Its CEntity.
        std::uintptr_t entity{0};
    };

    /**
     * @enum ItemHolder
     * @brief What holds a world item.
     */
    enum class ItemHolder : std::uint8_t
    {
        /// Nothing, or the world inventory: it lies loose in the world (dropped, thrown, fallen from a body).
        None,
        /// A world item slot or pile (a shelf, a tool rack, a stack of bowls, a chopping block).
        Slot,
        /// An inventory: worn, wielded, carried, or on a body.
        Inventory,
        /// An NPC borrowed it for a task (in hand while eating or working).
        Borrowed,
        /// It is a part of another item.
        Attached,
        /// Something else, or the owner could not be read.
        Unknown,
    };

    /**
     * @brief Checks the anchors the natives need and reports which features are available.
     * @return Always succeeds; missing anchors disable only the tests that need them (logged).
     */
    [[nodiscard]] DMK::Result<void> initialize_game_natives();

    /** @brief Drops the cached module pointers. */
    void shutdown_game_natives() noexcept;

    // Actors and souls

    /**
     * @brief Copies the actors of the actor system whose entity lies within @p reach of @p center.
     * @details The map holds every actor of the level, so it is walked with plain reads under SEH and only the actors
     *          in reach are validated (class checks, entity, soul).
     * @param out Receives the actors (cleared first); the kind and soul are filled, unknown classes are skipped.
     * @param center The centre.
     * @param reach The distance in metres.
     * @param max_actors Safety bound on the walk.
     * @param total Receives the number of actors walked, when not null.
     * @return True when the map was walked to its end.
     */
    [[nodiscard]] bool collect_actors_near(
        std::vector<ActorView> &out,
        const game_structures::Vec3f &center,
        float reach,
        std::size_t max_actors,
        std::size_t *total = nullptr
    );

    /**
     * @brief Returns the actor of an entity (IActorSystem::GetActor), validated as a known actor class.
     * @param id The entity id.
     * @return The actor, or 0.
     */
    [[nodiscard]] std::uintptr_t actor_of(EntityId id) noexcept;

    /**
     * @brief Returns an actor's C_Soul (+0x668), class-checked.
     * @param actor An actor.
     * @return The soul, or 0.
     */
    [[nodiscard]] std::uintptr_t actor_soul(std::uintptr_t actor) noexcept;

    /**
     * @brief Reports whether the game took an actor out of the world: its AI (wh::xgenaimodule::C_NPC) let go of it.
     * @details A disposed body, or an NPC a quest removed, keeps its entity, actor and soul, invisible and inactive
     *          exactly like an NPC asleep out of view; only the AI's bound EntityId, 0 once it lets go, tells them
     *          apart (constants::AI_NPC_ACTOR_ID_OFFSET).
     * @param actor An actor.
     * @return True only for an actor whose class-checked AI reads a bound id of 0. An actor without an AI (the
     *         inventory dummies) or with unreadable memory answers false.
     */
    [[nodiscard]] bool actor_despawned(std::uintptr_t actor) noexcept;

    /** @brief actor:IsDead() (actor vtable slot 38). */
    [[nodiscard]] std::optional<bool> actor_is_dead(std::uintptr_t actor) noexcept;

    /** @brief actor:GetHealth() (actor vtable slot 30). */
    [[nodiscard]] std::optional<float> actor_health(std::uintptr_t actor) noexcept;

    /** @brief actor:IsUnconscious() (soul vtable slot 7: health above 0 and the consciousness stat at 0). */
    [[nodiscard]] std::optional<bool> soul_is_unconscious(std::uintptr_t soul) noexcept;

    /** @brief actor:CanBeButchered() (soul vtable slot 144). */
    [[nodiscard]] std::optional<bool> soul_can_be_butchered(std::uintptr_t soul) noexcept;

    /** @brief RPG.IsPublicEnemy for a soul: its reputation component (soul vtable slot 50) against the public-enemy
     *         faction tag. */
    [[nodiscard]] std::optional<bool> soul_is_public_enemy(std::uintptr_t soul) noexcept;

    /**
     * @brief soul:IsLegalToLoot(): a public enemy, or a soul carrying script context 84.
     * @param soul A soul.
     * @return The verdict, or std::nullopt when a part could not be evaluated.
     */
    [[nodiscard]] std::optional<bool> soul_is_legal_to_loot(std::uintptr_t soul) noexcept;

    /**
     * @brief soul:HasScriptContext(name).
     * @param soul A soul.
     * @param name The context name.
     * @return True or false, or std::nullopt when the context table or manager is unavailable (an unknown name is
     *         false, as in the game).
     */
    [[nodiscard]] std::optional<bool> soul_has_script_context(std::uintptr_t soul, std::string_view name) noexcept;

    /** @brief A soul's WUID (+0x30). */
    [[nodiscard]] Wuid soul_wuid(std::uintptr_t soul) noexcept;

    /** @brief Resolves a soul WUID through the RPG module's soul table. */
    [[nodiscard]] std::uintptr_t soul_from_wuid(Wuid wuid) noexcept;

    /** @brief A soul's C_Inventory (soul vtable slot 60, then the holder's slot 0). */
    [[nodiscard]] std::uintptr_t soul_inventory(std::uintptr_t soul) noexcept;

    /**
     * @brief player.actor:CanLoot(victim), the game's own loot-action test.
     * @param looter The player's actor (C_Player).
     * @param victim The victim's entity id.
     * @return The verdict, or std::nullopt when a part could not be evaluated.
     */
    [[nodiscard]] std::optional<bool> actor_can_loot(std::uintptr_t looter, EntityId victim) noexcept;

    // Inventories

    /** @brief The number of items an inventory holds (every item, hidden ones included). */
    [[nodiscard]] std::optional<std::size_t> inventory_item_count(std::uintptr_t inventory) noexcept;

    /**
     * @brief EntityModule.HasPlayerVisibleItems: true when no item the player would see in the transfer screen is
     *        left, the test behind the game's "(empty)" prompt.
     */
    [[nodiscard]] std::optional<bool> inventory_is_empty_for_player(std::uintptr_t inventory) noexcept;

    /** @brief EntityModule.CanUseInventory. */
    [[nodiscard]] std::optional<bool> inventory_is_usable(std::uintptr_t inventory) noexcept;

    /** @brief An inventory's WUID. */
    [[nodiscard]] Wuid inventory_wuid(std::uintptr_t inventory) noexcept;

    /** @brief Resolves an inventory WUID through the inventory manager. */
    [[nodiscard]] std::uintptr_t inventory_from_wuid(Wuid wuid) noexcept;

    /** @brief EntityModule.GetInventoryOwner: the WUID of the soul that owns an inventory, or 0. */
    [[nodiscard]] std::optional<Wuid> inventory_owner(std::uintptr_t inventory) noexcept;

    // World items

    /**
     * @brief Copies the world items of the item system whose entity lies within @p reach of @p center.
     * @details Walked like collect_actors_near(): plain reads under SEH for the whole map, validation for the items in
     *          reach.
     * @param out Receives the items (cleared first).
     * @param center The centre.
     * @param reach The distance in metres.
     * @param max_items Safety bound on the walk.
     * @param total Receives the number of items walked, when not null.
     * @return True when the map was walked to its end.
     */
    [[nodiscard]] bool collect_items_near(
        std::vector<ItemView> &out,
        const game_structures::Vec3f &center,
        float reach,
        std::size_t max_items,
        std::size_t *total = nullptr
    );

    /**
     * @brief The size field of the item system's map: every world item of the level, so a change means an item was
     *        dropped, spawned or picked up.
     * @return The count, or std::nullopt when the item system is unavailable.
     */
    [[nodiscard]] std::optional<std::size_t> item_map_size() noexcept;

    /**
     * @enum EntityPresence
     * @brief Where a remembered entity is now.
     */
    enum class EntityPresence : std::uint8_t
    {
        /// The pointer no longer carries the id and class it was remembered with (deleted or reused).
        Gone,
        /// Still the same entity, beyond the reach.
        Far,
        /// Still the same entity, within the reach.
        Near,
    };

    /**
     * @brief Tests, with plain reads under SEH, whether @p entity still carries @p id and class @p klass, and whether
     *        it lies within @p reach of @p center.
     * @details For filtering large remembered entity sets cheaply, and for dropping the entries that are gone; a
     *          pointer that passes is still validated before anything is called on it.
     */
    [[nodiscard]] EntityPresence entity_presence(
        std::uintptr_t entity,
        EntityId id,
        std::uintptr_t klass,
        const game_structures::Vec3f &center,
        float reach
    ) noexcept;

    /**
     * @brief Tests that a remembered world item still belongs to @p entity (the item's entity pointer reads back
     *        unchanged); @p entity must come from a validated lookup of the item's id.
     */
    [[nodiscard]] bool item_still_on(const ItemView &item, std::uintptr_t entity) noexcept;

    /** @brief The world item of an entity (IItemSystem::GetItem), as a C_PickableItem. */
    [[nodiscard]] std::optional<ItemView> item_of(EntityId id) noexcept;

    /**
     * @brief The item-class half of item:CanUse: a player item (class IsA 25) that is not in use. The reach half
     *        (a pickup helper within wh_pl_PickMaxDistance of the user) is left out, so it holds at any distance.
     */
    [[nodiscard]] std::optional<bool> item_is_pickable(const ItemView &item) noexcept;

    /** @brief item:IsUsed() (the in-use bit of the C_PickableItem). */
    [[nodiscard]] std::optional<bool> item_in_use(const ItemView &item) noexcept;

    /** @brief The script field npcOnly, which the game mirrors from the C_Item flags. */
    [[nodiscard]] std::optional<bool> item_is_npc_only(const ItemView &item) noexcept;

    /** @brief What holds a world item (the C_Item owner). */
    [[nodiscard]] ItemHolder item_holder(const ItemView &item) noexcept;

    /** @brief The raw owner object of a world item (+0x98, else +0x90), or 0. */
    [[nodiscard]] std::uintptr_t item_owner(const ItemView &item) noexcept;

    /** @brief item:CanSteal(user). */
    [[nodiscard]] std::optional<bool> item_can_steal(const ItemView &item, EntityId user) noexcept;

    /** @brief item:IsFromShop(): a shop lists the item among its goods. */
    [[nodiscard]] std::optional<bool> item_is_from_shop(const ItemView &item) noexcept;

    // Stashes and shops

    /** @brief The C_Stash game-object extension of an entity. */
    [[nodiscard]] std::uintptr_t stash_of(EntityId id) noexcept;

    /**
     * @brief The inventory a stash opens: its master stash's (stash:GetMasterInventory) when it is linked to one,
     *        else its own.
     * @param stash A C_Stash.
     * @return The C_Inventory, or 0.
     */
    [[nodiscard]] std::uintptr_t stash_inventory(std::uintptr_t stash) noexcept;

    // Script-table fields the game keeps only on the entity (read through the engine's IScriptTable)

    /** @brief A boolean field of an entity's script table; std::nullopt when absent or not a boolean. */
    [[nodiscard]] std::optional<bool> script_bool(std::uintptr_t entity, const char *key) noexcept;

    /** @brief A number field of an entity's script table; std::nullopt when absent or not a number. */
    [[nodiscard]] std::optional<float> script_number(std::uintptr_t entity, const char *key) noexcept;

    /** @brief A handle field (a WUID) of an entity's script table; std::nullopt when absent or not a handle. */
    [[nodiscard]] std::optional<Wuid> script_handle(std::uintptr_t entity, const char *key) noexcept;

    /**
     * @brief A boolean field of a table inside an entity's script table (Properties.bIsDirectlyReadable).
     * @param entity A CEntity.
     * @param table_key The nested table's key ("Properties").
     * @param key The field's key in that table.
     * @return The value; std::nullopt when either level is absent or of another type.
     * @note Main thread (it runs the script system).
     */
    [[nodiscard]] std::optional<bool>
    script_table_bool(std::uintptr_t entity, const char *table_key, const char *key) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_GAME_NATIVES_HPP
