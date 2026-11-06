/**
 * @file core/mod_tools.cpp
 * @brief Clip check and mod maker steps shared by the console commands and the offline tool (see mod_tools.hpp).
 */

#include "core/mod_tools.hpp"

#include "core/clip_check.hpp"
#include "core/text_util.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <set>

namespace AnimationFramework
{
    namespace
    {
        constexpr std::string_view MALE_CLIP_ROOT = "animations/humans/male/";

        /// Whether @p name can name a fragment: ASCII letters, digits and underscores, at least one.
        [[nodiscard]] bool is_fragment_name(std::string_view name) noexcept
        {
            return !name.empty() && std::ranges::all_of(
                                        name,
                                        [](char c)
                                        {
                                            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                                   (c >= '0' && c <= '9') || c == '_';
                                        }
                                    );
        }

        /**
         * @brief An entry as the game loads it, read by @p find from the newest patch that holds it, else from the
         *        game's copy, searching @p root and then the files @p next lists (sub-databases).
         */
        template <typename Find, typename Next>
        [[nodiscard]] std::optional<XmlElement>
        newest_entry(const AnimCatalogFiles &files, std::string_view root, Find find, Next next)
        {
            std::vector<std::string> queue{std::string{root}};
            std::set<std::string> seen;
            while (!queue.empty())
            {
                const std::string path = queue.front();
                queue.erase(queue.begin());
                if (!seen.insert(path).second)
                {
                    continue;
                }
                const std::vector<std::string> patches =
                    files.patches ? files.patches(path) : std::vector<std::string>{};
                for (auto it = patches.rbegin(); it != patches.rend(); ++it)
                {
                    if (const std::optional<XmlElement> patch = read_xml_elements(*it))
                    {
                        if (const XmlElement *found = find(*patch))
                        {
                            return *found;
                        }
                    }
                }
                const std::optional<std::string> text = files.read(path);
                const std::optional<XmlElement> game = text ? read_xml_elements(*text) : std::nullopt;
                if (!game)
                {
                    continue;
                }
                if (const XmlElement *found = find(*game))
                {
                    return *found;
                }
                for (std::string &sub : next(*game))
                {
                    queue.push_back(std::move(sub));
                }
            }
            return std::nullopt;
        }

        /// Prints the warnings of @p clip's check on Henry's skeleton, against @p reference when given.
        void print_warnings(
            const ModToolGame &game,
            const FoundClip &clip,
            const FoundClip *reference,
            bool loops,
            const ToolPrint &print
        )
        {
            const std::vector<SkeletonBone> *skeleton = game.skeleton ? game.skeleton() : nullptr;
            if (skeleton == nullptr)
            {
                print("  (Henry's skeleton could not be read: the clip is not checked)");
                return;
            }
            const ClipMotionCheck options{
                .loops = loops,
                .reference = reference != nullptr ? &reference->clip : nullptr,
            };
            for (const ClipFinding &finding : check_clip_motion(clip.clip, *skeleton, options))
            {
                if (finding.level == ClipFindingLevel::Warning)
                {
                    print(std::format("  WARNING: {} {}", clip.path, finding.text));
                }
            }
        }
    } // namespace

    int report_clip_check(const ModToolGame &game, const ClipCheckRequest &request, const ToolPrint &print)
    {
        const std::vector<SkeletonBone> *skeleton = game.skeleton ? game.skeleton() : nullptr;
        if (skeleton == nullptr)
        {
            print("Henry's skeleton could not be read");
            return -1;
        }
        auto clip = game.find_clip(request.clip);
        if (!clip)
        {
            print(clip.error());
            return -1;
        }
        std::optional<FoundClip> reference;
        if (!request.reference.empty())
        {
            auto found = game.find_clip(request.reference);
            if (!found)
            {
                print(std::format("reference {}", found.error()));
                return -1;
            }
            reference = std::move(*found);
        }

        print(std::format("{} ({}, from {}):", request.clip, clip->path, clip->source));
        // A clip under another folder of the game's data plays on another skeleton. A clip outside it (a file of an
        // exporter's) can be meant for any skeleton.
        if (clip->path.starts_with("animations/") && !clip->path.starts_with(MALE_CLIP_ROOT))
        {
            print("  - not a male clip: checked against Henry's skeleton all the same");
        }
        const ClipMotionCheck options{
            .loops = request.loops,
            .reference = reference ? &reference->clip : nullptr,
        };
        int warnings = 0;
        for (const ClipFinding &finding : check_clip_motion(clip->clip, *skeleton, options))
        {
            const bool warning = finding.level == ClipFindingLevel::Warning;
            warnings += warning ? 1 : 0;
            print(std::format("  {} {}", warning ? "WARNING:" : "-", finding.text));
        }
        print(std::format("  {} warning(s)", warnings));
        return warnings;
    }

