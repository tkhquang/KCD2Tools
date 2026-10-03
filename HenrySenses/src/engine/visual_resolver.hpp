/**
 * @file engine/visual_resolver.hpp
 * @brief Finds the mesh the player sees for an interaction trigger.
 *
 * Script triggers (wash tub, bench seat, bed) are invisible helpers placed on or above the object they belong to;
 * the object itself is a separate brush or entity. Two lookups find it:
 * - at a known pose: the prefab template a trigger was placed with (engine/prefab_templates.hpp) predicts where its
 *   object stands, and the brush or entity at that pose (within a few centimetres) is the object, whatever model the
 *   level put there;
 * - by search: the render octree's brushes and entity meshes around the trigger, of which the plausible one named
 *   after the trigger's object and centred nearest under it wins, skipping engine helpers, floors, actors and items.
 */
#ifndef HENRYSENSES_VISUAL_RESOLVER_HPP
#define HENRYSENSES_VISUAL_RESOLVER_HPP

#include "game_structures.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HenrySenses
{
    /**
     * @struct VisualNode
     * @brief The render node that shows an object.
     */
    struct VisualNode
    {
        std::uintptr_t node{0};
        /// IRenderNode::GetRenderNodeType (1 brush, 0x11 movable brush, 0x13 entity render proxy).
        std::uint32_t type{0};
        game_structures::Aabb bounds{};
    };

    /**
     * @struct TriggerSearch
     * @brief What resolve_trigger_visual() searches for.
     */
    struct TriggerSearch
    {
        /// Trigger world position.
        game_structures::Vec3f position{};
        /// The trigger's own world bounds (an area trigger's footprint), when it has any.
        std::optional<game_structures::Aabb> trigger_bounds{};
        /// The trigger's own render node, skipped (0 for none).
        std::uintptr_t exclude_node{0};
        /**
         * @brief Lower-case words the object's model file is expected to carry: the kind's words and the trigger
         * class's noun ("bed" of BedTrigger). When given, only a mesh named by one of them (or by the words below) is
         * accepted, exempt from the floor test (a low bed is flat); otherwise the trigger keeps its marker.
         */
        std::span<const std::string> keywords{};
        /**
         * @brief Words of what a bed may be made of (fur, straw, pillow, ...): the weakest name, below the keywords, so
         * a makeshift bed of furs on planks still outlines while a real bed under a pillow wins.
         */
        std::span<const std::string_view> bedding_words{};
        /**
         * @brief The trigger's prefab-instance tag (prefab_instance_tag()): an entity mesh of the same instance counts
         * as named, and so does a brush whose model file carries a furniture word of the prefab's name
         * ("[Bed/bed_bench16_<guid>]" -> a bench), ranked just below it.
         */
        std::string_view prefab_tag{};
        /// Meshes considered as well: a highlighted mesh leaves the octree (it is moved to the always-visible list).
        std::span<const std::uintptr_t> extra_nodes{};
        /**
         * @brief Words of the trigger's own name other than its class's noun ("trigger_pan" -> {"pan"}). A brush whose
         * file name has the last one and whose footprint covers the trigger is the strongest match; one with another of
         * them ranks just above the keywords.
         */
        std::span<const std::string> own_words{};
    };

    /**
     * @brief Finds the visible mesh that belongs to a trigger by searching around it.
     * @param search What to search for.
     * @param trace When set, receives one line per candidate mesh with its size and verdict, for the log: the first
     *        few when a mesh was found, every node in the box (with why it was dropped) when none was.
     * @return The node, or std::nullopt when nothing plausible lies under the trigger.
     * @note Main thread only.
     */
    [[nodiscard]] std::optional<VisualNode> resolve_trigger_visual(const TriggerSearch &search, std::string *trace);

    /**
     * @struct FacedSearch
     * @brief What resolve_faced_visual() searches for: a trigger and the spot the game puts the player at to use it.
     */
    struct FacedSearch
    {
        /// Trigger world position: the object must cover it.
        game_structures::Vec3f trigger{};
        /// The use spot (the smart object the trigger links: where the player kneels, stands or sits).
        game_structures::Vec3f spot{};
        /// The use spot's facing (its local +Y in the world).
        game_structures::Vec3f forward{};
        /// The trigger's own render node, skipped (0 for none).
        std::uintptr_t exclude_node{0};
        /// Meshes considered as well: a highlighted mesh leaves the octree (it is moved to the always-visible list).
        std::span<const std::uintptr_t> extra_nodes{};
    };

    /**
     * @brief Finds a trigger's object that no model name identifies, from where the game makes the player use it.
     * @details Only a mesh that both of the prefab's anchors agree on is taken: the use spot faces it within reach,
     *          and it covers the trigger point (a praying spot kneels a metre in front of its trigger, placed at the
     *          shrine). The spot must not lie on it (a pier the player stands on at a pond), it must not be a floor,
     *          and no other mesh may qualify about as well; otherwise the trigger keeps its marker.
     * @param search What to search for.
     * @param trace When set, receives one line per candidate mesh with its verdict, for the log.
     * @return The node, or std::nullopt.
     * @note Main thread only.
     */
    [[nodiscard]] std::optional<VisualNode> resolve_faced_visual(const FacedSearch &search, std::string *trace);

    /**
     * @struct PoseHit
     * @brief A brush found at a predicted pose.
     */
    struct PoseHit
    {
        std::uintptr_t node{0};
        std::uint32_t type{0};
        game_structures::Aabb bounds{};
        /// Its model path.
        std::string model{};
        /// Distance between its pivot and the predicted pose, metres.
        float offset{0.0f};
        /// Brushes that stood at the pose (more than one: a duplicate placed on top).
        std::size_t matches{0};
    };

    /**
     * @brief Finds the brush whose pivot stands at @p pose (within 3 cm), whatever its model.
     * @details Level designers swap a template's model for another one at the same pose (bed_shabby_a ->
     *          bed_shabby_b), so the model only breaks a tie: the template's model first, then the brush whose bounds
     *          contain the trigger's footprint point, then the smallest. Its size is not tested (a canopy bed is 15
     *          m3).
     * @param pose The predicted world pivot.
     * @param expected_model The template's model path (lower case, '/').
     * @param trigger The trigger's position (the tie-break's footprint point).
     * @param extra_nodes Highlighted brushes (off the octree) to consider as well.
     * @param trace When set, receives what stood near the pose, for the log.
     * @return The brush, or std::nullopt when none stands there (not streamed in yet, or replaced by another object).
     * @note Main thread only.
     */
    [[nodiscard]] std::optional<PoseHit> find_brush_at_pose(
        const game_structures::Vec3f &pose,
        std::string_view expected_model,
        const game_structures::Vec3f &trigger,
        std::span<const std::uintptr_t> extra_nodes,
        std::string *trace
    );

    /**
     * @struct PrefabBrushHit
     * @brief A brush a runtime prefab spawned, and the prefab entity that owns it.
     */
    struct PrefabBrushHit
    {
        PoseHit brush{};
        /// The RuntimePrefab entity's id.
        std::uint32_t owner_id{0};
        /// Distance between the RuntimePrefab entity and the predicted pose, metres.
        float owner_offset{0.0f};
    };

    /**
     * @brief Finds the brush a runtime prefab spawned (a COwnedBrush) that shows a trigger's object, from the prefab
     *        entity's predicted position.
     * @details A RuntimePrefab entity draws nothing, so it has no render node to find; its spawned brushes do, and each
     *          names its owner entity. The owner must be a RuntimePrefab of the trigger's prefab instance standing at
     *          the predicted position; the nearest such owner's brushes are the candidates.
     * @param pose The RuntimePrefab entity's predicted world position.
     * @param prefab_tag The trigger's prefab-instance tag, which the owner's name must carry (empty: not tested).
     * @param tolerance How far from @p pose the owner may stand, metres.
     * @param words The trigger's words ("kettle", "cauldron"): a brush named by one wins; otherwise the only one.
     * @param extra_nodes Highlighted brushes (off the octree) to consider as well.
     * @param trace When set, receives the owned brushes seen, for the log.
     * @return The brush, or std::nullopt when no such prefab owns a brush here, or it owns several none of which is
     *         named.
     * @note Main thread only.
     */
    [[nodiscard]] std::optional<PrefabBrushHit> find_prefab_brush(
        const game_structures::Vec3f &pose,
        std::string_view prefab_tag,
        float tolerance,
        std::span<const std::string> words,
        std::span<const std::uintptr_t> extra_nodes,
        std::string *trace
    );

    /**
     * @struct EntityHit
     * @brief An entity found at a predicted pose.
     */
    struct EntityHit
    {
        std::uintptr_t entity{0};
        std::uint32_t id{0};
        game_structures::Vec3f position{};
        game_structures::Aabb bounds{};
        /// Distance between its position and the predicted pose, metres.
        float offset{0.0f};
    };

    /**
     * @brief Finds the visible entity of class @p entity_class standing at @p pose.
     * @param pose The predicted world position.
     * @param entity_class The class it must have ("RuntimePrefab", "Chair").
     * @param prefab_tag When not empty, the prefab-instance tag its name must carry.
     * @param tolerance How far from @p pose it may stand, metres.
     * @param trace When set, receives what was seen, for the log.
     * @return The nearest such entity, or std::nullopt.
     * @note Main thread only.
     */
    [[nodiscard]] std::optional<EntityHit> find_entity_at_pose(
        const game_structures::Vec3f &pose,
        std::string_view entity_class,
        std::string_view prefab_tag,
        float tolerance,
        std::string *trace
    );

    /**
     * @brief Finds a brush again by what it looked like (model path and bounds), after the one found earlier was freed
     *        and recreated by streaming.
     * @param model Its model path.
     * @param bounds Its bounds (within 1 cm).
     * @param extra_nodes Highlighted brushes (off the octree) to consider as well.
     * @return The brush, or std::nullopt.
     * @note Main thread only.
     */
    [[nodiscard]] std::optional<PoseHit> find_brush_like(
        std::string_view model,
        const game_structures::Aabb &bounds,
        std::span<const std::uintptr_t> extra_nodes
    );

    /**
     * @brief The words of a trigger's own name that name its object: "trigger_pan" -> {"pan"},
     *        "KettleActionTrigger1" -> {"kettle"}. Generic words (trigger, action, sequence, ...) and the prefab tag
     *        are
     *        left out.
     */
    [[nodiscard]] std::vector<std::string> trigger_name_words(std::string_view entity_name);

    /**
     * @brief The prefab-instance tag of an entity name: everything from its first '[' ("[Fireplace/fireplace_home5_
     *        <guid>]"). Entities placed together in one prefab instance share it; an empty view when there is none.
     */
    [[nodiscard]] std::string_view prefab_instance_tag(std::string_view entity_name) noexcept;

    /**
     * @brief True when a word of @p text is one of @p words (or its plural with 's' or 'es'): "benches_a" has bench,
     *        "fur_c_benched" does not. A model path is split in its file name, an entity name before its prefab tag
     *        (at non-letters and camel-case capitals).
     */
    [[nodiscard]] bool has_word(std::string_view text, std::span<const std::string_view> words);

    /** @brief Matches words supplied as owning strings. */
    [[nodiscard]] bool has_word(std::string_view text, std::span<const std::string> words);

    /** @brief Compares two model paths as the engine names them (case and slash direction ignored). */
    [[nodiscard]] bool same_model(std::string_view a, std::string_view b) noexcept;

    /**
     * @brief Reports whether a node is a brush (CBrush, COwnedBrush or CMovableBrush).
     */
    [[nodiscard]] bool is_brush_node(std::uintptr_t node) noexcept;

    /**
     * @brief Reports whether a brush found earlier can still be touched: still a brush, with the same bounds.
     * @param brush The brush address.
     * @param bounds Its bounds when it was found.
     * @return True when the brush is the one seen earlier.
     * @note Main thread only.
     */
    [[nodiscard]] bool brush_matches(std::uintptr_t brush, const game_structures::Aabb &bounds) noexcept;

    /** @brief Reports whether a node is a CMovableBrush (a brush whose matrix may change while it lives). */
    [[nodiscard]] bool is_movable_brush_node(std::uintptr_t node) noexcept;

    /**
     * @brief Re-checks a brush found earlier and returns its bounds now.
     * @details A freed brush whose memory was reused fails the identity check. A CBrush or COwnedBrush never moves, so
     *          one reused by another brush fails the bounds test (brush_matches()). A CMovableBrush may move, so its
     *          bounds are not compared: it must still show @p model, and its new bounds are returned. With an empty
     *          @p model every brush takes the bounds test.
     * @param brush The brush address.
     * @param bounds Its bounds when it was found.
     * @param model Its model path when it was found (a movable brush's), or empty.
     * @return The brush's bounds now, or std::nullopt when the node is no longer that brush.
     * @note Main thread only.
     */
    [[nodiscard]] std::optional<game_structures::Aabb>
    brush_revalidate(std::uintptr_t brush, const game_structures::Aabb &bounds, std::string_view model) noexcept;

    /**
     * @brief Copies a render node's name (a brush's model path, an entity proxy's entity name), for log lines.
     */
    [[nodiscard]] std::string render_node_name(std::uintptr_t node);

} // namespace HenrySenses

#endif // HENRYSENSES_VISUAL_RESOLVER_HPP
