/**
 * @file core/xml_elements.hpp
 * @brief A forgiving reader and writer for the elements and attributes of the game's Mannequin XML files. Text,
 *        comments and processing instructions are skipped. Attribute values are kept as written, entities and all.
 */

#ifndef ANIMFW_CORE_XML_ELEMENTS_HPP
#define ANIMFW_CORE_XML_ELEMENTS_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AnimationFramework
{
    /// One attribute: its name, its value as written and the quote it was written with.
    struct XmlAttr
    {
        std::string name;
        /// As written, entities and all.
        std::string value;
        char quote = '"';
    };

    /// One element: its tag, its attributes and its child elements, in document order.
    struct XmlElement
    {
        std::string tag;
        std::vector<XmlAttr> attrs;
        std::vector<XmlElement> children;

        /// The value of attribute @p key (case-sensitive), or null.
        [[nodiscard]] const std::string *attr(std::string_view key) const noexcept;
        /// The value of attribute @p key (case-sensitive), or empty.
        [[nodiscard]] std::string get(std::string_view key) const;
        /// The first child tagged @p name (case-sensitive), or null.
        [[nodiscard]] const XmlElement *child(std::string_view name) const noexcept;
        /** @brief Sets attribute @p key (case-sensitive), adding it at the end when missing. */
        void set(std::string_view key, std::string value);
        /** @brief Removes attribute @p key when present. */
        void erase(std::string_view key);
    };

    /** @brief The document element of @p text, or nothing when it does not read as XML. */
    [[nodiscard]] std::optional<XmlElement> read_xml_elements(std::string_view text);

    /** @brief Appends @p element and its children to @p out, one element per line. */
    void write_xml_elements(const XmlElement &element, std::string &out);
} // namespace AnimationFramework

#endif // ANIMFW_CORE_XML_ELEMENTS_HPP
