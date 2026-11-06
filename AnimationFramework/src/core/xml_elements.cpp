/**
 * @file core/xml_elements.cpp
 * @brief Mannequin XML element reader and writer (see xml_elements.hpp).
 */

#include "core/xml_elements.hpp"

#include <cstddef>
#include <utility>

namespace AnimationFramework
{
    namespace
    {
        /** @brief True for the XML whitespace characters (ASCII only, whatever the C locale says). */
        [[nodiscard]] constexpr bool is_space(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        /** @brief A forgiving reader for the elements and attributes of a Mannequin XML file (text is skipped). */
        class Reader
        {
        public:
            explicit Reader(std::string_view text) noexcept : m_text(text) {}

            /// The document element, after a byte order mark and any prolog, or nothing.
            [[nodiscard]] std::optional<XmlElement> document()
            {
                if (m_text.starts_with("\xEF\xBB\xBF"))
                {
                    m_pos = 3;
                }
                skip_misc();
                if (m_pos >= m_text.size() || m_text[m_pos] != '<')
                {
                    return std::nullopt;
                }
                XmlElement root;
                if (!element(root, 0))
                {
                    return std::nullopt;
                }
                return root;
            }

        private:
            /// Whether the text continues with @p token.
            [[nodiscard]] bool at(std::string_view token) const noexcept
            {
                return m_pos <= m_text.size() &&
                       std::string_view{m_text.data() + m_pos, m_text.size() - m_pos}.starts_with(token);
            }

            /// Skips whitespace.
            void skip_space() noexcept
            {
                while (m_pos < m_text.size() && is_space(m_text[m_pos]))
                {
                    ++m_pos;
                }
            }

            /// Moves past the next @p token. False, without moving, when there is none.
            [[nodiscard]] bool skip_to(std::string_view token) noexcept
            {
                const std::size_t found = m_text.find(token, m_pos);
                if (found == std::string_view::npos)
                {
                    return false;
                }
                m_pos = found + token.size();
                return true;
            }

            /// Skips whitespace, processing instructions, comments and declarations.
            void skip_misc() noexcept
            {
                for (;;)
                {
                    skip_space();
                    if (at("<?"))
                    {
                        if (!skip_to("?>"))
                            return;
                    }
                    else if (at("<!--"))
                    {
                        if (!skip_to("-->"))
                            return;
                    }
                    else if (at("<!"))
                    {
                        if (!skip_to(">"))
                            return;
                    }
                    else
                    {
                        return;
                    }
                }
            }

            /// An element or attribute name: everything up to whitespace, '/', '>' or '='.
            [[nodiscard]] std::string name()
            {
                const std::size_t start = m_pos;
                while (m_pos < m_text.size() && !is_space(m_text[m_pos]) && m_text[m_pos] != '/' &&
                       m_text[m_pos] != '>' && m_text[m_pos] != '=')
                {
                    ++m_pos;
                }
                return std::string{m_text.substr(start, m_pos - start)};
            }

            /**
             * @brief Reads the element at the cursor, nested @p depth deep, into @p out. False on malformed markup or
             *        past 64 levels, which bounds the recursion.
             */
            [[nodiscard]] bool element(XmlElement &out, int depth)
            {
                if (depth > 64 || m_pos >= m_text.size() || m_text[m_pos] != '<')
                {
                    return false;
                }
                ++m_pos;
                out.tag = name();
                if (out.tag.empty())
                {
                    return false;
                }
                for (;;)
                {
                    skip_space();
                    if (m_pos >= m_text.size())
                    {
                        return false;
                    }
                    if (at("/>"))
                    {
                        m_pos += 2;
                        return true;
                    }
                    if (m_text[m_pos] == '>')
                    {
                        ++m_pos;
                        break;
                    }
                    XmlAttr attr;
                    attr.name = name();
                    skip_space();
                    if (attr.name.empty() || m_pos >= m_text.size() || m_text[m_pos] != '=')
                    {
                        return false;
                    }
                    ++m_pos;
                    skip_space();
                    if (m_pos >= m_text.size() || (m_text[m_pos] != '"' && m_text[m_pos] != '\''))
                    {
                        return false;
                    }
                    attr.quote = m_text[m_pos++];
                    const std::size_t close = m_text.find(attr.quote, m_pos);
                    if (close == std::string_view::npos)
                    {
                        return false;
                    }
                    attr.value = std::string{m_text.substr(m_pos, close - m_pos)};
                    m_pos = close + 1;
                    out.attrs.push_back(std::move(attr));
                }
                for (;;)
                {
                    const std::size_t lt = m_text.find('<', m_pos);
                    if (lt == std::string_view::npos)
                    {
                        return false;
                    }
                    m_pos = lt;
                    if (at("</"))
                    {
                        return skip_to(">");
                    }
                    if (at("<!--"))
                    {
                        if (!skip_to("-->"))
                            return false;
                        continue;
                    }
                    if (at("<![CDATA["))
                    {
                        if (!skip_to("]]>"))
                            return false;
                        continue;
                    }
                    // A processing instruction runs to its "?>", however much markup sits inside it: First Person
                    // Overhaul turns whole fragment options off by writing them as "<?Fragment ...>...?>".
                    if (at("<?"))
                    {
                        if (!skip_to("?>"))
                            return false;
                        continue;
                    }
                    if (at("<!"))
                    {
                        if (!skip_to(">"))
                            return false;
                        continue;
                    }
                    XmlElement child;
                    if (!element(child, depth + 1))
                    {
                        return false;
                    }
                    out.children.push_back(std::move(child));
                }
            }

            std::string_view m_text;
            std::size_t m_pos = 0;
        };

    } // namespace

    const std::string *XmlElement::attr(std::string_view key) const noexcept
    {
        for (const XmlAttr &a : attrs)
        {
            if (a.name == key)
            {
                return &a.value;
            }
        }
        return nullptr;
    }

    std::string XmlElement::get(std::string_view key) const
    {
        const std::string *value = attr(key);
        return value != nullptr ? *value : std::string{};
    }

    const XmlElement *XmlElement::child(std::string_view name) const noexcept
    {
        for (const XmlElement &c : children)
        {
            if (c.tag == name)
            {
                return &c;
            }
        }
        return nullptr;
    }

    void XmlElement::set(std::string_view key, std::string value)
    {
        for (XmlAttr &a : attrs)
        {
            if (a.name == key)
            {
                a.value = std::move(value);
                return;
            }
        }
        attrs.push_back(
            XmlAttr{
                .name = std::string{key},
                .value = std::move(value),
            }
        );
    }

    void XmlElement::erase(std::string_view key)
    {
        std::erase_if(attrs, [key](const XmlAttr &a) { return a.name == key; });
    }

    std::optional<XmlElement> read_xml_elements(std::string_view text)
    {
        return Reader{text}.document();
    }

    void write_xml_elements(const XmlElement &element, std::string &out)
    {
        out += '<';
        out += element.tag;
        for (const XmlAttr &a : element.attrs)
        {
            out += ' ';
            out += a.name;
            out += '=';
            out += a.quote;
            out += a.value;
            out += a.quote;
        }
        if (element.children.empty())
        {
            out += "/>\n";
            return;
        }
        out += ">\n";
        for (const XmlElement &c : element.children)
        {
            write_xml_elements(c, out);
        }
        out += "</";
        out += element.tag;
        out += ">\n";
    }
} // namespace AnimationFramework
