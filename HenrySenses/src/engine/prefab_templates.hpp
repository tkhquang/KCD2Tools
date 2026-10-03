/**
 * @file engine/prefab_templates.hpp
 * @brief The game's prefab template library, indexed by the interaction triggers its templates place, and the match
 *        that turns a placed trigger into the transform of its prefab instance.
 *
 * The game parses every Prefabs/*.xml at startup and keeps the trees for the session
 * (wh::framework::C_PrefabTemplateLibrary). A template lists its objects in template space: the trigger (class, name,
 * pose), the smart object its entity link points at, and the brush or entity that shows the object. A level instance
 * of a template keeps the trigger's name before its prefab tag ("BedTrigger12[Bed/bed_low54_<guid>]") and moves every
 * object by one transform, so the template whose trigger-to-linked-object offset matches the placed pair to a few
 * centimetres gives that transform, and with it the world pose of every object of the instance.
 *
 * The index is built once per session, a few templates per frame, with plain guarded reads: every node is checked to
 * be an XML node (the compact CXMLReadOnlyNode the library keeps, or a CXmlNode), every list is bounds-checked and
 * every loop is capped. Any structural surprise leaves the index failed for the session, and the callers fall back to
 * their search.
 */
#ifndef HENRYSENSES_PREFAB_TEMPLATES_HPP
#define HENRYSENSES_PREFAB_TEMPLATES_HPP