    std::expected<MadeAction, std::string> make_mod_action(
        const ModToolGame &game,
        const ModAction &action,
        std::string_view like,
        const ModPatches &current,
        const ToolPrint &print
    )
    {
        if (!is_mod_id(action.modid))
        {
            return std::unexpected{std::format("{}: a mod id is lowercase letters and underscores", action.modid)};
        }
        if (!is_fragment_name(action.fragment))
        {
            return std::unexpected{
                std::format("{}: a fragment name is letters, digits and underscores", action.fragment)
            };
        }
        // The clip name goes into the patches as the option's animation name, an attribute value the XML writer copies
        // verbatim, and it names an upper-body action's blend space file: no markup and no path.
        if (action.clip.empty() || action.clip.find_first_of("&<>\"'/\\:") != std::string::npos)
        {
            return std::unexpected{std::format(
                "{}: a clip name is the clip's file name without .caf, with no /, \\, :, &, <, >, \" or '",
                action.clip
            )};
        }
        const std::optional<std::string> ids_text = game.files.read(std::string{MALE_FRAGMENT_IDS});
        const std::optional<XmlElement> game_ids = ids_text ? read_xml_elements(*ids_text) : std::nullopt;
        if (game_ids && lists_fragment(*game_ids, action.fragment))
        {
            return std::unexpected{std::format(
                "{} is a game fragment: give the new one a name of its own, such as {}_{}",
                action.fragment,
                action.modid,
                action.fragment
            )};
        }
        const auto like_options = newest_entry(
            game.files,
            MALE_DATABASE,
            [like](const XmlElement &db) { return find_fragment_options(db, like); },
            [](const XmlElement &db) { return sub_databases(db); }
        );
        const auto like_definition = newest_entry(
            game.files,
            MALE_CONTROLLER_DEFS,
            [like](const XmlElement &defs) { return find_fragment_definition(defs, like); },
            [](const XmlElement &) { return std::vector<std::string>{}; }
        );
        if (!like_options || !like_definition)
        {
            return std::unexpected{
                std::format("{}: no game fragment of that name has options and a definition to copy", like)
            };
        }

        const auto clip = game.find_clip(action.clip);
        if (!clip)
        {
            return std::unexpected{clip.error()};
        }
        // The game clip the copied option plays: a full-body clip in its place must cover the bones it covers.
        const XmlElement *model = template_option(*like_options);
        const std::string model_clip = model != nullptr ? first_clip(*model) : std::string{};
        std::optional<FoundClip> reference;
        if (!model_clip.empty())
        {
            if (auto found = game.find_clip(model_clip))
            {
                reference = std::move(*found);
            }
        }
        print_warnings(game, *clip, reference ? &*reference : nullptr, action.loop, print);
        if (!clip_stem(clip->path).starts_with(action.modid))
        {
            print(
                std::format(
                    "  note: {} plays by its file name; one that starts with {} cannot collide with a game clip or "
                    "another mod's",
                    action.clip,
                    action.modid
                )
            );
        }

        auto patches = add_mod_action(current, action, *like_definition, *like_options);
        if (!patches)
        {
            return std::unexpected{std::format("{}: {}", action.fragment, patches.error())};
        }
        return MadeAction{
            .patches = std::move(*patches),
            .scopes = like_definition->get("scopes"),
        };
    }
} // namespace AnimationFramework
