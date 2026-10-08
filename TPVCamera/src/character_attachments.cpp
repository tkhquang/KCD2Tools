/**
 * @file character_attachments.cpp
 * @brief Implementation of the character attachment walk (see character_attachments.hpp).
 *
 * @details The walk runs on the main thread, in the camera frame, and only reads. Its core (walk_guarded) is a
 *          structured-exception frame over plain reads, and each attachment's item is read under a frame of its own
 *          (read_item_guarded): a pointer freed or reused under the read skips that item, or, on the character's own
 *          chain, ends that frame's walk as a fault, instead of a crash. Nothing it follows is trusted on its own: each
 *          object is checked by its primary vtable against the class identity resolved by RTTI, a render proxy must
 *          point back at its entity, an attachment manager at its character, and an entity found by id must carry that
 *          id. The entity array is read without the reader count GetEntity takes; on the main thread an entity is not
 *          freed under the read, and an entity spawned on another thread at the same moment at worst fails those
 *          checks for one frame.
 */

#include "character_attachments.hpp"
#include "constants.hpp"
#include "rtti_types.hpp"

#include <DetourModKit.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace TPVCamera
{

    namespace
    {
        // A pointer read from the game below this is a null or a small integer, never an object.
        constexpr std::uintptr_t k_min_address = 0x10000;
        // An engine class's secondary base sits within this many bytes of the object's start; a larger sub-object
        // offset read from a COL means the interface pointer was not what the walk took it for.
        constexpr std::uint32_t k_max_subobject_offset = 0x1000;
        // Items skipped by a walk (bound, but with no entity or render proxy to be found) kept for the debug log.
        constexpr int k_max_skipped = 8;
        // EntityIds whose skip was already logged, so each is logged once. A skipped id met once this many are kept is
        // not logged.
        constexpr int k_max_logged_skips = 32;

        /** @brief The primary vtables the walk compares objects against, resolved by RTTI. */
        struct Vtables
        {
            std::uintptr_t entity = 0;
            std::uintptr_t render_proxy = 0;
            std::uintptr_t char_instance = 0;
            std::uintptr_t attachment_manager = 0;
            std::uintptr_t attachment_bone = 0;
            std::uintptr_t entity_attachment = 0;
            std::uintptr_t entity_system = 0;
        };

        /** @brief The classes in Vtables, in member order, with the names the failure warning gives them. */
        struct ClassSlot
        {
            GameClass klass;
            std::uintptr_t Vtables::*member;
            std::string_view name;
        };
        constexpr std::array<ClassSlot, 7> k_classes = {{
            {GameClass::Entity, &Vtables::entity, "CEntity"},
            {GameClass::RenderProxy, &Vtables::render_proxy, "CRenderProxy"},
            {GameClass::CharInstance, &Vtables::char_instance, "CCharInstance"},
            {GameClass::AttachmentManager, &Vtables::attachment_manager, "CAttachmentManager"},
            {GameClass::AttachmentBone, &Vtables::attachment_bone, "CAttachmentBONE"},
            {GameClass::EntityAttachment, &Vtables::entity_attachment, "CEntityAttachment"},
            {GameClass::EntitySystem, &Vtables::entity_system, "CEntitySystem"},
        }};

        /** @brief What a walk starts from. */
        struct WalkInput
        {
            std::uintptr_t entity = 0;
            // The entity system, or 0 when it cannot be used: the walk then stops after the character's own node.
            std::uintptr_t entity_system = 0;
            Vtables vtables{};
        };

        /** @brief A walk's verdict, with the value read at the hop that failed (for the warning). */
        struct WalkResult
        {
            WalkError error = WalkError::None;
            std::uintptr_t detail = 0;
        };

        /** @brief An item bound to an attachment that the walk could not resolve. */
        struct SkippedItem
        {
            std::uint32_t id = 0;
            WalkError reason = WalkError::None;
        };

        /** @brief The items a walk skipped, for the debug log. */
        struct SkippedItems
        {
            int count = 0;
            SkippedItem items[k_max_skipped] = {};
        };

        /** @brief A render node read from an entity, or why there is none. */
        struct NodeRead
        {
            std::uintptr_t node = 0;
            WalkError error = WalkError::None;
            std::uintptr_t detail = 0;
        };

        // Set once at setup, before the first camera frame.
        std::uintptr_t s_genv = 0;
        // Main-thread state (the camera frame).
        Vtables s_vtables{};
        bool s_vtables_ready = false;
        // The entity and entity system the self-test last passed for, so a pass is logged once per load.
        std::uintptr_t s_tested_entity = 0;
        std::uintptr_t s_tested_system = 0;
        // One bit per WalkError already warned about in this session.
        std::uint32_t s_reported = 0;
        // The last walk failed, so the next success is logged as a recovery.
        bool s_failing = false;
        // The item nodes the change log last reported; -1 when nothing was reported since the last clear.
        std::array<std::uintptr_t, Constants::CHARACTER_FADE_MAX_CARRIED> s_logged_nodes{};
        int s_logged_count = -1;
        std::array<std::uint32_t, k_max_logged_skips> s_logged_skips{};
        int s_logged_skip_count = 0;
        // The table above is full and a skipped id it does not hold was met, which is logged once.
        bool s_logged_skips_full = false;

        /** @brief Reads a T at @p address. Only under a *_guarded caller's __try, where a fault unwinds to. */
        template <typename T> [[nodiscard]] T read(std::uintptr_t address)
        {
            return *reinterpret_cast<const T *>(address);
        }

        /**
         * @brief The render node of @p entity: its render proxy, the CRenderProxy whose IEntityRenderProxy interface
         *        the entity's proxy map holds under type 0.
         * @note Runs under a *_guarded caller's __try.
         */
        [[nodiscard]] NodeRead render_node_of(const Vtables &vtables, std::uintptr_t entity)
        {
            const auto head = read<std::uintptr_t>(entity + Constants::ENTITY_PROXY_MAP_OFFSET);
            if (head < k_min_address)
            {
                return {0, WalkError::NoProxy, head};
            }
            // The leftmost node holds the smallest key; the render proxy's type 0 is the smallest there is.
            const auto first = read<std::uintptr_t>(head + Constants::MSVC_MAP_NODE_LEFT_OFFSET);
            if (first < k_min_address || first == head ||
                read<std::uint8_t>(first + Constants::MSVC_MAP_NODE_ISNIL_OFFSET) != 0 ||
                read<std::uint32_t>(first + Constants::MSVC_MAP_NODE_KEY_OFFSET) != Constants::ENTITY_PROXY_TYPE_RENDER)
            {
                return {0, WalkError::NoProxy, first};
            }
            const auto iface = read<std::uintptr_t>(first + Constants::MSVC_MAP_NODE_POINTER_VALUE_OFFSET);
            if (iface < k_min_address)
            {
                return {0, WalkError::NoProxy, iface};
            }
            // The interface is a secondary base: its vtable's COL records how far below it the object starts.
            const auto iface_vtable = read<std::uintptr_t>(iface);
            const auto col = (iface_vtable >= k_min_address) ? read<std::uintptr_t>(iface_vtable - sizeof(void *)) : 0;
            if (col < k_min_address ||
                read<std::uint32_t>(col + Constants::RTTI_COL_SIGNATURE_OFFSET) != Constants::RTTI_COL_SIGNATURE_X64)
            {
                return {0, WalkError::NotRenderProxy, iface_vtable};
            }
            const auto offset = read<std::uint32_t>(col + Constants::RTTI_COL_SUBOBJECT_OFFSET);
            if (offset > k_max_subobject_offset || offset % sizeof(void *) != 0)
            {
                return {0, WalkError::NotRenderProxy, offset};
            }
            const std::uintptr_t node = iface - offset;
            const auto node_vtable = read<std::uintptr_t>(node);
            if (node_vtable != vtables.render_proxy)
            {
                return {0, WalkError::NotRenderProxy, node_vtable};
            }
            const auto owner = read<std::uintptr_t>(node + Constants::RENDER_PROXY_ENTITY_OFFSET);
            if (owner != entity)
            {
                return {0, WalkError::NoBackLink, owner};
            }
            return {node, WalkError::None, 0};
        }

        /**
         * @brief The CCharInstance that render node @p node draws, read the way CEntity::GetCharacter(0) does, or 0
         *        for a static item.
         * @note Runs under a *_guarded caller's __try.
         */
        [[nodiscard]] std::uintptr_t character_of(const Vtables &vtables, std::uintptr_t node)
        {
            const auto begin = read<std::uintptr_t>(node + Constants::RENDER_PROXY_SLOTS_BEGIN_OFFSET);
            const auto end = read<std::uintptr_t>(node + Constants::RENDER_PROXY_SLOTS_END_OFFSET);
            if (begin < k_min_address || end <= begin)
            {
                return 0;
            }
            const auto slot = read<std::uintptr_t>(begin);
            if (slot < k_min_address)
            {
                return 0;
            }
            std::uintptr_t character = read<std::uintptr_t>(node + Constants::RENDER_PROXY_CHARACTER_OFFSET);
            if (character == 0)
            {
                character = read<std::uintptr_t>(slot + Constants::ENTITY_SLOT_CHARACTER_OFFSET);
            }
            if (character < k_min_address)
            {
                return 0;
            }
            return read<std::uintptr_t>(character) == vtables.char_instance ? character : 0;
        }

        /**
         * @brief The CEntity with EntityId @p id, read from the entity system's array the way GetEntity reads it, or 0.
         * @note Runs under a *_guarded caller's __try.
         */
        [[nodiscard]] std::uintptr_t entity_by_id(std::uintptr_t system, std::uint32_t id, std::uintptr_t entity_vtable)
        {
            const std::uint32_t index = id & Constants::ENTITY_ID_INDEX_MASK;
            if (index == 0 || index > Constants::ENTITY_ID_MAX_INDEX)
            {
                return 0;
            }
            const std::uintptr_t entry = system + Constants::ENTITY_SYSTEM_ARRAY_OFFSET +
                                         static_cast<std::uintptr_t>(index) * Constants::ENTITY_SYSTEM_ENTRY_STRIDE;
            if (read<std::uint16_t>(entry) != (id >> Constants::ENTITY_ID_SALT_SHIFT))
            {
                return 0;
            }
            const auto entity = read<std::uintptr_t>(entry + Constants::ENTITY_SYSTEM_ENTRY_ENTITY_OFFSET);
            if (entity < k_min_address || read<std::uint32_t>(entity + Constants::ENTITY_ID_OFFSET) != id ||
                read<std::uintptr_t>(entity) != entity_vtable)
            {
                return 0;
            }
            return entity;
        }

        /** @brief What one attachment holds, as the walk reads it. */
        struct ItemRead
        {
            // The bound entity's render node, or 0 when the attachment holds no entity binding or it was skipped.
            std::uintptr_t node = 0;
            std::uintptr_t entity = 0;
            // The character model the item draws (a bow, a crossbow), or 0 for a static mesh.
            std::uintptr_t character = 0;
            std::uint32_t id = 0;
            // Why a bound entity was skipped: its id led to no entity (NotEntity), it has no usable render proxy, or a
            // read faulted.
            WalkError error = WalkError::None;
        };

        /**
         * @brief Reads what @p attachment holds: nothing, unless it is a bone attachment bound to an entity
         *        (a CEntityAttachment), which is resolved through the entity array to its render node.
         * @details Under its own structured-exception frame, so an attachment or an item freed under the read skips
         *          that item instead of ending the whole walk.
         * @note SEH-guarded, POD body.
         */
        [[nodiscard]] ItemRead read_item_guarded(const Vtables &vt, std::uintptr_t system, std::uintptr_t owner_entity,
                                                 std::uintptr_t attachment) noexcept
        {
            ItemRead item{};
            __try
            {
                // Only a bone attachment holds an entity; a skin or cloth one draws inside its owner's render.
                if (attachment < k_min_address || read<std::uintptr_t>(attachment) != vt.attachment_bone)
                {
                    return item;
                }
                // A static mesh or a skin bound to a bone draws with its owner too; only an entity binding is an item
                // of its own.
                const auto object = read<std::uintptr_t>(attachment + Constants::ATTACHMENT_BONE_OBJECT_OFFSET);
                if (object < k_min_address || read<std::uintptr_t>(object) != vt.entity_attachment)
                {
                    return item;
                }
                item.id = read<std::uint32_t>(object + Constants::ENTITY_ATTACHMENT_ID_OFFSET);
                if (item.id == 0)
                {
                    return item;
                }
                item.entity = entity_by_id(system, item.id, vt.entity);
                if (item.entity == 0)
                {
                    item.error = WalkError::NotEntity;
                    return item;
                }
                if (item.entity == owner_entity)
                {
                    return item;
                }
                const NodeRead node = render_node_of(vt, item.entity);
                if (node.error != WalkError::None)
                {
                    item.error = node.error;
                    return item;
                }
                item.node = node.node;
                item.character = character_of(vt, node.node);
                return item;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                item.node = 0;
                item.error = WalkError::Fault;
                return item;
            }
        }

        /** @brief True when @p node is already listed in @p out (the character's own node included). */
        [[nodiscard]] bool listed(const CharacterNodes &out, std::uintptr_t node) noexcept
        {
            return std::find(out.nodes.begin(), out.nodes.begin() + 1 + out.count, node) !=
                   out.nodes.begin() + 1 + out.count;
        }

        /**
         * @brief The walk itself: the character's own node, then the attachments of each character met, depth first.
         * @note Runs under walk_guarded's __try. POD state only.
         */
        [[nodiscard]] WalkResult walk_body(const WalkInput &in, CharacterNodes &out, SkippedItems &skipped)
        {
            const Vtables &vt = in.vtables;
            const auto entity_vtable = read<std::uintptr_t>(in.entity);
            if (entity_vtable != vt.entity)
            {
                return {WalkError::NotEntity, entity_vtable};
            }
            const NodeRead root = render_node_of(vt, in.entity);
            if (root.error != WalkError::None)
            {
                return {root.error, root.detail};
            }
            out.nodes[0] = root.node;
            if (in.entity_system == 0)
            {
                return {};
            }
            const std::uintptr_t root_character = character_of(vt, root.node);
            if (root_character == 0)
            {
                return {WalkError::NoCharacter,
                        read<std::uintptr_t>(root.node + Constants::RENDER_PROXY_CHARACTER_OFFSET)};
            }

            // Characters still to walk. Each is walked once (a binding loop cannot recurse), and only
            // CHARACTER_ATTACHMENT_MAX_CHARACTERS are walked in all, so the stack never holds more.
            struct Pending
            {
                std::uintptr_t character;
                std::int8_t item; // the item that draws it, or -1 for the character itself
                std::uint8_t depth;
            };
            constexpr int k_max_characters = Constants::CHARACTER_ATTACHMENT_MAX_CHARACTERS;
            Pending stack[k_max_characters] = {};
            std::uintptr_t walked[k_max_characters] = {};
            int top = 0;
            int n_walked = 0;
            stack[top++] = Pending{root_character, -1, 0};
            walked[n_walked++] = root_character;
            while (top > 0)
            {
                const Pending at = stack[--top];
                const bool own = at.depth == 0;
                const std::uintptr_t manager = at.character + Constants::CHAR_INSTANCE_ATTACHMENT_MANAGER_OFFSET;
                const auto manager_vtable = read<std::uintptr_t>(manager);
                if (manager_vtable != vt.attachment_manager)
                {
                    if (own)
                    {
                        return {WalkError::NotAttachmentManager, manager_vtable};
                    }
                    continue;
                }
                const auto owner = read<std::uintptr_t>(manager + Constants::ATTACHMENT_MANAGER_OWNER_OFFSET);
                if (owner != at.character)
                {
                    if (own)
                    {
                        return {WalkError::ManagerOwner, owner};
                    }
                    continue;
                }
                // The vector is read once: a binding made on a job thread at this moment shows next frame.
                const auto begin = read<std::uintptr_t>(manager + Constants::ATTACHMENT_MANAGER_LIST_BEGIN_OFFSET);
                const auto end = read<std::uintptr_t>(manager + Constants::ATTACHMENT_MANAGER_LIST_END_OFFSET);
                const std::uintptr_t bytes = end - begin;
                if (begin < k_min_address || end < begin || bytes % sizeof(void *) != 0 ||
                    bytes / sizeof(void *) > static_cast<std::uintptr_t>(Constants::CHARACTER_ATTACHMENT_SCAN_CAP))
                {
                    if (own)
                    {
                        return {WalkError::BadList, bytes};
                    }
                    continue;
                }
                const auto n_attachments = static_cast<int>(bytes / sizeof(void *));
                for (int i = 0; i < n_attachments; ++i)
                {
                    const auto attachment =
                        read<std::uintptr_t>(begin + static_cast<std::uintptr_t>(i) * sizeof(void *));
                    const ItemRead item = read_item_guarded(vt, in.entity_system, in.entity, attachment);
                    if (item.error != WalkError::None)
                    {
                        if (skipped.count < k_max_skipped)
                        {
                            skipped.items[skipped.count++] = SkippedItem{item.id, item.error};
                        }
                        continue;
                    }
                    if (item.node == 0 || listed(out, item.node))
                    {
                        continue;
                    }
                    if (out.count >= Constants::CHARACTER_FADE_MAX_CARRIED)
                    {
                        out.truncated = true;
                        continue;
                    }
                    const int index = out.count++;
                    out.nodes[static_cast<std::size_t>(1 + index)] = item.node;
                    out.items[static_cast<std::size_t>(index)] = CarriedItem{
                        .node = item.node,
                        .entity = item.entity,
                        .attachment = attachment,
                        .id = item.id,
                        .parent = at.item,
                        .depth = static_cast<std::uint8_t>(at.depth + 1),
                        .character = item.character != 0,
                    };
                    if (item.character == 0)
                    {
                        continue;
                    }
                    out.character_mask |= 1u << index;
                    // A bow or a crossbow: what is bound to it (the nocked arrow, the lever) is carried too.
                    if (at.depth + 1 < Constants::CHARACTER_ATTACHMENT_MAX_DEPTH && n_walked < k_max_characters &&
                        std::find(walked, walked + n_walked, item.character) == walked + n_walked)
                    {
                        walked[n_walked++] = item.character;
                        stack[top++] = Pending{item.character, static_cast<std::int8_t>(index),
                                               static_cast<std::uint8_t>(at.depth + 1)};
                    }
                }
            }
            return {};
        }

        /** @brief walk_body under a structured-exception frame. A fault ends the walk as WalkError::Fault. */
        [[nodiscard]] WalkResult walk_guarded(const WalkInput &in, CharacterNodes &out, SkippedItems &skipped) noexcept
        {
            __try
            {
                return walk_body(in, out, skipped);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return WalkResult{WalkError::Fault, 0};
            }
        }

        /**
         * @brief Looks @p entity's own EntityId up in the entity array, which must give back @p entity.
         * @param id Receives the id read from the entity (0 on a fault).
         * @return The entity the array holds under that id, or 0.
         * @note SEH-guarded, POD body.
         */
        [[nodiscard]] std::uintptr_t lookup_guarded(std::uintptr_t entity, std::uintptr_t system,
                                                    std::uintptr_t entity_vtable, std::uint32_t &id) noexcept
        {
            __try
            {
                id = read<std::uint32_t>(entity + Constants::ENTITY_ID_OFFSET);
                return entity_by_id(system, id, entity_vtable);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        /**
         * @brief Copies the C string whose pointer is stored at @p object + @p offset into @p out, printable only.
         * @note SEH-guarded, POD body. @p out is empty on a fault.
         */
        void copy_text_field_guarded(std::uintptr_t object, std::ptrdiff_t offset, char *out, std::size_t cap) noexcept
        {
            __try
            {
                const char *text = *reinterpret_cast<const char *const *>(object + offset);
                std::size_t i = 0;
                if (text != nullptr)
                {
                    for (; i + 1 < cap && text[i] != '\0'; ++i)
                    {
                        out[i] = (text[i] >= 0x20 && text[i] < 0x7F) ? text[i] : '?';
                    }
                }
                out[i] = '\0';
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                out[0] = '\0';
            }
        }

        /** @brief Resolves the walk's class identities into s_vtables; on a miss, the index of the first one missing.
         */
        [[nodiscard]] int resolve_vtables() noexcept
        {
            Vtables resolved{};
            for (std::size_t i = 0; i < k_classes.size(); ++i)
            {
                const std::optional<std::uintptr_t> vtable = class_vtable(k_classes[i].klass);
                if (!vtable.has_value())
                {
                    return static_cast<int>(i);
                }
                resolved.*(k_classes[i].member) = *vtable;
            }
            s_vtables = resolved;
            s_vtables_ready = true;
            return -1;
        }

        /** @brief The hop a WalkError names, for the warning. */
        [[nodiscard]] std::string_view walk_error_text(WalkError error) noexcept
        {
            switch (error)
            {
            case WalkError::Identity:
                return "a class the walk checks did not resolve by RTTI";
            case WalkError::EntitySystem:
                return "g_env's entity system is not a CEntitySystem";
            case WalkError::SelfTest:
                return "the entity lookup self-test failed";
            case WalkError::NotEntity:
                return "the character's entity is not a CEntity";
            case WalkError::NoProxy:
                return "the entity's proxy map holds no render proxy";
            case WalkError::NotRenderProxy:
                return "the render proxy is not a CRenderProxy";
            case WalkError::NoBackLink:
                return "the render proxy does not point back at the entity";
            case WalkError::NoCharacter:
                return "the render proxy holds no CCharInstance";
            case WalkError::NotAttachmentManager:
                return "char+0x8 is not a CAttachmentManager";
            case WalkError::ManagerOwner:
                return "the attachment manager does not point back at the character";
            case WalkError::BadList:
                return "the attachment list is not a plausible vector";
            case WalkError::Fault:
                return "a read faulted";
            case WalkError::None:
            case WalkError::Count:
                break;
            }
            return "unknown";
        }

        /** @brief Warns about @p error once per session, naming the hop and @p text (the value read there). */
        void report_failure(WalkError error, std::string_view text) noexcept
        {
            s_failing = true;
            const std::uint32_t bit = 1u << static_cast<unsigned>(error);
            if ((s_reported & bit) != 0)
            {
                return;
            }
            s_reported |= bit;
            (void)DMK::log().try_log(DMK::LogLevel::Warning,
                                     "CloseUpFade: the character's attachments cannot be walked ({}: {}); what it "
                                     "carries stays solid while it fades",
                                     walk_error_text(error), text);
        }

        /**
         * @brief Logs at Debug, once per EntityId, the items a walk found bound but could not resolve.
         * @details Only the first k_max_logged_skips ids are logged, then one line says further ones are not, so a skip
         *          that persists never repeats its line every frame.
         */
        void log_skipped(const SkippedItems &skipped) noexcept
        {
            for (int i = 0; i < skipped.count; ++i)
            {
                const SkippedItem &item = skipped.items[i];
                const auto end = s_logged_skips.begin() + s_logged_skip_count;
                if (std::find(s_logged_skips.begin(), end, item.id) != end)
                {
                    continue;
                }
                if (s_logged_skip_count >= k_max_logged_skips)
                {
                    if (!s_logged_skips_full)
                    {
                        s_logged_skips_full = true;
                        (void)DMK::log().try_log(DMK::LogLevel::Debug,
                                                 "CloseUpFade: {} skipped items were logged; further ones are not",
                                                 k_max_logged_skips);
                    }
                    continue;
                }
                s_logged_skips[static_cast<std::size_t>(s_logged_skip_count++)] = item.id;
                (void)DMK::log().try_log(
                    DMK::LogLevel::Debug, "CloseUpFade: an item bound to the character (id {:#x}) is skipped: {}",
                    item.id,
                    item.reason == WalkError::NotEntity ? "its entity was not found" : walk_error_text(item.reason));
            }
        }

        /**
         * @brief Logs the carried items at Info when the set changed since the last report: each with its socket path
         *        (a bow's arrow as weapon_bow>arrow), entity name, EntityId, render node and kind, plus what was added
         *        and removed, so a fade that misses an item is identifiable from the log.
         */
        void log_carried_change(const CharacterNodes &nodes) noexcept
        {
            const auto logged_end = s_logged_nodes.begin() + std::max(s_logged_count, 0);
            int added = 0;
            for (int i = 0; i < nodes.count; ++i)
            {
                if (std::find(s_logged_nodes.begin(), logged_end, nodes.items[static_cast<std::size_t>(i)].node) ==
                    logged_end)
                {
                    ++added;
                }
            }
            const int removed = std::max(s_logged_count, 0) + added - nodes.count;
            if (s_logged_count >= 0 && added == 0 && removed == 0)
            {
                return;
            }
            std::copy_n(nodes.nodes.begin() + 1, nodes.count, s_logged_nodes.begin());
            s_logged_count = nodes.count;
            if (!DMK::log().is_enabled(DMK::LogLevel::Info))
            {
                return;
            }
            try
            {
                std::string text;
                for (int i = 0; i < nodes.count; ++i)
                {
                    const CarriedItem &item = nodes.items[static_cast<std::size_t>(i)];
                    // The socket path runs from the character down: weapon_bow>arrow.
                    std::string path;
                    for (int at = i; at >= 0; at = nodes.items[static_cast<std::size_t>(at)].parent)
                    {
                        char socket[32];
                        copy_text_field_guarded(nodes.items[static_cast<std::size_t>(at)].attachment,
                                                Constants::ATTACHMENT_BONE_NAME_OFFSET, socket, sizeof(socket));
                        path.insert(0, path.empty() ? std::string{socket} : std::string{socket} + '>');
                    }
                    char name[64];
                    copy_text_field_guarded(item.entity, Constants::ENTITY_NAME_OFFSET, name, sizeof(name));
                    text += std::format("{}[{}] {} id {:#x} rn {:#x} {}", i == 0 ? "" : "; ", path,
                                        name[0] != '\0' ? name : "?", item.id, item.node,
                                        item.character ? "character" : "static");
                }
                (void)DMK::log().try_log(DMK::LogLevel::Info,
                                         "CloseUpFade: carried set ({}{}, walk {:.1f} us): {} (added {}, removed {})",
                                         nodes.count, nodes.truncated ? ", more not listed" : "", nodes.walk_us,
                                         nodes.count == 0 ? std::string{"nothing"} : text, added, removed);
            }
            catch (...)
            {
                // Out of memory for the line: the set is still remembered, so it is not retried every frame.
            }
        }

        /** @brief Seconds per QueryPerformanceCounter tick, for the walk time. */
        [[nodiscard]] double counter_period() noexcept
        {
            static const double s_period = []
            {
                LARGE_INTEGER frequency{};
                return (QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0)
                           ? 1.0 / static_cast<double>(frequency.QuadPart)
                           : 0.0;
            }();
            return s_period;
        }

    } // namespace

    void initialize_character_attachments(std::uintptr_t g_env) noexcept
    {
        s_genv = g_env;
    }

    bool collect_character_nodes(std::uintptr_t entity, CharacterNodes &out) noexcept
    {
        LARGE_INTEGER started{};
        QueryPerformanceCounter(&started);
        out = CharacterNodes{};
        out.entity = entity;
        if (entity == 0)
        {
            return false;
        }
        if (!s_vtables_ready)
        {
            if (const int missing = resolve_vtables(); missing >= 0)
            {
                out.error = WalkError::Identity;
                report_failure(WalkError::Identity, k_classes[static_cast<std::size_t>(missing)].name);
                return false;
            }
        }

        // The entity system, and the self-test: the character's own EntityId must lead back to it through the array,
        // which proves the array's base, stride and salt for this build. Either failing stops the walk after the
        // character's own node, so the character still fades and what it carries stays solid.
        WalkInput in{.entity = entity, .entity_system = 0, .vtables = s_vtables};
        WalkError system_error = WalkError::None;
        std::string system_detail;
        std::uintptr_t system = 0;
        std::uintptr_t system_vtable = 0;
        if (s_genv != 0)
        {
            if (const auto read =
                    DMK::memory::read<std::uintptr_t>(DMK::Address{s_genv + Constants::GENV_ENTITY_SYSTEM_OFFSET});
                read.has_value())
            {
                system = *read;
            }
        }
        if (system >= k_min_address)
        {
            if (const auto read = DMK::memory::read<std::uintptr_t>(DMK::Address{system}); read.has_value())
            {
                system_vtable = *read;
            }
        }
        if (system_vtable == 0 || system_vtable != s_vtables.entity_system)
        {
            system_error = WalkError::EntitySystem;
            try
            {
                system_detail = std::format("system {:#x}, vtable {:#x}", system, system_vtable);
            }
            catch (...)
            {
            }
        }
        else
        {
            std::uint32_t id = 0;
            const std::uintptr_t found = lookup_guarded(entity, system, s_vtables.entity, id);
            if (found != entity)
            {
                system_error = WalkError::SelfTest;
                try
                {
                    system_detail = std::format("id {:#x} -> {:#x}, expected {:#x}", id, found, entity);
                }
                catch (...)
                {
                }
            }
            else
            {
                in.entity_system = system;
                if (entity != s_tested_entity || system != s_tested_system)
                {
                    s_tested_entity = entity;
                    s_tested_system = system;
                    (void)DMK::log().try_log(DMK::LogLevel::Info,
                                             "CloseUpFade: entity lookup self-test passed (id {:#x} -> CEntity {:#x}, "
                                             "entity system {:#x})",
                                             id, entity, system);
                }
            }
        }

        SkippedItems skipped{};
        const WalkResult result = walk_guarded(in, out, skipped);
        LARGE_INTEGER finished{};
        QueryPerformanceCounter(&finished);
        out.walk_us =
            static_cast<float>(static_cast<double>(finished.QuadPart - started.QuadPart) * counter_period() * 1.0e6);
        const WalkError error = (result.error != WalkError::None) ? result.error : system_error;
        if (error != WalkError::None)
        {
            // Fail closed: no item is listed. The character's own node stays when the walk got that far.
            out.error = error;
            out.count = 0;
            out.character_mask = 0;
            out.truncated = false;
            std::string detail;
            try
            {
                detail = (result.error != WalkError::None) ? std::format("value {:#x}", result.detail) : system_detail;
            }
            catch (...)
            {
            }
            report_failure(error, detail);
            return false;
        }
        if (s_failing)
        {
            s_failing = false;
            (void)DMK::log().try_log(DMK::LogLevel::Info, "CloseUpFade: the character's attachments are walked again");
        }
        log_skipped(skipped);
        log_carried_change(out);
        return true;
    }

    void clear_character_nodes(CharacterNodes &nodes) noexcept
    {
        nodes = CharacterNodes{};
        s_logged_count = -1;
    }

} // namespace TPVCamera
