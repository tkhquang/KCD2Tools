/**
 * @file core/scene_move.hpp
 * @brief Moves a cutscene's runtime layer (its entities and the keys of its sequence) to another spot of the level.
 *
 * A layer saves every entity at a world position, except entities parented to another (local to the parent). A
 * sequence's Position keys are world positions too, except while a LinkTo key attaches the node to someone (then they
 * are local to that bone). The game authors a cutscene's animations around one point: its characters stand there and
 * their clips carry the moves, so shifting the entities and the world keys by one offset keeps the scene as it was.
 * Rotations stay: the scene keeps its heading.
 */

#ifndef ANIMFW_CORE_SCENE_MOVE_HPP
#define ANIMFW_CORE_SCENE_MOVE_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace AnimationFramework
{
    /// A point of the level, in metres.
    struct ScenePoint
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    /** @brief The point that `x,y,z` (a layer's `Pos` value) names, or nothing when it is not three numbers. */
    [[nodiscard]] std::optional<ScenePoint> parse_scene_point(std::string_view text);

    /**
     * @brief The point the sequence @p sequence of @p layer_xml is authored around: the position most of the
     *        characters it animates (AnimChar entities with an Animation track) stand at, or nothing when it animates
     *        none.
     */
    [[nodiscard]] std::optional<ScenePoint> scene_origin(std::string_view layer_xml, std::string_view sequence);

    /** @brief A moved layer and what moved. */
    struct MovedScene
    {
        std::string layer_xml;
        /// Entities moved (top-level entities with a position).
        std::size_t entities = 0;
        /// Position keys and default values moved.
        std::size_t keys = 0;
    };

    /**
     * @brief @p layer_xml with its entities and its sequences' world positions moved by @p offset.
     * @details Moved: the Pos of every entity that is not parented to another, and the Position keys of the sequence
     *          nodes that drive those entities, except keys while a LinkTo key attaches the node. Nodes of entities
     *          outside the layer (the level's own doors and props) stay where they are.
     */
    [[nodiscard]] MovedScene move_scene(std::string_view layer_xml, ScenePoint offset);
} // namespace AnimationFramework

#endif // ANIMFW_CORE_SCENE_MOVE_HPP