#include "game_structures.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace HenrySenses
{
    /**
     * @struct Quaternionf
     * @brief A rotation as the template XML writes it (Rotate = "w,x,y,z").
     */
    struct Quaternionf
    {
        float w{1.0f};
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
    };

    /**
     * @enum TemplateObjectKind
     * @brief What shows a template object in the world.
     */
    enum class TemplateObjectKind : std::uint8_t
    {
        /// A static brush (Type="Brush"), placed as a CBrush at the instance pose.
        Brush,
        /// A RuntimePrefab entity, which spawns its own template's brushes as COwnedBrush nodes it owns.
        RuntimePrefab,
        /// An entity that shows a mesh of its own (a Chair, an AnimObject, a GeomEntity).
        MeshEntity,
    };

    /**
     * @struct TemplateObject
     * @brief One object of a template that may be the mesh a trigger belongs to.
     */
    struct TemplateObject
    {
        TemplateObjectKind kind{TemplateObjectKind::Brush};
        /// The object's Name as written ("benchBrush", "chairEntity2").
        std::string name{};
        /// The entity class ("RuntimePrefab", "Chair"); empty for a brush.
        std::string entity_class{};
        /// A brush's model path, lower case with '/' separators; empty otherwise.
        std::string model{};
        game_structures::Vec3f position{};
        Quaternionf rotation{};
        game_structures::Vec3f scale{1.0f, 1.0f, 1.0f};
    };

    /**
     * @struct PrefabTemplate
     * @brief One indexed template: its library name and the objects that can show a trigger's object.
     */
    struct PrefabTemplate
    {
        /// The library name: the file path without "Prefabs/" and ".xml", lower case ("bed/bed_low").
        std::string name{};
        std::vector<TemplateObject> objects{};
    };

    /**
     * @struct TemplateTrigger
     * @brief One interaction trigger a template places.
     */
    struct TemplateTrigger
    {
        std::size_t template_index{0};
        game_structures::Vec3f position{};
        Quaternionf rotation{};
        game_structures::Vec3f scale{1.0f, 1.0f, 1.0f};
        /// Template-space positions of the objects its entity links point at, in link order.
        std::vector<game_structures::Vec3f> link_positions{};
    };

    /**
     * @struct TemplateMatch
     * @brief A placed trigger matched to a template: the instance transform (world = position + rotation * (scale *
     *        template point)).
     * @note The pointers stay valid until the next advance_prefab_templates() or reset_prefab_templates(); a match is
     *       used within the call that made it, never cached.
     */
    struct TemplateMatch
    {
        const PrefabTemplate *prefab{nullptr};
        const TemplateTrigger *trigger{nullptr};
        /// Instance rotation, row-major 3x3.
        std::array<float, 9> rotation{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        game_structures::Vec3f scale{1.0f, 1.0f, 1.0f};
        game_structures::Vec3f position{};
        /// Distance between the linked object's predicted and placed positions, metres.
        float error{0.0f};
    };

    /**
     * @enum TemplateMatchStatus
     * @brief Outcome of match_prefab_template().
     */
    enum class TemplateMatchStatus : std::uint8_t
    {
        /// The index is not built (or failed); nothing was looked up.
        NotReady,
        /// No template places a trigger of this class and name.
        NoTemplate,
        /// The trigger or its templates have no entity link to measure against.
        NoLink,
        /// Templates exist, but none puts the linked object where the level has it.
        Mismatch,
        /// A template matched within the tolerance.
        Matched,
    };

    /**
     * @struct TemplateMatchResult
     * @brief match_prefab_template()'s answer.
     */
    struct TemplateMatchResult
    {
        TemplateMatchStatus status{TemplateMatchStatus::NotReady};
        /// The best template (valid for Matched, and for Mismatch as the closest one).
        TemplateMatch match{};
        /// Templates tried.
        std::size_t candidates{0};
    };

    /**
     * @enum TemplateIndexState
     * @brief Progress of the template index.
     */
    enum class TemplateIndexState : std::uint8_t
    {
        /// Not started, or waiting for the library to exist.
        Idle,
        /// Being read, a few templates per frame.
        Building,
        /// Built; matches are answered.
        Ready,
        /// The library did not have the expected layout; nothing is matched this session.
        Failed,
    };

    /**
     * @brief Starts or continues the index build within @p budget_us of main-thread time, and once built, checks now
     *        and then that the library is still the one indexed (a new one is indexed again).
     * @param budget_us Main-thread time this call may spend.
     * @note Main thread only.
     */
    void advance_prefab_templates(std::int64_t budget_us);

    /** @brief The index state. */
    [[nodiscard]] TemplateIndexState prefab_template_state() noexcept;

    /**
     * @brief Drops the index; the next advance_prefab_templates() starts over.
     * @note Main thread, or teardown after the main thread stopped ticking.
     */
    void reset_prefab_templates() noexcept;

    /**
     * @brief True for the trigger classes the index keeps (BedTrigger, ActionTrigger, WaterTubeActionTrigger,
     *        KettleActionTrigger, FoodProcessingTrigger, SequenceTrigger, SmartObjectTrigger, InteractionTrigger).
     */
    [[nodiscard]] bool template_trigger_class(std::string_view entity_class) noexcept;

    /**
     * @brief Finds the template instance a placed trigger belongs to.
     * @param entity_class The trigger's class.
     * @param entity_name The trigger's entity name (its part before '[' is the template object's Name).
     * @param world The trigger's world Matrix34.
     * @param linked World positions of the entities the trigger's entity links point at.
     * @return The best template whose linked object lands within 3 cm of a placed one, or why there is none.
     * @note Main thread only.
     */
    [[nodiscard]] TemplateMatchResult match_prefab_template(
        std::string_view entity_class,
        std::string_view entity_name,
        const game_structures::Matrix34f &world,
        std::span<const game_structures::Vec3f> linked
    );

    /** @brief The world position of a template-space point of a matched instance. */
    [[nodiscard]] game_structures::Vec3f
    template_to_world(const TemplateMatch &match, const game_structures::Vec3f &local) noexcept;

    /**
     * @brief Picks the object of a matched template that shows the trigger's object.
     * @details The single brush of a template without runtime prefabs; else the brush or runtime prefab whose Name or
     *          model file name contains the trigger's last own word ("trigger_pan" -> "pan_d1"); else the brush,
     *          runtime prefab or mesh entity nearest the trigger in template space (a kettle trigger's cauldron prefab
     *          sits 5 cm from it, the tripod 15 cm).
     * @param match A match.
     * @param own_words The words of the trigger's own name (trigger_name_words()).
     * @param rule Receives the rule that picked it, for the log.
     * @return The object, or nullptr when the template has none.
     */
    [[nodiscard]] const TemplateObject *
    pick_template_object(const TemplateMatch &match, std::span<const std::string> own_words, std::string_view &rule);

} // namespace HenrySenses

#endif // HENRYSENSES_PREFAB_TEMPLATES_HPP
