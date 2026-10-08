/**
 * @file character_attachments.hpp
 * @brief Finds what the player's character carries, by the engine's own attachment link.
 *
 * Every item the character carries (a weapon in the hand or in its holster, the quiver, a shield, a tool, a torch) is
 * an entity of its own, bound to a bone attachment of the character's CCharInstance. The item draws through its own
 * render node, so the close-up fade, which dissolves the character's render proxy, has to be told about each of them.
 * This module walks the character's attachment manager once per camera frame, resolves each bound EntityId through the
 * entity system to the item's render node, and follows an item that is a character model of its own (a bow, a
 * crossbow) down to what is bound to it (the nocked arrow, the crossbow's lever). Every hop is checked by RTTI identity
 * or by a back-link, and every read is SEH-guarded; nothing is called on the engine and nothing is written.
 */
#ifndef TPVCAMERA_CHARACTER_ATTACHMENTS_HPP
#define TPVCAMERA_CHARACTER_ATTACHMENTS_HPP

#include "constants.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace TPVCamera
{
    /** @brief Why a walk of the character's attachments failed, at the hop that failed. */
    enum class WalkError : std::uint8_t
    {
        /// The walk succeeded.
        None,
        /// A class the walk checks against did not resolve by RTTI.
        Identity,
        /// g_env's entity system is missing or is not a CEntitySystem.
        EntitySystem,
        /// Looking the character's own EntityId up in the entity array did not give back the character.
        SelfTest,
        /// The character's entity is not a CEntity.
        NotEntity,
        /// The entity's proxy map holds no render proxy.
        NoProxy,
        /// The render proxy's interface does not lead to a CRenderProxy.
        NotRenderProxy,
        /// The render proxy does not point back at the entity.
        NoBackLink,
        /// The render proxy holds no CCharInstance.
        NoCharacter,
        /// The character's embedded attachment manager is not a CAttachmentManager.
        NotAttachmentManager,
        /// The attachment manager does not point back at the character.
        ManagerOwner,
        /// The attachment list is not a plausible vector.
        BadList,
        /// A read faulted.
        Fault,
        /// Enumerator count. Not an error.
        Count,
    };

    /** @brief One item bound to the character's attachments, or to those of a character model it carries. */
    struct CarriedItem
    {
        /// The item's render node (its CRenderProxy), which the character fade matches.
        std::uintptr_t node = 0;
        /// The item's CEntity.
        std::uintptr_t entity = 0;
        /// The bone attachment it is bound to, for the socket name in the log. Valid for the frame of the walk.
        std::uintptr_t attachment = 0;
        /// The item's EntityId.
        std::uint32_t id = 0;
        /// The item this one is bound to (a bow for its arrow), as an index into CharacterNodes::items, or -1 when it
        /// is bound to the character itself.
        std::int8_t parent = -1;
        /// 1 on the character, 2 on a character model it carries, and so on.
        std::uint8_t depth = 0;
        /// The item draws a character model of its own (a bow, a crossbow) rather than a static mesh.
        bool character = false;
    };

    /** @brief The render nodes of the player's character and of everything it carries, from one walk. */
    struct CharacterNodes
    {
        /// The walked CEntity.
        std::uintptr_t entity = 0;
        /// How many items are listed.
        int count = 0;
        /// The character's own render node first (0 when even that failed), then each item's, in item order.
        std::array<std::uintptr_t, 1 + Constants::CHARACTER_FADE_MAX_CARRIED> nodes{};
        /// The items, in the order the walk met them.
        std::array<CarriedItem, Constants::CHARACTER_FADE_MAX_CARRIED> items{};
        /// Bit i is set when items[i] draws a character model.
        std::uint32_t character_mask = 0;
        /// More items were bound than CHARACTER_FADE_MAX_CARRIED; the extra ones are not listed.
        bool truncated = false;
        /// Why the walk stopped short, or WalkError::None. The items are empty unless it is None.
        WalkError error = WalkError::None;
        /// How long the walk took (microseconds).
        float walk_us = 0.0f;

        /** @brief The character's own render node, or 0. */
        [[nodiscard]] std::uintptr_t root() const noexcept { return nodes[0]; }

        /** @brief The carried items' render nodes. */
        [[nodiscard]] std::span<const std::uintptr_t> carried() const noexcept
        {
            return std::span<const std::uintptr_t>(nodes.data() + 1, static_cast<std::size_t>(count));
        }
    };

    /**
     * @brief Hands the module the g_env base the entity system is read from.
     * @param g_env The SSystemGlobalEnvironment base, or 0 when it did not resolve (every walk then fails closed).
     * @note Setup only, before the first camera frame.
     */
    void initialize_character_attachments(std::uintptr_t g_env) noexcept;

    /**
     * @brief Walks the attachments of @p entity's character into @p out.
     * @details Reads the entity's render proxy and its CCharInstance, then every bone attachment the character's
     *          attachment manager lists. Each one bound to an entity (a CEntityAttachment) is resolved through the
     *          entity system's array to that entity's render proxy; an item that is a character model has its own
     *          attachments walked the same way, CHARACTER_ATTACHMENT_MAX_DEPTH deep. On every walk the character's own
     *          EntityId is looked up in the array first and must give back the character, which proves the array's
     *          layout for this build; the pass is logged once per entity and entity system. A failure of that check,
     *          or at the character itself, leaves the item list empty for that frame only, so the carried items stay
     *          solid while the character still fades; it is logged once per kind of failure as a warning, and the next
     *          walk that succeeds is logged as a recovery. A change of the listed items is logged with their sockets
     *          and names. Main thread only (the camera frame).
     * @param entity The player's CEntity.
     * @param out Receives the nodes; cleared first.
     * @return True when the walk succeeded (the list may still be empty).
     */
    [[nodiscard]] bool collect_character_nodes(std::uintptr_t entity, CharacterNodes &out) noexcept;

    /**
     * @brief Clears @p nodes and forgets the item set last logged, so the next walk logs its set afresh.
     * @details For a frame that does not walk: the close-up fade off, or the camera leaving third person. Main thread
     *          only.
     */
    void clear_character_nodes(CharacterNodes &nodes) noexcept;

} // namespace TPVCamera

#endif // TPVCAMERA_CHARACTER_ATTACHMENTS_HPP
