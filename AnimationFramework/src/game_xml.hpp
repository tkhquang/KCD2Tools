/**
 * @file game_xml.hpp
 * @brief The engine's XML nodes, held and read through their virtuals.
 *
 * The slots are the same on the Steam 1.5.6, GOG 1.5 and Game Pass 1.4 builds:
 *   - CXmlNode: 4 Release, 6 getTag, 33 getAttr(key) (the value, "" when absent), 38 addChild, 39 newChild,
 *     40 removeChild, 41 insertChild, 43 removeAllChilds, 44 getChildCount, 45 getChild, 46 findChild, 78 setAttr(key,
 *     value) (the first-declared setAttr overload, last of the group). A returned XmlNodeRef comes back through a
 *     hidden out-pointer holding one reference.
 *   - CXmlUtils: 1 LoadXmlFromFile(out, name, patch, clean, read-only, reuse), and 2 LoadXmlFromBuffer(out, buffer,
 *     size, read-only, reuse) with a 32-bit size. The read-only flag selects XmlParserReadOnly, whose nodes ignore
 *     every edit.
 */
#ifndef ANIMFW_GAME_XML_HPP
#define ANIMFW_GAME_XML_HPP

#include "game_memory.hpp"

#include <cstddef>
#include <string_view>
#include <utility>

namespace AnimationFramework
{
    inline constexpr std::size_t XML_UTILS_LOAD_FILE_SLOT = 1;
    inline constexpr std::size_t XML_UTILS_LOAD_BUFFER_SLOT = 2;

    inline constexpr std::size_t XML_NODE_RELEASE_SLOT = 4;
    inline constexpr std::size_t XML_NODE_GET_TAG_SLOT = 6;
    inline constexpr std::size_t XML_NODE_GET_ATTR_SLOT = 33;
    inline constexpr std::size_t XML_NODE_ADD_CHILD_SLOT = 38;
    inline constexpr std::size_t XML_NODE_NEW_CHILD_SLOT = 39;
    inline constexpr std::size_t XML_NODE_REMOVE_CHILD_SLOT = 40;
    inline constexpr std::size_t XML_NODE_INSERT_CHILD_SLOT = 41;
    inline constexpr std::size_t XML_NODE_REMOVE_ALL_CHILDREN_SLOT = 43;
    inline constexpr std::size_t XML_NODE_CHILD_COUNT_SLOT = 44;
    inline constexpr std::size_t XML_NODE_GET_CHILD_SLOT = 45;
    inline constexpr std::size_t XML_NODE_FIND_CHILD_SLOT = 46;
    inline constexpr std::size_t XML_NODE_SET_ATTR_SLOT = 78;

    /** @brief One XmlNodeRef: the node and the reference it holds. */
    class XmlRef
    {
    public:
        XmlRef() noexcept = default;
        XmlRef(const XmlRef &) = delete;
        XmlRef &operator=(const XmlRef &) = delete;
        XmlRef(XmlRef &&other) noexcept : m_node(std::exchange(other.m_node, nullptr)) {}
        XmlRef &operator=(XmlRef &&other) noexcept
        {
            if (this != &other)
            {
                reset();
                m_node = std::exchange(other.m_node, nullptr);
            }
            return *this;
        }
        ~XmlRef() noexcept { reset(); }

        /// Drops the held reference, if any.
        void reset() noexcept
        {
            if (m_node != nullptr)
            {
                vcall<void>(std::exchange(m_node, nullptr), XML_NODE_RELEASE_SLOT);
            }
        }
        /// The node, or nullptr.
        [[nodiscard]] void *get() const noexcept { return m_node; }
        /** @brief Hands the reference over (to a caller's XmlNodeRef out-parameter). */
        [[nodiscard]] void *release() noexcept { return std::exchange(m_node, nullptr); }
        /** @brief Drops the held reference and returns the slot a call fills as its XmlNodeRef out-parameter. */
        [[nodiscard]] void **out() noexcept
        {
            reset();
            return &m_node;
        }
        /// True while a node is held.
        explicit operator bool() const noexcept { return m_node != nullptr; }

    private:
        void *m_node = nullptr;
    };

    /// The tag of @p node, empty when it has none.
    [[nodiscard]] inline std::string_view tag_of(void *node) noexcept
    {
        const char *tag = vcall<const char *>(node, XML_NODE_GET_TAG_SLOT);
        return tag != nullptr ? std::string_view{tag} : std::string_view{};
    }

    /// The value of attribute @p key of @p node, empty when absent.
    [[nodiscard]] inline std::string_view attr_of(void *node, const char *key) noexcept
    {
        const char *value = vcall<const char *>(node, XML_NODE_GET_ATTR_SLOT, key);
        return value != nullptr ? std::string_view{value} : std::string_view{};
    }

    /// The number of children of @p node.
    [[nodiscard]] inline int child_count(void *node) noexcept
    {
        return vcall<int>(node, XML_NODE_CHILD_COUNT_SLOT);
    }

    /// Child @p index of @p node, holding its own reference.
    [[nodiscard]] inline XmlRef child_at(void *node, int index) noexcept
    {
        XmlRef child;
        vcall<void **>(node, XML_NODE_GET_CHILD_SLOT, child.out(), index);
        return child;
    }
} // namespace AnimationFramework

#endif // ANIMFW_GAME_XML_HPP
