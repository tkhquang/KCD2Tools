/**
 * @file engine/entity_access.hpp
 * @brief EntityId to CEntity to render node, the per-node fields a highlight writes, and entity enumeration.
 *
 * Path: IEntitySystem (gEnv + GENV_ENTITY_SYSTEM_OFFSET) resolves an EntityId to a CEntity; CEntity::GetProxy
 * (vtable slot 74, key ENTITY_PROXY_RENDER) returns the IEntityRenderProxy secondary base of a CRenderProxy, whose
 * primary base (value - RENDER_PROXY_SECONDARY_OFFSET) is the IRenderNode. A highlight writes the 4-byte HUD
 * silhouette word at RENDERNODE_HUD_SILHOUETTE_OFFSET, invalidates the node's persistent render object so the next
 * frame refills it, and may move the node into the 3D engine's always-visible list so occlusion culling cannot drop
 * it.
 *
 * Every virtual call is validated against its AOB-resolved function and runs under SEH. Node pointers are never
 * cached across calls: proxies are recreated on stream-out and on load, so every apply resolves again from the
 * EntityId. Every function here must run on the game main thread.
 */
#ifndef HENRYSENSES_ENTITY_ACCESS_HPP
#define HENRYSENSES_ENTITY_ACCESS_HPP

#include "game_structures.hpp"

#include <DetourModKit/error.hpp>
#include <DetourModKit/memory.hpp>

