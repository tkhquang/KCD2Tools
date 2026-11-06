/**
 * @file core/anim_catalog.cpp
 * @brief Reads the Mannequin files into the catalog's lines and searches them (see anim_catalog.hpp).
 *
 * The files are scanned as text instead of parsed: a database runs to tens of megabytes, only a few of its elements
 * matter here, and the shapes read are the ones Mannequin itself writes.
 */

#include "core/anim_catalog.hpp"
#include "core/text_util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::string_view FEMALE_DATABASE = "animations/mannequin/adb/wh_female_database.adb";

        constexpr std::string_view FRAGMENT_OPEN = "<Fragment ";
        constexpr std::string_view FRAGMENT_CLOSE = "</Fragment>";
        constexpr std::string_view SUB_DATABASE_OPEN = "<SubADB ";
        constexpr std::string_view TAG_OPEN = "<Tag name=\"";
        constexpr std::string_view ANIMATION_OPEN = "<Animation name=\"";
        constexpr std::string_view LAYER_OPEN = "<AnimLayer";
        constexpr std::string_view SCOPES_ATTRIBUTE = " scopes=\"";

        constexpr std::string_view SCOPE_TAG_PREFIX = "Scope_";
        constexpr std::string_view FULL_BODY_SCOPE = "FullBody";
        constexpr std::string_view HORSE_TAG = "horse";
        constexpr std::array<std::string_view, 2> SEAT_TAGS{"sitting", "sittingNoTable"};

        constexpr std::string_view MODE_APPEND = "append";
        constexpr std::string_view MODE_REMOVE = "remove";
        constexpr std::string_view MODE_REPLACE = "replace";

        constexpr std::string_view STAND = "stand";
        constexpr std::string_view SEAT = "seat";
        constexpr std::string_view HORSE = "horse";
        constexpr std::string_view SEAT_PREFIX = "seat:";
        constexpr std::string_view HORSE_PREFIX = "horse:";
        constexpr std::string_view PARTIAL = "partial";
        constexpr std::string_view WOMEN = "women";

        constexpr char OPTION_MARK = '@';
        constexpr char FIELD_SEPARATOR = '\t';
        /// The index of an option line's set field.
        constexpr std::size_t OPTION_SET_FIELD = 4;
        /// The index of a fragment line's seat field.
        constexpr std::size_t FRAGMENT_SEAT_FIELD = 3;
        /// The index of a fragment line's horse field.
        constexpr std::size_t FRAGMENT_HORSE_FIELD = 4;

        /** @brief One option of a fragment. */
        struct Option
        {
            /// The Tags and then the FragTags, as written: an option's number is counted on them in this order.
            std::vector<std::string> tags;
            /// Every clip with a name.
            std::vector<std::string> clips;
            /// Whether it has an animation layer, as every option that plays a clip has.
            bool has_layer = false;
            /// The tag sets, the same for options a patch counts as one.
            std::string key;
            /// A patch option's Mode, lowered.
            std::string mode;
        };

        /** @brief One fragment of a database and its options in order. */
        struct Fragment
        {
            std::string name;
            std::vector<Option> options;
        };

        /** @brief One database file with its patches merged, its fragments in file order. */
        struct Database
        {
            std::vector<Fragment> fragments;
            /** @brief Each fragment's position by its lowercased name, as the game load matches fragment names. */
            std::unordered_map<std::string, std::size_t> index;
        };

        /** @brief The tags a fragment plays on, by posture. */
        struct PostureTags
        {
            std::string stand;
            std::optional<std::string> seat;
            std::optional<std::string> horse;
        };

        /// Whether @p c can be part of an element or attribute name: an ASCII letter, a digit or an underscore.
        [[nodiscard]] constexpr bool is_word(char c) noexcept
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }

        /// Whether @p c is XML whitespace.
        [[nodiscard]] constexpr bool is_space(char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        /** @brief The value of the attribute written `name="` in @p attributes, as a whole word, or empty. */
        [[nodiscard]] std::string_view attribute(std::string_view attributes, std::string_view name)
        {
            for (std::size_t at = attributes.find(name); at != std::string_view::npos;
                 at = attributes.find(name, at + 1))
            {
                if (at > 0 && is_word(attributes[at - 1]))
                {
                    continue;
                }
                const std::size_t start = at + name.size();
                const std::size_t end = attributes.find('"', start);
                return end == std::string_view::npos ? std::string_view{} : attributes.substr(start, end - start);
            }
            return {};
        }

        /** @brief @p parts joined by '+', leaving out the scope tags when @p drop_scopes. */
        [[nodiscard]] std::string join_tags(const std::vector<std::string> &parts, bool drop_scopes)
        {
            std::string joined;
            for (const std::string &part : parts)
            {
                if (drop_scopes && part.starts_with(SCOPE_TAG_PREFIX))
                {
                    continue;
                }
                if (!joined.empty())
                {
                    joined += '+';
                }
                joined += part;
            }
            return joined;
        }

        /// Whether @p option is for another scope: one of its tags is a `Scope_` tag.
        [[nodiscard]] bool has_scope_tag(const Option &option)
        {
            return std::ranges::any_of(
                option.tags,
                [](const std::string &t) { return t.starts_with(SCOPE_TAG_PREFIX); }
            );
        }

        /** @brief One option from its element's @p attributes and its @p inner text (empty for `<Fragment .../>`). */
        [[nodiscard]] Option read_option(std::string_view attributes, std::string_view inner)
        {
            const std::string_view tags = attribute(attributes, "Tags=\"");
            const std::string_view frag_tags = attribute(attributes, "FragTags=\"");
            Option option;
            option.tags = split_tags(tags);
            std::ranges::move(split_tags(frag_tags), std::back_inserter(option.tags));
            option.has_layer = inner.contains(LAYER_OPEN);
            option.key = tag_set(tags) + "|" + tag_set(frag_tags);
            option.mode = lowercase(attribute(attributes, "Mode=\""));
            for (std::size_t at = inner.find(ANIMATION_OPEN); at != std::string_view::npos;
                 at = inner.find(ANIMATION_OPEN, at))
            {
                at += ANIMATION_OPEN.size();
                const std::size_t end = inner.find('"', at);
                if (end == std::string_view::npos)
                {
                    break;
                }
                if (end > at)
                {
                    option.clips.emplace_back(inner.substr(at, end - at));
                }
            }
            return option;
        }

        /** @brief The options of one fragment's element, `<Fragment .../>` and `<Fragment ...>...</Fragment>`. */
        [[nodiscard]] std::vector<Option> read_options(std::string_view body)
        {
            std::vector<Option> options;
            for (std::size_t at = body.find(FRAGMENT_OPEN); at != std::string_view::npos;
                 at = body.find(FRAGMENT_OPEN, at))
            {
                const std::size_t start = at + FRAGMENT_OPEN.size();
                const std::size_t close = body.find('>', start);
                if (close == std::string_view::npos)
                {
                    break;
                }
                if (close > start && body[close - 1] == '/')
                {
                    options.push_back(read_option(body.substr(start, close - 1 - start), {}));
                    at = close + 1;
                    continue;
                }
                const std::size_t end = body.find(FRAGMENT_CLOSE, close + 1);
                if (end == std::string_view::npos)
                {
                    break;
                }
                options.push_back(
                    read_option(body.substr(start, close - start), body.substr(close + 1, end - close - 1))
                );
                at = end + FRAGMENT_CLOSE.size();
            }
            return options;
        }

        /**
         * @brief Calls @p visit(name, body) for each fragment of a database: an attribute-free element whose first
         *        child is a `<Fragment `. The element list, blends and sub-databases never have that shape.
         */
        template <typename Visit> void for_each_fragment(std::string_view text, Visit visit)
        {
            std::size_t at = text.find('<');
            while (at != std::string_view::npos)
            {
                std::size_t end = at + 1;
                while (end < text.size() && is_word(text[end]))
                {
                    ++end;
                }
                std::size_t next = end;
                if (end > at + 1 && end < text.size() && text[end] == '>')
                {
                    std::size_t body = end + 1;
                    while (body < text.size() && is_space(text[body]))
                    {
                        ++body;
                    }
                    if (text.substr(body).starts_with(FRAGMENT_OPEN))
                    {
                        const std::string_view name = text.substr(at + 1, end - at - 1);
                        const std::string closing = "</" + std::string{name} + ">";
                        if (const std::size_t close = text.find(closing, body); close != std::string_view::npos)
                        {
                            visit(name, text.substr(body, close - body));
                            next = close + closing.size();
                        }
                    }
                }
                at = text.find('<', next);
            }
        }

        /**
         * @brief Merges @p patch into @p stock the way the game load does (merge_options in hooks/anim_patch.cpp), so
         *        an option's number here is its number in the loaded database.
         */
        void merge_options(std::vector<Option> &stock, std::vector<Option> patch)
        {
            /// A patch's options of one key and the Mode its first one states.
            struct Group
            {
                std::string mode;
                std::vector<Option> options;
            };
            std::vector<std::string> order;
            std::map<std::string, Group> groups;
            for (Option &option : patch)
            {
                auto [it, inserted] = groups.try_emplace(option.key);
                if (inserted)
                {
                    order.push_back(option.key);
                    it->second.mode = option.mode;
                }
                it->second.options.push_back(std::move(option));
            }
            std::map<std::string, std::size_t> last;
            for (std::size_t i = 0; i < stock.size(); ++i)
            {
                last[stock[i].key] = i;
            }
            std::vector<Option> merged;
            merged.reserve(stock.size() + patch.size());
            std::set<std::string> placed;
            const auto place = [&merged](Group &group) -> void
            {
                std::ranges::move(group.options, std::back_inserter(merged));
                group.options.clear();
            };
            for (std::size_t i = 0; i < stock.size(); ++i)
            {
                const std::string key = stock[i].key;
                const auto group = groups.find(key);
                if (group == groups.end())
                {
                    merged.push_back(std::move(stock[i]));
                    continue;
                }
                if (group->second.mode == MODE_APPEND)
                {
                    merged.push_back(std::move(stock[i]));
                    if (last[key] == i)
                    {
                        place(group->second);
                        placed.insert(key);
                    }
                    continue;
                }
                if (placed.insert(key).second && group->second.mode != MODE_REMOVE)
                {
                    place(group->second);
                }
            }
            for (const std::string &key : order)
            {
                Group &group = groups.at(key);
                if (!placed.contains(key) && group.mode != MODE_REMOVE)
                {
                    place(group);
                }
            }
            stock = std::move(merged);
        }

        /**
         * @brief Adds a database's or a patch's text to @p database. A new fragment goes at the end with its options in
         *        file order, as the game load adds it whole. A patch merges into a fragment the database has.
         */
        void add_database_text(Database &database, std::string_view text, bool is_patch)
        {
            for_each_fragment(
                text,
                [&database, is_patch](std::string_view name, std::string_view body)
                {
                    auto [it, inserted] = database.index.try_emplace(lowercase(name), database.fragments.size());
                    if (inserted)
                    {
                        database.fragments.push_back(
                            Fragment{
                                .name = std::string{name},
                                .options = {},
                            }
                        );
                    }
                    std::vector<Option> &stock = database.fragments[it->second].options;
                    std::vector<Option> options = read_options(body);
                    if (is_patch && !inserted)
                    {
                        merge_options(stock, std::move(options));
                    }
                    else
                    {
                        std::ranges::move(options, std::back_inserter(stock));
                    }
                }
            );
        }

        /** @brief Queues the files @p text names in its `<SubADB File="...">` elements. */
        void add_sub_databases(std::string_view text, std::vector<std::string> &waiting)
        {
            for (std::size_t at = text.find(SUB_DATABASE_OPEN); at != std::string_view::npos;
                 at = text.find(SUB_DATABASE_OPEN, at + 1))
            {
                const std::size_t close = text.find('>', at);
                if (close == std::string_view::npos)
                {
                    break;
                }
                const std::string_view file = attribute(text.substr(at, close - at), "File=\"");
                if (!file.empty())
                {
                    waiting.push_back(fold_path(file));
                }
            }
        }

        /** @brief The database at @p root and every one it includes, each with its patches merged. */
        [[nodiscard]] std::vector<Database> read_databases(const AnimCatalogFiles &files, std::string_view root)
        {
            std::vector<Database> databases;
            std::vector<std::string> waiting{std::string{root}};
            std::unordered_set<std::string> seen;
            while (!waiting.empty())
            {
                const std::string path = std::move(waiting.back());
                waiting.pop_back();
                if (seen.contains(path))
                {
                    continue;
                }
                const std::optional<std::string> text = files.read(path);
                if (!text.has_value())
                {
                    continue;
                }
                seen.insert(path);
                Database database;
                add_database_text(database, *text, false);
                add_sub_databases(*text, waiting);
                for (const std::string &patch : files.patches(path))
                {
                    add_database_text(database, patch, true);
                    add_sub_databases(patch, waiting);
                }
                databases.push_back(std::move(database));
            }
            return databases;
        }

        /** @brief Every fragment ID of the definition and its patches. */
        [[nodiscard]] std::unordered_set<std::string> read_fragment_ids(const AnimCatalogFiles &files)
        {
            std::unordered_set<std::string> ids;
            const auto add = [&ids](std::string_view text) -> void
            {
                for (std::size_t at = text.find(TAG_OPEN); at != std::string_view::npos; at = text.find(TAG_OPEN, at))
                {
                    at += TAG_OPEN.size();
                    const std::size_t end = text.find('"', at);
                    if (end == std::string_view::npos)
                    {
                        break;
                    }
                    ids.emplace(text.substr(at, end - at));
                }
            };
            const std::string path{MALE_FRAGMENT_IDS};
            if (const std::optional<std::string> text = files.read(path); text.has_value())
            {
                add(*text);
            }
            for (const std::string &patch : files.patches(path))
            {
                add(patch);
            }
            return ids;
        }

        /**
         * @brief Sets each fragment's scopes, by lowercased name as the game load matches definitions, from a
         *        controller definition's `<name scopes="...">` elements.
         */
        void add_fragment_scopes(std::unordered_map<std::string, std::string> &scopes, std::string_view text)
        {
            for (std::size_t at = text.find(SCOPES_ATTRIBUTE); at != std::string_view::npos;
                 at = text.find(SCOPES_ATTRIBUTE, at + 1))
            {
                std::size_t start = at;
                while (start > 0 && is_word(text[start - 1]))
                {
                    --start;
                }
                if (start == at || start == 0 || text[start - 1] != '<')
                {
                    continue;
                }
                const std::size_t value = at + SCOPES_ATTRIBUTE.size();
                const std::size_t end = text.find('"', value);
                const std::size_t close = end == std::string_view::npos ? end : text.find('>', end);
                if (close == std::string_view::npos)
                {
                    break;
                }
                // As the game load does (merge_fragment_def): only Mode="replace" clears a fragment's scopes.
                const std::string_view list = text.substr(value, end - value);
                if (!list.empty() || lowercase(attribute(text.substr(end, close - end), "Mode=\"")) == MODE_REPLACE)
                {
                    scopes[lowercase(text.substr(start, at - start))] = list;
                }
            }
        }

        /**
         * @brief The lowercased names of the fragments whose definition leaves out the full body, as the face and
         *        upper-body ones do.
         */
        [[nodiscard]] std::unordered_set<std::string> read_partial_fragments(const AnimCatalogFiles &files)
        {
            std::unordered_map<std::string, std::string> scopes;
            const std::string path{MALE_CONTROLLER_DEFS};
            if (const std::optional<std::string> text = files.read(path); text.has_value())
            {
                add_fragment_scopes(scopes, *text);
            }
            for (const std::string &patch : files.patches(path))
            {
                add_fragment_scopes(scopes, patch);
            }
            std::unordered_set<std::string> partial;
            for (const auto &[name, list] : scopes)
            {
                const std::vector<std::string> parts = split_tags(list);
                if (!parts.empty() && std::ranges::find(parts, FULL_BODY_SCOPE) == parts.end())
                {
                    partial.insert(name);
                }
            }
            return partial;
        }

        /** @brief The posture an option is for: a horse, a seat, or else standing. */
        [[nodiscard]] std::string_view posture_of(const Option &option)
        {
            const auto has = [&option](std::string_view tag) -> bool
            { return std::ranges::find(option.tags, tag) != option.tags.end(); };
            if (has(HORSE_TAG))
            {
                return HORSE;
            }
            return std::ranges::any_of(SEAT_TAGS, has) ? SEAT : STAND;
        }

        /**
         * @brief The tags of the best of @p options (one or more): not one for another scope, with an animation layer,
         *        on the fewest tags.
         */
        [[nodiscard]] std::string fewest_tags(std::span<const Option *const> options)
        {
            const auto rank = [](const Option *option) -> std::tuple<bool, bool, std::size_t>
            { return std::tuple{has_scope_tag(*option), !option->has_layer, option->tags.size()}; };
            // min_element keeps the first of equals, so the file's order decides a tie.
            return join_tags((*std::ranges::min_element(options, {}, rank))->tags, true);
        }

        /** @brief The tags a fragment with @p options (one or more) plays on in each posture (see fewest_tags). */
        [[nodiscard]] PostureTags posture_tags(const std::vector<const Option *> &options)
        {
            const auto best = [&options](std::string_view posture) -> std::optional<std::string>
            {
                std::vector<const Option *> chosen;
                std::ranges::copy_if(
                    options,
                    std::back_inserter(chosen),
                    [posture](const Option *option) { return posture_of(*option) == posture; }
                );
                return chosen.empty() ? std::nullopt : std::optional{fewest_tags(chosen)};
            };
            return PostureTags{
                .stand = best(STAND).value_or(fewest_tags(options)),
                .seat = best(SEAT),
                .horse = best(HORSE),
            };
        }

        /** @brief The field @p index of a tab-separated line, empty when it has fewer. */
        [[nodiscard]] std::string_view field(std::string_view line, std::size_t index)
        {
            std::size_t start = 0;
            for (std::size_t i = 0; i < index; ++i)
            {
                const std::size_t tab = line.find(FIELD_SEPARATOR, start);
                if (tab == std::string_view::npos)
                {
                    return {};
                }
                start = tab + 1;
            }
            return line.substr(start, line.find(FIELD_SEPARATOR, start) - start);
        }

        /// Whether a catalog line has a version in @p posture: "seat" or "horse", and for an option line also "stand".
        [[nodiscard]] bool has_posture(std::string_view line, std::string_view posture)
        {
            if (line.starts_with(OPTION_MARK))
            {
                return field(line, OPTION_SET_FIELD) == posture;
            }
            return posture == SEAT    ? field(line, FRAGMENT_SEAT_FIELD).starts_with(SEAT_PREFIX)
                   : posture == HORSE ? field(line, FRAGMENT_HORSE_FIELD).starts_with(HORSE_PREFIX)
                                      : false;
        }

        /// Whether a catalog line's fragment name starts with one of the @p hidden prefixes.
        [[nodiscard]] bool is_hidden(std::string_view line, std::span<const std::string> hidden)
        {
            const std::string_view name = line.starts_with(OPTION_MARK) ? line.substr(1) : line;
            return std::ranges::any_of(hidden, [name](const std::string &prefix) { return name.starts_with(prefix); });
        }

        /** @brief The catalog's text, and how many of its first lines are fragments. */
        struct BuiltText
        {
            std::string text;
            std::size_t fragment_count = 0;
        };

        /** @brief The catalog's lines (see anim_catalog.hpp) from Henry's files and the women's database. */
        [[nodiscard]] BuiltText build_text(const AnimCatalogFiles &files)
        {
            const std::vector<Database> male = read_databases(files, MALE_DATABASE);
            const std::unordered_set<std::string> ids = read_fragment_ids(files);
            const std::unordered_set<std::string> partial = read_partial_fragments(files);
            std::unordered_set<std::string> women;
            for (const Database &database : read_databases(files, FEMALE_DATABASE))
            {
                for (const Fragment &fragment : database.fragments)
                {
                    women.insert(fragment.name);
                }
            }

            std::unordered_map<std::string, std::vector<const Option *>> by_fragment;
            for (const Database &database : male)
            {
                for (const Fragment &fragment : database.fragments)
                {
                    auto &options = by_fragment[fragment.name];
                    for (const Option &option : fragment.options)
                    {
                        options.push_back(&option);
                    }
                }
            }
            std::unordered_map<std::string, PostureTags> needs;
            for (const auto &[name, options] : by_fragment)
            {
                if (!options.empty())
                {
                    needs.emplace(name, posture_tags(options));
                }
            }

            std::vector<std::string> names;
            for (const auto &[name, tags] : needs)
            {
                if (ids.contains(name))
                {
                    names.push_back(name);
                }
            }
            std::ranges::sort(names, {}, [](const std::string &name) { return std::pair{lowercase(name), name}; });

            BuiltText built;
            built.fragment_count = names.size();
            std::string &text = built.text;
            for (const std::string &name : names)
            {
                const PostureTags &tags = needs.at(name);
                text += name;
                text += FIELD_SEPARATOR;
                text += tags.stand;
                text += FIELD_SEPARATOR;
                text += partial.contains(lowercase(name)) ? PARTIAL : std::string_view{};
                text += FIELD_SEPARATOR;
                if (tags.seat.has_value())
                {
                    text += SEAT_PREFIX;
                    text += *tags.seat;
                }
                text += FIELD_SEPARATOR;
                if (tags.horse.has_value())
                {
                    text += HORSE_PREFIX;
                    text += *tags.horse;
                }
                text += FIELD_SEPARATOR;
                text += women.contains(name) ? WOMEN : std::string_view{};
                text += '\n';
            }

            std::map<std::pair<std::string, std::string>, int> numbers;
            for (const Database &database : male)
            {
                for (const Fragment &fragment : database.fragments)
                {
                    const std::string_view fragment_partial =
                        partial.contains(lowercase(fragment.name)) ? PARTIAL : std::string_view{};
                    for (const Option &option : fragment.options)
                    {
                        if (option.clips.empty() || has_scope_tag(option))
                        {
                            continue;
                        }
                        const std::string joined = join_tags(option.tags, false);
                        const int number = numbers[{fragment.name, joined}]++;
                        std::string_view where = posture_of(option);
                        if (where == STAND)
                        {
                            const auto need = needs.find(fragment.name);
                            where = need != needs.end() && need->second.stand == joined ? STAND : std::string_view{};
                        }
                        text += OPTION_MARK;
                        text += fragment.name;
                        text += FIELD_SEPARATOR;
                        text += option.clips.front();
                        text += FIELD_SEPARATOR;
                        text += joined;
                        text += FIELD_SEPARATOR;
                        text += std::to_string(number);
                        text += FIELD_SEPARATOR;
                        text += where;
                        text += FIELD_SEPARATOR;
                        text += fragment_partial;
                        text += FIELD_SEPARATOR;
                        for (std::size_t i = 1; i < option.clips.size(); ++i)
                        {
                            if (i > 1)
                            {
                                text += ' ';
                            }
                            text += option.clips[i];
                        }
                        text += '\n';
                    }
                }
            }
            return built;
        }
    } // namespace

    AnimCatalog::AnimCatalog(const AnimCatalogFiles &files)
    {
        BuiltText built = build_text(files);
        m_text = std::move(built.text);
        m_fragment_count = built.fragment_count;
        m_lowered = lowercase(m_text);
        for (std::size_t start = 0; start < m_text.size();)
        {
            const std::size_t end = std::min(m_text.find('\n', start), m_text.size());
            m_lines.push_back(
                Line{
                    .text = std::string_view{m_text}.substr(start, end - start),
                    .lowered = std::string_view{m_lowered}.substr(start, end - start),
                }
            );
            start = end + 1;
        }
    }

    AnimCatalogPage AnimCatalog::find(const AnimCatalogQuery &query, std::span<const std::string> hidden) const
    {
        const std::string wanted = lowercase(query.text);
        const std::size_t end = query.options ? m_lines.size() : m_fragment_count;
        AnimCatalogPage page;
        for (std::size_t i = 0; i < end; ++i)
        {
            const Line &line = m_lines[i];
            if ((!wanted.empty() && !line.lowered.contains(wanted)) || is_hidden(line.text, hidden) ||
                (!query.posture.empty() && !has_posture(line.text, query.posture)))
            {
                continue;
            }
            if (page.total >= query.first && page.total - query.first < query.count)
            {
                page.lines += line.text;
                page.lines += '\n';
            }
            ++page.total;
        }
        return page;
    }

    std::optional<std::string_view> AnimCatalog::fragment_line(std::string_view name) const
    {
        for (std::size_t i = 0; i < m_fragment_count; ++i)
        {
            const std::string_view line = m_lines[i].text;
            if (line.starts_with(name) && line.size() > name.size() && line[name.size()] == FIELD_SEPARATOR)
            {
                return line;
            }
        }
        return std::nullopt;
    }
} // namespace AnimationFramework