#include <xmmintrin.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace HenrySenses
{
    /// Engine entity identifier (IEntitySystem EntityId).
    using EntityId = std::uint32_t;

    /**
     * @brief Starts loading the cache line at @p address, so a loop over many engine objects overlaps their cache
     *        misses instead of taking them one after another.
     * @details A prefetch is a hint and never faults, so it needs no guard; the guarded read that follows stays the
     *          only access. The level-wide walks read objects scattered over the game heap, one cold line each.
     */
    inline void prefetch_line(std::uintptr_t address) noexcept
    {
        if (DetourModKit::memory::is_plausible_ptr(DetourModKit::Address{address}))
        {
            _mm_prefetch(reinterpret_cast<const char *>(address), _MM_HINT_T0);
        }
    }

    /**
     * @brief Prefetches an entity's identity line (flags, id, class) and, when asked, the two lines of its world
     *        matrix.
     */
    void prefetch_entity(std::uintptr_t entity, bool with_matrix) noexcept;

    /// How many entries ahead of the current slot the level-wide walks prefetch.
    inline constexpr std::size_t PREFETCH_DISTANCE = 8;

    /**
     * @class ClassMemo
     * @brief A small direct-mapped memo of a per-class value, in front of a map lookup: an entity walk looks one class
     *        up per entity (~98k per walk) among ~150 classes.
     * @tparam Value The value kept per class.
     */
    template <typename Value> class ClassMemo
    {
    public:
        /** @brief The value remembered for @p klass, or nullptr. */
        [[nodiscard]] const Value *find(std::uintptr_t klass) const noexcept
        {
            const Slot &slot = m_slots[index(klass)];
            return klass != 0 && slot.klass == klass ? &slot.value : nullptr;
        }

        /** @brief Remembers @p value for @p klass (replacing whatever shared its slot). */
        void store(std::uintptr_t klass, const Value &value) noexcept { m_slots[index(klass)] = Slot{klass, value}; }

        /** @brief Forgets every class. */
        void clear() noexcept { m_slots = {}; }

    private:
        struct Slot
        {
            std::uintptr_t klass{0};
            Value value{};
        };

        static constexpr std::size_t SLOTS = 256;

        [[nodiscard]] static std::size_t index(std::uintptr_t klass) noexcept
        {
            return ((klass >> 4) ^ (klass >> 12)) & (SLOTS - 1);
        }

        std::array<Slot, SLOTS> m_slots{};
    };

    /**
     * @enum RenderAlwaysResult
     * @brief Outcome of moving a render node into (or out of) the always-visible list.
     */
    enum class RenderAlwaysResult : std::uint8_t
    {
        /// The node was moved.
        Applied,
        /// The node already carried ERF_RENDER_ALWAYS from the engine or another highlight set, so nothing changed.
        AlreadyAlways,
        /// The node is not registered with the 3D engine (streamed out, hidden, or excluded); nothing was changed.
        NotRegistered,
        /// The 3D engine or its validated slots are unavailable, or a call faulted.
        Failed,
    };

    /**
     * @brief Checks that the entity-side validators resolved.
     * @return An empty value when entity access can run, or NoMatch naming the missing anchor.
     */
    [[nodiscard]] DMK::Result<void> initialize_entity_access();

    /** @brief Marks entity access unavailable. */
    void shutdown_entity_access() noexcept;

    /**
     * @brief Reports whether entity lookups are available.
     * @return True once initialize_entity_access() succeeded.
     */
    [[nodiscard]] bool entity_access_available() noexcept;

    /**
     * @brief Opens a main-thread tick: its first lookup validates the entity system and its GetEntity slot, and the
     *        tick's other lookups (entity_from_id(), EntityLookup, the counters) reuse them.
     * @note Main thread, at the start of each tick; pair with end_entity_tick().
     */
    void begin_entity_tick() noexcept;

    /**
     * @brief Drops what begin_entity_tick() validated; lookups outside a tick validate on every call again.
     * @note Main thread, at the end of each tick.
     */
    void end_entity_tick() noexcept;

    /**
     * @brief Resolves an EntityId to its CEntity.
     * @param id The entity id.
     * @return The CEntity address (RTTI-validated), or 0 when the id is unknown or access is unavailable.
     */
    [[nodiscard]] std::uintptr_t entity_from_id(EntityId id) noexcept;

    /**
     * @brief Reads an entity's own EntityId.
     * @param entity A CEntity.
     * @return The id, or 0 on a failed read.
     */
    [[nodiscard]] EntityId entity_id_of(std::uintptr_t entity) noexcept;

    /**
     * @brief Returns the IRenderNode (CRenderProxy primary base) of an entity.
     * @param entity A CEntity from entity_from_id().
     * @return The render node address, or 0 when the entity has no render proxy.
     */
    [[nodiscard]] std::uintptr_t render_node_of(std::uintptr_t entity) noexcept;

    /**
     * @brief Returns one proxy of an entity (CEntity::GetProxy).
     * @param entity A CEntity.
     * @param type The proxy map key (constants::ENTITY_PROXY_*).
     * @return The proxy interface pointer, or 0.
     */
    [[nodiscard]] std::uintptr_t entity_proxy(std::uintptr_t entity, std::uint32_t type) noexcept;

    /**
     * @brief Reports whether an entity is hidden (the flag entity:IsHidden() reads).
     * @param entity A CEntity.
     * @return True when hidden or unreadable.
     */
    [[nodiscard]] bool entity_is_hidden(std::uintptr_t entity) noexcept;

    /**
     * @brief Reads an entity's internal flag word (constants::ENTITY_FLAGS_OFFSET).
     * @param entity A CEntity.
     * @return The flags, or std::nullopt on a failed read.
     */
    [[nodiscard]] std::optional<std::uint32_t> entity_flags(std::uintptr_t entity) noexcept;

    /**
     * @brief Reports whether a flag word says the game keeps the entity from being drawn without hiding it: Invisible
     *        set, Hidden clear.
     * @details KCD2 makes the NPCs and items the camera cannot see invisible (and inactive) and shows them again when
     *          the camera can. The entity keeps its render node, so a silhouette word stays in place, but nothing is
     *          drawn, so it cannot show an outline meanwhile.
     */
    [[nodiscard]] bool entity_flags_game_hidden(std::uint32_t flags) noexcept;

    /** @brief Reports whether a flag word has the entity active (constants::ENTITY_ACTIVE_FLAG), for log lines. */
    [[nodiscard]] bool entity_flags_active(std::uint32_t flags) noexcept;

    /**
     * @brief Copies an entity's class name.
     * @param entity A CEntity.
     * @return The class name, or an empty string.
     */
    [[nodiscard]] std::string entity_class_name(std::uintptr_t entity);

    /**
     * @brief Copies an entity's name.
     * @param entity A CEntity.
     * @return The entity name, or an empty string.
     */
    [[nodiscard]] std::string entity_name(std::uintptr_t entity);

    /**
     * @brief Copies the model file an entity shows: the path of its first slot's static mesh (.cgf) or character
     *        (.cdf, .chr).
     * @return The path, or an empty string when the entity shows no model.
     */
    [[nodiscard]] std::string entity_model_path(std::uintptr_t entity);

    /**
     * @brief Reads an entity's world position (the translation column of its world matrix).
     * @param entity A CEntity.
     * @return The position, or std::nullopt on a failed read.
     */
    [[nodiscard]] std::optional<game_structures::Vec3f> entity_world_position(std::uintptr_t entity) noexcept;

    /**
     * @brief Returns an entity's world-space bounds (CEntity::GetWorldBounds).
     * @param entity A CEntity.
     * @return The bounds, or std::nullopt when the call is unavailable, faults, or the bounds are empty.
     */
    [[nodiscard]] std::optional<game_structures::Aabb> entity_world_bounds(std::uintptr_t entity) noexcept;

    /**
     * @brief Reads the HUD silhouette word of a render node.
     * @param node A render node from render_node_of().
     * @return The word, or std::nullopt on a failed read.
     */
    [[nodiscard]] std::optional<std::uint32_t> read_hud_word(std::uintptr_t node) noexcept;

    /**
     * @brief Writes exactly 4 bytes of HUD silhouette word into a render node.
     * @param node A render node from render_node_of().
     * @param word Packed 0xRRGGBBAA, or 0 to clear.
     * @return True when the write landed.
     */
    [[nodiscard]] bool write_hud_word(std::uintptr_t node, std::uint32_t word) noexcept;

    /**
     * @brief Invalidates a render node's persistent render object so the next frame refills it.
     * @param node A render node from render_node_of().
     * @return True when the node had render data and the invalidation was applied.
     */
    [[nodiscard]] bool invalidate_render_object(std::uintptr_t node) noexcept;

    /**
     * @brief Reads whether ERF_RENDER_ALWAYS is set on a render node.
     * @param node A render node from render_node_of().
     * @return The bit state, or std::nullopt on a failed read.
     */
    [[nodiscard]] std::optional<bool> read_render_always(std::uintptr_t node) noexcept;

    /**
     * @brief Reads the distance past which the 3D engine stops drawing a render node.
     * @param node A render node from render_node_of() or a brush.
     * @return The distance in metres, or std::nullopt on a failed read.
     */
    [[nodiscard]] std::optional<float> read_max_view_dist(std::uintptr_t node) noexcept;

    /**
     * @brief Reads a render node's view-distance ratio (100 is the default reach, 255 the farthest).
     * @param node A render node from render_node_of() or a brush.
     * @return The ratio, or std::nullopt on a failed read.
     */
    [[nodiscard]] std::optional<std::uint8_t> read_view_dist_ratio(std::uintptr_t node) noexcept;

    /**
     * @struct RenderNodeWants
     * @brief The changes a highlight needs on its render node.
     */
    struct RenderNodeWants
    {
        /// The node moves into the always-visible list, which skips the occlusion test.
        bool always_visible{false};
        /// The node carries constants::VIEW_DIST_RATIO_FAR, so the engine draws it out to any highlight radius.
        bool raise_view_distance{false};
    };

    /**
     * @struct RenderNodeOverrides
     * @brief The changes the mod holds on one render node, with what a restore needs.
     */
    struct RenderNodeOverrides
    {
        /// The mod moved the node into the always-visible list.
        bool always_visible{false};
        /**
         * @brief The node's own view-distance ratio while the mod holds the node at constants::VIEW_DIST_RATIO_FAR,
         *        or std::nullopt while the ratio is the node's own.
         */
        std::optional<std::uint8_t> own_view_dist_ratio{};

        /** @brief Reports whether the mod holds a change on the node. */
        [[nodiscard]] bool changed() const noexcept { return always_visible || own_view_dist_ratio.has_value(); }
    };

    /**
     * @brief Brings the mod's changes on a render node to what a highlight needs.
     * @details A silhouette shows only on a mesh that the 3D engine draws. The engine skips every node past its max
     *          view distance, in the octree and in the always-visible list alike. A small item reaches only 5 to 25 m,
     *          and the far ratio multiplies that reach by 100.
     *
     *          Each change takes effect through a re-registration of the node. A move into or out of the
     *          always-visible list writes the ratio inside its own re-registration. A ratio that the game rewrote
     *          during a raise becomes the node's own ratio, and the call raises the node again.
     * @param node The node, resolved this frame.
     * @param overrides The changes on @p node, updated to what is in place afterwards.
     * @param wants The changes the highlight needs.
     * @return The outcome of the always-visible move when one ran, else std::nullopt.
     * @note Main thread only.
     */
    std::optional<RenderAlwaysResult> update_render_node_overrides(
        std::uintptr_t node,
        RenderNodeOverrides &overrides,
        const RenderNodeWants &wants
    ) noexcept;

    /**
     * @brief Undoes the mod's changes on a render node and empties @p overrides.
     * @details The node moves back into the octree and gets its own view-distance ratio back. A ratio that the game
     *          rewrote during the raise belongs to the game and stays.
     * @param node The node that the changes were applied to, resolved this frame.
     * @param overrides The changes on @p node.
     * @return The outcome of the move out of the always-visible list when one ran, else std::nullopt.
     * @note Main thread only.
     */
    std::optional<RenderAlwaysResult>
    restore_render_node_overrides(std::uintptr_t node, RenderNodeOverrides &overrides) noexcept;

    /**
     * @class EntityLookup
     * @brief entity_from_id() for many ids in a row: the entity system and its GetEntity slot are validated once, at
     *        construction, instead of on every lookup.
     * @note Main thread; construct one per batch, never keep it across frames.
     */
    class EntityLookup
    {
    public:
        EntityLookup() noexcept;

        /** @brief The entity with @p id, or 0 (as entity_from_id()). */
        [[nodiscard]] std::uintptr_t find(EntityId id) const noexcept;

    private:
        std::uintptr_t m_system{0};
        std::uintptr_t m_get_entity{0};
    };

    /**
     * @struct WalkedEntity
     * @brief One entity an EntityWalk step returned, with its class and id.
     */
    struct WalkedEntity
    {
        std::uintptr_t entity{0};
        /// Its IEntityClass, or 0 when unreadable.
        std::uintptr_t klass{0};
        EntityId id{0};
    };

    /**
     * @class EntityWalk
     * @brief An IEntitySystem iteration spread over several main-thread calls.
     * @details The walk reads the entity salt buffer directly (constants::ENTITY_SYSTEM_SALT_BUFFER_OFFSET) in slot
     *          order, in guarded chunks, and keeps every slot that holds an entity. A slot's salt gives the entity's id
     *          (slot | salt << 18) and a per-slot cache its class, so a walk reads only the entities it has not seen
     *          before (a whole-level walk otherwise misses the cache once per entity). The engine iterator
     *          (CEntityItMap)
     *          follows the buffer's used list instead, one dependent cache miss per entity, and takes the entity-system
     *          reader lock on every call. The direct layout is checked once per session against the iterator's first
     *          entity; if they differ, every walk uses the iterator. The direct walk holds only a slot number across
     *          frames; the iterator keeps a salt-checked handle, so an entity deleted under it only ends the walk
     *          early. The iterator is released by the step that reaches the end or by cancel(); the destructor does
     *          not release it, because a static walk is destroyed at image unload, after the engine may be gone.
     */
    class EntityWalk
    {
    public:
        /** @brief Outcome of one step. */
        enum class Step
        {
            /// The walk has more entities.
            More,
            /// The iterator reached its end and was released.
            Done,
            /// The walk could not start or an engine call faulted; the iterator was released.
            Failed,
        };

        EntityWalk() = default;
        EntityWalk(const EntityWalk &) = delete;
        EntityWalk &operator=(const EntityWalk &) = delete;

        /**
         * @brief Starts a walk (acquires and references a new engine iterator).
         * @return True when the walk started.
         */
        [[nodiscard]] bool begin() noexcept;

        /**
         * @brief Appends up to @p max_entities entities, with their classes and ids, to @p out.
         * @return More, Done or Failed.
         */
        [[nodiscard]] Step step(std::vector<WalkedEntity> &out, std::size_t max_entities) noexcept;

        /** @brief Ends an unfinished walk (releases the iterator of one that uses it). */
        void cancel() noexcept;

        /** @brief True while a walk is in progress. */
        [[nodiscard]] bool active() const noexcept { return m_iterator != 0 || m_direct_system != 0; }

    private:
        [[nodiscard]] Step step_direct(std::vector<WalkedEntity> &out, std::size_t max_entities) noexcept;

        std::uintptr_t m_iterator{0};
        std::uintptr_t m_is_end{0};
        std::uintptr_t m_next{0};
        std::uintptr_t m_release{0};
        // Direct walk: the entity system and the next salt-buffer slot to read (0 = end).
        std::uintptr_t m_direct_system{0};
        std::uint32_t m_slot{0};
    };

    /**
     * @struct EntityCounters
     * @brief The entity system's salt-buffer bookkeeping (constants::ENTITY_SYSTEM_SALT_COUNTERS_OFFSET).
     */
    struct EntityCounters
    {
        /// Pushes onto the used list (the sum of its three insert counters).
        std::uint64_t inserts{0};
        /// Frees of a used slot.
        std::uint64_t deletes{0};

        [[nodiscard]] bool operator==(const EntityCounters &) const noexcept = default;
    };

    /**
     * @brief Reads the salt-buffer counters with one guarded read and no engine lock.
     * @return The counters, or std::nullopt when entity access or the read failed.
     * @note Main thread.
     */
    [[nodiscard]] std::optional<EntityCounters> read_entity_counters() noexcept;

    /**
     * @enum RecentWalk
     * @brief Outcome of walk_recent_entities().
     */
    enum class RecentWalk : std::uint8_t
    {
        /// Every node walked held an entity (or the list ended first, so every live entity was walked).
        Complete,
        /**
         * @brief A node walked held no entity (a slot being allocated or freed): what was found is valid, but the walk
         * may have missed an entity and should run again.
         */
        Unsettled,
        /**
         * @brief The list could not be read, or the salt-buffer layout is not trusted (the engine iterator walks);
         * nothing was appended.
         */
        Failed,
    };

    /**
     * @brief Appends the entities of the first @p count nodes of the salt buffer's used list: every entity inserted
     *        since the insert counter was @p count lower is among them (each insert pushes at the head).
     * @details Guarded reads only, no engine lock; the count bounds the walk whatever the links read. Each entity
     *          comes with its id (slot | salt << 18) and class, as an EntityWalk step gives them.
     * @param count How many nodes to follow at most.
     * @param out Receives the entities.
     * @return Complete, Unsettled or Failed.
     * @note Main thread, after an EntityWalk has checked the direct layout.
     */
    [[nodiscard]] RecentWalk walk_recent_entities(std::size_t count, std::vector<WalkedEntity> &out) noexcept;

} // namespace HenrySenses

#endif // HENRYSENSES_ENTITY_ACCESS_HPP
